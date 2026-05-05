#pragma once

namespace kmp::input_hooks {

bool Install();
void Uninstall();

// Defense-in-depth: hook MyGUI's InputManager so that key events delivered
// through the MyGUI input layer are also swallowed while our chat or menu
// is open. Idempotent — safe to call repeatedly. Must only be called once
// MyGUI has finished loading (MyGUIEngine_x64.dll mapped + exports
// resolvable). Returns true if the hooks are installed (or were already
// installed). Driven from render_hooks::HookPresent once the bridge
// reports IsReady().
bool InstallMyGuiSwallow();

// Hook Kenshi's per-frame hotkey dispatcher (RVA 0x82B370 — same on
// 1.0.51 and 1.0.68, confirmed by Recon6). When chat or our native
// menu is modal, the hook returns early and skips the entire vanilla
// hotkey poll — F1 help menu, M map, etc. don't fire under our UI.
// Idempotent. Safe to call repeatedly. Call once during Install().
//
// NOTE 2026-05-04: This RVA was disproved (Recon7+8 — 0x82B370 is engine
// init, NOT the hotkey dispatcher). Left in source for reference; not
// called from Install(). Replaced by InstallKenshiKeyPressed() below.
bool InstallKenshiHotkey();

// Hook Kenshi's _keyPressed function at RVA 0x360B30. Discovered via
// Recon11-13 (2026-05-04). This is the function that fires hotkey
// bindings: when a scancode comes in, it walks Kenshi's two binding
// trees (this+0x30 and this+0x58) and invokes the bound action.
//
// Signature is `void __fastcall(this, uint scanCode)`. We hook it,
// drop the call when chat or our native menu is modal — no Kenshi
// hotkey fires while the user is typing. Idempotent; safe to call
// repeatedly. Call once during Install().
bool InstallKenshiKeyPressed();

// Hook Kenshi's upstream OIS keyDown listener at RVA 0x82B010
// (FUN_14082B010, 630 bytes). Recon17 (2026-05-04) showed this is
// the function that:
//   1. Receives the OIS KeyEvent
//   2. Calls MyGUI::InputManager::injectKeyPress
//   3. Checks if MyGUI's key-focus widget isOfType(EditBox) — if yes,
//      skips hotkey dispatch (Kenshi's vanilla "no hotkey while typing")
//   4. Calls thunk_FUN_140360b30 (_keyPressed) to fire hotkey bindings
//
// We hook this whole function and return early when modal UI is
// active — the entire chain (MyGUI inject + EditBox check + _keyPressed)
// is bypassed. Equivalent in effect to setting Kenshi's own +0xD0 gate
// flag but doesn't require finding the `this` pointer.
//
// Signature is `uint64_t __fastcall(longlong p1, longlong keyEvent)` —
// returns 1 to indicate "event consumed" so OIS doesn't propagate.
// Idempotent. Call once during Install().
bool InstallKenshiOisKeyDown();

} // namespace kmp::input_hooks
