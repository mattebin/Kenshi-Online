#include "speed_probe.h"
#include "../game/game_types.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

namespace kmp::speed_probe {

namespace {

// KenshiLib layout (RE_Kenshi/KenshiLib Include/kenshi/GameWorld.h, captured
// against Kenshi 1.0.51). See docs/SPEED_SYNC_LEAD.md.
constexpr std::ptrdiff_t kOff_FrameSpeedMult = 0x700;  // float (1.0.51)
constexpr std::ptrdiff_t kOff_ZoneMgr        = 0x8B0;  // ZoneManager*
constexpr std::ptrdiff_t kOff_DebugFlag      = 0x8B8;  // bool
constexpr std::ptrdiff_t kOff_Paused         = 0x8B9;  // bool
constexpr std::ptrdiff_t kOff_GameResetting  = 0x8BA;  // bool
constexpr std::ptrdiff_t kOff_AudioThread    = 0x8C0;  // AudioSystemGlobal*
constexpr std::ptrdiff_t kOff_SteamEnabled   = 0x4F0;  // bool

// Float-search window: ±0x800 around the documented offset. On 1.0.68 the
// layout shifted from 1.0.51 so the real frameSpeedMult is somewhere in this
// range — we find it by watching for a slot whose value changes.
constexpr std::ptrdiff_t kHuntStart = kOff_FrameSpeedMult - 0x800;  // -0x100
constexpr std::ptrdiff_t kHuntEnd   = kOff_FrameSpeedMult + 0x800;  //  0xF00

// Background thread cadence — 1 ms / 1 kHz, per "fire every milisec".
constexpr auto kPollInterval = std::chrono::milliseconds(1);

// How many "value changed" lines to emit before going quiet. Fast enough to
// catch a speed key press, capped so we don't fill the log if some other
// float in the window is animating every frame.
constexpr int kChangeEmissionCap = 200;

std::atomic<bool>      s_threadStarted{false};
std::atomic<bool>      s_stopRequested{false};
std::atomic<uintptr_t> s_cachedGw{0};
std::atomic<int>       s_emissionsLogged{0};

template <typename T>
bool SafeRead(const void* addr, T& out) {
    __try {
        out = *reinterpret_cast<const T*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsInModule(uintptr_t p, HMODULE mod) {
    if (!mod || !p) return false;
    auto base = reinterpret_cast<uintptr_t>(mod);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    return p >= base && p < base + nt->OptionalHeader.SizeOfImage;
}

// Quick GameWorld signature: vtable in module + zoneMgr / audioThread also
// vtables-in-module + paused is 0 or 1. No frameSpeedMult check (that's what
// we're hunting for, can't gate on it).
bool LooksLikeGameWorld(uintptr_t gw, HMODULE kenshiMod) {
    uintptr_t vt = 0;
    if (!SafeRead(reinterpret_cast<const void*>(gw), vt) || !IsInModule(vt, kenshiMod))
        return false;
    uintptr_t zone = 0, audio = 0;
    if (!SafeRead(reinterpret_cast<const void*>(gw + kOff_ZoneMgr),     zone)
     || !SafeRead(reinterpret_cast<const void*>(gw + kOff_AudioThread), audio))
        return false;
    if (zone) {
        uintptr_t zVt = 0;
        if (!SafeRead(reinterpret_cast<const void*>(zone), zVt) || !IsInModule(zVt, kenshiMod))
            return false;
    }
    if (audio) {
        uintptr_t aVt = 0;
        if (!SafeRead(reinterpret_cast<const void*>(audio), aVt) || !IsInModule(aVt, kenshiMod))
            return false;
    }
    uint8_t paused = 0;
    if (!SafeRead(reinterpret_cast<const void*>(gw + kOff_Paused), paused))
        return false;
    return paused <= 1;
}

uintptr_t ScanForGameWorld(HMODULE kenshiMod) {
    if (!kenshiMod) return 0;
    auto base = reinterpret_cast<uintptr_t>(kenshiMod);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(kenshiMod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        DWORD c = sec->Characteristics;
        if (!(c & IMAGE_SCN_MEM_WRITE) || !(c & IMAGE_SCN_CNT_INITIALIZED_DATA))
            continue;
        uintptr_t start = base + sec->VirtualAddress;
        uintptr_t end   = start + sec->Misc.VirtualSize;
        for (uintptr_t p = (start + 7) & ~uintptr_t(7); p + 8 <= end; p += 8) {
            uintptr_t target = 0;
            if (!SafeRead(reinterpret_cast<const void*>(p), target)) continue;
            if (target == 0 || target < 0x10000ULL || target >= 0x800000000000ULL) continue;
            if (IsInModule(target, kenshiMod)) continue;
            if (LooksLikeGameWorld(target, kenshiMod)) return target;
        }
    }
    return 0;
}

// Snapshot the entire ±0x800 window. Returns size of window in floats so the
// caller can size its previous-state buffer once.
constexpr int kWindowFloats = (kHuntEnd - kHuntStart) / 4;

void SnapshotFloats(uintptr_t gw, float* out) {
    for (int i = 0; i < kWindowFloats; ++i) {
        std::ptrdiff_t off = kHuntStart + i * 4;
        // Skip negative offsets (would read before GameWorld struct).
        if (off < 0 || !SafeRead(reinterpret_cast<const void*>(gw + off), out[i])) {
            out[i] = NAN;
        }
    }
}

bool LooksLikeSpeedValue(float v) {
    return v > 0.04f && v < 10.0f && v == v; // not NaN, in range
}

void ProbeThread() {
    HMODULE kenshiMod = GetModuleHandleA(nullptr);
    spdlog::info("speed_probe: background thread started (poll every {} ms, "
                 "kenshi base=0x{:X}, window=±0x800 around +0x700)",
                 (long)kPollInterval.count(), (uintptr_t)kenshiMod);

    std::vector<float> prev(kWindowFloats, NAN);
    std::vector<float> curr(kWindowFloats, NAN);
    bool havePrev = false;

    while (!s_stopRequested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(kPollInterval);

        // 1. Acquire GameWorld pointer (rescan periodically until found).
        uintptr_t gw = s_cachedGw.load(std::memory_order_acquire);
        if (gw == 0 || !LooksLikeGameWorld(gw, kenshiMod)) {
            uintptr_t found = ScanForGameWorld(kenshiMod);
            if (found != gw) {
                if (found) {
                    spdlog::info("speed_probe: GameWorld discovered at 0x{:X}", found);
                    havePrev = false; // reset diff state on new pointer
                }
                s_cachedGw.store(found, std::memory_order_release);
            }
            gw = found;
            if (!gw) continue;
        }

        // 2. Snapshot the window.
        SnapshotFloats(gw, curr.data());

        // 3. Diff against previous snapshot.
        if (!havePrev) {
            // First time we have a snapshot — log the initial state of any
            // slot that already holds a sane-looking speed value. Otherwise
            // stay quiet until something changes.
            int initialFound = 0;
            for (int i = 0; i < kWindowFloats; ++i) {
                if (LooksLikeSpeedValue(curr[i])) {
                    if (initialFound == 0) {
                        spdlog::info("=== KMP SPEED_PROBE INITIAL @ 0x{:X} ===", gw);
                    }
                    spdlog::info("speed_probe[init]:   +0x{:X}  = {:.4f}",
                                 (long)(kHuntStart + i * 4), curr[i]);
                    ++initialFound;
                }
            }
            if (initialFound) {
                spdlog::info("=== KMP SPEED_PROBE INITIAL END ({} slots) ===", initialFound);
            }
            std::swap(prev, curr);
            havePrev = true;
            continue;
        }

        // Build a list of changed slots (skip NaNs and uninit garbage).
        struct Change { std::ptrdiff_t off; float oldV; float newV; };
        std::vector<Change> changes;
        for (int i = 0; i < kWindowFloats; ++i) {
            float a = prev[i], b = curr[i];
            if (a != a || b != b) continue;          // NaN skip
            if (a == b) continue;                    // unchanged
            // Only report changes between sane speed values OR a transition
            // INTO the sane range. This filters animation timers / counters
            // that take huge values.
            bool aSane = LooksLikeSpeedValue(a);
            bool bSane = LooksLikeSpeedValue(b);
            if (!aSane && !bSane) continue;
            changes.push_back({kHuntStart + i * 4, a, b});
        }

        if (!changes.empty()
            && s_emissionsLogged.fetch_add(1, std::memory_order_relaxed) < kChangeEmissionCap) {
            spdlog::info("=== KMP SPEED_PROBE CHANGE @ 0x{:X} ({} slot(s)) ===",
                         gw, changes.size());
            for (const auto& c : changes) {
                spdlog::info("speed_probe[chg]:   +0x{:X}  {:.4f} -> {:.4f}",
                             (long)c.off, c.oldV, c.newV);
            }
        }
        std::swap(prev, curr);
    }

    spdlog::info("speed_probe: background thread exiting "
                 "(emissions logged: {})",
                 s_emissionsLogged.load(std::memory_order_relaxed));
}

} // namespace

void TickOnce() {
    // Idempotent — only the first call does any setup work.
    if (s_threadStarted.exchange(true)) return;

    char buf[8];
    DWORD len = GetEnvironmentVariableA("KMP_SPEED_PROBE", buf, sizeof(buf));
    if (len == 0) {
        // Not opted in — leave s_threadStarted true so we never re-check.
        return;
    }

    spdlog::info("speed_probe: KMP_SPEED_PROBE set, spawning background "
                 "watcher thread (independent of game state, polls @ 1 kHz).");
    std::thread([] { ProbeThread(); }).detach();
}

} // namespace kmp::speed_probe
