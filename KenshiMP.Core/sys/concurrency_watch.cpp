#include "concurrency_watch.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <vector>
#include <mutex>

namespace kmp::concurrency_watch {

namespace {

// Global registry of every PerHookState that's been used. Populated lazily
// from each Guard ctor. Used only by EmitSummary — performance doesn't
// matter on the registry path; correctness does.
std::mutex                    g_registryMutex;
std::vector<PerHookState*>    g_registry;

void RegisterIfNew(PerHookState* state) {
    std::lock_guard lk(g_registryMutex);
    for (auto* s : g_registry) {
        if (s == state) return;
    }
    g_registry.push_back(state);
}

} // namespace

Guard::Guard(PerHookState& state, const char* hookName) : m_state(state) {
    if (state.name == nullptr || state.name[0] == '\0') {
        state.name = hookName ? hookName : "<unnamed>";
        RegisterIfNew(&state);
    }

    state.totalEntries.fetch_add(1, std::memory_order_relaxed);

    const uint32_t myTid = GetCurrentThreadId();
    const int depth = state.depth.fetch_add(1, std::memory_order_acq_rel) + 1;

    if (depth == 1) {
        // First entry — record the thread for later collision comparison.
        state.firstTid.store(myTid, std::memory_order_release);
    } else {
        // Already inside. Either reentrancy (same thread) or a real
        // multi-thread collision. Warn at most once per state to avoid
        // log flooding under sustained collisions.
        const uint32_t firstTid = state.firstTid.load(std::memory_order_acquire);
        const uint64_t prevWarn = state.warnedCount.fetch_add(1, std::memory_order_relaxed);
        if (prevWarn == 0) {
            if (firstTid == myTid) {
                spdlog::warn("concurrency_watch[{}]: REENTRANT — same thread "
                             "(tid={}) entered hook depth={} (likely the hook "
                             "called something that called the hook again)",
                             state.name, myTid, depth);
            } else {
                spdlog::warn("concurrency_watch[{}]: COLLISION — thread {} "
                             "entered while thread {} is still inside (depth={}). "
                             "Hook is likely not thread-safe.",
                             state.name, myTid, firstTid, depth);
                state.collisionTid.store(myTid, std::memory_order_release);
            }
        }
    }
}

Guard::~Guard() {
    m_state.depth.fetch_sub(1, std::memory_order_acq_rel);
}

void EmitSummary() {
    std::lock_guard lk(g_registryMutex);
    if (g_registry.empty()) return;

    spdlog::info("=== KMP CONCURRENCY WATCH SUMMARY ===");
    for (auto* s : g_registry) {
        if (!s) continue;
        const auto entries = s->totalEntries.load(std::memory_order_relaxed);
        const auto warned  = s->warnedCount.load(std::memory_order_relaxed);
        const auto first   = s->firstTid.load(std::memory_order_relaxed);
        const auto coll    = s->collisionTid.load(std::memory_order_relaxed);
        if (warned > 0) {
            spdlog::warn("  [{}] entries={} collisions/reentries={} firstTid={} collidingTid={}",
                         s->name, entries, warned, first, coll);
        } else {
            spdlog::info("  [{}] entries={} (no concurrent reentry observed)",
                         s->name, entries);
        }
    }
    spdlog::info("=== END CONCURRENCY WATCH SUMMARY ===");
}

} // namespace kmp::concurrency_watch
