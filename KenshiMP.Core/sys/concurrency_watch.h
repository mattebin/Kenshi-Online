#pragma once
//
// concurrency_watch — detect unsafe concurrent reentry into a hook body.
// Race conditions and reentrancy bugs are notoriously hard to bisect after
// the fact; this catches them at the moment of collision so the log shows
// exactly which hook had which two threads inside it simultaneously.
//
// Detection strategy:
//   * Per-hook atomic depth counter, incremented on entry, decremented on exit
//     (RAII helper guarantees the decrement even on exception).
//   * Entry also sticks the current thread ID into a small fixed-size set.
//   * If depth > 1 at entry: log warn + thread IDs (multi-thread collision).
//   * If same thread enters twice without exiting: log warn (reentrancy —
//     usually fine for re-entrant code paths, but worth flagging because it
//     often indicates the hook called something that called the hook again
//     via a different code path).
//
// Usage:
//
//   void __fastcall Hook_AICreate(void* a, void* b, ...) {
//       KMP_CONCURRENCY_GUARD("AICreate");   // RAII — bumps counter
//       s_origAICreate(a, b, ...);
//   }
//
// Cheap: one atomic add + one TID write on entry, one atomic sub on exit.
// Doesn't slow hot-path hooks measurably.

#include <atomic>
#include <cstdint>

namespace kmp::concurrency_watch {

// Per-hook state. Stored as static in each call site via a tag-string lookup
// the first time. Or — simpler, faster — declared inline at each call site
// with a unique static. We use the inline-static approach in the macro.
struct PerHookState {
    std::atomic<int>      depth{0};       // Concurrent entries.
    std::atomic<uint32_t> firstTid{0};    // First thread that entered.
    std::atomic<uint32_t> collisionTid{0};// Thread that triggered the warning.
    std::atomic<uint64_t> totalEntries{0};
    std::atomic<uint64_t> warnedCount{0};
    const char*           name = "";
};

// RAII guard. Constructor bumps depth and emits a warn-level log if depth
// already had a thread inside. Destructor decrements.
class Guard {
public:
    Guard(PerHookState& state, const char* hookName);
    ~Guard();
private:
    PerHookState& m_state;
};

// Print a one-shot summary of every guard's totalEntries / warnedCount.
// Called from install_audit::Emit so the bug-report block surfaces any
// hooks that have ever seen a concurrent collision.
void EmitSummary();

} // namespace kmp::concurrency_watch

// One unique static state per call site, named after the tag. Caller still
// supplies the name string so logs are readable.
#define KMP_CONCURRENCY_GUARD(tag)                                         \
    static ::kmp::concurrency_watch::PerHookState __kmp_cw_state_##__LINE__; \
    ::kmp::concurrency_watch::Guard __kmp_cw_guard_##__LINE__(             \
        __kmp_cw_state_##__LINE__, tag)
