#pragma once
#include "kmp/types.h"
#include <string>
#include <functional>
#include <vector>

namespace kmp::char_tracker_hooks {

bool Install();
void Uninstall();

struct TrackedChar {
    void* animClassPtr;     // AnimationClassHuman*
    void* characterPtr;     // CharacterHuman* — auto-discovered offset
    std::string name;
    uintptr_t factionPtr;   // CharacterHuman.faction (char+0x10). Used as
                             // the canonical identity marker for cross-character
                             // matching. Names collide (kenshi-online.mod emits
                             // many "Player 1" characters); the faction pointer
                             // does not.
    Vec3 position;
    uint64_t lastSeenTick;
};

const TrackedChar* FindByName(const std::string& name);
const TrackedChar* FindByPtr(void* characterPtr);

// Pointer-based identity lookups. factionPtr=0 returns nullptr / empty.
const TrackedChar* FindByFactionPtr(uintptr_t factionPtr);
std::vector<TrackedChar> FindAllByFactionPtr(uintptr_t factionPtr);

// First tracked character whose faction matches AND whose name is NOT the
// supplied placeholder. Used to pull the player's PC out of a faction that
// contains many NPCs sharing a placeholder name.
const TrackedChar* FindUniqueByFactionPtr(uintptr_t factionPtr,
                                           const std::string& placeholder);

// Resolve faction pointer for the FIRST tracked character whose name
// matches exactly. Bootstrap step to convert a server-provided faction
// string into an in-process faction pointer.
uintptr_t ResolveFactionPtrByName(const std::string& name);

void* GetLocalPlayerAnimClass();
void* GetRemotePlayerAnimClass(const std::string& name);
void SetOnNewCharacter(std::function<void(const TrackedChar&)> callback);
int GetTrackedCount();
void DumpTrackedChars();

// Process deferred character discoveries from safe game-tick context.
// Called from Core::OnGameTick — NOT from inside the inline hook.
void ProcessDeferredDiscovery();

} // namespace kmp::char_tracker_hooks
