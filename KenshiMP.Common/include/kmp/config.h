#pragma once
#include "constants.h"
#include <string>
#include <vector>
#include <cstdint>

namespace kmp {

struct ClientConfig {
    std::string playerName     = "Player";
    std::string lastServer     = "127.0.0.1";
    uint16_t    lastPort       = KMP_DEFAULT_PORT;
    bool        autoConnect    = false;
    float       overlayScale   = 1.0f;
    std::string masterServer   = "127.0.0.1";        // Master server address
    uint16_t    masterPort     = 27801;               // Master server port
    std::vector<std::string> favoriteServers = {"127.0.0.1:27800"};
    bool        useSyncOrchestrator = false; // New 7-stage sync pipeline (set true to test)

    // ── Experimental/workaround flags ──
    // Each flag below corresponds to an investigation outcome from the
    // coop-stability-2026-04 effort. Defaults reflect what was empirically
    // shown to work on Kenshi v1.0.65 (Steam) and avoid the recurring
    // engine-side null-deref crash. Flipping any of these to the upstream
    // pre-investigation behaviour is one line each — useful when reproducing
    // the issues for further investigation, e.g. with a debugger attached.
    bool        verboseWatchLog                = false; // WATCH/* trace markers
                                                         // for hooks, packets,
                                                         // tick boundaries
    bool        kenshiCrashRecovery            = true;  // VEH redirects rax
                                                         // on the periodic
                                                         // game+0x644365 null
                                                         // deref; pattern is
                                                         // scanned at install
    bool        enableCharacterCreateHook      = false; // RE-DISABLED
                                                         // 2026-05-06 (later
                                                         // run). Enabling it
                                                         // caused
                                                         // back-to-back
                                                         // silent
                                                         // terminations of
                                                         // Kenshi (PIDs
                                                         // 22468, 29744)
                                                         // right after the
                                                         // first
                                                         // post-connect
                                                         // CharacterCreate
                                                         // — `__fastfail`-
                                                         // class
                                                         // termination
                                                         // that bypasses
                                                         // both VEH and
                                                         // `SetUnhandledExceptionFilter`,
                                                         // so we can't
                                                         // intercept it
                                                         // from user mode.
                                                         // Until factory
                                                         // capture has a
                                                         // hook-free
                                                         // alternative
                                                         // (vtable-scan
                                                         // path noted in
                                                         // re_kenshi 2/
                                                         // manual_findings),
                                                         // leave this OFF
                                                         // and accept that
                                                         // the spawn
                                                         // pipeline stays
                                                         // gated on
                                                         // `factory=false`.
                                                         // See
                                                         // KNOWN_ISSUES.md.
    bool        safeModeFirstConnectedCreate   = true;  // companion to the
                                                         // above: when the
                                                         // hook is on, skip
                                                         // capture work for
                                                         // the very first
                                                         // connected create.
    bool        factionSignExtRescue           = true;  // VEH rescue for the
                                                         // recurring AV at a
                                                         // sign-extended
                                                         // Faction* + 0x250
                                                         // (isPlayer field).
                                                         // Detects AV target
                                                         // of form
                                                         // 0xFFFFFFFF<low>,
                                                         // finds the matching
                                                         // GPR with high-bits
                                                         // = 0xFFFFFFFF, masks
                                                         // them off, resumes.
                                                         // See manual_findings/
                                                         // notes/bind_crash_
                                                         // faction_0x250_
                                                         // signext.md.
    bool        useNativeApplyDamage           = false; // When true,
                                                         // PacketHandler::
                                                         // HandleCombatHit
                                                         // calls into the
                                                         // game's resolved
                                                         // ApplyDamage
                                                         // function pointer
                                                         // (RVA 0x7A33A0
                                                         // via pattern
                                                         // scan).  That
                                                         // function's true
                                                         // signature does
                                                         // not match our
                                                         // 6-arg
                                                         // ApplyDamageFn
                                                         // typedef on
                                                         // 1.0.68 — calling
                                                         // it via 6-arg
                                                         // shim
                                                         // corrupts the
                                                         // heap (observed
                                                         // STATUS_HEAP_
                                                         // CORRUPTION
                                                         // 0xC0000374
                                                         // crash 2026-05-
                                                         // 07 in PID
                                                         // 22724).
                                                         // Default false:
                                                         // PacketHandler
                                                         // takes the
                                                         // direct-memory-
                                                         // write health
                                                         // fallback that
                                                         // was already in
                                                         // place for
                                                         // this exact
                                                         // case.  Flip
                                                         // back to true
                                                         // when the
                                                         // correct
                                                         // ApplyDamage
                                                         // RVA + signature
                                                         // are confirmed
                                                         // (master_index
                                                         // suggests
                                                         // MedicalSystem::
                                                         // applyDamage
                                                         // at 0x64F300 is
                                                         // the right
                                                         // target, but
                                                         // its signature
                                                         // is also not
                                                         // 6 ints+floats).

    bool Load(const std::string& path);
    bool Save(const std::string& path) const;

    static std::string GetDefaultPath();     // Shared path (Injector writes here)
    static std::string GetInstancePath();    // PID-specific path (Core saves here)
};

struct ServerConfig {
    std::string serverName   = "KenshiMP Server";
    uint16_t    port         = KMP_DEFAULT_PORT;
    int         maxPlayers   = KMP_MAX_PLAYERS;
    std::string password;
    std::string savePath     = "world.kmpsave";
    int         tickRate     = KMP_TICK_RATE;
    bool        pvpEnabled   = true;
    float       gameSpeed    = 1.0f;
    bool        enablePortForwarding = false; // UPnP/firewall rule for public hosting
    std::string masterServer = "162.248.94.149"; // Master server address
    uint16_t    masterPort   = 27801;            // Master server port

    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
};

} // namespace kmp
