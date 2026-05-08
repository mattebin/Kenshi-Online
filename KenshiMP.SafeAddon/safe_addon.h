// =========================================================================
//                          KenshiMP.SafeAddon
// =========================================================================
// Single-class entry point for the clean-room addon.  Lives entirely in
// its own worker thread; the game-side surface is a 2-call interface
// (`Start` + `Stop`) invoked from `dllStartPlugin` / `dllStopPlugin`.
//
// The addon does NOT install any MinHook trampolines, does NOT rewrite
// any function prologues, and does NOT subscribe to any game-side
// callback.  It runs on its own clock and reads game state by walking
// well-known memory offsets, all of which are validated through SEH-
// guarded reads.  A fault in our worker thread is caught locally; the
// rest of the Kenshi process continues running.
//
// Data model
// ----------
// Everything we want to know about the world lives in our own structs
// (`SafeWorldSnapshot`, `SafeEntityState`).  Per-tick we sample game
// memory into these structs.  All cross-process / cross-thread work
// happens against our snapshots — never against live game memory
// directly.  This means our spawn / sync logic can be developed and
// debugged like ordinary application code, without worrying that the
// game's internal layout changes between two reads.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kmp::safe {

// Plain-old-data snapshot of one entity Kenshi has in its world.  All
// fields are populated by reading game memory; nothing in here points
// into the game.  Safe to copy, move, serialise.
struct SafeEntityState {
    uintptr_t   gameAddr = 0;     // address of the Kenshi-side object,
                                  // for re-reading next tick
    std::string name;
    std::string factionName;
    float       posX = 0, posY = 0, posZ = 0;
    bool        isPlayer = false; // player faction membership, derived
                                  // from a SEH-guarded faction read
};

// One frame's view of the world.  Single producer (worker thread),
// single consumer (whoever wants to inspect it through `SnapshotCopy`).
struct SafeWorldSnapshot {
    uint64_t                       frameIndex = 0;
    std::chrono::steady_clock::time_point sampledAt{};
    SafeEntityState                localPlayer;
    std::vector<SafeEntityState>   entities;
};

// Tunables for the worker thread.  Defaulted here for the MVP; will
// move into ClientConfig once the addon is the primary path.
struct SafeAddonOptions {
    // How long the worker sleeps between sample passes.  16 ms ≈ 60 Hz,
    // but the addon's clock is independent of Kenshi's render rate so
    // this does NOT need to match the game's frame time.
    std::chrono::milliseconds sampleInterval{16};
    // Throttle for spdlog "I'm alive" heartbeats.  Without this the
    // worker would log thousands of lines per second.
    std::chrono::milliseconds heartbeatInterval{1000};
    // Maximum entities sampled per tick.  Caps the cost of a sample
    // pass when the world is dense.
    size_t maxEntities = 2048;
};

class SafeAddon {
public:
    static SafeAddon& Get();

    // Boot the addon.  Idempotent — second call is a no-op.  Spawns the
    // worker thread and starts ticking immediately.  Call site:
    // `dllStartPlugin` (the "first call" the game makes).
    void Start();

    // Tear down.  Stops the worker thread, joins it, releases all
    // resources.  Call site: `dllStopPlugin` (the "end call").
    void Stop();

    // Snapshot copy for the rest of our code (e.g. the network sender)
    // to inspect the world without racing the worker thread.  Cheap;
    // the worker double-buffers internally.
    SafeWorldSnapshot SnapshotCopy() const;

    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }

private:
    SafeAddon() = default;
    SafeAddon(const SafeAddon&) = delete;
    SafeAddon& operator=(const SafeAddon&) = delete;

    // Worker entry point.  Runs `OnFrameStart` → sample → `OnFrameEnd`
    // in a loop, sleeping for `sampleInterval` between iterations.
    void WorkerMain();

    // The two callbacks the user described as "first call" and "end
    // call" semantically — but invoked PER-TICK by our own loop, not
    // by the game.  Keeping the names so the architectural intent
    // stays visible in code.
    void OnFrameStart(SafeWorldSnapshot& snap);
    void OnFrameEnd(const SafeWorldSnapshot& snap);

    // Read player position from a known-stable offset chain.  All reads
    // are SEH-guarded; failure returns false and leaves the snapshot's
    // localPlayer at default values.
    bool SamplePlayer(SafeEntityState& outPlayer);

    SafeAddonOptions    m_opts{};
    std::atomic<bool>   m_running{false};
    std::thread         m_worker;
    mutable std::mutex  m_snapMutex;
    SafeWorldSnapshot   m_lastSnap;
};

} // namespace kmp::safe
