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

    // Layers 4-6 (IAT-GetKeyboardState, KenshiKeyPressed @ 0x360B30,
    // KenshiOisKeyDown @ 0x82B010) are DISABLED. They were tested and
    // didn't reliably gate the vanilla hotkey path on 1.0.68 — likely
    // because IsModalUiActive() returned false on the input thread, or
    // dispatch reached the function via a path our entry-point hook
    // didn't cover. Implementations kept for reference / re-enabling.
    constexpr bool kInstallLegacyInputHooks = false;
    bool iatOk = false, keyPressedOk = false, oisKeyDownOk = false;
    if (kInstallLegacyInputHooks) {
        iatOk        = InstallGetKeyboardStateIatHook();
        keyPressedOk = InstallKenshiKeyPressed();
        oisKeyDownOk = InstallKenshiOisKeyDown();
    }

    s_installed = true;
    spdlog::info("input_hooks: Installed (WndProc + OIS gate keyDown={} "
                 "keyUp={}; legacy layers disabled)",
                 keyDownOk, keyUpOk);
    (void)iatOk; (void)keyPressedOk; (void)oisKeyDownOk;
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

using GetKeyboardStateFn  = BOOL(WINAPI*)(PBYTE);
using GetAsyncKeyStateFn  = SHORT(WINAPI*)(int);
static GetKeyboardStateFn s_origGetKeyboardState = nullptr;
static void**             s_iatSlot              = nullptr;
static GetAsyncKeyStateFn s_origGetAsyncKeyState = nullptr;
static void**             s_iatSlotAsync         = nullptr;
static std::atomic<int>   s_iatHookCalls{0};
static std::atomic<int>   s_iatHookMasked{0};
static std::atomic<int>   s_iatAsyncCalls{0};
static std::atomic<int>   s_iatAsyncMasked{0};

static bool ShouldMaskVk(int vk) {
    // Letters A-Z
    if (vk >= 0x41 && vk <= 0x5A) return true;
    // Top-row digits 0-9
    if (vk >= 0x30 && vk <= 0x39) return true;
    // F1-F12 (covers Kenshi's F2/F3/F4 speed keys + F1 vanilla help)
    if (vk >= 0x70 && vk <= 0x7B) return true;
    switch (vk) {
        case VK_SPACE: case VK_TAB:
        case VK_INSERT: case VK_DELETE:
        case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT:
            return true;
    }
    return false;
}

static BOOL WINAPI Hook_GetKeyboardState(PBYTE lpKeyState) {
    int n = s_iatHookCalls.fetch_add(1, std::memory_order_relaxed);
    if (n == 0) {
        spdlog::info("input_hooks: Hook_GetKeyboardState fired first time "
                     "(modal={})", IsModalUiActive() ? "yes" : "no");
    }
    BOOL r = s_origGetKeyboardState ? s_origGetKeyboardState(lpKeyState) : 0;
    if (!r || !lpKeyState) return r;
    if (!IsModalUiActive()) return r;

    s_iatHookMasked.fetch_add(1, std::memory_order_relaxed);
    for (int vk = 0; vk < 256; ++vk) {
        if (ShouldMaskVk(vk)) lpKeyState[vk] = 0;
    }
    return r;
}

static SHORT WINAPI Hook_GetAsyncKeyState(int vKey) {
    int n = s_iatAsyncCalls.fetch_add(1, std::memory_order_relaxed);
    if (n == 0) {
        spdlog::info("input_hooks: Hook_GetAsyncKeyState fired first time "
                     "(vk=0x{:X}, modal={})", vKey,
                     IsModalUiActive() ? "yes" : "no");
    }
    SHORT r = s_origGetAsyncKeyState ? s_origGetAsyncKeyState(vKey) : 0;
    if (!IsModalUiActive()) return r;
    if (!ShouldMaskVk(vKey)) return r;
    s_iatAsyncMasked.fetch_add(1, std::memory_order_relaxed);
    return 0;  // not pressed, no transition
}

