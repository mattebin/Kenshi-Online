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
bool InstallKenshiHotkey();

} // namespace kmp::input_hooks
