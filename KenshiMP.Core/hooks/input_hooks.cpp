#include "input_hooks.h"
#include "../core.h"
#include "kmp/hook_manager.h"
#include <spdlog/spdlog.h>
#include <atomic>
#include <cstdint>
#include <Windows.h>

namespace kmp::input_hooks {

// Forward decl — defined below.
static bool IsModalUiActive();

// WndProc handles our UI text input and visible keybinds, but Kenshi also
// consumes keyboard events through OIS. Hook Kenshi's InputHandler so modal
// multiplayer UI can stop those events from reaching game actions like
// camera movement and hotkeys — fixes the double-input bug where typing
// in chat also drove gameplay.
//
// Borrowed from andperks6/Kenshi-Online (commit 2d1a04c). Their version uses
// hardcoded RVAs for the InputHandler key event functions:
//   keyDownEvent  RVA 0x00360680
//   keyUpEvent    RVA 0x003608F0
// These are valid for Kenshi 1.0.68 Steam ("Newland"). On future builds the
// RVAs may shift; the install logs a warning and degrades to WndProc-only
// when the hooks fail to install. TODO: migrate to a string-xref pattern
// for build-independent resolution.

// Keybinds (handled in render_hooks WndProc):
// Tab        - Toggle player list
// Enter      - Toggle chat
// F1         - Toggle connection UI
// Escape     - Close any open overlay panel
// Tilde (~)  - Toggle debug overlay

static bool s_installed = false;

using InputKeyEventFn = void(__fastcall*)(void* inputHandler, int key);

static InputKeyEventFn s_origKeyDown = nullptr;
static InputKeyEventFn s_origKeyUp   = nullptr;

// Kenshi 1.0.68 (Steam, "Newland") RVAs for InputHandler key event handlers.
static constexpr uintptr_t RVA_INPUT_KEY_DOWN = 0x00360680;
static constexpr uintptr_t RVA_INPUT_KEY_UP   = 0x003608F0;
static constexpr int OIS_KC_F1 = 0x3B;

static std::atomic<uint32_t> s_swallowedKeyDown{0};
static std::atomic<uint32_t> s_swallowedKeyUp{0};

static bool IsModalUiActive() {
    auto& core = Core::Get();
    if (core.GetNativeHud().IsChatInputActive()) return true;
    if (core.GetOverlay().GetNativeMenu().IsVisible()) return true;
    return false;
}

static bool ShouldCaptureOisKey(int key) {
    // F1 is our menu-toggle; Kenshi otherwise binds it. Always swallow.
    if (key == OIS_KC_F1) return true;
    return IsModalUiActive();
}

static void __fastcall Hook_InputKeyDown(void* inputHandler, int key) {
    if (ShouldCaptureOisKey(key)) {
        s_swallowedKeyDown.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    __try {
        s_origKeyDown(inputHandler, key);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("input_hooks: InputHandler::keyDownEvent trampoline crashed");
    }
}

static void __fastcall Hook_InputKeyUp(void* inputHandler, int key) {
    if (ShouldCaptureOisKey(key)) {
        s_swallowedKeyUp.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    __try {
        s_origKeyUp(inputHandler, key);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("input_hooks: InputHandler::keyUpEvent trampoline crashed");
    }
}

// Walk to the .pdata-reported real function start when the candidate RVA
// lands mid-function. .pdata is authoritative — same table Windows uses
// for SEH unwinding. Same recovery pattern used in entity_hooks.cpp for
// the factory functions; surfaced by the 2026-05-04 two-player session
// where Kenny's slightly-different 1.0.68 exe had the OIS keyUpEvent
// RVA 0x3608F0 mid-instruction.
static uintptr_t RecoverFunctionStart(uintptr_t addr, const char* name) {
    DWORD64 imageBase = 0;
    auto* rtFunc = RtlLookupFunctionEntry(
        static_cast<DWORD64>(addr), &imageBase, nullptr);
    if (!rtFunc) return addr;
    uintptr_t funcStart = static_cast<uintptr_t>(imageBase) + rtFunc->BeginAddress;
    if (funcStart == addr) return addr;
    spdlog::warn("input_hooks: {} at 0x{:X} is MID-FUNCTION "
                 "(real start 0x{:X}, offset +0x{:X}) — RECOVERING to real start",
                 name, addr, funcStart, addr - funcStart);
    return funcStart;
}

bool Install() {
    auto& scanner = Core::Get().GetScanner();
    const uintptr_t base = scanner.GetBase();
    if (!base) {
        spdlog::warn("input_hooks: scanner base unavailable, OIS input gate not installed");
        return false;
    }

    auto& hookMgr = HookManager::Get();
    const uintptr_t keyDownTarget = RecoverFunctionStart(
        base + RVA_INPUT_KEY_DOWN, "InputKeyDown");
    const uintptr_t keyUpTarget = RecoverFunctionStart(
        base + RVA_INPUT_KEY_UP, "InputKeyUp");

    const bool keyDownOk = hookMgr.InstallAt(
        "InputKeyDown", keyDownTarget,
        &Hook_InputKeyDown, &s_origKeyDown);
    const bool keyUpOk = hookMgr.InstallAt(
        "InputKeyUp", keyUpTarget,
        &Hook_InputKeyUp, &s_origKeyUp);

    s_installed = true;
    spdlog::info("input_hooks: Installed (WndProc + OIS gate, keyDown={}, keyUp={})",
                 keyDownOk, keyUpOk);
    return keyDownOk && keyUpOk;
}

// ── MyGUI input swallowing ─────────────────────────────────────────────────
//
// In addition to OIS-level swallowing above, hook MyGUI's InputManager so
// keystrokes delivered to MyGUI widgets (e.g. squad UI, character pickers,
// inventory list filters) don't fire while our chat or native menu is the
// active modal. Hooks are installed lazily from render_hooks once
// MyGUIEngine_x64.dll has finished loading.
//
// MyGUI exports are version-stable mangled names — these have been the same
// across MyGUI 3.x. If a future MyGUI bumps the export naming the install
// will simply fail and we degrade to OIS-only swallowing.

using InjectKeyPressFn   = bool (*)(void*, std::uint32_t, std::uint32_t);
using InjectKeyReleaseFn = bool (*)(void*, std::uint32_t);

static InjectKeyPressFn   s_origInjectKeyPress   = nullptr;
static InjectKeyReleaseFn s_origInjectKeyRelease = nullptr;
static bool s_myguiInstalled = false;

static std::atomic<uint32_t> s_swallowedMyGuiPress{0};
static std::atomic<uint32_t> s_swallowedMyGuiRelease{0};

// Walk simple jump trampolines so hot-patched MyGUI exports still hook the
// real implementation. Pattern matches what x64dbg/IDA see for E9 / EB / FF25.
static void* ResolveCodeTarget(void* p) {
    auto* cur = static_cast<std::uint8_t*>(p);
    if (!cur) return nullptr;
    for (int i = 0; i < 8; ++i) {
        if (cur[0] == 0xE9) {
            std::int32_t r = *reinterpret_cast<std::int32_t*>(cur + 1);
            cur += 5 + r;
            continue;
        }
        if (cur[0] == 0xEB) {
            std::int8_t r = *reinterpret_cast<std::int8_t*>(cur + 1);
            cur += 2 + r;
            continue;
        }
        if (cur[0] == 0xFF && cur[1] == 0x25) {
            std::int32_t d = *reinterpret_cast<std::int32_t*>(cur + 2);
            cur = *reinterpret_cast<std::uint8_t**>(cur + 6 + d);
            continue;
        }
        break;
    }
    return cur;
}

static bool Hook_MyGui_InjectKeyPress(void* inputMgr, std::uint32_t keyCode,
                                       std::uint32_t text) {
    if (IsModalUiActive()) {
        s_swallowedMyGuiPress.fetch_add(1, std::memory_order_relaxed);
        return true; // tell MyGUI we handled it; no widget receives the press
    }
    return s_origInjectKeyPress
        ? s_origInjectKeyPress(inputMgr, keyCode, text)
        : false;
}

static bool Hook_MyGui_InjectKeyRelease(void* inputMgr, std::uint32_t keyCode) {
    if (IsModalUiActive()) {
        s_swallowedMyGuiRelease.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return s_origInjectKeyRelease
        ? s_origInjectKeyRelease(inputMgr, keyCode)
        : false;
}

bool InstallMyGuiSwallow() {
    if (s_myguiInstalled) return true;

    HMODULE mygui = GetModuleHandleA("MyGUIEngine_x64.dll");
    if (!mygui) {
        // Caller is expected to retry once MyGuiBridge::IsReady() is true.
        return false;
    }

    auto pPress = GetProcAddress(
        mygui,
        "?injectKeyPress@InputManager@MyGUI@@QEAA_NUKeyCode@2@I@Z");
    auto pRelease = GetProcAddress(
        mygui,
        "?injectKeyRelease@InputManager@MyGUI@@QEAA_NUKeyCode@2@@Z");
    if (!pPress || !pRelease) {
        spdlog::warn(
            "input_hooks: MyGUI injectKeyPress/Release exports not found — "
            "MyGUI swallow not installed (degrading to OIS-only)");
        return false;
    }

    auto rPress   = ResolveCodeTarget(reinterpret_cast<void*>(pPress));
    auto rRelease = ResolveCodeTarget(reinterpret_cast<void*>(pRelease));

    auto& hookMgr = HookManager::Get();
    bool pressOk = hookMgr.InstallAt(
        "MyGUI_InjectKeyPress",
        reinterpret_cast<uintptr_t>(rPress),
        &Hook_MyGui_InjectKeyPress, &s_origInjectKeyPress);
    bool releaseOk = hookMgr.InstallAt(
        "MyGUI_InjectKeyRelease",
        reinterpret_cast<uintptr_t>(rRelease),
        &Hook_MyGui_InjectKeyRelease, &s_origInjectKeyRelease);

    if (!pressOk || !releaseOk) {
        spdlog::warn(
            "input_hooks: MyGUI hook install partial (press={}, release={})",
            pressOk, releaseOk);
        return false;
    }

    s_myguiInstalled = true;
    spdlog::info("input_hooks: MyGUI keyboard hooks installed");
    return true;
}

void Uninstall() {
    auto& hookMgr = HookManager::Get();
    if (s_installed) {
        hookMgr.Remove("InputKeyDown");
        hookMgr.Remove("InputKeyUp");
        spdlog::info("input_hooks: Uninstalled OIS gate "
                     "(swallowedDown={}, swallowedUp={})",
                     s_swallowedKeyDown.load(std::memory_order_relaxed),
                     s_swallowedKeyUp.load(std::memory_order_relaxed));
    }
    if (s_myguiInstalled) {
        hookMgr.Remove("MyGUI_InjectKeyPress");
        hookMgr.Remove("MyGUI_InjectKeyRelease");
        s_origInjectKeyPress   = nullptr;
        s_origInjectKeyRelease = nullptr;
        s_myguiInstalled = false;
        spdlog::info("input_hooks: Uninstalled MyGUI swallow "
                     "(swallowedPress={}, swallowedRelease={})",
                     s_swallowedMyGuiPress.load(std::memory_order_relaxed),
                     s_swallowedMyGuiRelease.load(std::memory_order_relaxed));
    }
    s_installed = false;
}

} // namespace kmp::input_hooks
