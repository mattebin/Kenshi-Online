#include "render_hooks.h"
#include "../core.h"
#include "entity_hooks.h"
#include "input_hooks.h"
#include "../ui/mygui_bridge.h"
#include "kmp/hook_manager.h"
#include <spdlog/spdlog.h>
#include <d3d11.h>
#include <dxgi.h>
#include <chrono>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace kmp::render_hooks {

// Custom message for spawn queue processing — DEPRECATED.
// ProcessSpawnQueue() consumed the queue before the in-place replay (entity_hooks)
// could use it. The in-place replay is the ONLY safe spawn mechanism.
static constexpr UINT WM_KMP_SPAWN = WM_USER + 100;

// ── State ──
static HWND                  s_hwnd = nullptr;
static WNDPROC               s_originalWndProc = nullptr;

// ── Types ──
using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
static PresentFn s_originalPresent = nullptr;

// ── SEH wrapper for spawn queue processing from WndProc ──
// DISABLED: ProcessSpawnQueue() consumed requests before the in-place replay
// (entity_hooks) could use them. In-place replay is the only safe spawn mechanism.

// ── SEH wrapper for OnGameTick ──
static void SEH_OnGameTick(float dt) {
    __try {
        Core::Get().OnGameTick(dt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_crashCount = 0;
        s_crashCount++;
        int lastStep = Core::Get().GetLastCompletedStep();
        DWORD code = GetExceptionCode();
        if (s_crashCount <= 10 || s_crashCount % 100 == 0) {
            char buf[256];
            sprintf_s(buf, "KMP: OnGameTick SEH crash #%d — exception 0x%08lX at step %d "
                           "(dt=%.4f)\n", s_crashCount, code, lastStep, dt);
            OutputDebugStringA(buf);
            spdlog::error("render_hooks: OnGameTick SEH crash #{} — exception 0x{:08X} "
                          "at step {} (dt={:.4f})", s_crashCount, code, lastStep, dt);
        }
    }
}

// ── MULTIPLAYER button bounds (from Kenshi_MainMenu.layout position_real) ──
static constexpr float MP_BTN_X = 0.260417f;
static constexpr float MP_BTN_Y = 0.582407f;  // Must match Kenshi_MainMenu.layout MultiplayerButton position
static constexpr float MP_BTN_W = 0.15625f;
static constexpr float MP_BTN_H = 0.0638889f;

// ── Startup timestamp: don't allow native menu until main menu is likely loaded ──
static auto s_firstPresentTime = std::chrono::steady_clock::time_point{};
static bool s_firstPresentRecorded = false;

static bool IsMainMenuReady() {
    // Don't allow native menu for the first 15 seconds after first Present.
    // The logo/splash screen runs during this time — MyGUI resources aren't loaded yet.
    if (!s_firstPresentRecorded) return false;
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - s_firstPresentTime);
    return elapsed.count() >= 15;
}

static bool IsMouseMessage(UINT msg) {
    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        return true;
    default:
        return false;
    }
}

static bool IsKeyboardMessage(UINT msg) {
    switch (msg) {
    case WM_KEYDOWN: case WM_KEYUP:
    case WM_SYSKEYDOWN: case WM_SYSKEYUP:
    case WM_CHAR: case WM_SYSCHAR: case WM_UNICHAR:
        return true;
    default:
        return false;
    }
}

static bool IsPrintableChatChar(WPARAM wParam) {
    // Basic printable range. Keeps chat simple and prevents weird control chars.
    return (wParam >= 32 && wParam != 127);
}

