#pragma once
#include <cstdint>
#include <Windows.h>

namespace kmp::entity_hooks {

bool Install();
void Uninstall();

// Enable the CharacterCreate hook for multiplayer.
// Call this when connecting to a server so new character creates are captured.
void ResumeForNetwork();

// Disable the CharacterCreate hook on disconnect.
// Prevents MovRaxRsp heap corruption from zone-load bursts while not connected.
void SuspendForDisconnect();

// Set/clear the direct spawn bypass flag.
// When true, Hook_CharacterCreate skips all spawn/registration logic
// and just passes through to the original function. Used by SpawnManager
// when calling the factory from GameFrameUpdate to avoid recursive spawn logic.
void SetDirectSpawnBypass(bool bypass);

// Check if an in-place replay spawn succeeded recently.
// Used by game_tick_hooks to avoid competing with in-place replay.
bool HasRecentInPlaceSpawn(int withinSeconds = 30);

// Get total number of successful in-place spawns
int GetInPlaceSpawnCount();

// Decrement the per-player spawn cap counter when a remote entity dies or despawns.
// Must be called from any code path that removes a remote entity (despawn handler,
// character destroy hook, heartbeat cleanup, etc.) to prevent the cap from saturating.
void DecrementSpawnCount(uint32_t owner);

// Diagnostic getters (for PipelineOrchestrator snapshot collection)
int  GetTotalCreates();
int  GetTotalDestroys();

// ── Loading detection via create events ──
// Returns milliseconds since the last CharacterCreate hook fired (any mode).
// Used by PollForGameLoad to detect when the loading burst has finished
// without needing CharacterIterator (which corrupts the heap during loading).
int64_t GetTimeSinceLastCreate();

// Returns how many characters were created during the current loading burst.
// Resets to 0 when ResetLoadingCreateCount() is called.
int GetLoadingCreateCount();

// Reset the loading create counter (called when transitioning to Loading phase).
void ResetLoadingCreateCount();

// Enable ultra-lightweight passthrough mode for loading.
// When true, Hook_CharacterCreate only updates timestamp/counter and calls original.
// No game memory reads, no faction voting, no entity registration.
void SetLoadingPassthrough(bool enabled);

// Get pointers to mod template characters captured during loading.
// Returns count of captured pointers (up to 16). Fills outPtrs array.
int GetCapturedModTemplates(void** outPtrs, int maxCount);

// Call the factory function DIRECTLY via the raw MinHook trampoline,
// completely bypassing the hook and MovRaxRsp wrapper.
// The raw trampoline starts with `mov rax, rsp` and sets up its own
// frame correctly — no stack swap, no heap corruption.
// Returns the created character or nullptr.
void* CallFactoryDirect(void* factory, void* requestStruct);

// Call RootObjectFactory::createRandomCharacter (RVA 0x5836E0) with the GameData*
// template as a constraint hint. Internally invokes the native 7-arg function:
//   (factory, faction, Vector3 pos, container=null, gameData, building=null, scale=1.0).
// Faction is resolved from the early-captured player faction (set during savegame load)
// or the last-seen fallback faction. Returns the created character or nullptr.
void* CallFactoryCreate(void* factory, void* gameData, float x, float y, float z);

// Same native call with gameData=nullptr → fully random NPC. Position is required
// because the native signature passes Vector3 by value.
void* CallFactoryCreateRandom(void* factory, float x, float y, float z);

// Get the fallback faction pointer (last valid faction seen from any character creation).
// Used by SEH_FixUpFaction_Core when primary character faction isn't available.
// Validates the pointer before returning; returns 0 if stale.
uintptr_t GetFallbackFaction();

// Get the player faction elected during savegame loading via multi-source voting.
// Scans the first N characters and picks the most common faction, with bonus
// weight for characters whose name matches the config playerName and for
// factions with the isPlayerFaction flag set. Returns 0 if not yet elected.
// Validates the pointer before returning; returns 0 if stale.
uintptr_t GetEarlyPlayerFaction();

// Externally publish a faction observation. Safe to call from any thread.
// Used by ai_hooks::Hook_AICreate after the CharBody::create 6-arg fix —
// the 5th arg is a Character*, and Character.owner @ +0x10 is the Faction.
// Without this path the spawn pipeline starves while waiting for a faction
// that the disabled CharacterCreate hook will never produce.
//
// Both setters silently drop addresses that fail IsHeapResidentPtr — Kenshi
// can leave Character.owner pointing into the EXE's static mod-data region
// during the early phase of CharBody::create; that pointer is technically
// non-null but createRandomCharacter SEHs when it tries to use it.
void PublishFallbackFaction(uintptr_t faction);
void PublishEarlyPlayerFaction(uintptr_t faction);

// True if `val` is in user-mode VA space and properly aligned. Looser than
// IsHeapResidentPtr — accepts pointers inside the game module's image range,
// because some Kenshi globals (e.g. Faction objects) live in the writable
// .data section. Use this when you need a validity check but the pointer
// might legitimately point at module-mapped writable memory.
inline bool IsValidVAPtr(uintptr_t val) {
    if (val < 0x10000 || val >= 0x00007FFFFFFFFFFFull) return false;
    if ((val & 0x7) != 0) return false;
    return true;
}

// True if `val` is in user-mode VA space, properly aligned, AND outside the
// game module's image range. The image-range check is what catches static
// mod-data masquerading as a heap pointer (e.g. 0x7FF6510CBC08 in the
// 12:57 test). Inlined header definition so every translation unit has it.
inline bool IsHeapResidentPtr(uintptr_t val) {
    if (val < 0x10000 || val >= 0x00007FFFFFFFFFFFull) return false;
    if ((val & 0x7) != 0) return false;
    HMODULE mod = ::GetModuleHandleW(nullptr);
    if (!mod) return true; // can't check; trust the alignment/range tests
    auto modBase = reinterpret_cast<uintptr_t>(mod);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(modBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return true;
    uintptr_t modEnd = modBase + nt->OptionalHeader.SizeOfImage;
    return !(val >= modBase && val < modEnd);
}

// ── Donor character pool ──
// Hook_AICreate captures every observed Character* into a small ring so the
// spawn pipeline can repurpose them for remote characters instead of calling
// Kenshi's broken createRandomCharacter.
// Thread-safe; no allocations on the hot path.
void RecordDonorCharacter(void* character);
// Returns one donor (round-robin) or nullptr if pool empty.
void* PickDonorCharacter();
// Returns one UNUSED donor (skip-already-claimed semantics). Caller is
// expected to immediately ClaimDonorCharacter() on the result. Returns
// nullptr if every donor is already claimed.
void* PickFreshDonorCharacter();
// Mark a donor as claimed — once claimed it will not be picked again.
// The repurposed character is now logically "owned" by the spawn pipeline.
void  ClaimDonorCharacter(void* character);
bool  IsDonorClaimed(void* character);
// Returns the count of donors currently in the pool (including claimed ones).
int   DonorPoolSize();
// Returns the count of donors not yet claimed.
int   DonorPoolFree();

// ── Tick-driven GameWorld faction harvester ──
// Walks GameWorld+0x0888 character list, picks the first Character whose
// +0x10 owner passes IsHeapResidentPtr, publishes it as the spawn pipeline's
// faction. Cheap; safe to call every tick — bails out fast once a faction
// has already been published.
void HarvestFactionFromGameWorld();

// Get the detected GameData pointer offset within the factory request struct.
// Returns -1 if not yet detected.
int GetGameDataOffsetInStruct();

// Get the detected position offset within the factory request struct.
// Returns -1 if not yet detected.
int GetPositionOffsetInStruct();

// Process any pending hook-state changes that were queued from inside a hook
// callback. Calling HookManager::Disable() while we are still on the hook's
// own call stack puts the toggle on the same call frame as the active wrapper
// invocation, which is fragile under racy thread scheduling. This poll runs
// from a safe context (game tick / main loop) so the toggle happens cleanly.
void PollDeferredHookState();

} // namespace kmp::entity_hooks