// Patch one IAT entry in USER32.DLL. Returns the original target via
// `outOrig` and the slot address via `outSlot`. Both can be passed null
// to ignore. Returns true if patched.
static bool PatchUser32Iat(const char* importName, void* hookFn,
                           void** outSlot, void** outOrig) {
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe) return false;
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
        auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + desc->OriginalFirstThunk);
        auto* iatThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + desc->FirstThunk);
        for (; nameThunk->u1.AddressOfData; ++nameThunk, ++iatThunk) {
            if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal)) continue;
            auto* impName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + nameThunk->u1.AddressOfData);
            if (strcmp(impName->Name, importName) != 0) continue;

            void** slot =
                reinterpret_cast<void**>(&iatThunk->u1.Function);
            DWORD oldProt = 0;
            if (!VirtualProtect(slot, sizeof(void*),
                                PAGE_READWRITE, &oldProt)) {
                return false;
            }
            void* origPtr = *slot;
            *slot = hookFn;
            VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
            if (outSlot) *outSlot = slot;
            if (outOrig) *outOrig = origPtr;
            spdlog::info("input_hooks: IAT hook '{}' INSTALLED "
                         "(orig=0x{:X}, slot=0x{:X})",
                         importName,
                         reinterpret_cast<uintptr_t>(origPtr),
                         reinterpret_cast<uintptr_t>(slot));
            return true;
        }
        break;
    }
    spdlog::warn("input_hooks: IAT hook '{}' import NOT FOUND", importName);
    return false;
}

static bool InstallGetKeyboardStateIatHook() {
    bool a = false, b = false;
    if (!s_iatSlot) {
        void* orig = nullptr;
        a = PatchUser32Iat("GetKeyboardState",
                           reinterpret_cast<void*>(&Hook_GetKeyboardState),
                           reinterpret_cast<void**>(&s_iatSlot), &orig);
        if (a) s_origGetKeyboardState =
            reinterpret_cast<GetKeyboardStateFn>(orig);
    } else { a = true; }
    if (!s_iatSlotAsync) {
        void* orig = nullptr;
        b = PatchUser32Iat("GetAsyncKeyState",
                           reinterpret_cast<void*>(&Hook_GetAsyncKeyState),
                           reinterpret_cast<void**>(&s_iatSlotAsync), &orig);
        if (b) s_origGetAsyncKeyState =
            reinterpret_cast<GetAsyncKeyStateFn>(orig);
    } else { b = true; }
    return a || b;
}

