#pragma once

#include <cstdint>

namespace kmp::game_tick_hooks {

bool Install();
void Uninstall();

// ── DIAGNOSTIC ACCESSORS (added for v1.0.68 time/clock investigation) ────
//
// Read-only counters / observed values. Safe to call from any context (no
// spdlog, no allocation — just atomic loads). Used by Core::OnGameTick to
// emit rate snapshots from a non-detour path.
//
// All zeros / null until the hook actually fires for the first time. If
// these stay zero across an entire session, GAME_FRAME_UPDATE is NOT
// the live game-tick path on this Kenshi build.

uint64_t  GetTotalCallCount();
uintptr_t GetLastObservedRcx();   // 0 if no call has fired yet
bool      IsRcxStableAcrossCalls(); // false if rcx changed at any point
uintptr_t GetTargetAddress();      // hook install address, 0 if not installed
bool      IsInstalled();

// One-shot snapshot used by the rate logger. Atomically reads the current
// total count, returning the delta from the last snapshot AND the wallclock
// timestamp of THIS read so the caller can compute calls/sec. Safe to call
// from OnGameTick.
struct RateSnapshot {
    uint64_t totalCalls;
    uint64_t deltaCalls;     // since the previous snapshot
    double   deltaSeconds;   // wallclock seconds since the previous snapshot
    uintptr_t lastRcx;
    bool      rcxStable;
};
RateSnapshot TakeRateSnapshot();

} // namespace kmp::game_tick_hooks
