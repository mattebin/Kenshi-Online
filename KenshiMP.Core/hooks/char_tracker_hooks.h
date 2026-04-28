#pragma once
#include "kmp/types.h"
#include <string>
#include <functional>

namespace kmp::char_tracker_hooks {

bool Install();
void Uninstall();

struct TrackedChar {
    void* animClassPtr;     // AnimationClassHuman*
    void* characterPtr;     // CharacterHuman* (at animClass+0x2D8)
    std::string name;
    Vec3 position;
    uint64_t lastSeenTick;
};

const TrackedChar* FindByName(const std::string& name);
const TrackedChar* FindByPtr(void* characterPtr);
void* GetLocalPlayerAnimClass();
void* GetRemotePlayerAnimClass(const std::string& name);
void SetOnNewCharacter(std::function<void(const TrackedChar&)> callback);
int GetTrackedCount();
void DumpTrackedChars();

// Process deferred character discoveries from safe game-tick context.
// Called from Core::OnGameTick — NOT from inside the inline hook.
void ProcessDeferredDiscovery();

// Diagnostic: dump per-stage hook call counters to the log so we can tell
// whether the inline hook is firing at all and (if so) where calls are
// being filtered out. Cheap atomics — safe to call every few seconds.
void DumpHookCounters();

} // namespace kmp::char_tracker_hooks
