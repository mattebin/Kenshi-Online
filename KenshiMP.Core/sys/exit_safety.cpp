#include "exit_safety.h"
#include "../core.h"
#include "../game/game_types.h"
#include "kmp/hook_manager.h"
#include <spdlog/spdlog.h>
#include <atomic>

namespace kmp::exit_safety {

namespace {

// GameWorld::_DESTRUCTOR. KenshiLib's header documents RVA 0x86C0C0 from
// Kenshi 1.0.51; that doesn't match our 1.0.68 binary (rva_validator
// reports the RVA points at mid-instruction bytes 28 89 8B 88 ...).
// Fallback path: GameWorld is a C++ object with a virtual destructor at
// vtable[0]. We have a GameWorld instance pointer (the dereferenced
// singleton); reading vtable[0] gives us the actual destructor address
// for whichever Kenshi build we're on. Build-agnostic.
constexpr uintptr_t RVA_GAMEWORLD_DESTRUCTOR_LEGACY_HINT = 0x86C0C0;

// GameWorld is __thiscall-passed (RCX = this), no further args.
using GameWorldDtorFn = void(__fastcall*)(void* gameWorld);
GameWorldDtorFn s_origGameWorldDtor = nullptr;

LPTOP_LEVEL_EXCEPTION_FILTER s_vanillaFilter   = nullptr;
std::atomic<bool>            s_installed{false};
std::atomic<bool>            s_filterRestored{false};

// VEH handle borrowed from core.cpp. We don't own it — we just need to
// know about it so we can take it down alongside the UEF before
// ~GameWorld runs (otherwise our VEH catches the dtor's normal-flow
// exceptions and they end up in CRASH.log as "real" crashes).
PVOID                        s_vehHandle      = nullptr;
std::atomic<bool>            s_vehRemoved{false};

// Our UEF — last-chance handler for exceptions VEH lets through. We log
// what we caught and let Kenshi's vanilla handler (captured into
// s_vanillaFilter at install time) take it from there. This is purely a
// diagnostic layer; we don't try to "fix" or swallow anything here.
LONG WINAPI OurUnhandledExceptionFilter(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord) {
        const auto code = ep->ExceptionRecord->ExceptionCode;
        const auto addr = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
        // Cheap log — UEF context is restricted, no allocations beyond
        // sprintf into a stack buffer + OutputDebugString.
        char buf[256];
        sprintf_s(buf, sizeof(buf),
            "KMP UEF: caught code=0x%08lX at RIP=0x%016llX — handing to vanilla\n",
            code, static_cast<unsigned long long>(addr));
        OutputDebugStringA(buf);
    }
    if (s_vanillaFilter) {
        return s_vanillaFilter(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void __fastcall Hook_GameWorldDtor(void* gameWorld) {
    // Critical: detach our exception handlers BEFORE running ~GameWorld.
    // The destructor is known to throw; with our handlers attached those
    // throws become "crashes" in our log noise. Restoring the vanilla
    // filter here means any remaining issue gets handled by Kenshi's
    // own crash report path (or no path at all on a normal exit).
    RestoreVanillaUnhandledFilter();

    // Call original. Wrapped in SEH because the destructor's normal-flow
    // exceptions can still surface here even with the filter restored,
    // and we don't want THIS function to AV out — that would mask the
    // "no real crash on exit" signal we're trying to surface.
    if (s_origGameWorldDtor) {
        __try {
            s_origGameWorldDtor(gameWorld);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Swallowed; this is the expected normal-exit case. Logging
            // would race against destructor teardown — skip it.
        }
    }
}

} // namespace

void Install() {
    // Capture whatever filter is currently registered (Kenshi installs
    // its own crash dialog handler during startup) and replace it with
    // ours. We MUST run after Kenshi's main has installed its filter
    // for this to capture the right vanilla behavior.
    s_vanillaFilter = SetUnhandledExceptionFilter(&OurUnhandledExceptionFilter);
    s_installed.store(true, std::memory_order_release);
    spdlog::info("exit_safety: UEF installed — vanilla filter captured at 0x{:X}",
                 reinterpret_cast<uintptr_t>(s_vanillaFilter));
}

// Resolve GameWorld's destructor by reading vtable[0] of the live instance.
// Build-agnostic (works whatever Kenshi version we're on) as long as the
// destructor stays at vtable offset 0 — which it does, KenshiLib documents
// it as "vtable offset = 0x0" for both RootObjectBase and GameWorld.
//
// Returns 0 if the singleton isn't resolved yet, the dereference fails,
// or the vtable[0] entry doesn't look like a code pointer.
uintptr_t ResolveDestructorViaVtable() {
    const uintptr_t slot = game::GetResolvedGameWorld();
    if (!slot) return 0;

    uintptr_t inst = 0;
    __try {
        inst = *reinterpret_cast<volatile uintptr_t*>(slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (inst < 0x10000 || inst > 0x00007FFFFFFFFFFFULL) return 0;

    // Read vtable pointer (first 8 bytes of the object).
    uintptr_t vtable = 0;
    __try {
        vtable = *reinterpret_cast<volatile uintptr_t*>(inst);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (vtable < 0x10000 || vtable > 0x00007FFFFFFFFFFFULL) return 0;

    // vtable[0] = destructor address.
    uintptr_t dtor = 0;
    __try {
        dtor = *reinterpret_cast<volatile uintptr_t*>(vtable);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (dtor < 0x10000 || dtor > 0x00007FFFFFFFFFFFULL) return 0;

    return dtor;
}

void InstallGameWorldDestructorHook() {
    if (!s_installed.load(std::memory_order_acquire)) {
        spdlog::warn("exit_safety: InstallGameWorldDestructorHook called before "
                     "Install() — vanilla filter not captured, exit cleanup will be no-op");
    }

    auto& scanner = Core::Get().GetScanner();
    const uintptr_t base = scanner.GetBase();
    if (!base) {
        spdlog::warn("exit_safety: scanner base unavailable, GameWorld dtor hook skipped");
        return;
    }

    // Try vtable resolution first — works on any Kenshi build that keeps
    // the destructor at vtable offset 0 (KenshiLib documents this is the
    // case from the headers).
    uintptr_t target = ResolveDestructorViaVtable();
    if (target) {
        spdlog::info("exit_safety: GameWorld destructor resolved via vtable at 0x{:X}", target);
    } else {
        // Fallback to the legacy KenshiLib RVA hint. Won't work on 1.0.68
        // (rva_validator confirms) but documented for completeness.
        target = base + RVA_GAMEWORLD_DESTRUCTOR_LEGACY_HINT;
        spdlog::warn("exit_safety: vtable resolution unavailable (singleton not yet "
                     "resolved?) — falling back to legacy KenshiLib RVA hint 0x{:X}. "
                     "If this fails install, the UEF capture alone (Bug 3) is what's "
                     "actually keeping CRASH.log clean on exit.",
                     target);
    }

    auto& hookMgr = HookManager::Get();
    if (!hookMgr.InstallAt("GameWorldDestructor", target,
                           &Hook_GameWorldDtor, &s_origGameWorldDtor)) {
        spdlog::warn("exit_safety: failed to install GameWorld dtor hook at 0x{:X} "
                     "— UEF restore alone (without dtor hook) is still functional but "
                     "fires only when the OS unwinds past Kenshi's vanilla filter",
                     target);
        return;
    }
    spdlog::info("exit_safety: GameWorld dtor hook installed at 0x{:X}", target);
}

void RegisterVectoredHandler(PVOID handle) {
    s_vehHandle = handle;
    // If the caller is clearing (handle == nullptr) we also reset the
    // "removed" flag so a future register/restore cycle works again.
    if (!handle) {
        s_vehRemoved.store(false, std::memory_order_release);
    }
}

void RestoreVanillaUnhandledFilter() {
    // Take down the VEH first. Order matters: if the UEF restore runs
    // first and an exception fires between calls, the vanilla UEF will
    // see it but our VEH would have run before it anyway and logged the
    // "crash" — exactly what we're trying to avoid. Removing VEH first
    // closes that window.
    bool vehExpected = false;
    if (s_vehHandle &&
        s_vehRemoved.compare_exchange_strong(vehExpected, true,
                                             std::memory_order_acq_rel)) {
        if (RemoveVectoredExceptionHandler(s_vehHandle)) {
            OutputDebugStringA("KMP exit_safety: VEH removed\n");
        } else {
            OutputDebugStringA("KMP exit_safety: VEH remove FAILED\n");
        }
        s_vehHandle = nullptr;
    }

    bool expected = false;
    if (!s_filterRestored.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel)) {
        return; // already restored
    }
    if (s_vanillaFilter) {
        SetUnhandledExceptionFilter(s_vanillaFilter);
    }
    OutputDebugStringA("KMP exit_safety: vanilla UEF restored\n");
}

bool IsInstalled() {
    return s_installed.load(std::memory_order_acquire);
}

namespace {
std::atomic<bool> s_dtorHookInstalled{false};
std::atomic<int>  s_dtorTryTickCount{0};
} // namespace

void TryInstallDestructorHookLater() {
    if (s_dtorHookInstalled.load(std::memory_order_acquire)) return;

    // Throttle attempts — try once per ~120 ticks (~2s at 60fps) so we
    // don't pound the singleton-deref code path every frame.
    const int n = s_dtorTryTickCount.fetch_add(1, std::memory_order_relaxed);
    if ((n % 120) != 0) return;

    const uintptr_t target = ResolveDestructorViaVtable();
    if (!target) return; // singleton not ready yet — try again next interval

    auto& hookMgr = HookManager::Get();
    if (!hookMgr.InstallAt("GameWorldDestructor", target,
                           &Hook_GameWorldDtor, &s_origGameWorldDtor)) {
        spdlog::warn("exit_safety: deferred install at 0x{:X} failed — will keep "
                     "retrying every 120 ticks", target);
        return;
    }
    s_dtorHookInstalled.store(true, std::memory_order_release);
    spdlog::info("exit_safety: GameWorld dtor hook installed (deferred path) at 0x{:X} "
                 "after {} ticks", target, n + 1);
}

} // namespace kmp::exit_safety
