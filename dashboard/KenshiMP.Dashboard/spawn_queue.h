// =========================================================================
//                         spawn_queue (Dashboard side)
// =========================================================================
// The spawn pipeline as it lives in the v2 architecture.
//
// Why this lives outside the game
// -------------------------------
// In v1 the spawn manager lived inside Kenshi's address space (KenshiMP.
// Core/game/spawn_manager.{h,cpp}) — every queued spawn ran through
// MinHook trampolines, MovRaxRsp wrappers, SEH-guarded factory probes,
// and a CharacterCreate hook whose prologue patch was the source of the
// recurring `__fastfail` terminations.
//
// In v2 the queue itself, the priority logic, the template lookups,
// the deduplication, and every retry/timeout policy lives in this
// out-of-game module.  The ONLY thing that crosses into the game's
// address space is a single RPC over the named pipe:
//
//     { "op": "spawn", "template": "...", "x": .., "y": .., "z": ..,
//       "factionId": .. } → { "op": "spawnResult", "ok": ..,
//                              "entityAddr": "0x..." }
//
// The in-game shim's job is exactly:  receive the message → invoke
// `RootObjectFactory::createRandomCharacter` → return the resulting
// pointer.  Nothing more.  All policy lives here.  All bugs in the
// queue or its retry logic are fixable without touching the game DLL.
//
// Phase 1 status
// --------------
// The queue is implemented in full but the RPC dispatch is currently
// a stub that logs the request and pretends it succeeded.  When
// the named-pipe channel lands in phase 2, the only change required
// is to swap `StubDispatch` for a real `IpcDispatch` — every other
// part of the module stays.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace kmp::dash {

struct SpawnRequest {
    uint64_t    netId       = 0;     // server-assigned entity id
    std::string templateName;        // e.g. "Wanderer", "Player 1"
    float       x = 0, y = 0, z = 0;
    uint32_t    factionId   = 0;     // faction slot index
    int         attempts    = 0;     // retry counter
    std::chrono::steady_clock::time_point queuedAt{};
};

struct SpawnResult {
    bool        ok            = false;
    uint64_t    netId         = 0;
    uintptr_t   entityAddr    = 0;
    std::string failureReason;   // non-empty when ok=false
};

// The dispatcher contract: how the queue gets a SpawnRequest into the
// game's address space and back.  Phase 1 has only `StubDispatcher`;
// Phase 2 adds `IpcDispatcher` that funnels the request through the
// SafeAddon named pipe.
struct ISpawnDispatcher {
    virtual ~ISpawnDispatcher() = default;
    // Synchronous send.  Implementations may block briefly waiting for
    // the SafeAddon to reply.  Returning false means "not connected"
    // — the queue will retry on the next tick.
    virtual bool Dispatch(const SpawnRequest& req, SpawnResult& out) = 0;
    // Cheap "is the in-game shim reachable" check, used by the
    // dashboard's status panel.  Phase 1 always returns false.
    virtual bool IsConnected() const = 0;
};

// Does nothing real — logs requests and returns ok=false with a
// "stub dispatcher" reason.  Default until phase 2 wires the IPC.
class StubDispatcher : public ISpawnDispatcher {
public:
    bool Dispatch(const SpawnRequest& req, SpawnResult& out) override;
    bool IsConnected() const override { return false; }
};

class SpawnQueue {
public:
    explicit SpawnQueue(ISpawnDispatcher& disp) : m_disp(disp) {}

    // Add a request to the queue.  Thread-safe; called from the
    // network receive thread when the server announces an entity.
    void Submit(SpawnRequest req);

    // Drain pending requests.  Called from the dashboard's UI tick
    // (currently 1 Hz; bump to higher rate when IPC arrives).  Hands
    // each request to the dispatcher; on success, the request is
    // marked done; on failure, retried up to `kMaxAttempts` times
    // before being dropped with a logged warning.
    void DrainTick();

    // Inspection helpers for the status panel.
    size_t PendingCount() const;
    size_t CompletedCount() const { return m_completed.load(); }
    size_t FailedCount() const { return m_failed.load(); }

    // Reset on disconnect — when the server connection drops we want
    // a clean slate rather than firing stale spawns at reconnect.
    void Clear();

private:
    static constexpr int kMaxAttempts = 5;

    ISpawnDispatcher&         m_disp;
    mutable std::mutex        m_mu;
    std::deque<SpawnRequest>  m_q;
    std::atomic<size_t>       m_completed{0};
    std::atomic<size_t>       m_failed{0};
};

} // namespace kmp::dash
