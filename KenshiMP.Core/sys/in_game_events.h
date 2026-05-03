#pragma once
//
// in_game_events — replace fragile timing heuristics with direct hooks
// on Kenshi's own state-transition functions.
//
// What we used to do (still do, fallback):
//   render_hooks watches Present-frame timing. If gap > 2s → "loading
//   started." If 8s of smooth frames after a gap → "probably done loading."
//   Then PollForGameLoad checks CharacterIterator to confirm.
//
// What this module does:
//   Hook two specific Kenshi functions that Kenshi calls AT the actual
//   state transitions:
//     LoadingWindow::hide()        — RVA 0x911C10 — fires when the loading
//                                    screen is dismissed (world is loaded
//                                    and visible).
//     MainBarGUI::_CONSTRUCTOR()   — RVA 0x72C1E0 — fires when the in-game
//                                    HUD is being constructed (UI is
//                                    available for hooking).
//
// The two events fire in order: Loading hides → MainBarGUI is constructed.
// Together they replace ~80 lines of timing heuristics in render_hooks.

#include <atomic>

namespace kmp::in_game_events {

// Install both hooks. Safe to call any time after Core::GetScanner().GetBase()
// is valid (i.e. after InitScanner). Logs a warning and degrades to "fallback
// to render_hooks heuristics" if either hook fails to install.
bool Install();

// True after LoadingWindow::hide() has fired at least once this session.
// Set by the hook body. Cleared on disconnect/teardown if needed.
bool LoadingWindowHidden();

// True after MainBarGUI::_CONSTRUCTOR has fired at least once this session.
// This is the strongest "in-game UI is real" signal — both world AND HUD
// are confirmed to exist.
bool MainBarReady();

// Number of times each event fired this session (diagnostic).
unsigned LoadingWindowHideCount();
unsigned MainBarConstructCount();

} // namespace kmp::in_game_events