static void UninstallGetKeyboardStateIatHook() {
    if (s_iatSlot && s_origGetKeyboardState) {
        DWORD oldProt = 0;
        if (VirtualProtect(s_iatSlot, sizeof(void*),
                           PAGE_READWRITE, &oldProt)) {
            *s_iatSlot = reinterpret_cast<void*>(s_origGetKeyboardState);
            VirtualProtect(s_iatSlot, sizeof(void*), oldProt, &oldProt);
        }
        s_iatSlot = nullptr;
        s_origGetKeyboardState = nullptr;
    }
    if (s_iatSlotAsync && s_origGetAsyncKeyState) {
        DWORD oldProt = 0;
        if (VirtualProtect(s_iatSlotAsync, sizeof(void*),
                           PAGE_READWRITE, &oldProt)) {
            *s_iatSlotAsync = reinterpret_cast<void*>(s_origGetAsyncKeyState);
            VirtualProtect(s_iatSlotAsync, sizeof(void*), oldProt, &oldProt);
        }
        s_iatSlotAsync = nullptr;
        s_origGetAsyncKeyState = nullptr;
    }
    spdlog::info("input_hooks: IAT hooks removed "
                 "(KB calls={} masked={}, Async calls={} masked={})",
                 s_iatHookCalls.load(std::memory_order_relaxed),
                 s_iatHookMasked.load(std::memory_order_relaxed),
                 s_iatAsyncCalls.load(std::memory_order_relaxed),
                 s_iatAsyncMasked.load(std::memory_order_relaxed));
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

// ── Kenshi _keyPressed hook (THE correct dispatcher) ──────────────────────
//
// Recon11→13 (2026-05-04) located Kenshi 1.0.68's hotkey dispatcher:
//
//   FUN_140360B30 — _keyPressed(this, scanCode):
//       this+0xd8 / +0xd9 / +0xda  — ctrl / shift / alt held flags
//       this+0xd0                 — gate flag (input enabled?)
//       this+0x30 / +0x58          — std::set<HotkeyBinding> trees
//       binding+0x10               — "currently held" byte
//
//   Function structure (matches symmetric _keyReleased at 0x360DA0):
//       1. update modifier flag for sc==0x2a/0x36/0x1d/0x9d/0x38/0xb8
//       2. if (this+0xd0 == 0) return;        ← Kenshi's own gate
//       3. compute uVar7 = scanCode | (modifiers << 8)
//       4. walk trees, fire matching bindings
//
//   Both keyDownEvent (0x360680) we already hook and _keyPressed
//   (0x360B30) we add now are SEPARATE OIS::KeyListener subscribers
//   on the same OIS::Keyboard. The earlier hook only covered the
//   keyDownEvent listener (Kenshi's tree at +8); the hotkey dispatch
//   was happening via the _keyPressed listener which uses different
//   member trees (+0x30 and +0x58).
//
// We hook _keyPressed and drop the call entirely when our chat or
// menu is modal — no hotkey binding fires while user is typing.
// Modifier flags (this+0xd8/d9/da) are NOT updated when we drop, but
// that only affects future hotkey-recognition combos which we're
// also dropping, so consistent.

using KenshiKeyPressedFn = void(__fastcall*)(void* self, std::uint32_t scanCode);
static constexpr uintptr_t RVA_KENSHI_KEY_PRESSED = 0x360B30;
static KenshiKeyPressedFn s_origKenshiKeyPressed = nullptr;
static std::atomic<int> s_keyPressedSwallowed{0};
static std::atomic<int> s_keyPressedForwarded{0};
// Diagnostic — log first N calls so we can SEE the function fires per
// keypress and which scancodes hit it. Unset after we've validated.
static std::atomic<int> s_keyPressedDiagLogged{0};

static void __fastcall Hook_KenshiKeyPressed(void* self, std::uint32_t scanCode) {
    int n = s_keyPressedDiagLogged.fetch_add(1, std::memory_order_relaxed);
    bool modal = IsModalUiActive();
    if (n < 30) {
        spdlog::info(
            "input_hooks: KenshiKeyPressed FIRED #{} self=0x{:X} sc=0x{:X} "
            "modal={} -> {}",
            n, reinterpret_cast<uintptr_t>(self), scanCode,
            modal ? "yes" : "no", modal ? "DROP" : "forward");
    }
    if (modal) {
        s_keyPressedSwallowed.fetch_add(1, std::memory_order_relaxed);
        return; // drop entire dispatcher — no hotkey binding fires
    }
    s_keyPressedForwarded.fetch_add(1, std::memory_order_relaxed);
    if (s_origKenshiKeyPressed) {
        __try {
            s_origKenshiKeyPressed(self, scanCode);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // No destructible locals — same SEH-isolation pattern as
            // CallOrigAddToUpdateListMainSafe.
            OutputDebugStringA("KMP: KenshiKeyPressed trampoline crashed\n");
        }
    }
}

bool InstallKenshiKeyPressed() {
    if (s_origKenshiKeyPressed) return true;
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe) return false;
    auto target = reinterpret_cast<uintptr_t>(exe) + RVA_KENSHI_KEY_PRESSED;

    // .pdata sanity — refuse if mid-function on this build.
    DWORD64 imageBase = 0;
    auto* rt = RtlLookupFunctionEntry(static_cast<DWORD64>(target),
                                      &imageBase, nullptr);
    if (rt) {
        uintptr_t funcStart = static_cast<uintptr_t>(imageBase) + rt->BeginAddress;
        if (funcStart != target) {
            spdlog::warn(
                "input_hooks: KenshiKeyPressed RVA 0x{:X} is MID-FUNCTION "
                "(real start 0x{:X}, offset +0x{:X}) — RECOVERING",
                RVA_KENSHI_KEY_PRESSED, funcStart, target - funcStart);
            target = funcStart;
        } else {
            spdlog::info(
                "input_hooks: KenshiKeyPressed .pdata bounds OK "
                "(start=0x{:X}, end=0x{:X}, size=0x{:X})",
                funcStart, static_cast<uintptr_t>(imageBase) + rt->EndAddress,
                rt->EndAddress - rt->BeginAddress);
        }
    } else {
        spdlog::warn("input_hooks: KenshiKeyPressed .pdata lookup FAILED — "
                     "function may not exist on this build");
    }

    // Dump first 16 bytes of prologue for diagnosis. MinHook needs at
    // least 5 bytes of overwriteable instructions for a JMP rel32.
    auto* p = reinterpret_cast<const std::uint8_t*>(target);
    spdlog::info(
        "input_hooks: KenshiKeyPressed prologue at 0x{:X}: "
        "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
        "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
        target,
        p[0], p[1], p[2],  p[3],  p[4],  p[5],  p[6],  p[7],
        p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);

    auto& hookMgr = HookManager::Get();
    if (!hookMgr.InstallAt("KenshiKeyPressed", target,
                           &Hook_KenshiKeyPressed, &s_origKenshiKeyPressed)) {
        spdlog::warn("input_hooks: KenshiKeyPressed install FAILED at 0x{:X}",
                     target);
        return false;
    }
    spdlog::info(
        "input_hooks: KenshiKeyPressed hook INSTALLED at 0x{:X} "
        "(RVA 0x{:X}) — vanilla hotkeys skip while modal UI active. "
        "trampoline=0x{:X}",
        target, RVA_KENSHI_KEY_PRESSED,
        reinterpret_cast<uintptr_t>(s_origKenshiKeyPressed));
    return true;
}

