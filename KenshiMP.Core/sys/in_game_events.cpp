#include "in_game_events.h"
#include "../core.h"
#include "kmp/hook_manager.h"
#include <spdlog/spdlog.h>

namespace kmp::in_game_events {

namespace {

// Kenshi RVAs from KenshiLib v0.3.0 (documented for v1.0.51).
//
//   LoadingWindow::hide        @ 0x911C10
//   MainBarGUI::_CONSTRUCTOR   @ 0x72C1E0
//
// On our v1.0.68 binary HookManager's mid-function detector refuses both
// installs and "helpfully" reports nearby prologue-shaped bytes
// (LoadingWindow at -0x40, MainBarGUI at -0xCA0). Earlier we trusted those
// suggestions and used them as the new RVAs — that hooked *unknown
// functions* (the walker only finds the nearest preceding function-looking
// pattern, not necessarily the function we named) and the MainBarGUI
// "fix" froze the loading thread mid-mesh-load on the first user test
// (kenshi.log stopped, then network packets bunched at one timestamp
// and the process died).
//
// Until we identify the real v1.0.68 entries (e.g. via signature scanning
// against the function's body bytes documented in KenshiLib, or via PDB
// symbols if the user obtains a debug build), we use the KenshiLib values
// and accept that InstallAt will refuse with "MID-FUNCTION". With the
// hooks not installed we silently fall back on render_hooks' heuristic
// detection, which is what was working before.
constexpr uintptr_t RVA_LOADING_WINDOW_HIDE = 0x911C10;
constexpr uintptr_t RVA_MAIN_BAR_CONSTRUCTOR = 0x72C1E0;

// Both are member functions taking only `this`. __fastcall on x64 is the
// Microsoft convention — RCX = this. Return type irrelevant for hook (we
// only act on the call event).
using ThisCallVoidFn = void(__fastcall*)(void* thisPtr);

ThisCallVoidFn s_origLoadingHide  = nullptr;
ThisCallVoidFn s_origMainBarCtor  = nullptr;

std::atomic<bool>     s_loadingHidden{false};
std::atomic<bool>     s_mainBarReady{false};
std::atomic<unsigned> s_loadingHideCount{0};
std::atomic<unsigned> s_mainBarCtorCount{0};

void __fastcall Hook_LoadingWindow_hide(void* thisPtr) {
    // Fire-and-forget signal. The render_hooks polling code can read this
    // flag instead of relying on Present-gap timing.
    s_loadingHidden.store(true, std::memory_order_release);
    const unsigned n = s_loadingHideCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 5) {
        spdlog::info("in_game_events: LoadingWindow::hide() #{} — world loaded", n);
    }
    if (s_origLoadingHide) {
        __try {
            s_origLoadingHide(thisPtr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Original threw — game's hide path probably mid-teardown.
            // Silent skip; signal is already set.
        }
    }
}

void __fastcall Hook_MainBarGUI_ctor(void* thisPtr) {
    s_mainBarReady.store(true, std::memory_order_release);
    const unsigned n = s_mainBarCtorCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 5) {
        spdlog::info("in_game_events: MainBarGUI::_CONSTRUCTOR #{} — HUD ready", n);
    }
    if (s_origMainBarCtor) {
        __try {
            s_origMainBarCtor(thisPtr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Same comment as above.
        }
    }
}

} // namespace

bool Install() {
    auto& scanner = Core::Get().GetScanner();
    const uintptr_t base = scanner.GetBase();
    if (!base) {
        spdlog::warn("in_game_events: scanner base unavailable, hooks not installed");
        return false;
    }
    auto& hookMgr = HookManager::Get();
    int ok = 0;
    if (hookMgr.InstallAt("LoadingWindow_hide", base + RVA_LOADING_WINDOW_HIDE,
                          &Hook_LoadingWindow_hide, &s_origLoadingHide)) {
        ++ok;
        spdlog::info("in_game_events: LoadingWindow::hide hook at 0x{:X}",
                     base + RVA_LOADING_WINDOW_HIDE);
    }
    if (hookMgr.InstallAt("MainBarGUI_ctor", base + RVA_MAIN_BAR_CONSTRUCTOR,
                          &Hook_MainBarGUI_ctor, &s_origMainBarCtor)) {
        ++ok;
        spdlog::info("in_game_events: MainBarGUI::_CONSTRUCTOR hook at 0x{:X}",
                     base + RVA_MAIN_BAR_CONSTRUCTOR);
    }
    if (ok == 0) {
        spdlog::warn("in_game_events: 0/2 hooks installed — render_hooks heuristics "
                     "remain the only in-game-detection path");
    }
    return ok > 0;
}

bool LoadingWindowHidden()       { return s_loadingHidden.load(std::memory_order_acquire); }
bool MainBarReady()              { return s_mainBarReady.load(std::memory_order_acquire); }
unsigned LoadingWindowHideCount() { return s_loadingHideCount.load(std::memory_order_relaxed); }
unsigned MainBarConstructCount()  { return s_mainBarCtorCount.load(std::memory_order_relaxed); }

} // namespace kmp::in_game_events
