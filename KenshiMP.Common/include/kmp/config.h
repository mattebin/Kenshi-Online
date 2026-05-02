#pragma once
#include "constants.h"
#include <string>
#include <vector>
#include <cstdint>

namespace kmp {

struct ClientConfig {
    std::string playerName     = "Player";
    std::string lastServer     = "162.248.94.149";
    uint16_t    lastPort       = KMP_DEFAULT_PORT;
    bool        autoConnect    = true;
    float       overlayScale   = 1.0f;
    std::string masterServer   = "162.248.94.149";   // Master server address
    uint16_t    masterPort     = 27801;               // Master server port
    std::vector<std::string> favoriteServers = {"162.248.94.149:27800"};
    bool        useSyncOrchestrator = false; // New 7-stage sync pipeline (set true to test)

    // When true, an in-process VEH handler catches the recurring engine
    // null-deref at a pattern-discovered RVA (instruction signature
    // movss xmm0,[rax+0x90]; mulss xmm0,[rax+0x34]) and resumes execution
    // by redirecting RAX to a static zero buffer. Default ON because the
    // bug is observed across Kenshi versions and the fault is fatal
    // otherwise. Set to false when investigating the root cause with a
    // debugger so the fault propagates normally.
    bool        kenshiCrashRecovery = true;

    // When true, every WATCH/* trace marker emits a flush-forced spdlog
    // entry. Useful for debugging the moment of a silent termination —
    // each marker is flushed before the next instruction so the log can
    // never be lost in a buffered write. Off by default because the
    // markers fire at packet/tick rates (50 Hz+) and would bloat the log
    // file under normal play.
    bool        verboseWatchLog = false;

    // Maximum number of remote-player characters that can spawn into one
    // local Kenshi instance via the NPC-hijack mechanism in entity_hooks.
    // Capped at 4 in the upstream v1.0.2 stability pass — too low for
    // 16-player co-op, since vanilla Kenshi squads regularly run to 8+
    // characters per player. With the upstream cap, only the first 4 of
    // each remote player's squad would actually appear; the rest stay
    // queued forever as the spawn manager retries to find an NPC slot.
    // Vanilla squad cap is 30; defaulting to 32 leaves a safety margin
    // and lets typical squads work as designed. Configurable so the cap
    // can be tuned downward on memory-constrained hosts if needed.
    int         maxSpawnsPerPlayer = 32;

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
