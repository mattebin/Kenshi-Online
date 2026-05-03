#include "speed_probe.h"
#include "../game/game_types.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace kmp::speed_probe {

namespace {

// KenshiLib layout (RE_Kenshi/KenshiLib Include/kenshi/GameWorld.h, captured
// against Kenshi 1.0.51). See docs/SPEED_SYNC_LEAD.md for the full table.
constexpr std::ptrdiff_t kOff_FrameSpeedMult     = 0x700;  // float
constexpr std::ptrdiff_t kOff_TimeStamper        = 0x8A0;  // SimpleTimeStamper
constexpr std::ptrdiff_t kOff_ZoneMgr            = 0x8B0;  // ZoneManager*  (sanity)
constexpr std::ptrdiff_t kOff_DebugFlag          = 0x8B8;  // bool
constexpr std::ptrdiff_t kOff_Paused             = 0x8B9;  // bool
constexpr std::ptrdiff_t kOff_GameResetting      = 0x8BA;  // bool
constexpr std::ptrdiff_t kOff_AudioThread        = 0x8C0;  // AudioSystemGlobal*
constexpr std::ptrdiff_t kOff_SteamEnabled       = 0x4F0;  // bool

std::atomic<bool> s_logged{false};

// Read with a SEH guard; returns false on access violation.
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
    uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
    return p >= base && p < end;
}

// Returns true when frameSpeedMult is in a sane range and the surrounding
// pointers look like real engine objects.
struct Candidate {
    uintptr_t gw;
    float     frameSpeedMult;
    bool      paused;
    bool      gameResetting;
    bool      debugFlag;
    bool      steamEnabled;
    uintptr_t zoneMgr;
    uintptr_t audioThread;
    uintptr_t vtable;
    bool      vtableInModule;
    bool      zoneInModule;
    bool      audioInModule;
    bool      speedSane;
    int       score; // 0..5, see scoring below
};

bool ProbeAt(uintptr_t gw, HMODULE kenshiMod, Candidate& out) {
    out.gw = gw;
    if (!gw) return false;

    if (!SafeRead(reinterpret_cast<const void*>(gw), out.vtable))           return false;
    if (!SafeRead(reinterpret_cast<const void*>(gw + kOff_FrameSpeedMult),
                  out.frameSpeedMult))                                       return false;
    if (!SafeRead(reinterpret_cast<const void*>(gw + kOff_Paused),
                  out.paused))                                               return false;
    if (!SafeRead(reinterpret_cast<const void*>(gw + kOff_GameResetting),
                  out.gameResetting))                                        return false;
    if (!SafeRead(reinterpret_cast<const void*>(gw + kOff_DebugFlag),
                  out.debugFlag))                                            return false;
    if (!SafeRead(reinterpret_cast<const void*>(gw + kOff_SteamEnabled),
                  out.steamEnabled))                                         return false;
    if (!SafeRead(reinterpret_cast<const void*>(gw + kOff_ZoneMgr),
                  out.zoneMgr))                                              return false;
    if (!SafeRead(reinterpret_cast<const void*>(gw + kOff_AudioThread),
                  out.audioThread))                                          return false;

    out.vtableInModule = IsInModule(out.vtable, kenshiMod);
    out.zoneInModule   = false;
    out.audioInModule  = false;
    if (out.zoneMgr) {
        uintptr_t zoneVt = 0;
        if (SafeRead(reinterpret_cast<const void*>(out.zoneMgr), zoneVt))
            out.zoneInModule = IsInModule(zoneVt, kenshiMod);
    }
    if (out.audioThread) {
        uintptr_t audioVt = 0;
        if (SafeRead(reinterpret_cast<const void*>(out.audioThread), audioVt))
            out.audioInModule = IsInModule(audioVt, kenshiMod);
    }

    out.speedSane =
        out.frameSpeedMult > 0.04f && out.frameSpeedMult < 10.0f;

    out.score =
        (out.vtableInModule  ? 1 : 0) +
        (out.zoneInModule    ? 1 : 0) +
        (out.audioInModule   ? 1 : 0) +
        (out.speedSane       ? 1 : 0) +
        (out.paused == 0 || out.paused == 1 ? 1 : 0);
    return true;
}

