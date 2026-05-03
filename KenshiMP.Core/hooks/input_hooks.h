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

} // namespace kmp::input_hooks