// ── WndProc Hook (pure Win32 input — no ImGui) ──
// Inner function does the actual work — called from SEH wrapper.
//
// Modal-gate model (borrowed from upstream The404Studios commit 5731fc4,
// adapted to coexist with our OIS + MyGUI hooks). When chat or our native
// menu is open we are MODAL: every keyboard / mouse message that isn't
// explicitly forwarded to the active UI gets swallowed here. Anything we
// do *not* consume here can still reach Kenshi via OIS / GetKeyboardState
// — those paths are gated by input_hooks.cpp (OIS) and the MyGUI hooks.
static LRESULT WndProcInner(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // WM_KMP_SPAWN: no longer used — spawn queue is handled by in-place replay only.
    if (uMsg == WM_KMP_SPAWN) {
        return 0;
    }

    auto& core       = Core::Get();
    auto& overlay    = core.GetOverlay();
    auto& nativeMenu = overlay.GetNativeMenu();
    auto& nativeHud  = core.GetNativeHud();

    bool chatActive    = nativeHud.IsChatInputActive();
    bool menuVisible   = nativeMenu.IsVisible();
    bool modalUiActive = chatActive || menuVisible;

    // ── F1: toggle native menu. Disabled while chat is typing. ──
    if (uMsg == WM_KEYDOWN && wParam == VK_F1 && !(lParam & 0x40000000)) {
        if (chatActive) return 0; // swallow while typing
        if (menuVisible) {
            nativeMenu.Hide();
        } else if (!core.IsGameLoaded() && !IsMainMenuReady()) {
            OutputDebugStringA("KMP: F1 pressed too early (logo/splash) — ignoring\n");
        } else {
            nativeMenu.Show();
        }
        return 0;
    }

    // ── Escape: close active modal first; otherwise let the game handle it. ──
    if (uMsg == WM_KEYDOWN && wParam == VK_ESCAPE && !(lParam & 0x40000000)) {
        if (chatActive) {
            nativeHud.CloseChatInput();
            return 0;
        }
        if (menuVisible) {
            nativeMenu.OnKeyDown(VK_ESCAPE);
            nativeMenu.Hide();
            return 0;
        }
    }

    // ── Enter: open chat or commit chat line (game-loaded only). ──
    if (uMsg == WM_KEYDOWN && wParam == VK_RETURN && !(lParam & 0x40000000)) {
        if (!menuVisible && core.IsGameLoaded()) {
            if (chatActive) {
                nativeHud.OnChatKeyDown(VK_RETURN);
            } else {
                nativeHud.OpenChatInput();
            }
            return 0;
        }
    }

    // ── Tab: toggle player list. Disabled while modal. ──
    if (uMsg == WM_KEYDOWN && wParam == VK_TAB && !(lParam & 0x40000000)) {
        if (modalUiActive) return 0;
        if (core.IsGameLoaded() && core.IsConnected()) {
            nativeHud.TogglePlayerList();
            return 0;
        }
    }

    // ── Insert: toggle log panel. Disabled while modal. ──
    if (uMsg == WM_KEYDOWN && wParam == VK_INSERT && !(lParam & 0x40000000)) {
        if (modalUiActive) return 0;
        nativeHud.ToggleLogPanel();
        return 0;
    }

    // ── Backtick: toggle debug info. Disabled while modal. ──
    if (uMsg == WM_KEYDOWN && wParam == VK_OEM_3 && !(lParam & 0x40000000)) {
        if (modalUiActive) return 0;
        if (core.IsGameLoaded() && core.IsConnected()) {
            nativeHud.ToggleDebugInfo();
            return 0;
        }
    }

    // ── Strict modal gates ──
    // When chat or menu is active, swallow ALL further input that isn't
    // explicitly forwarded to the active UI. This is the line of defense
    // for Win32 messages; OIS / MyGUI gates handle the engine-side paths.
    if (modalUiActive) {
        if (chatActive) {
            if (uMsg == WM_CHAR || uMsg == WM_SYSCHAR || uMsg == WM_UNICHAR) {
                if (IsPrintableChatChar(wParam)) {
                    nativeHud.OnChatChar(static_cast<wchar_t>(wParam));
                }
                return 0;
            }
            if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) {
                switch (wParam) {
                case VK_BACK:
                case VK_RETURN:
                case VK_ESCAPE:
                    nativeHud.OnChatKeyDown(static_cast<int>(wParam));
                    break;
                default:
                    break; // swallow everything else
                }
                return 0;
            }
            if (uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) return 0;
            if (IsMouseMessage(uMsg)) return 0;
        }

        if (menuVisible) {
            if (uMsg == WM_CHAR || uMsg == WM_SYSCHAR || uMsg == WM_UNICHAR) {
                if (nativeMenu.HasActiveEditBox() && IsPrintableChatChar(wParam)) {
                    nativeMenu.OnChar(static_cast<wchar_t>(wParam));
                }
                return 0;
            }
            if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) {
                switch (wParam) {
                case VK_BACK:
                case VK_RETURN:
                case VK_TAB:
                case VK_ESCAPE:
                    nativeMenu.OnKeyDown(static_cast<int>(wParam));
                    break;
                default:
                    break;
                }
                return 0;
            }
            if (uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) return 0;
            if (uMsg == WM_LBUTTONDOWN) {
                nativeMenu.OnClick(LOWORD(lParam), HIWORD(lParam));
                return 0;
            }
            if (IsMouseMessage(uMsg)) return 0;
        }

        // Failsafe: any other input message is swallowed while modal.
        if (IsKeyboardMessage(uMsg) || IsMouseMessage(uMsg)) return 0;
    }

    // ── Mouse click handling outside modal UI ──
    if (uMsg == WM_LBUTTONDOWN) {
        int mx = LOWORD(lParam);
        int my = HIWORD(lParam);
        if (!core.IsGameLoaded() && IsMainMenuReady()) {
            // On main menu — check if click hit our MULTIPLAYER button
            RECT clientRect;
            if (GetClientRect(hWnd, &clientRect)) {
                float screenW = static_cast<float>(clientRect.right - clientRect.left);
                float screenH = static_cast<float>(clientRect.bottom - clientRect.top);
                if (screenW > 0 && screenH > 0) {
                    float nx = static_cast<float>(mx) / screenW;
                    float ny = static_cast<float>(my) / screenH;
                    if (nx >= MP_BTN_X && nx <= (MP_BTN_X + MP_BTN_W) &&
                        ny >= MP_BTN_Y && ny <= (MP_BTN_Y + MP_BTN_H)) {
                        spdlog::info("render_hooks: MULTIPLAYER button clicked ({}, {})", mx, my);
                        nativeMenu.Show();
                        return 0;
                    }
                }
            }
        }
    }

    return CallWindowProcA(s_originalWndProc, hWnd, uMsg, wParam, lParam);
}

