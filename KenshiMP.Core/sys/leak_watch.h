#pragma once
//
// leak_watch — detect memory leaks and unbounded-collection growth in long
// sessions. The kind of bug you can't reproduce in a 5-minute QA test but
// definitely WILL hit in a 4-hour 16-player session.
//
// Two collection points:
//
//   1. Process memory: working set + private bytes, sampled periodically.
//      Logged with a delta-from-baseline so monotonic growth across the
//      session shows up at a glance.
//
//   2. Registered collection sizes: any unordered_map / vector / queue that
//      grows over a session can call `RegisterSize("name", []{ return map.size(); })`
//      once. Every snapshot emits its current size. A monotonic increase
//      across snapshots = leaking that collection.
//
// Snapshots fire from `Tick()` — call once per OnGameTick. Internally
// rate-limited to one snapshot per 5 minutes (configurable). Cheap when
// not snapshotting (just an atomic load + timestamp compare).
//
// On disconnect / shutdown, `EmitSummary()` writes a final block with all
// collection sizes and total memory delta — that's the "did this session
// leak" answer in one place.

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>

namespace kmp::leak_watch {

// Register a collection-size getter. Called once per registration site
// (typically near the collection's definition with a lambda capture).
// `name` is the human label that goes into logs.
void RegisterSize(const std::string& name, std::function<size_t()> sizeGetter);

// Per-tick poke. Cheap unless the snapshot interval has elapsed.
// Default snapshot interval = 5 minutes.
void Tick();

// Force a snapshot now (ignores interval). Useful from /audit slash command
// or on disconnect.
void SnapshotNow();

// Final summary. Logs every registered collection's current size, every
// recorded snapshot's process memory, and the delta from baseline.
void EmitSummary();

// Configurable: change snapshot interval. 0 = disabled.
void SetIntervalSeconds(int seconds);

} // namespace kmp::leak_watch
