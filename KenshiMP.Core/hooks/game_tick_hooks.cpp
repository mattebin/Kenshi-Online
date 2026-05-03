#include "game_tick_hooks.h"
#include "entity_hooks.h"
#include "../core.h"
#include "../game/spawn_manager.h"
#include "../game/game_types.h"
#include "../game/player_controller.h"
#include "kmp/hook_manager.h"
#include <spdlog/spdlog.h>
#include <atomic>
#include <chrono>
#include <Windows.h>

namespace kmp::game_tick_hooks {

// ── DIAGNOSTIC STATE for v1.0.68 time/clock investigation ────────────────
//
// All updates here happen INSIDE the MovRaxRsp detour body. Per the file's
// existing constraint ("NO spdlog inside MovRaxRsp detour — use
// OutputDebugStringA only") we use only atomic primitives and ODS prints.
// The actual rate logging happens from Core::OnGameTick which runs in a
// safe Present-hook context.
namespace {

// Total number of times Hook_GameFrameUpdate has been invoked since DLL load.
// Read by TakeRateSnapshot() to compute calls/sec.
std::atomic<uint64_t>  g_totalCalls{0};

// rcx (this/RCX) of the most recent call. Used to confirm what object
// Kenshi is calling GameFrameUpdate against — and whether it stays
// constant across calls (consistent with a single GameWorld instance).
std::atomic<uintptr_t> g_lastRcx{0};

// Set to false the first time we see g_lastRcx change between calls. If
// stays true after many calls, rcx is a stable singleton pointer.
std::atomic<bool>      g_rcxStable{true};

// One-shot first-fire flag for the safe-context spdlog log line. The
// detour-side ODS print fires from inside the detour; this flag lets
// Core::OnGameTick emit a clean spdlog::info breadcrumb the first time
// it observes a non-zero count.
std::atomic<bool>      g_firstSpdlogPending{false};

// Snapshot bookkeeping for TakeRateSnapshot.
std::atomic<uint64_t>  g_lastSnapshotCalls{0};
std::atomic<int64_t>   g_lastSnapshotSteadyTimeNs{0};
std::atomic<bool>      g_snapshotInitialized{false};

} // namespace

// GameFrameUpdate starts with `mov rax, rsp` (48 8B C4).
// HookManager automatically applies the MovRaxRsp fix: a naked detour captures
// RSP at hook entry, and the trampoline wrapper restores RAX before entering
// the original function body. This ensures correct RBP derivation.
using GameFrameUpdateFn = void(__fastcall*)(void* rcx, void* rdx);

static GameFrameUpdateFn s_originalFn = nullptr; // trampoline — USED for calling
static uintptr_t s_targetAddr = 0;               // real function address (for diagnostics)

// ── SEH wrapper for calling original GameFrameUpdate via TRAMPOLINE ──
static void SEH_CallOriginal(GameFrameUpdateFn trampoline, void* rcx, void* rdx) {
    __try {
        trampoline(rcx, rdx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_crashCount = 0;
        if (++s_crashCount <= 5) {
            char buf[128];
            sprintf_s(buf, "KMP: GameFrameUpdate TRAMPOLINE CRASHED #%d\n", s_crashCount);
            OutputDebugStringA(buf);
        }
    }
}

static std::atomic<int> s_tickCount{0};

static void __fastcall Hook_GameFrameUpdate(void* rcx, void* rdx) {
    int tick = s_tickCount.fetch_add(1) + 1;

    // ── DIAGNOSTIC counters (v1.0.68 time/clock investigation) ──────────
    // Atomic-only updates — no allocations, no spdlog, safe inside the
    // MovRaxRsp detour body. Read out by Core::OnGameTick later.
    {
        const uint64_t total = g_totalCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        const uintptr_t rcxAddr = reinterpret_cast<uintptr_t>(rcx);
        // Latch lastRcx and detect instability (any change from previous
        // non-zero value flips the stable flag false, permanently).
        const uintptr_t prevRcx = g_lastRcx.exchange(rcxAddr, std::memory_order_relaxed);
        if (prevRcx != 0 && prevRcx != rcxAddr) {
            g_rcxStable.store(false, std::memory_order_relaxed);
        }
        if (total == 1) {
            // Mark for the safe-context one-shot spdlog. Core::OnGameTick
            // will see this flag and emit a clean log line from a non-detour
            // context (spdlog is unsafe here).
            g_firstSpdlogPending.store(true, std::memory_order_release);
        }
    }

    // ── DEBUG: Log every step for first 5 ticks ──
    if (tick <= 5) {
        char buf[256];
        sprintf_s(buf, "KMP: GameFrameUpdate ENTER tick #%d rcx=0x%p rdx=0x%p\n", tick, rcx, rdx);
        OutputDebugStringA(buf);
    }

    // ═══ SPAWN DIAGNOSTICS (no spawning here) ═══
    // Spawn fallback is handled ONLY in Core::HandleSpawnQueue (10s timeout).
    // Previously this hook also had a 3s direct spawn fallback, but it raced with
    // the safer in-place replay method in entity_hooks (which needs ~5s to settle
    // after loading burst). By removing the competing 3s spawner, in-place replay
    // gets first crack at the queue before the Core fallback kicks in at 10s.
    {
        auto& core = Core::Get();
        bool connected = core.IsConnected();

        // Log spawn conditions every 3000 ticks (~20 seconds at 150 fps)
        // NO spdlog inside MovRaxRsp detour — use OutputDebugStringA only
        if (tick % 3000 == 0 && connected) {
            auto& spawnMgr = core.GetSpawnManager();
            size_t pendingCount = spawnMgr.GetPendingSpawnCount();
            int inPlaceCount = entity_hooks::GetInPlaceSpawnCount();
            char diagBuf[128];
            sprintf_s(diagBuf, "KMP: tick=%d pending=%zu inPlace=%d\n",
                      tick, pendingCount, inPlaceCount);
            OutputDebugStringA(diagBuf);
        }
    }

    if (tick <= 5) {
        char buf[128];
        sprintf_s(buf, "KMP: GameFrameUpdate tick #%d — about to call original (TRAMPOLINE)\n", tick);
        OutputDebugStringA(buf);
    }

    // Call original via MovRaxRsp trampoline wrapper.
    // The wrapper swaps to the game caller's stack and restores RAX before
    // entering the original function body — correct RBP and stack layout.
    SEH_CallOriginal(s_originalFn, rcx, rdx);

    if (tick <= 5) {
        OutputDebugStringA("KMP: GameFrameUpdate — trampoline returned OK\n");
    }

    // ═══ DEFERRED PROBES — DISABLED ═══
    // AnimClass probing was flooding the log with failures every frame and never
    // succeeding. PlayerControlled probing relies on CharacterIterator which also
    // fails. Both are non-essential optimizations. Disabled to eliminate as crash source.

    if (tick <= 5) {
        char buf[128];
        sprintf_s(buf, "KMP: GameFrameUpdate tick #%d DONE\n", tick);
        OutputDebugStringA(buf);
    }
}

bool Install() {
    auto& core = Core::Get();
    auto& hookMgr = HookManager::Get();
    auto& funcs = core.GetGameFunctions();

    if (!funcs.GameFrameUpdate) {
        spdlog::warn("game_tick_hooks: GameFrameUpdate not found, skipping");
        return false;
    }

    s_targetAddr = reinterpret_cast<uintptr_t>(funcs.GameFrameUpdate);

    OutputDebugStringA("KMP: game_tick_hooks — calling InstallAt...\n");

    if (!hookMgr.InstallAt("GameFrameUpdate",
                            s_targetAddr,
                            &Hook_GameFrameUpdate, &s_originalFn)) {
        spdlog::error("game_tick_hooks: Failed to hook GameFrameUpdate");
        OutputDebugStringA("KMP: game_tick_hooks — InstallAt FAILED\n");
        return false;
    }

    char buf[128];
    sprintf_s(buf, "KMP: game_tick_hooks INSTALLED at 0x%llX\n", (unsigned long long)s_targetAddr);
    OutputDebugStringA(buf);
    spdlog::info("game_tick_hooks: Installed at 0x{:X} (trampoline mode — no thread suspension) "
                 "[DIAGNOSTIC: counting calls to verify GAME_FRAME_UPDATE is the live "
                 "v1.0.68 game-tick path]", s_targetAddr);
    return true;
}

void Uninstall() {
    HookManager::Get().Remove("GameFrameUpdate");
}

// ── Public diagnostic accessors (read-only, safe from any context) ──────

uint64_t  GetTotalCallCount()      { return g_totalCalls.load(std::memory_order_relaxed); }
uintptr_t GetLastObservedRcx()     { return g_lastRcx.load(std::memory_order_relaxed); }
bool      IsRcxStableAcrossCalls() { return g_rcxStable.load(std::memory_order_relaxed); }
uintptr_t GetTargetAddress()       { return s_targetAddr; }
bool      IsInstalled()            { return s_targetAddr != 0 && s_originalFn != nullptr; }

RateSnapshot TakeRateSnapshot() {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    const int64_t nowNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

    const uint64_t total = g_totalCalls.load(std::memory_order_relaxed);

    // Initialise on first snapshot — delta is meaningless.
    if (!g_snapshotInitialized.load(std::memory_order_acquire)) {
        g_lastSnapshotCalls.store(total, std::memory_order_relaxed);
        g_lastSnapshotSteadyTimeNs.store(nowNs, std::memory_order_relaxed);
        g_snapshotInitialized.store(true, std::memory_order_release);
        return RateSnapshot{
            total,
            0,
            0.0,
            g_lastRcx.load(std::memory_order_relaxed),
            g_rcxStable.load(std::memory_order_relaxed),
        };
    }

    const uint64_t prevCalls = g_lastSnapshotCalls.exchange(total, std::memory_order_relaxed);
    const int64_t  prevNs    = g_lastSnapshotSteadyTimeNs.exchange(nowNs, std::memory_order_relaxed);

    return RateSnapshot{
        total,
        total - prevCalls,
        (nowNs - prevNs) / 1e9,
        g_lastRcx.load(std::memory_order_relaxed),
        g_rcxStable.load(std::memory_order_relaxed),
    };
}

} // namespace kmp::game_tick_hooks
