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
    void* characterPtr;     // CharacterHuman* — discovered offset, see PROBING log
    std::string name;
    uintptr_t factionPtr;   // CharacterHuman.faction (char+0x10). Used as the
                             // canonical identity marker for cross-character
                             // matching. Names collide (the kenshi-online.mod
                             // emits multiple characters all called "Player 1");
                             // the faction pointer does not.
    Vec3 position;
    uint64_t lastSeenTick;
};

const TrackedChar* FindByName(const std::string& name);
const TrackedChar* FindByPtr(void* characterPtr);

// Pointer-based identity lookup. Returns the first tracked character whose
// CharacterHuman.faction equals `factionPtr`. Pass 0 to get a nullptr.
const TrackedChar* FindByFactionPtr(uintptr_t factionPtr);

// All currently-tracked characters whose CharacterHuman.faction matches.
// The vector is a snapshot — the underlying tracker may keep mutating.
std::vector<TrackedChar> FindAllByFactionPtr(uintptr_t factionPtr);

// Find the first tracked character whose faction matches AND whose name is
// NOT equal to the supplied placeholder. Used to pull the player's PC out
// of a faction that contains many NPCs with a placeholder name — the
// kenshi-online.mod emits 18+ characters all literally named "Player 1"
// in the local faction, but the *player's* PC has a unique name they
// chose at character creation. Pointer-by-faction match plus
// name-disambiguation gives us "the unique one" without depending on the
// PC's actual name (which we don't know in advance).
const TrackedChar* FindUniqueByFactionPtr(uintptr_t factionPtr,
                                           const std::string& placeholder);

// Resolve the faction pointer for the FIRST tracked character whose name
// matches exactly. This is the bootstrap step to convert a server-provided
// faction string ("12-kenshi-online.mod") into an in-process faction pointer:
// once any character with the right name has animated, its faction pointer
// becomes the authoritative identity marker for everyone in that faction.
// Returns 0 if no tracked character carries that name yet.
uintptr_t ResolveFactionPtrByName(const std::string& name);

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
