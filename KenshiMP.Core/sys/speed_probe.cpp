#include "speed_probe.h"
#include "../game/game_types.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <vector>

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

// Walk every section in kenshi_x64.exe that contains writable initialized
// data (.data, .CRT, occasionally a custom section) and look for any 8-byte
// aligned slot that holds a pointer to something with the GameWorld signature
// (vtable in module + sane frameSpeedMult + zoneMgr/audioThread point to
// vtables in module). Returns up to 8 best candidates by score, deduped by
// target gw.
std::vector<Candidate> ScanDataForGameWorld(HMODULE kenshiMod) {
    std::vector<Candidate> out;
    if (!kenshiMod) return out;

    auto base = reinterpret_cast<uintptr_t>(kenshiMod);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(kenshiMod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return out;
    auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return out;

    auto* sec = IMAGE_FIRST_SECTION(nt);
    int seen = 0;
    int hits = 0;
    constexpr int kHitCap = 16;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        // Only writable initialized data sections; skip .text/.rdata/.pdata.
        DWORD c = sec->Characteristics;
        bool writable    = (c & IMAGE_SCN_MEM_WRITE) != 0;
        bool initialized = (c & IMAGE_SCN_CNT_INITIALIZED_DATA) != 0;
        if (!writable || !initialized) continue;

        uintptr_t secStart = base + sec->VirtualAddress;
        uintptr_t secEnd   = secStart + sec->Misc.VirtualSize;
        // Walk 8-byte aligned slots.
        for (uintptr_t p = (secStart + 7) & ~uintptr_t(7); p + 8 <= secEnd; p += 8) {
            uintptr_t target = 0;
            if (!SafeRead(reinterpret_cast<const void*>(p), target)) continue;
            ++seen;
            // Heap-ish range: typical user-mode allocs sit in 0x000001'00000000+
            // and below 0x00007FFF'FFFFFFFF. We exclude anything inside the
            // module itself (vtables/static data) which can't be a GameWorld
            // heap object.
            if (target == 0) continue;
            if (target < 0x000010000ULL) continue;
            if (target >= 0x00800000000000ULL) continue;
            if (IsInModule(target, kenshiMod)) continue;

            Candidate c{};
            if (!ProbeAt(target, kenshiMod, c)) continue;
            if (c.score < 3) continue; // filter noise
            // Dedup by target gw.
            bool dup = false;
            for (const auto& e : out) if (e.gw == c.gw) { dup = true; break; }
            if (dup) continue;
            out.push_back(c);
            ++hits;
            if (hits >= kHitCap) {
                spdlog::info("speed_probe: scan hit cap ({}), stopping early "
                             "(seen={} pointers)", kHitCap, seen);
                return out;
            }
        }
    }
    spdlog::info("speed_probe: scan complete (examined {} 8-byte slots, kept {})",
                 seen, hits);
    return out;
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

    HMODULE kenshiMod = GetModuleHandleA(nullptr);
    uintptr_t slot = game::GetResolvedGameWorld();

    spdlog::info("=== KMP SPEED_PROBE BEGIN ===");
    spdlog::info("speed_probe: existing resolver slot = 0x{:X}", slot);

    Candidate caseA{};
    Candidate caseB{};

    if (slot != 0) {
        // Case A — slot holds a pointer to the GameWorld struct.
        uintptr_t derefed = 0;
        if (SafeRead(reinterpret_cast<const void*>(slot), derefed) && derefed != 0
            && ProbeAt(derefed, kenshiMod, caseA)) {
            EmitLog("A:slot->ptr->gw", caseA);
        } else {
            spdlog::info("speed_probe[A:slot->ptr->gw]: failed (deref=0x{:X})", derefed);
        }
        // Case B — slot IS the GameWorld struct.
        if (ProbeAt(slot, kenshiMod, caseB)) {
            EmitLog("B:slot==gw", caseB);
        } else {
            spdlog::info("speed_probe[B:slot==gw]: read failed");
        }
    } else {
        spdlog::info("speed_probe: existing resolver returned 0 — falling back "
                     "to .data section scan.");
    }

    // Always scan: finds the real GameWorld even when the existing resolver
    // failed, and lets us cross-check Case A/B when it didn't.
    spdlog::info("speed_probe: scanning kenshi_x64.exe .data for GameWorld "
                 "candidates (this is read-only and one-shot)...");
    auto scanCandidates = ScanDataForGameWorld(kenshiMod);
    spdlog::info("speed_probe: scan found {} candidate(s)", scanCandidates.size());
    int idx = 0;
    Candidate bestScan{};
    for (const auto& sc : scanCandidates) {
        char label[32];
        wsprintfA(label, "C%d:scan", idx++);
        EmitLog(label, sc);
        if (sc.score > bestScan.score) bestScan = sc;
    }

    // Verdict — explicit so the next person reading doesn't squint at scores.
    auto winner = [](const Candidate& c) { return c.gw && c.score >= 4; };
    if (winner(caseA)) {
        spdlog::info("speed_probe: VERDICT — Case A. *(uintptr_t*)0x{:X} → "
                     "+0x700 for speed.", slot);
    } else if (winner(caseB)) {
        spdlog::info("speed_probe: VERDICT — Case B. 0x{:X} + 0x700 for speed.", slot);
    } else if (winner(bestScan)) {
        spdlog::info("speed_probe: VERDICT — scan winner. GameWorld at 0x{:X} "
                     "(score {}/5). Read 0x{:X}+0x700 for frameSpeedMult.",
                     bestScan.gw, bestScan.score, bestScan.gw);
    } else {
        spdlog::warn("speed_probe: VERDICT — no candidate scored >=4. Best: "
                     "A={}/5 B={}/5 scan={}/5. Layout may have shifted.",
                     caseA.score, caseB.score, bestScan.score);
    }
    spdlog::info("=== KMP SPEED_PROBE END ===");

    s_logged.store(true, std::memory_order_release);
}

} // namespace kmp::speed_probe