// SEH wrapper — a crash in our WndProc must not kill the game
static LRESULT CALLBACK HookWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    __try {
        return WndProcInner(hWnd, uMsg, wParam, lParam);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_count = 0;
        if (++s_count <= 10) {
            char buf[128];
            sprintf_s(buf, "KMP: SEH CRASH in WndProc (msg=0x%X, wp=0x%llX)\n",
                      uMsg, (unsigned long long)wParam);
            OutputDebugStringA(buf);
        }
        return CallWindowProcA(s_originalWndProc, hWnd, uMsg, wParam, lParam);
    }
}

// ── Present Hook ──
// Passthrough: HWND discovery + WndProc hook + OnGameTick fallback.
// NO ImGui rendering — Ogre3D/DX11 conflict causes crash.
static std::chrono::steady_clock::time_point s_lastFrameTime{};
static bool s_hasLastFrameTime = false;

// ── SEH wrappers for per-frame calls ──
static void SEH_OverlayUpdate() {
    __try {
        Core::Get().GetOverlay().Update();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_count = 0;
        if (++s_count <= 5) OutputDebugStringA("KMP: SEH CRASH in Overlay::Update()\n");
    }
}

// All rendering via native MyGUI NativeHud — no GDI overlay.

static void SEH_NativeHudUpdate() {
    __try {
        Core::Get().GetNativeHud().Update();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_count = 0;
        if (++s_count <= 5) OutputDebugStringA("KMP: SEH CRASH in NativeHud::Update()\n");
    }
}

// No GDI overlay — NativeHud handles all display.

// Track frame timing for loading gap detection
static std::chrono::steady_clock::time_point s_prevPresentTime{};
static bool s_hasPrevPresentTime = false;

