#include "ai_hooks.h"
#include "entity_hooks.h"
#include "kmp/hook_manager.h"
#include "kmp/patterns.h"
#include "../core.h"
#include "../game/game_types.h"
#include "../native/our_factory.h"
#include <spdlog/spdlog.h>
#include <unordered_set>
#include <mutex>
#include <Windows.h>

namespace kmp::ai_hooks {

// ── Function typedefs ──
//
// NOTE 2026-05-07: the runtime scanner has resolved AICreate to an AI::create
// callsite on v1.0.68 in recent logs. We still forward the wide signature to
// preserve register/stack args, but this hook is no longer used as a proven
// Character* source for donor, faction, or OurFactory harvesting.
//
// What we historically called "AICreate" is actually CharBody::create at
// RVA 0x621460 (master_index entry 1113, BINDIFF_EXACT). Real signature:
//   CharBody::create(CharMovement*, AI*, AnimationClass*, Character*, CharStats*)
// + implicit `this` (CharBody*). Six total args.
//
// Bug history:
//  - Original: typed as 2-arg → Hook_AICreate forwarded only RCX/RDX, leaving
//    R8 (AI*), R9 (AnimationClass*), [rsp+0x28] (Character*), [rsp+0x30] (CharStats*)
//    as garbage from prior frame.
//  - CharBody::create writes [rsp+0x28] → this+0x318, [rsp+0x30] → this+0x10.
//  - AI scoring later dereferences this+0x318. Garbage = null = crash at
//    game+0x59820D. Manifests as "combat doesn't work, can't enter combat mode"
//    because every CharBody attached to a fresh character has a corrupt internal
//    Character backref.
//  - Fix (this change): typedef and Hook_AICreate now match the real 6-arg
//    signature. All six args forwarded verbatim.
using AICreateFn   = void*(__fastcall*)(
    void* charBody,        // RCX (this — CharBody*)
    void* charMovement,    // RDX
    void* ai,              // R8
    void* animationClass,  // R9
    void* character,       // [rsp+0x28]  → written to this+0x318
    void* charStats);      // [rsp+0x30]  → written to this+0x10
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

static void* __fastcall Hook_AICreate(void* charBody, void* charMovement,
                                      void* ai, void* animationClass,
                                      void* character, void* charStats) {
    s_createCount++;

    // Forward ALL SIX args verbatim. Previously this was 2-arg → R8/R9/stack
    // args were garbage → CharBody members at +0x318 and +0x10 corrupted →
    // combat scoring null-deref'd at game+0x59820D.
    void* result = nullptr;
    __try {
        result = s_origAICreate(charBody, charMovement, ai, animationClass,
                                character, charStats);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("ai_hooks: CharBody::create crashed (charBody=0x{:X} char=0x{:X})",
                      (uintptr_t)charBody, (uintptr_t)character);
        return nullptr;
    }

    if (s_createCount % 1000 == 1) {
        spdlog::debug("ai_hooks: AICreate #{} forwarded; donor/factory harvesting "
                      "is disabled for this unverified target",
                      s_createCount);
    }

#if 0

    // ── Publish faction for the spawn pipeline ──
    // Character.owner (Faction*) is at offset +0x10 (KServerMod-verified, see
    // CharacterOffsets::faction). CharBody::create fires for every character
    // including the player at savegame load.
    //
    // CAUTION: during the early phase of CharBody::create, Character.owner
    // can briefly point into the EXE's static mod-data region (e.g. 0x7FF6...
    // observed in the 12:57 test). createRandomCharacter SEHs when given
    // such a pointer. Publish setters now require IsHeapResidentPtr — they
    // silently drop EXE-section addresses, so we wait for the GameWorld
    // tick scanner (or a later AICreate fire) to surface a real heap Faction.
    if (character) {
        __try {
            uintptr_t* ownerSlot = reinterpret_cast<uintptr_t*>(
                reinterpret_cast<uintptr_t>(character) + 0x10);
            uintptr_t faction = *ownerSlot;
            kmp::entity_hooks::PublishFallbackFaction(faction);
            kmp::entity_hooks::PublishEarlyPlayerFaction(faction);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Character was unmapped or layout changed — drop silently.
        }

        // Donor pool: record the Character* for future clone-from-existing
        // spawning. Bounded ring buffer; thread-safe reader/writer.
        kmp::entity_hooks::RecordDonorCharacter(character);
    }

    // ── OurFactory: record (Faction*, GameData*) from this Character ──
    // The factory now constructs a CreatelistItem and calls process() —
    // the 2-arg internal dispatcher — which only requires a heap-resident
    // Faction* and a GameData*. Both are read out of the Character at
    // verified KenshiLib offsets (+0x10 and +0x40). No HeapSize, no memcpy,
    // no allocator mismatch risk.
    kmp::our_factory::RecordPlayerFactionFromCharacter(character);

    auto& core = Core::Get();
    if (!core.IsConnected()) return result;

    // Entity registry tracks Character*, not CharBody* — use the 5th arg.
    auto& registry = core.GetEntityRegistry();
    EntityID netId = registry.GetNetId(character);
    if (netId != INVALID_ENTITY) {
        auto info = registry.GetInfo(netId);
        if (info.has_value() && info->isRemote) {
            MarkRemoteControlled(character);
            spdlog::info("ai_hooks: CharBody::create for remote entity {} — body 0x{:X} "
                         "(decisions overridden by network), char=0x{:X}",
                         netId, (uintptr_t)charBody, (uintptr_t)character);
        }
    }

    if (s_createCount % 100 == 1) {
        spdlog::debug("ai_hooks: CharBody::create #{} (body=0x{:X} char=0x{:X} stats=0x{:X})",
                       s_createCount, (uintptr_t)charBody, (uintptr_t)character,
                       (uintptr_t)charStats);
    }

#endif
    return result;
}

static void __fastcall Hook_AIPackages(void* character, void* aiPackage) {
    s_packageCount++;
    // Watcher entry log skipped — uses __try below.

    // ALWAYS let AI packages load — the character needs valid behavior trees
    // to prevent crashes when the engine queries them. Even for remote characters,
    // the behavior tree structure must exist; we just override the actual decisions.
    __try {
        s_origAIPackages(character, aiPackage);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("ai_hooks: AIPackages crashed");
        return;
    }

    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    // Log for remote characters (diagnostic only — no suppression)
    auto& registry = core.GetEntityRegistry();
    EntityID netId = registry.GetNetId(character);
    if (netId != INVALID_ENTITY) {
        auto info = registry.GetInfo(netId);
        if (info.has_value() && info->isRemote) {
            spdlog::debug("ai_hooks: AI packages LOADED for remote entity {} "
                           "(behavior tree valid, decisions overridden)",
                           netId);
        }
    }

    if (s_packageCount % 200 == 1) {
        spdlog::debug("ai_hooks: AIPackages #{} (char=0x{:X}, pkg=0x{:X})",
                       s_packageCount, (uintptr_t)character, (uintptr_t)aiPackage);
    }
}

// ── Install / Uninstall ──

bool Install() {
    auto& funcs = Core::Get().GetGameFunctions();
    auto& hooks = HookManager::Get();
    int installed = 0;

    if (funcs.AICreate) {
        if (hooks.InstallAt("AICreate", reinterpret_cast<uintptr_t>(funcs.AICreate),
                            &Hook_AICreate, &s_origAICreate)) {
            installed++;
            spdlog::info("ai_hooks: AICreate hook installed");
        }
    }

    if (funcs.AIPackages) {
        if (hooks.InstallAt("AIPackages", reinterpret_cast<uintptr_t>(funcs.AIPackages),
                            &Hook_AIPackages, &s_origAIPackages)) {
            installed++;
            spdlog::info("ai_hooks: AIPackages hook installed");
        }
    }

    spdlog::info("ai_hooks: {}/2 hooks installed", installed);
    return installed > 0;
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
