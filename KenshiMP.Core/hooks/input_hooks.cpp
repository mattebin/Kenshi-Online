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
static bool InstallGetKeyboardStateIatHook();
static void UninstallGetKeyboardStateIatHook();

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

    // Layer 4: IAT-hook USER32!GetKeyboardState. Kenshi's hotkey dispatcher
    // (location unknown post-Recon7+8) calls this Win32 API to read keyboard
    // state. By masking specific VK slots in the returned buffer when our
    // modal UI is active, the engine's hotkey poll sees those keys as not-
    // pressed and skips the corresponding actions. Layer 3 (Kenshi-function
    // hook) is left disabled — wrong target on 1.0.68.
    bool iatOk = InstallGetKeyboardStateIatHook();

    s_installed = true;
    spdlog::info("input_hooks: Installed (WndProc + OIS gate keyDown={} "
                 "keyUp={}, IAT-GetKeyboardState={})",
                 keyDownOk, keyUpOk, iatOk);
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

// ── IAT hook on USER32!GetKeyboardState ────────────────────────────────────
//
// Layer 4 of the input gate (joins WndProc + OIS + MyGUI). Different from
// the others: we hook a Windows API import in kenshi_x64.exe's IAT rather
// than a Kenshi function. When the engine's per-frame hotkey poll calls
// GetKeyboardState, our hook fills the buffer normally, then masks
// specific VK slots if our chat/menu is modal — Kenshi sees those keys
// as not-pressed and skips the corresponding hotkey. Outside modal UI
// the hook is a passthrough.
//
// Why this and not a Kenshi-function hook: Recon7+8 ruled out the two
// candidate dispatcher RVAs (0x82B370 was engine init, 0x22B370 was
// physics joints). The actual Kenshi hotkey poll is somewhere we can't
// easily classify by static analysis. Hooking the Win32 API itself
// sidesteps the search entirely — works on any Kenshi version, requires
// no signature scan.

using GetKeyboardStateFn = BOOL(WINAPI*)(PBYTE);
static GetKeyboardStateFn s_origGetKeyboardState = nullptr;
static void**             s_iatSlot              = nullptr;
static std::atomic<int>   s_iatHookCalls{0};
static std::atomic<int>   s_iatHookMasked{0};

static BOOL WINAPI Hook_GetKeyboardState(PBYTE lpKeyState) {
    s_iatHookCalls.fetch_add(1, std::memory_order_relaxed);
    BOOL r = s_origGetKeyboardState ? s_origGetKeyboardState(lpKeyState) : 0;
    if (!r || !lpKeyState) return r;
    if (!IsModalUiActive()) return r;

    s_iatHookMasked.fetch_add(1, std::memory_order_relaxed);
    // Targeted whitelist of VKs to clear while modal. Keep BACK / RETURN /
    // ESC / arrows so chat editing works; mask everything else that could
    // fire a game hotkey. WM_CHAR for typed text comes through WndProc on
    // a separate path and is unaffected by this mask.
    auto clear = [&](int vk) { lpKeyState[vk] = 0; };
    // Letter keys (A..Z = 0x41..0x5A) — covers M=map, Y=craft, I=inv, etc.
    for (int vk = 0x41; vk <= 0x5A; ++vk) clear(vk);
    // Top-row digits (0..9 = 0x30..0x39) — Kenshi's 1×/2×/3× speed keys.
    for (int vk = 0x30; vk <= 0x39; ++vk) clear(vk);
    // Function keys (F1..F12 = 0x70..0x7B) — vanilla help menu, etc.
    for (int vk = 0x70; vk <= 0x7B; ++vk) clear(vk);
    // Space (pause), Tab (auto-pilot), Insert/Delete/Home/End/PgUp/PgDn —
    // all common game hotkeys.
    clear(VK_SPACE);
    clear(VK_TAB);
    clear(VK_INSERT);
    clear(VK_DELETE);
    clear(VK_HOME);
    clear(VK_END);
    clear(VK_PRIOR);  // PgUp
    clear(VK_NEXT);   // PgDn
    return r;
}

static bool InstallGetKeyboardStateIatHook() {
    if (s_iatSlot) return true; // idempotent

    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe) {
        spdlog::warn("input_hooks: IAT hook: GetModuleHandle failed");
        return false;
    }
    auto base = reinterpret_cast<uintptr_t>(exe);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    auto& importDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.Size == 0) return false;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + importDir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* dllName = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(dllName, "USER32.dll") != 0
            && _stricmp(dllName, "USER32") != 0) {
            continue;
        }

        // Walk the thunks. OriginalFirstThunk is the name table (read-only),
        // FirstThunk is the IAT (writable, what we patch).
        auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + desc->OriginalFirstThunk);
        auto* iatThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + desc->FirstThunk);

        for (; nameThunk->u1.AddressOfData; ++nameThunk, ++iatThunk) {
            if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal)) continue;
            auto* impName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + nameThunk->u1.AddressOfData);
            if (strcmp(impName->Name, "GetKeyboardState") != 0) continue;

            // Found it. Patch the IAT slot.
            void** slot =
                reinterpret_cast<void**>(&iatThunk->u1.Function);
            DWORD oldProt = 0;
            if (!VirtualProtect(slot, sizeof(void*),
                                PAGE_READWRITE, &oldProt)) {
                spdlog::error("input_hooks: IAT VirtualProtect RW failed");
                return false;
            }
            s_origGetKeyboardState =
                reinterpret_cast<GetKeyboardStateFn>(*slot);
            *slot = reinterpret_cast<void*>(&Hook_GetKeyboardState);
            VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
            s_iatSlot = slot;
            spdlog::info("input_hooks: GetKeyboardState IAT hook INSTALLED "
                         "(orig=0x{:X}, slot=0x{:X})",
                         reinterpret_cast<uintptr_t>(s_origGetKeyboardState),
                         reinterpret_cast<uintptr_t>(slot));
            return true;
        }
        break; // found USER32, no need to keep walking other DLLs
    }
    spdlog::warn("input_hooks: IAT hook: GetKeyboardState import not found");
    return false;
}