// Smooth-frame game-load detection: after Loading starts, if we get
// 5 seconds of smooth frames (no >2s gaps), the game has finished loading.
static std::chrono::steady_clock::time_point s_loadingSmoothStart{};
static bool s_loadingSmoothStarted = false;
// Snapshot of entity_hooks::GetTotalCreates() when Loading phase started.
// We check (current - snapshot > 5) to detect real save loads vs character
// creation screens, and to avoid false positives on second loads.
static int s_createsAtLoadingStart = 0;

static HRESULT __stdcall HookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    // Record first Present time for startup guard (logo/splash delay)
    if (!s_firstPresentRecorded) {
        s_firstPresentTime = std::chrono::steady_clock::now();
        s_firstPresentRecorded = true;
        OutputDebugStringA("KMP: First Present — recording startup time\n");
    }

    static int s_presentCount = 0;
    s_presentCount++;
    if (s_presentCount <= 3) {
        char buf[128];
        sprintf_s(buf, "KMP: HookPresent #%d\n", s_presentCount);
        OutputDebugStringA(buf);
    }

    // ── Phase transitions driven by Present timing ──
    auto& core = Core::Get();
    auto now = std::chrono::steady_clock::now();
    {
        ClientPhase phase = core.GetClientPhase();

        // Startup → MainMenu: after 5 seconds of Present firing (splash is done)
        if (phase == ClientPhase::Startup) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - s_firstPresentTime);
            if (elapsed.count() >= 5) {
                core.TransitionTo(ClientPhase::MainMenu);
            }
        }

        // MainMenu → Loading: detect a long gap between Present calls (>2s).
        // During normal menu rendering, Present fires every ~4-16ms.
        // When the user clicks New Game / Continue / Load, the game blocks
        // for 10-60 seconds while loading. The first Present AFTER that gap
        // is our signal that loading just finished (or is finishing).
        //
        // IMPORTANT: Only transition from MainMenu. While Connected/GameReady,
        // zone loading also causes >2s gaps — but that's handled by entity_hooks
        // burst guard, NOT by re-entering the Loading phase.
        if (s_hasPrevPresentTime) {
            auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_prevPresentTime);
            if (gap.count() > 2000) {
                if (phase == ClientPhase::MainMenu ||
                    ((phase == ClientPhase::Connected || phase == ClientPhase::Connecting) &&
                     !core.IsGameLoaded())) {
                    // Detect initial save loads from MainMenu, or from an
                    // already-connected pre-load state. NOT from Startup.
                    // The initial engine loading (shaders, textures, splash screen)
                    // creates >2s gaps during Startup, which is NOT a save game load.
                    // Startup → MainMenu transition happens after 5s of smooth Present calls.
                    spdlog::info("render_hooks: Loading gap detected ({} ms between frames, phase={})",
                                 gap.count(), ClientPhaseToString(phase));
                    // Reset smooth-frame tracking for new load (prevents stale state
                    // from first load causing premature OnGameLoaded on second load)
                    s_loadingSmoothStarted = false;
                    s_createsAtLoadingStart = entity_hooks::GetTotalCreates();
                    core.OnLoadingGapDetected();
                } else if (phase == ClientPhase::Loading) {
                    // Still loading — another gap means assets are still streaming.
                    // Reset smooth-frame timer.
                    s_loadingSmoothStarted = false;
                    spdlog::debug("render_hooks: Loading gap ({} ms) — reset smooth timer", gap.count());
                } else if (phase == ClientPhase::Connected && !core.IsGameLoaded()) {
                    // Connected from the main menu, then the player loaded a save.
                    // Treat this like the normal MainMenu -> Loading transition so
                    // the deferred sync hooks resume after the world is ready.
                    spdlog::info("render_hooks: Connected-before-load save gap detected ({} ms)",
                                 gap.count());
                    s_loadingSmoothStarted = false;
                    s_createsAtLoadingStart = entity_hooks::GetTotalCreates();
                    core.OnLoadingGapDetected();
                } else if (phase == ClientPhase::GameReady && gap.count() > 10000) {
                    // Very long gap (>10s) during GameReady = user loaded a new save
                    // from the in-game Load menu. Zone transitions are 2-5s max.
                    // Reset state and re-enter Loading so OnGameLoaded fires again.
                    spdlog::info("render_hooks: In-game save load detected ({} ms gap during GameReady)", gap.count());
                    s_loadingSmoothStarted = false;
                    s_createsAtLoadingStart = entity_hooks::GetTotalCreates();
                    core.OnLoadingGapDetected();
                } else if (phase == ClientPhase::Connected || phase == ClientPhase::GameReady) {
                    spdlog::info("render_hooks: Zone load gap detected ({} ms) during {} — no phase change",
                                 gap.count(), ClientPhaseToString(phase));
                }
            }
        }

        // Smooth-frame game-load detection: while in Loading phase, track how long
        // we've had smooth frames (no >2s gaps). After 8 seconds of smooth rendering,
        // trigger PollForGameLoad which checks CharacterIterator for actual characters.
        // Don't call OnGameLoaded directly — the poll validates that characters exist.
        static bool s_smoothTriggeredPoll = false;
        if (phase == ClientPhase::Loading) {
            if (!s_loadingSmoothStarted) {
                s_loadingSmoothStart = now;
                s_loadingSmoothStarted = true;
            }
            auto smoothDuration = std::chrono::duration_cast<std::chrono::seconds>(now - s_loadingSmoothStart);
            if (smoothDuration.count() >= 8 && !core.IsGameLoaded()) {
                // Don't fire OnGameLoaded blindly — let PollForGameLoad verify
                // that characters actually exist via CharacterIterator.
                // This prevents false positives on the main menu or character creation.
                if (!s_smoothTriggeredPoll) {
                    s_smoothTriggeredPoll = true;
                    spdlog::info("render_hooks: 8s of smooth frames during Loading — triggering PollForGameLoad");
                    core.PollForGameLoad();
                }
            }
        } else {
            s_smoothTriggeredPoll = false;
        }
    }
    s_prevPresentTime = now;
    s_hasPrevPresentTime = true;

    // ── Periodic diagnostic ──
    if (s_presentCount % 300 == 1) {
        spdlog::info("render_hooks: frame={} phase={} gameLoaded={} connected={}",
                     s_presentCount, ClientPhaseToString(core.GetClientPhase()),
                     core.IsGameLoaded(), core.IsConnected());
    }

    // One-time: grab HWND from the swap chain for WndProc hook
    if (!s_hwnd) {
        DXGI_SWAP_CHAIN_DESC desc;
        if (SUCCEEDED(swapChain->GetDesc(&desc))) {
            s_hwnd = desc.OutputWindow;
            OutputDebugStringA("KMP: Got HWND from swap chain\n");

            // Install WndProc hook for input (F1, mouse clicks, etc.)
            SetLastError(0);
            s_originalWndProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrA(s_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookWndProc)));
            if (!s_originalWndProc && GetLastError() != 0) {
                // SetWindowLongPtrA failed — don't keep our WndProc installed
                // because CallWindowProcA(nullptr) would crash
                spdlog::error("render_hooks: SetWindowLongPtrA FAILED (err={})", GetLastError());
                OutputDebugStringA("KMP: WndProc hook FAILED\n");
                s_hwnd = nullptr; // Force retry next frame
            } else {
                OutputDebugStringA("KMP: WndProc hook installed\n");
            }

            // GDI overlay removed — native MyGUI HUD (NativeHud) handles all rendering
        }
    }

    // ── Per-frame overlay update (auto-connect, connection state, disconnect detect) ──
    // Each call is SEH-protected so a crash in one doesn't kill the game.
    {
        static bool s_overlayUpdateStarted = false;
        if (!s_overlayUpdateStarted) {
            OutputDebugStringA("KMP: HookPresent — starting per-frame Update() calls\n");
            s_overlayUpdateStarted = true;
        }
        SEH_OverlayUpdate();
        // NativeHud handles all display
        SEH_NativeHudUpdate();
    }

    // ── Lazy MyGUI input-swallow install ──
    // input_hooks::Install() (OIS gate) ran during Core::InitHooks. The MyGUI
    // gate has to wait until MyGUIEngine_x64.dll is mapped and its singletons
    // are constructed; otherwise GetProcAddress finds nothing or hooks the
    // wrong target. MyGuiBridge::IsReady() flips true the moment we can
    // resolve the engine, so polling here per-frame is cheap and correct.
    {
        static bool s_myguiInputInstalled = false;
        if (!s_myguiInputInstalled && MyGuiBridge::Get().IsReady()) {
            if (input_hooks::InstallMyGuiSwallow()) {
                s_myguiInputInstalled = true;
            }
        }
    }

    // ── OnGameTick driver ──
    // Always drive OnGameTick from Present hook. TimeUpdate at RVA 0x214B50
    // was found to never fire on Steam builds (the game doesn't call that
    // function). The 4ms dedup guard inside OnGameTick prevents double-processing
    // if TimeUpdate ever starts working.
    if (core.IsConnected()) {
        static int s_connectedFrames = 0;
        s_connectedFrames++;

        // Log first 20 connected frames, then every 100th
        if (s_connectedFrames <= 20 || s_connectedFrames % 100 == 0) {
            spdlog::debug("render_hooks: Connected frame #{} (present #{})",
                          s_connectedFrames, s_presentCount);
        }

        auto now = std::chrono::steady_clock::now();
        if (s_hasLastFrameTime) {
            float dt = std::chrono::duration<float>(now - s_lastFrameTime).count();
            if (dt > 0.0f && dt < 0.5f) {
                SEH_OnGameTick(dt);
            }
        } else {
            OutputDebugStringA("KMP: First OnGameTick from Present hook\n");
            spdlog::info("render_hooks: First OnGameTick from Present hook (connected frame #{})",
                         s_connectedFrames);
        }
        s_lastFrameTime = now;
        s_hasLastFrameTime = true;
    }

    if (s_originalPresent) {
        return s_originalPresent(swapChain, syncInterval, flags);
    }
    return E_FAIL;
}

