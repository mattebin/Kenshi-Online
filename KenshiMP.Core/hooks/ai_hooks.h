#pragma once
#include <cstdint>

namespace kmp::ai_hooks {

bool Install();
void Uninstall();

// Hooks are installed-but-DISABLED at startup, mirroring the entity_hooks
// CharacterCreate pattern. They corrupt character state if they fire during
// SP / pre-connect, so they only become active under controlled timing.
void ResumeForNetwork();      // Enable both hooks (call after connect)
void SuspendForDisconnect();  // Disable both hooks (call on disconnect)

// Remote-controlled character tracking.
// Characters marked as remote get their AI decisions overridden (not suppressed).
void MarkRemoteControlled(void* character);
void UnmarkRemoteControlled(void* character);
bool IsRemoteControlled(void* character);

} // namespace kmp::ai_hooks