static void UninstallGetKeyboardStateIatHook() {
    if (!s_iatSlot || !s_origGetKeyboardState) return;
    DWORD oldProt = 0;
    if (VirtualProtect(s_iatSlot, sizeof(void*),
                       PAGE_READWRITE, &oldProt)) {
        *s_iatSlot = reinterpret_cast<void*>(s_origGetKeyboardState);
        VirtualProtect(s_iatSlot, sizeof(void*), oldProt, &oldProt);
    }
    spdlog::info("input_hooks: GetKeyboardState IAT hook removed "
                 "(calls={}, masked={})",
                 s_iatHookCalls.load(std::memory_order_relaxed),
                 s_iatHookMasked.load(std::memory_order_relaxed));
    s_iatSlot = nullptr;
    s_origGetKeyboardState = nullptr;
}

// ── Kenshi hotkey dispatcher hook ──────────────────────────────────────────
//
// Kenshi reads keyboard state via three paths:
//   1. WndProc            — gated by render_hooks's modal logic
//   2. MyGUI inputManager — gated by InstallMyGuiSwallow above
//   3. Per-frame hotkey poll inside FUN_14082B370 — this hook
//
// Path 3 is what triggers the vanilla F1 help menu and similar global
// shortcuts. It pre-dates input dispatch and reads keyboard state via
// GetKeyboardState directly. Skip the entire function call when our chat
// or native menu is modal so no Kenshi hotkey fires while the user types.
//
// 2026-05-04 Recon6 confirmed RVA 0x82B370 is the same function on 1.0.68
// as on 1.0.51 — same prologue (40 57 48 83 EC 60 = push rdi; sub rsp,0x60),
// same body size class. RE_Kenshi's reference value was right; we just
// hadn't tried it yet.

using KenshiHotkeyFn = void (*)(void* self);
static constexpr uintptr_t RVA_KENSHI_HOTKEY = 0x82B370;
static KenshiHotkeyFn s_origKenshiHotkey = nullptr;
static std::atomic<int> s_kenshiHotkeySwallowed{0};
static std::atomic<int> s_kenshiHotkeyForwarded{0};

static void Hook_KenshiHotkey(void* self) {
    if (IsModalUiActive()) {
        s_kenshiHotkeySwallowed.fetch_add(1, std::memory_order_relaxed);
        return; // skip entire dispatcher — no hotkey fires
    }
    s_kenshiHotkeyForwarded.fetch_add(1, std::memory_order_relaxed);
    if (s_origKenshiHotkey) {
        __try {
            s_origKenshiHotkey(self);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // intentionally minimal — same SEH-isolation pattern as
            // CallOrigAddToUpdateListMainSafe (no destructible locals)
            OutputDebugStringA("KMP: KenshiHotkey trampoline crashed\n");
        }
    }
}

bool InstallKenshiHotkey() {
    if (s_origKenshiHotkey) return true;
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe) return false;
    auto target = reinterpret_cast<uintptr_t>(exe) + RVA_KENSHI_HOTKEY;

    // .pdata sanity — refuse if we're mid-function on this build.
    DWORD64 imageBase = 0;
    auto* rt = RtlLookupFunctionEntry(static_cast<DWORD64>(target),
                                      &imageBase, nullptr);
    if (rt) {
        uintptr_t funcStart = static_cast<uintptr_t>(imageBase) + rt->BeginAddress;
        if (funcStart != target) {
            spdlog::warn("input_hooks: KenshiHotkey RVA 0x{:X} is MID-FUNCTION "
                         "(real start at 0x{:X}) — refusing to install",
                         RVA_KENSHI_HOTKEY, funcStart);
            return false;
        }
    }

    auto& hookMgr = HookManager::Get();
    if (!hookMgr.InstallAt("KenshiHotkey", target,
                           &Hook_KenshiHotkey, &s_origKenshiHotkey)) {
        spdlog::warn("input_hooks: KenshiHotkey install FAILED at 0x{:X}", target);
        return false;
    }
    spdlog::info("input_hooks: KenshiHotkey hook INSTALLED at 0x{:X} "
                 "(RVA 0x{:X}) — F1/hotkeys skip while modal UI active",
                 target, RVA_KENSHI_HOTKEY);
    return true;
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
    if (s_origKenshiHotkey) {
        hookMgr.Remove("KenshiHotkey");
        spdlog::info("input_hooks: Uninstalled KenshiHotkey "
                     "(swallowed={}, forwarded={})",
                     s_kenshiHotkeySwallowed.load(std::memory_order_relaxed),
                     s_kenshiHotkeyForwarded.load(std::memory_order_relaxed));
        s_origKenshiHotkey = nullptr;
    }
    // Always try to undo the IAT hook; safe no-op when not installed.
    UninstallGetKeyboardStateIatHook();
    s_installed = false;
}

} // namespace kmp::input_hooks
