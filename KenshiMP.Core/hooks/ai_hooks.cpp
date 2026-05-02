#include "ai_hooks.h"
#include "kmp/hook_manager.h"
#include "kmp/patterns.h"
#include "../core.h"
#include "../game/game_types.h"
#include <spdlog/spdlog.h>
#include <unordered_set>
#include <mutex>

namespace kmp::ai_hooks {

// ── Function typedefs ──
// AI::create is a C++ MEMBER function: void* AI::create(Character*, Faction*)
//   RCX = AI* this        — the AI controller instance being created
//   RDX = Character*       — the character the AI is being attached to
//   R8  = Faction*         — the faction the character belongs to
// Bug fixed 2026-05-02: previously typed as 2-arg (character, faction). RCX was
// misread as character (was actually `this`), RDX as faction (was actually
// character), and R8 (the real faction) was never forwarded — it leaked whatever
// junk was in the register. The original then took the "[AI::create] No faction
// for" error path and produced a faction-less AI controller, which couldn't
// dispatch attack actions. Symptom: chars could move but not attack.
using AICreateFn   = void*(__fastcall*)(void* aiThis, void* character, void* faction);
// AI::loadPackages is similarly likely a member function. Add the `this` slot
// for consistency. If this turns out to be a free 2-arg function, the only cost
// is one extra register pass (R8 unused) — harmless.
using AIPackagesFn = void(__fastcall*)(void* aiThis, void* character, void* aiPackage);

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

// Hook_AICreate: MINIMAL PASS-THROUGH.
//
// AI::create(this, Character*, Faction*) is sensitive to anything our hook does
// beyond forwarding the call. Bisect 2026-05-02:
//   - Minimal body (this version):              NPCs behave normally
//   - + SEH __try/__except wrap:                MP crash at game+0x59820D ~5s
//                                                after world load (null this).
//                                                SEH frame interferes with C++
//                                                exception unwinding inside
//                                                AICreate.
//   - + Core::Get/EntityRegistry/MarkRemote:    NPC flee on attack, then crash
//                                                on next attack. Mutex/singleton
//                                                access from this context is
//                                                unsafe.
//
// ai_hooks::IsRemoteControlled() tracking should be done from the entity
// registry's MarkRemote path or in packet_handler when a remote entity is
// registered — NOT from inside this hook. (Currently it has no consumer
// either way: movement_hooks is also bypassed for trampoline reasons.)
//
// Args (verified by log inspection of real Kenshi 1.0.68 calls):
//   RCX = AI* this — instance being created
//   RDX = Character* — character receiving the AI controller
//   R8  = Faction* — faction the character belongs to
// Pre-fix the typedef was 2-arg, dropping R8 (the real faction). That made
// the original take the "[AI::create] No faction for" error path and produce
// a faction-less AI controller — char could move but not attack.
static void* __fastcall Hook_AICreate(void* aiThis, void* character, void* faction) {
    return s_origAICreate(aiThis, character, faction);
}

// Hook_AIPackages: MINIMAL PASS-THROUGH (same constraints as Hook_AICreate).
//
// AI::loadPackages signature mirrors AI::create — assumed to be a member
// function (this, Character*, AIPackage*). The 3-arg signature has not been
// independently verified by binary inspection; if the underlying function is
// actually 2-arg, the extra R8 forwarding is harmless (callee ignores it).
//
// No SEH, no post-call. Same hard-won lesson as Hook_AICreate: any work beyond
// forwarding from inside this hook destabilizes the engine.
static void __fastcall Hook_AIPackages(void* aiThis, void* character, void* aiPackage) {
    s_origAIPackages(aiThis, character, aiPackage);
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
        if (hooks.InstallAt("AICreate", reinterpret_cast<uintptr_t>(funcs.AICreate),
                            &Hook_AICreate, &s_origAICreate)) {
            hooks.Disable("AICreate");
            installed++;
            spdlog::info("ai_hooks: AICreate hook installed (DISABLED until ResumeForNetwork)");
        }
    }

    if (funcs.AIPackages) {
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
