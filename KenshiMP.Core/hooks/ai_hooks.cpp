#include "ai_hooks.h"
#include "kmp/hook_manager.h"
#include "kmp/patterns.h"
#include "../core.h"
#include "../game/game_types.h"
#include "../sys/prologue_analyzer.h"
#include <spdlog/spdlog.h>
#include <unordered_set>
#include <mutex>

namespace kmp::ai_hooks {

// ── Function typedefs ──
// Local typedefs match KenshiMP.Core/game/game_types.h (kept in sync).
// AI::create is a constructor-style initializer with 6 parameters:
//   RCX=this, RDX=character, R8=arg3, R9=arg4, stack args 5/6 stored at
//   this+0x318 and this+0x10. Forwarding only RCX/RDX leaves +0x318 null
//   and AI scoring crashes later at game+0x59820D (null `this` AV at +0x60).
// Earlier 2-arg/3-arg signatures both produced the deferred crash signature
// — bisect 2026-05-02 (and independent confirmation from andperks6 fork
// commit f5330f9) showed all 6 args must be forwarded.
using AICreateFn   = void(__fastcall*)(void* ai, void* character, void* arg3,
                                       void* arg4, void* arg5, void* arg6);
using AIPackagesFn = void(__fastcall*)(void* character, void* aiPackage);

// ── State ──
static AICreateFn   s_origAICreate   = nullptr;
static AIPackagesFn s_origAIPackages = nullptr;
static int s_createCount = 0;
static int s_packageCount = 0;

// ── Remote-controlled character tracking ──
// Characters in this set have their AI decisions overridden by network input.
// The AI CONTROLLER is kept valid (no nullptr!) so the engine doesn't crash
// when downstream code dereferences it. Only the AI DECISIONS are suppressed.
static std::unordered_set<void*> s_remoteControlled;
static std::mutex s_remoteMutex;

void MarkRemoteControlled(void* character) {
    std::lock_guard lock(s_remoteMutex);
    s_remoteControlled.insert(character);
    spdlog::info("ai_hooks: Marked character 0x{:X} as remote-controlled ({} total)",
                 (uintptr_t)character, s_remoteControlled.size());
}

void UnmarkRemoteControlled(void* character) {
    std::lock_guard lock(s_remoteMutex);
    s_remoteControlled.erase(character);
}

bool IsRemoteControlled(void* character) {
    std::lock_guard lock(s_remoteMutex);
    return s_remoteControlled.count(character) > 0;
}

// ── Hooks ──

// Hook_AICreate: 6-arg pass-through.
//
// AI::create is a constructor-style initializer:
//   RCX=this, RDX=character, R8=arg3, R9=arg4,
//   stack arg 5 stored at this+0x318, stack arg 6 stored at this+0x10.
//
// Earlier wrong signatures (2-arg upstream, our 3-arg) caused a deferred
// null-this crash at game+0x59820D (~5s into world load) — the args 5/6
// path leaves this+0x318 null and AI scoring later dereferences it.
// Bisect history (2026-05-02):
//   2-arg / 3-arg + SEH:                       crash on world load
//   3-arg minimal pass-through:                no immediate crash, but
//                                              this+0x318 still gets junk;
//                                              latent corruption.
//   6-arg minimal pass-through (this version): all upper slots forwarded;
//                                              this+0x318 / this+0x10 land
//                                              correctly.
//
// Independent verification from andperks6/Kenshi-Online fork (commit f5330f9).
//
// No SEH, no post-call. The hook body is intentionally a forwarder; any
// Core::Get / EntityRegistry / mutex access from inside this context
// destabilizes the engine in MP. IsRemoteControlled tracking, when its
// consumer (movement_hooks) gets re-enabled, should live in
// packet_handler::HandleSpawnEntity instead.
static void __fastcall Hook_AICreate(void* ai, void* character, void* arg3,
                                     void* arg4, void* arg5, void* arg6) {
    s_origAICreate(ai, character, arg3, arg4, arg5, arg6);
}

// Hook_AIPackages: minimal 2-arg pass-through.
// Signature confirmed 2-arg by andperks6 binary inspection (commit f5330f9).
static void __fastcall Hook_AIPackages(void* character, void* aiPackage) {
    s_origAIPackages(character, aiPackage);
}

// ── Install / Uninstall ──

bool Install() {
    auto& funcs = Core::Get().GetGameFunctions();
    auto& hooks = HookManager::Get();
    int installed = 0;

    // ─────────────────────────────────────────────────────────────────────────
    // Both hooks are installed but IMMEDIATELY DISABLED. They corrupt character
    // state when active during SP / pre-connect (chars can move but cannot
    // attack — verified by bisect 2026-05-02 in pure vanilla Kenshi).
    // ResumeForNetwork() enables them after a server connection is up.
    // Mirrors the entity_hooks CharacterCreate pattern.
    // ─────────────────────────────────────────────────────────────────────────

    if (funcs.AICreate) {
        // Verify our typedef arg count against the live binary BEFORE
        // installing — same diagnostic that would have caught the
        // upstream 2-arg-vs-actually-6 bug at install time instead of
        // ~5s into world load.
        prologue_analyzer::VerifyArgCount(
            "AICreate", reinterpret_cast<uintptr_t>(funcs.AICreate),
            /*expectedArgCount=*/6);
        if (hooks.InstallAt("AICreate", reinterpret_cast<uintptr_t>(funcs.AICreate),
                            &Hook_AICreate, &s_origAICreate)) {
            hooks.Disable("AICreate");
            installed++;
            spdlog::info("ai_hooks: AICreate hook installed (DISABLED until ResumeForNetwork)");
        }
    }

    if (funcs.AIPackages) {
        prologue_analyzer::VerifyArgCount(
            "AIPackages", reinterpret_cast<uintptr_t>(funcs.AIPackages),
            /*expectedArgCount=*/2);
        if (hooks.InstallAt("AIPackages", reinterpret_cast<uintptr_t>(funcs.AIPackages),
                            &Hook_AIPackages, &s_origAIPackages)) {
            hooks.Disable("AIPackages");
            installed++;
            spdlog::info("ai_hooks: AIPackages hook installed (DISABLED until ResumeForNetwork)");
        }
    }

    spdlog::info("ai_hooks: {}/2 hooks installed (both bypassed at startup)", installed);
    return installed > 0;
}

void ResumeForNetwork() {
    auto& hooks = HookManager::Get();
    int enabled = 0;
    if (s_origAICreate   && hooks.Enable("AICreate"))   enabled++;
    if (s_origAIPackages && hooks.Enable("AIPackages")) enabled++;
    spdlog::info("ai_hooks: ResumeForNetwork — {} hook(s) enabled", enabled);
}

void SuspendForDisconnect() {
    auto& hooks = HookManager::Get();
    if (s_origAICreate)   hooks.Disable("AICreate");
    if (s_origAIPackages) hooks.Disable("AIPackages");
    spdlog::info("ai_hooks: SuspendForDisconnect — hooks bypassed");

    // Clear remote tracking on disconnect — stale entries would be wrong on reconnect
    std::lock_guard lock(s_remoteMutex);
    s_remoteControlled.clear();
}

void Uninstall() {
    auto& hooks = HookManager::Get();
    if (s_origAICreate)   hooks.Remove("AICreate");
    if (s_origAIPackages) hooks.Remove("AIPackages");
    s_origAICreate = nullptr;
    s_origAIPackages = nullptr;

    // Clear remote tracking
    std::lock_guard lock(s_remoteMutex);
    s_remoteControlled.clear();
}

} // namespace kmp::ai_hooks
