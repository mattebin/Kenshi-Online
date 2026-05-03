#pragma once
//
// exit_safety — kill two classes of misleading "crash" reports.
//
// 1. GameWorld destructor crashes
//    Kenshi's normal exit path runs ~GameWorld which throws exceptions
//    on the way down (this is by design — the engine cleans up containers
//    that would AV under normal access). With our VEH or UEF active those
//    throws look like fatal crashes and end up in KenshiOnline_CRASH.log.
//    Hook ~GameWorld and detach our handlers BEFORE the destructor runs.
//
// 2. Unhandled-exception coverage gap
//    We have a VEH (vectored handler) for first-chance exceptions. It
//    chooses what to handle and what to let through. Anything VEH lets
//    through goes to whatever filter is registered with
//    SetUnhandledExceptionFilter — if nothing is registered, the process
//    is terminated by the OS. Adding a UEF here gives us last-chance
//    coverage for the gap VEH leaves.
//
// Both pieces of behaviour come from RE_Kenshi's `Bugs.cpp` design which
// has been stable across many Kenshi versions.

#include <Windows.h>

namespace kmp::exit_safety {

// Snapshot the OS's existing UEF (whatever Kenshi or Windows registered
// before us) and install our own. Call once at DLL init, before anything
// else might trigger an exception.
void Install();

// Hook GameWorld::_DESTRUCTOR so we can detach our exception handlers
// before the destructor's known-throwing teardown runs. Call AFTER the
// scanner has resolved the host module base — needs the live game base
// to compute the absolute address from the static RVA.
//
// On 1.0.68 binaries (where KenshiLib's documented RVA misses) this
// install will FAIL during InitHooks because the GameWorld singleton
// instance doesn't exist yet — the user hasn't loaded a save. Recovery
// path: TryInstallDestructorHookLater() polls from OnGameTick and installs
// once the instance becomes available.
void InstallGameWorldDestructorHook();

// Per-tick poke from OnGameTick. No-op once the dtor hook is installed.
// While not installed, attempts vtable resolution every N ticks; on
// success, installs and stops trying.
void TryInstallDestructorHookLater();

// Register the VEH handle owned by core.cpp so we can tear it down at the
// same point we restore the vanilla UEF. Without this, ~GameWorld's normal
// throws still hit our VEH and end up logged as "crashes" in
// KenshiOnline_CRASH.log even after the UEF is restored. Pass nullptr to
// clear (e.g. on shutdown ordering changes).
void RegisterVectoredHandler(PVOID handle);

// Test helper / for the hook body: restore the captured vanilla UEF AND
// remove our VEH if one was registered via RegisterVectoredHandler.
// Idempotent. Called both from the GameWorld destructor hook and during
// our own DLL detach. Both layers must come down — UEF alone doesn't stop
// VEH from logging first-chance exceptions during teardown.
void RestoreVanillaUnhandledFilter();

// Read-only — was Install() called and did it capture a vanilla filter?
bool IsInstalled();

} // namespace kmp::exit_safety