// ── Kenshi OIS keyDown listener hook ───────────────────────────────────────
//
// FUN_14082B010 at RVA 0x82B010 (Recon17 2026-05-04) is the OIS
// KeyListener::keyPressed callback registered by Kenshi. It receives
// the OIS KeyEvent, calls MyGUI::injectKeyPress, then performs the
// "is keyboard-focused widget an EditBox" check and (if not) calls
// thunk_FUN_140360b30 to fire hotkey bindings.
//
// By hooking this whole function and dropping the call when our
// chat/menu is modal, we cut every downstream path simultaneously
// without needing to find the `this` pointer for Kenshi's input
// handler instance (the +0xD0 gate flag).
//
// Returns 1 (low byte) to mimic "event consumed" so OIS won't
// propagate to other listeners — matches the original function's
// success-path return value.

using KenshiOisKeyDownFn = std::uint64_t(__fastcall*)(std::int64_t p1,
                                                      std::int64_t keyEvent);
static constexpr uintptr_t RVA_KENSHI_OIS_KEYDOWN = 0x82B010;
static KenshiOisKeyDownFn s_origKenshiOisKeyDown = nullptr;
static std::atomic<int> s_oisKeyDownSwallowed{0};
static std::atomic<int> s_oisKeyDownForwarded{0};
static std::atomic<int> s_oisKeyDownDiagLogged{0};

