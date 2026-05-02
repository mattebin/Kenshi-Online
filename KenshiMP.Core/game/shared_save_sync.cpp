#include "shared_save_sync.h"
#include "../sys/watcher.h"
#include "game_types.h"
#include "../core.h"
#include "../hooks/char_tracker_hooks.h"
#include "../hooks/ai_hooks.h"
#include "kmp/protocol.h"
#include "kmp/memory.h"
#include <spdlog/spdlog.h>
#include <atomic>
#include <mutex>
#include <chrono>
#include <Windows.h>

namespace kmp::shared_save_sync {

// ═══════════════════════════════════════════════════════════════════════════
//  SHARED-SAVE SYNC
//
//  Both players load the SAME save with kenshi-online.mod.
//  The mod defines "Player 1" and "Player 2" factions + characters.
//  Server assigns each player a faction string:
//    "10-kenshi-online" → you control "Player 1", other player is "Player 2"
//    "12-kenshi-online" → you control "Player 2", other player is "Player 1"
//  Characters already exist in the save — no factory spawning needed.
//  We just find them by name and sync positions.
// ═══════════════════════════════════════════════════════════════════════════

// ── State ──
static std::string s_ownCharName;     // bootstrap label only — see ResolveFactionPtrByName
static std::string s_otherCharName;

static void* s_ownAnimClass = nullptr;
static void* s_otherAnimClass = nullptr;
static void* s_ownCharPtr = nullptr;
static void* s_otherCharPtr = nullptr;

// Pointer-based identity (preferred over name-based — names collide; the
// kenshi-online.mod emits many characters all called "Player 1"). These
// resolve once we observe at least one character with the bootstrap name,
// after which everything matches by faction pointer instead.
static uintptr_t s_ownFactionPtr   = 0;
static uintptr_t s_otherFactionPtr = 0;

static bool s_initialized = false;
static bool s_ownFound = false;
static bool s_otherFound = false;

// Position sending throttle
static auto s_lastPosSend = std::chrono::steady_clock::time_point{};
static constexpr int POS_SEND_INTERVAL_MS = 50; // 20 Hz position updates

// Discovery retry
static auto s_lastDiscoveryLog = std::chrono::steady_clock::time_point{};
static int s_discoveryAttempts = 0;

// Remote position — mutex-protected because OnRemotePositionReceived is called
// from the network thread while Update reads from the game thread.
static std::mutex s_remoteMutex;
static Vec3 s_remotePosition{0, 0, 0};
static bool s_hasRemotePosition = false;
static std::atomic<float> s_remoteGameSpeed{-1.f};

// ── Faction string → character name mapping ──
// Server sends faction strings with the originating mod's load-order prefix
// (e.g. "10-kenshi-online.mod") because that is how Kenshi addresses faction
// records internally. Strip the mod suffix before matching so any reasonable
// variant maps to the same player slot.
static std::string NormalizeFactionKey(const std::string& faction) {
    std::string s = faction;
    auto dot = s.find('.');
    if (dot != std::string::npos) s.resize(dot);
    return s;
}

static std::string FactionToOwnName(const std::string& faction) {
    const std::string s = NormalizeFactionKey(faction);
    if (s == "10-kenshi-online") return "Player 1";
    if (s == "12-kenshi-online") return "Player 2";
    return "";
}

static std::string FactionToOtherName(const std::string& faction) {
    const std::string s = NormalizeFactionKey(faction);
    if (s == "10-kenshi-online") return "Player 2";
    if (s == "12-kenshi-online") return "Player 1";
    return "";
}

void Init() {
    auto& lobby = Core::Get().GetLobbyManager();
    if (!lobby.HasFaction()) {
        spdlog::warn("shared_save_sync: Init — no faction yet (will retry in Update)");
        return;
    }

    std::string faction = lobby.GetFactionString();
    s_ownCharName = FactionToOwnName(faction);
    s_otherCharName = FactionToOtherName(faction);

    if (s_ownCharName.empty() || s_otherCharName.empty()) {
        // Init runs from Update() every tick until s_initialized flips. A
        // hard error here used to flood the log with tens of thousands of
        // identical lines per minute (7.5 MB log in 4 minutes observed in
        // test session 26436). Log only on first occurrence and on every
        // transition (i.e. when the faction string changes).
        static std::string s_lastWarnedFaction;
        if (faction != s_lastWarnedFaction) {
            s_lastWarnedFaction = faction;
            spdlog::error("shared_save_sync: Unknown faction '{}' — cannot determine "
                          "character names (further occurrences of this exact value "
                          "suppressed)", faction);
        }
        return;
    }

    s_initialized = true;
    s_ownFound = false;
    s_otherFound = false;
    s_ownAnimClass = nullptr;
    s_otherAnimClass = nullptr;
    s_ownCharPtr = nullptr;
    s_otherCharPtr = nullptr;
    {
        std::lock_guard lock(s_remoteMutex);
        s_hasRemotePosition = false;
        s_remotePosition = {0, 0, 0};
    }
    s_remoteGameSpeed.store(-1.f);
    s_discoveryAttempts = 0;

    spdlog::info("shared_save_sync: Initialized — own='{}' other='{}'",
                 s_ownCharName, s_otherCharName);
    Core::Get().GetNativeHud().AddSystemMessage(
        "Shared save sync: you are " + s_ownCharName + ", looking for " + s_otherCharName + "...");
}

void Reset() {
    s_initialized = false;
    s_ownFound = false;
    s_otherFound = false;
    s_ownAnimClass = nullptr;
    s_otherAnimClass = nullptr;
    s_ownCharPtr = nullptr;
    s_otherCharPtr = nullptr;
    s_ownCharName.clear();
    s_otherCharName.clear();
    {
        std::lock_guard lock(s_remoteMutex);
        s_hasRemotePosition = false;
    }
    s_remoteGameSpeed.store(-1.f);
    spdlog::info("shared_save_sync: Reset");
}

// ── SEH-protected position read from AnimClass chain ──
static bool SEH_ReadAnimClassPosition(void* animClass, Vec3& out) {
    __try {
        uintptr_t animPtr = reinterpret_cast<uintptr_t>(animClass);
        if (animPtr < 0x10000 || animPtr > 0x00007FFFFFFFFFFF) return false;

        uintptr_t charMovement = 0;
        if (!Memory::Read(animPtr + 0xC0, charMovement) || charMovement == 0) return false;
        if (charMovement < 0x10000 || charMovement > 0x00007FFFFFFFFFFF) return false;

        uintptr_t posStruct = 0;
        if (!Memory::Read(charMovement + 0x320, posStruct) || posStruct == 0) return false;
        if (posStruct < 0x10000 || posStruct > 0x00007FFFFFFFFFFF) return false;

        Memory::Read(posStruct + 0x20, out.x);
        Memory::Read(posStruct + 0x24, out.y);
        Memory::Read(posStruct + 0x28, out.z);

        return (out.x != 0.f || out.y != 0.f || out.z != 0.f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── SEH-protected position read directly from the Character struct ──
// Used as a fallback when the AnimClass chain returns zero — happens during
// the first ~1-2 seconds after world load when animClass is still being
// populated. Borrowed from andperks6/Kenshi-Online (commit 0385189).
static bool SEH_ReadCharacterPosition(void* charPtr, Vec3& out) {
    __try {
        uintptr_t charAddr = reinterpret_cast<uintptr_t>(charPtr);
        if (charAddr < 0x10000 || charAddr > 0x00007FFFFFFFFFFF) return false;

        int posOff = game::GetOffsets().character.position;
        if (posOff < 0) return false;

        Memory::ReadVec3(charAddr + posOff, out.x, out.y, out.z);
        return (out.x != 0.f || out.y != 0.f || out.z != 0.f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── SEH-protected position write to AnimClass chain ──
static bool SEH_WriteAnimClassPosition(void* animClass, const Vec3& pos) {
    __try {
        uintptr_t animPtr = reinterpret_cast<uintptr_t>(animClass);
        if (animPtr < 0x10000 || animPtr > 0x00007FFFFFFFFFFF) return false;

        uintptr_t charMovement = 0;
        if (!Memory::Read(animPtr + 0xC0, charMovement) || charMovement == 0) return false;
        if (charMovement < 0x10000 || charMovement > 0x00007FFFFFFFFFFF) return false;

        uintptr_t posStruct = 0;
        if (!Memory::Read(charMovement + 0x320, posStruct) || posStruct == 0) return false;
        if (posStruct < 0x10000 || posStruct > 0x00007FFFFFFFFFFF) return false;

        Memory::Write(posStruct + 0x20, pos.x);
        Memory::Write(posStruct + 0x24, pos.y);
        Memory::Write(posStruct + 0x28, pos.z);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void SEH_WriteCachedPosition(void* charPtr, const Vec3& pos) {
    if (!charPtr) return;
    __try {
        uintptr_t charAddr = reinterpret_cast<uintptr_t>(charPtr);
        Memory::Write(charAddr + 0x48, pos.x);
        Memory::Write(charAddr + 0x4C, pos.y);
        Memory::Write(charAddr + 0x50, pos.z);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Update(float deltaTime) {
    auto& core = Core::Get();
    if (!core.IsConnected() || !core.IsGameLoaded()) return;

    // Watcher: throttled enter marker so a session log records when
    // shared_save_sync is alive vs. when the truncation happened.
    static int s_updateNum = 0;
    s_updateNum++;
    bool watch = kmp::watcher::IsEnabled() &&
                 (s_updateNum <= 20 || s_updateNum % 200 == 0);
    if (watch) {
        spdlog::info("WATCH/SYNC: Update enter #{} (ownFound={}, otherFound={})",
                     s_updateNum, s_ownFound, s_otherFound);
        spdlog::default_logger()->flush();
    }

    // ── LAZY INIT: faction assignment arrives AFTER SetConnected(true) ──
    // Init() is called from SetConnected but faction isn't assigned yet.
    // Retry here every tick until the faction arrives.
    if (!s_initialized) {
        auto& lobby = core.GetLobbyManager();
        if (lobby.HasFaction()) {
            Init(); // Now the faction is available
        }
        if (!s_initialized) return;
    }

    // ── STEP 1: Discover characters by FACTION POINTER ──
    // Bootstrap: convert the well-known Player 1 / Player 2 *names* into
    // faction *pointers* the first time the tracker has any character with
    // that name. After that, every match is pointer-based — names can
    // collide (kenshi-online.mod has many "Player 1" characters), faction
    // pointers do not.
    if (!s_ownFound || !s_otherFound) {
        s_discoveryAttempts++;

        if (s_ownFactionPtr == 0) {
            uintptr_t fp = char_tracker_hooks::ResolveFactionPtrByName(s_ownCharName);
            if (fp != 0) {
                s_ownFactionPtr = fp;
                spdlog::info("shared_save_sync: Resolved OWN faction pointer 0x{:X} "
                             "(via name '{}')", fp, s_ownCharName);
            }
        }
        if (s_otherFactionPtr == 0) {
            uintptr_t fp = char_tracker_hooks::ResolveFactionPtrByName(s_otherCharName);
            if (fp != 0) {
                s_otherFactionPtr = fp;
                spdlog::info("shared_save_sync: Resolved OTHER faction pointer 0x{:X} "
                             "(via name '{}')", fp, s_otherCharName);
            }
        }

        // OWN selection: prefer the unique-named character in either
        // kenshi-online faction (the user's actual PC, which they
        // custom-named at character creation, sitting alongside ~18
        // placeholder NPCs all literally named "Player 1"). Falls back
        // to faction-only when no unique name is in the world yet.
        if (!s_ownFound && (s_ownFactionPtr != 0 || s_otherFactionPtr != 0)) {
            const char_tracker_hooks::TrackedChar* tc = nullptr;
            const char* matchKind = nullptr;

            if (s_ownFactionPtr != 0) {
                tc = char_tracker_hooks::FindUniqueByFactionPtr(s_ownFactionPtr, s_ownCharName);
                if (tc) matchKind = "unique-name (own faction)";
            }
            if (!tc && s_otherFactionPtr != 0) {
                tc = char_tracker_hooks::FindUniqueByFactionPtr(s_otherFactionPtr, s_otherCharName);
                if (tc) matchKind = "unique-name (other faction — slot mismatch)";
            }
            if (!tc && s_ownFactionPtr != 0) {
                tc = char_tracker_hooks::FindByFactionPtr(s_ownFactionPtr);
                if (tc) matchKind = "faction-only";
            }

            if (tc && tc->animClassPtr) {
                s_ownAnimClass = tc->animClassPtr;
                s_ownCharPtr = tc->characterPtr;
                s_ownFound = true;
                spdlog::info("shared_save_sync: Found OWN '{}' [{}] "
                             "animClass=0x{:X} char=0x{:X} faction=0x{:X}",
                             tc->name, matchKind,
                             reinterpret_cast<uintptr_t>(s_ownAnimClass),
                             reinterpret_cast<uintptr_t>(s_ownCharPtr),
                             tc->factionPtr);
                core.GetNativeHud().AddSystemMessage(
                    "Found your character: " + tc->name + " (" + matchKind + ")");
            }
        }

        if (!s_otherFound && s_ownFactionPtr != 0 && s_otherFactionPtr != 0) {
            // OTHER must be in a *different* faction than OWN. Compute the
            // expected other-faction from OWN's actual faction (now that
            // OWN's resolved we know which side it landed on).
            uintptr_t expectedOtherFaction = s_otherFactionPtr;
            if (s_ownFound && s_ownCharPtr) {
                if (auto* ownTc = char_tracker_hooks::FindByPtr(s_ownCharPtr)) {
                    if (ownTc->factionPtr == s_ownFactionPtr) expectedOtherFaction = s_otherFactionPtr;
                    else if (ownTc->factionPtr == s_otherFactionPtr) expectedOtherFaction = s_ownFactionPtr;
                }
            }

            const char_tracker_hooks::TrackedChar* tc =
                char_tracker_hooks::FindUniqueByFactionPtr(expectedOtherFaction, s_otherCharName);
            const char* matchKind = "unique-name";
            if (!tc) {
                tc = char_tracker_hooks::FindByFactionPtr(expectedOtherFaction);
                matchKind = "faction-only";
            }
            if (tc && tc->animClassPtr) {
                s_otherAnimClass = tc->animClassPtr;
                s_otherCharPtr = tc->characterPtr;
                s_otherFound = true;

                if (s_otherCharPtr) {
                    ai_hooks::MarkRemoteControlled(s_otherCharPtr);
                }

                spdlog::info("shared_save_sync: Found OTHER '{}' [{}] "
                             "animClass=0x{:X} char=0x{:X} faction=0x{:X}",
                             tc->name, matchKind,
                             reinterpret_cast<uintptr_t>(s_otherAnimClass),
                             reinterpret_cast<uintptr_t>(s_otherCharPtr),
                             tc->factionPtr);
                core.GetNativeHud().AddSystemMessage(
                    "Found remote player: " + tc->name + " (" + matchKind + ")");
            }
        }

        // Periodic status log
        auto now = std::chrono::steady_clock::now();
        auto sinceLog = std::chrono::duration_cast<std::chrono::seconds>(now - s_lastDiscoveryLog);
        if (sinceLog.count() >= 5) {
            s_lastDiscoveryLog = now;
            if (!s_ownFound || !s_otherFound) {
                core.GetNativeHud().AddSystemMessage(
                    "Looking for characters... (tracked: " +
                    std::to_string(char_tracker_hooks::GetTrackedCount()) + ")");
            }
        }

        if (!s_ownFound || !s_otherFound) return;

        core.GetNativeHud().AddSystemMessage("Both players found! Position sync active.");
        spdlog::info("shared_save_sync: BOTH CHARACTERS FOUND — sync active");
    } else {
        // Re-validate AnimClass pointers periodically — char_tracker may
        // have re-keyed entries across a zone load, but the *character*
        // pointer and the identity (faction) are stable. Look up by the
        // cached character pointer (NOT by name — name-based lookup would
        // match an NPC placeholder and silently swap s_ownAnimClass to a
        // stationary 'Player N' NPC, which produced a 4150-packet stuck-
        // coordinates trail in test session 22004 of the prior branch).
        static int s_revalidateCounter = 0;
        if (++s_revalidateCounter % 300 == 0) { // ~5 seconds at 60 fps
            if (s_ownCharPtr) {
                auto* tc = char_tracker_hooks::FindByPtr(s_ownCharPtr);
                if (tc && tc->animClassPtr != s_ownAnimClass) {
                    s_ownAnimClass = tc->animClassPtr;
                    spdlog::debug("shared_save_sync: Own animClass refreshed to 0x{:X}",
                                  reinterpret_cast<uintptr_t>(s_ownAnimClass));
                }
            }
            if (s_otherCharPtr) {
                auto* tc2 = char_tracker_hooks::FindByPtr(s_otherCharPtr);
                if (tc2 && tc2->animClassPtr != s_otherAnimClass) {
                    s_otherAnimClass = tc2->animClassPtr;
                    if (s_otherCharPtr) ai_hooks::MarkRemoteControlled(s_otherCharPtr);
                    spdlog::debug("shared_save_sync: Other animClass refreshed to 0x{:X}",
                                  reinterpret_cast<uintptr_t>(s_otherAnimClass));
                }
            }
        }
    }

    // ── STEP 2: Read own position and send to server ──
    // Uses the EXISTING C2S_PositionUpdate format that the server already handles.
    // The server stores position on the ConnectedPlayer and broadcasts via
    // S2C_PositionUpdate to other clients.
    auto now = std::chrono::steady_clock::now();
    auto sinceSend = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastPosSend);
    if (sinceSend.count() >= POS_SEND_INTERVAL_MS && s_ownAnimClass) {
        s_lastPosSend = now;

        Vec3 myPos;
        // Prefer character-direct read (works on frame 1); fall back to
        // AnimClass chain when the character position field is itself zero.
        bool gotPos = false;
        if (s_ownCharPtr) {
            gotPos = SEH_ReadCharacterPosition(s_ownCharPtr, myPos);
        }
        if (!gotPos) {
            gotPos = SEH_ReadAnimClassPosition(s_ownAnimClass, myPos);
        }
        if (gotPos) {
            // Use the existing position update format — the server reads:
            // U32(sourcePlayer) [handled by server from peer], U8(count), then
            // CharacterPosition structs. We need to match this EXACTLY.
            PacketWriter writer;
            writer.WriteHeader(MessageType::C2S_PositionUpdate);
            // The server reads sourcePlayer as U32 first, but the canonical client
            // code (core.cpp PollLocalPositions) writes U8(count) first, then
            // CharacterPosition structs. Let me match the canonical format.
            writer.WriteU8(1); // count = 1 (FIX: was U32, must be U8)

            // CharacterPosition struct — must match the server's ReadRaw size
            CharacterPosition cp{};
            cp.entityId = 0; // Shared-save mode uses entityId 0 as "player avatar"
            cp.posX = myPos.x;
            cp.posY = myPos.y;
            cp.posZ = myPos.z;
            cp.compressedQuat = 0;
            cp.animStateId = 0;
            cp.moveSpeed = 0;
            cp.flags = 0;
            writer.WriteRaw(&cp, sizeof(cp));

            core.GetClient().SendUnreliable(writer.Data(), writer.Size());

            // Watcher: throttled outbound position log. SendUnreliable
            // doesn't log per-call (correctly — would flood at 20 Hz).
            // Surfaces here in 1/50 form so a verbose session shows the
            // movement trajectory.
            static int s_posSendCount = 0;
            int n = ++s_posSendCount;
            if (kmp::watcher::IsEnabled() && (n <= 5 || n % 50 == 0)) {
                spdlog::info("WATCH/POS: sent #{} pos=({:.1f},{:.1f},{:.1f}) "
                             "from animClass=0x{:X}",
                             n, myPos.x, myPos.y, myPos.z,
                             reinterpret_cast<uintptr_t>(s_ownAnimClass));
                spdlog::default_logger()->flush();
            }
        }
    }

    // ── STEP 3: Write received position to other player's character ──
    {
        Vec3 remotePos;
        bool hasRemote;
        {
            std::lock_guard lock(s_remoteMutex);
            remotePos = s_remotePosition;
            hasRemote = s_hasRemotePosition;
        }
        if (hasRemote && s_otherAnimClass) {
            SEH_WriteAnimClassPosition(s_otherAnimClass, remotePos);
            SEH_WriteCachedPosition(s_otherCharPtr, remotePos);
        }
    }

    // ── STEP 4: Game speed sync ──
    float speed = s_remoteGameSpeed.load();
    if (speed >= 0.f) {
        uintptr_t gwSingleton = core.GetGameFunctions().GameWorldSingleton;
        if (gwSingleton != 0) {
            game::GameWorldAccessor gw(gwSingleton);
            if (gw.IsValid()) {
                float currentSpeed = gw.GetGameSpeed();
                if (std::abs(currentSpeed - speed) > 0.01f) {
                    gw.WriteGameSpeed(speed);
                }
            }
        }
        s_remoteGameSpeed.store(-1.f);
    }
}

void OnRemotePositionReceived(const Vec3& pos) {
    std::lock_guard lock(s_remoteMutex);
    s_remotePosition = pos;
    s_hasRemotePosition = true;
}

void OnRemoteGameSpeedReceived(float speed) {
    s_remoteGameSpeed.store(speed);
}

bool IsOwnCharacterFound() { return s_ownFound; }
bool IsOtherCharacterFound() { return s_otherFound; }
const std::string& GetOwnCharacterName() { return s_ownCharName; }
const std::string& GetOtherCharacterName() { return s_otherCharName; }

} // namespace kmp::shared_save_sync
