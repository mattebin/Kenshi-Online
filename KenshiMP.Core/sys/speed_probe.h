#pragma once

// Read-only probe for the RE_Kenshi-derived GameWorld layout.
//
// Goal: determine whether the GameWorld pointer we resolved in core.cpp,
// dereferenced according to KenshiLib's documented layout, exposes a
// believable frameSpeedMult (+0x700) and paused (+0x8B9) on Kenshi
// 1.0.68 Steam Newland. NOTHING is written to game memory. Safe to leave
// in the build — gated behind the env var KMP_SPEED_PROBE=1 so it only
// runs when explicitly opted in.
//
// See docs/SPEED_SYNC_LEAD.md for the full layout reference.

namespace kmp::speed_probe {

// Run the probe once. Idempotent: subsequent calls are a no-op so it's
// safe to drive from OnGameTick. Returns immediately if KMP_SPEED_PROBE
// is not set in the environment, or if no GameWorld pointer has been
// resolved yet.
void TickOnce();

} // namespace kmp::speed_probe