void EmitLog(const char* label, const Candidate& c) {
    spdlog::info(
        "speed_probe[{}]: gw=0x{:X} vt=0x{:X}{} score={}/5 "
        "frameSpeedMult={:.4f}({}) paused={} gameResetting={} "
        "debugFlag={} steamEnabled={} zoneMgr=0x{:X}{} audioThread=0x{:X}{}",
        label, c.gw, c.vtable, c.vtableInModule ? "(in-mod)" : "(off-mod)",
        c.score,
        c.frameSpeedMult, c.speedSane ? "sane" : "OUT-OF-RANGE",
        c.paused ? "true" : "false",
        c.gameResetting ? "true" : "false",
        c.debugFlag ? "true" : "false",
        c.steamEnabled ? "true" : "false",
        c.zoneMgr, c.zoneInModule ? "(in-mod)" : "(off-mod)",
        c.audioThread, c.audioInModule ? "(in-mod)" : "(off-mod)");
}

} // namespace

void TickOnce() {
    if (s_logged.load(std::memory_order_acquire)) return;

    char buf[8];
    DWORD len = GetEnvironmentVariableA("KMP_SPEED_PROBE", buf, sizeof(buf));
    if (len == 0) return; // not opted in

    uintptr_t slot = game::GetResolvedGameWorld();
    if (slot == 0) return; // wait until core resolves it

    HMODULE kenshiMod = GetModuleHandleA(nullptr);

    spdlog::info("=== KMP SPEED_PROBE BEGIN ===");
    spdlog::info("speed_probe: GameWorld slot = 0x{:X}", slot);

    // Case A — slot holds a pointer to the GameWorld struct (RE_Kenshi
    // pattern: a global like `GameWorld* g_gw`).
    Candidate caseA{};
    uintptr_t derefed = 0;
    if (SafeRead(reinterpret_cast<const void*>(slot), derefed) && derefed != 0
        && ProbeAt(derefed, kenshiMod, caseA)) {
        EmitLog("A:slot->ptr->gw", caseA);
    } else {
        spdlog::info("speed_probe[A:slot->ptr->gw]: dereference failed or null "
                     "(deref=0x{:X})", derefed);
    }

    // Case B — slot IS the GameWorld struct (static singleton inlined into
    // .data, e.g. `static GameWorld g_gw;`).
    Candidate caseB{};
    if (ProbeAt(slot, kenshiMod, caseB)) {
        EmitLog("B:slot==gw", caseB);
    } else {
        spdlog::info("speed_probe[B:slot==gw]: read failed at slot");
    }

    // Suggest the winner so the next person reading the log doesn't have to
    // squint at the score columns.
    if (caseA.gw && caseA.score >= 4) {
        spdlog::info("speed_probe: VERDICT — Case A wins (slot is a pointer to "
                     "GameWorld). Use *(uintptr_t*)0x{:X} → +0x700 for speed.",
                     slot);
    } else if (caseB.gw && caseB.score >= 4) {
        spdlog::info("speed_probe: VERDICT — Case B wins (slot IS GameWorld). "
                     "Use 0x{:X} + 0x700 for speed.", slot);
    } else {
        spdlog::warn(
            "speed_probe: VERDICT — neither case scored ≥4. "
            "Either the resolver pointed at the wrong global, or the GameWorld "
            "layout shifted between Kenshi 1.0.51 and 1.0.68. Highest scoring: "
            "A={}/5 B={}/5", caseA.score, caseB.score);
    }
    spdlog::info("=== KMP SPEED_PROBE END ===");

    s_logged.store(true, std::memory_order_release);
}

} // namespace kmp::speed_probe