static std::uint64_t __fastcall Hook_KenshiOisKeyDown(std::int64_t p1,
                                                      std::int64_t keyEvent) {
    int n = s_oisKeyDownDiagLogged.fetch_add(1, std::memory_order_relaxed);
    bool modal = IsModalUiActive();
    if (n < 30) {
        // KeyEvent struct layout: scancode at +0x10 per the decompile.
        std::uint32_t sc = 0;
        if (keyEvent) {
            sc = *reinterpret_cast<const std::uint32_t*>(
                reinterpret_cast<const std::uint8_t*>(keyEvent) + 0x10);
        }
        spdlog::info(
            "input_hooks: KenshiOisKeyDown FIRED #{} p1=0x{:X} ev=0x{:X} "
            "sc=0x{:X} modal={} -> {}",
            n, static_cast<uintptr_t>(p1), static_cast<uintptr_t>(keyEvent),
            sc, modal ? "yes" : "no", modal ? "DROP" : "forward");
    }
    if (modal) {
        s_oisKeyDownSwallowed.fetch_add(1, std::memory_order_relaxed);
        return 1; // claim "event consumed"
    }
    s_oisKeyDownForwarded.fetch_add(1, std::memory_order_relaxed);
    if (s_origKenshiOisKeyDown) {
        __try {
            return s_origKenshiOisKeyDown(p1, keyEvent);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            OutputDebugStringA("KMP: KenshiOisKeyDown trampoline crashed\n");
        }
    }
    return 0;
}

bool InstallKenshiOisKeyDown() {
    if (s_origKenshiOisKeyDown) return true;
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe) return false;
    auto target = reinterpret_cast<uintptr_t>(exe) + RVA_KENSHI_OIS_KEYDOWN;

    DWORD64 imageBase = 0;
    auto* rt = RtlLookupFunctionEntry(static_cast<DWORD64>(target),
                                      &imageBase, nullptr);
    if (rt) {
        uintptr_t funcStart = static_cast<uintptr_t>(imageBase) + rt->BeginAddress;
        if (funcStart != target) {
            spdlog::warn(
                "input_hooks: KenshiOisKeyDown RVA 0x{:X} is MID-FUNCTION "
                "(real start 0x{:X}) — RECOVERING",
                RVA_KENSHI_OIS_KEYDOWN, funcStart);
            target = funcStart;
        } else {
            spdlog::info(
                "input_hooks: KenshiOisKeyDown .pdata bounds OK "
                "(start=0x{:X}, size=0x{:X})",
                funcStart, rt->EndAddress - rt->BeginAddress);
        }
    }

    auto* p = reinterpret_cast<const std::uint8_t*>(target);
    spdlog::info(
        "input_hooks: KenshiOisKeyDown prologue at 0x{:X}: "
        "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
        target, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);

    auto& hookMgr = HookManager::Get();
    if (!hookMgr.InstallAt("KenshiOisKeyDown", target,
                           &Hook_KenshiOisKeyDown, &s_origKenshiOisKeyDown)) {
        spdlog::warn("input_hooks: KenshiOisKeyDown install FAILED at 0x{:X}",
                     target);
        return false;
    }
    spdlog::info(
        "input_hooks: KenshiOisKeyDown hook INSTALLED at 0x{:X} (RVA 0x{:X}) "
        "— full OIS keyDown chain skipped while modal UI active",
        target, RVA_KENSHI_OIS_KEYDOWN);
    return true;
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
    if (s_origKenshiKeyPressed) {
        hookMgr.Remove("KenshiKeyPressed");
        spdlog::info("input_hooks: Uninstalled KenshiKeyPressed "
                     "(swallowed={}, forwarded={})",
                     s_keyPressedSwallowed.load(std::memory_order_relaxed),
                     s_keyPressedForwarded.load(std::memory_order_relaxed));
        s_origKenshiKeyPressed = nullptr;
    }
    if (s_origKenshiOisKeyDown) {
        hookMgr.Remove("KenshiOisKeyDown");
        spdlog::info("input_hooks: Uninstalled KenshiOisKeyDown "
                     "(swallowed={}, forwarded={})",
                     s_oisKeyDownSwallowed.load(std::memory_order_relaxed),
                     s_oisKeyDownForwarded.load(std::memory_order_relaxed));
        s_origKenshiOisKeyDown = nullptr;
    }
    // Always try to undo the IAT hook; safe no-op when not installed.
    UninstallGetKeyboardStateIatHook();
    s_installed = false;
}

} // namespace kmp::input_hooks
