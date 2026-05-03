#pragma once
//
// host_game_speed is intentionally disabled for Kenshi 1.0.68.
//
// Previous attempts depended on dead/unproven paths:
//   - TIME_UPDATE did not fire, so TimeManager was never captured.
//   - TimeManager +0x08/+0x10 were never proven live on this build.
//   - GameWorld +0x700 returned stale/garbage values.
//   - The broad time_discovery scan did not identify a reliable source.
//
// Until the HUD clock text path proves a live source, this module must not
// send C2S_HostGameSpeed or claim speed sync is working.

namespace kmp::host_game_speed {

void Tick(float deltaTime);
float LastReportedSpeed();

} // namespace kmp::host_game_speed