// ── Get DXGI VTable ──
// Create a temporary D3D11 device + swap chain to read the vtable
static bool GetDXGIVTable(void**& vtable) {
    WNDCLASSEXA wc = {sizeof(WNDCLASSEXA), CS_CLASSDC, DefWindowProcA, 0, 0,
                     GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                     "KMP_TEMP", nullptr};
    RegisterClassExA(&wc);
    HWND tempHwnd = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW,
                                 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = tempHwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* tempDevice = nullptr;
    IDXGISwapChain* tempSwapChain = nullptr;
    ID3D11DeviceContext* tempContext = nullptr;
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &scd, &tempSwapChain, &tempDevice, &featureLevel, &tempContext);

    if (FAILED(hr)) {
        DestroyWindow(tempHwnd);
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return false;
    }

    vtable = *reinterpret_cast<void***>(tempSwapChain);

    tempSwapChain->Release();
    tempContext->Release();
    tempDevice->Release();
    DestroyWindow(tempHwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);

    return true;
}

// ── Install/Uninstall ──

bool Install() {
    void** vtable = nullptr;
    if (!GetDXGIVTable(vtable)) {
        spdlog::error("render_hooks: Failed to get DXGI vtable");
        return false;
    }

    auto& hookMgr = HookManager::Get();

    // Present is vtable index 8
    if (!hookMgr.InstallAt("DXGI_Present",
                           reinterpret_cast<uintptr_t>(vtable[8]),
                           &HookPresent, &s_originalPresent)) {
        spdlog::error("render_hooks: Failed to hook Present");
        return false;
    }

    spdlog::info("render_hooks: Installed successfully (passthrough + WndProc)");
    return true;
}

void Uninstall() {
    if (s_originalWndProc && s_hwnd) {
        SetWindowLongPtrA(s_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(s_originalWndProc));
    }

    HookManager::Get().Remove("DXGI_Present");
}

void PostSpawnTrigger() {
    if (s_hwnd) {
        PostMessageA(s_hwnd, WM_KMP_SPAWN, 0, 0);
    }
}

} // namespace kmp::render_hooks
