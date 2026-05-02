#include "leak_watch.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <Psapi.h>
#include <chrono>
#include <mutex>
#include <vector>
#include <atomic>

#pragma comment(lib, "Psapi.lib")

namespace kmp::leak_watch {

namespace {

struct RegisteredCollection {
    std::string                  name;
    std::function<size_t()>      sizeGetter;
};

struct MemorySnapshot {
    std::chrono::steady_clock::time_point t;
    size_t                                workingSet = 0;
    size_t                                privateBytes = 0;
    std::vector<std::pair<std::string, size_t>> collectionSizes;
};

std::mutex                          g_mutex;
std::vector<RegisteredCollection>   g_registered;
std::vector<MemorySnapshot>         g_history;
std::atomic<int>                    g_intervalSeconds{300};   // 5 min default
std::chrono::steady_clock::time_point g_lastSnapshot{};
bool                                g_haveBaseline = false;
MemorySnapshot                      g_baseline;

bool QueryProcessMemory(size_t& workingSet, size_t& privateBytes) {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                              sizeof(pmc))) {
        return false;
    }
    workingSet   = pmc.WorkingSetSize;
    privateBytes = pmc.PrivateUsage;
    return true;
}

MemorySnapshot CaptureLocked() {
    MemorySnapshot snap;
    snap.t = std::chrono::steady_clock::now();
    QueryProcessMemory(snap.workingSet, snap.privateBytes);
    snap.collectionSizes.reserve(g_registered.size());
    for (auto& reg : g_registered) {
        size_t sz = 0;
        try {
            sz = reg.sizeGetter ? reg.sizeGetter() : 0;
        } catch (...) {
            sz = static_cast<size_t>(-1); // sentinel for getter throw
        }
        snap.collectionSizes.emplace_back(reg.name, sz);
    }
    return snap;
}

const char* HumanBytes(size_t n, char* buf, size_t bufSize) {
    if (n >= (size_t)1024 * 1024 * 1024) {
        sprintf_s(buf, bufSize, "%.2f GiB", n / 1073741824.0);
    } else if (n >= 1024 * 1024) {
        sprintf_s(buf, bufSize, "%.2f MiB", n / 1048576.0);
    } else if (n >= 1024) {
        sprintf_s(buf, bufSize, "%.2f KiB", n / 1024.0);
    } else {
        sprintf_s(buf, bufSize, "%zu B", n);
    }
    return buf;
}

void EmitOneSnapshot(const MemorySnapshot& snap, const MemorySnapshot* baseline) {
    char wsBuf[32], pbBuf[32], wsDelta[32], pbDelta[32];
    HumanBytes(snap.workingSet, wsBuf, sizeof(wsBuf));
    HumanBytes(snap.privateBytes, pbBuf, sizeof(pbBuf));

    if (baseline) {
        intptr_t wsd = static_cast<intptr_t>(snap.workingSet) -
                       static_cast<intptr_t>(baseline->workingSet);
        intptr_t pbd = static_cast<intptr_t>(snap.privateBytes) -
                       static_cast<intptr_t>(baseline->privateBytes);
        sprintf_s(wsDelta, sizeof(wsDelta), "%s%lld B",
                  wsd >= 0 ? "+" : "", static_cast<long long>(wsd));
        sprintf_s(pbDelta, sizeof(pbDelta), "%s%lld B",
                  pbd >= 0 ? "+" : "", static_cast<long long>(pbd));
        spdlog::info("leak_watch: workingSet={} ({}) privateBytes={} ({})",
                     wsBuf, wsDelta, pbBuf, pbDelta);
    } else {
        spdlog::info("leak_watch: workingSet={} privateBytes={} (baseline)",
                     wsBuf, pbBuf);
    }

    for (auto& [name, sz] : snap.collectionSizes) {
        if (sz == static_cast<size_t>(-1)) {
            spdlog::warn("leak_watch:   collection '{}' threw in size getter", name);
        } else {
            spdlog::info("leak_watch:   collection '{}' size={}", name, sz);
        }
    }
}

} // namespace

void RegisterSize(const std::string& name, std::function<size_t()> sizeGetter) {
    std::lock_guard lk(g_mutex);
    g_registered.push_back({name, std::move(sizeGetter)});
}

void SetIntervalSeconds(int seconds) {
    g_intervalSeconds.store(seconds, std::memory_order_relaxed);
}

void SnapshotNow() {
    std::lock_guard lk(g_mutex);
    auto snap = CaptureLocked();
    if (!g_haveBaseline) {
        g_baseline     = snap;
        g_haveBaseline = true;
        EmitOneSnapshot(snap, nullptr);
    } else {
        EmitOneSnapshot(snap, &g_baseline);
    }
    g_history.push_back(std::move(snap));

    // Cap history at 64 entries (~5 hours at default interval) to bound
    // memory; we only need recent data points to detect monotonic growth.
    if (g_history.size() > 64) {
        g_history.erase(g_history.begin(),
                        g_history.begin() + (g_history.size() - 64));
    }
    g_lastSnapshot = std::chrono::steady_clock::now();
}

void Tick() {
    const int interval = g_intervalSeconds.load(std::memory_order_relaxed);
    if (interval <= 0) return;

    const auto now = std::chrono::steady_clock::now();
    if (g_haveBaseline) {
        auto since = std::chrono::duration_cast<std::chrono::seconds>(
            now - g_lastSnapshot).count();
        if (since < interval) return;
    }
    SnapshotNow();
}

void EmitSummary() {
    std::lock_guard lk(g_mutex);
    if (g_history.empty() && g_registered.empty()) return;

    spdlog::info("=== KMP LEAK WATCH SUMMARY ===");

    // Final snapshot.
    auto finalSnap = CaptureLocked();
    EmitOneSnapshot(finalSnap, g_haveBaseline ? &g_baseline : nullptr);

    // Trend across recorded history — any collection whose size increased
    // monotonically across at least 3 snapshots is suspect.
    if (g_history.size() >= 3) {
        spdlog::info("leak_watch: trend across {} snapshots:", g_history.size());
        for (size_t i = 0; i < g_registered.size(); ++i) {
            const auto& name = g_registered[i].name;
            bool monotonicGrowth = true;
            size_t prev = 0;
            int    samples = 0;
            for (auto& snap : g_history) {
                if (i >= snap.collectionSizes.size()) continue;
                size_t cur = snap.collectionSizes[i].second;
                if (samples > 0 && cur < prev) { monotonicGrowth = false; break; }
                prev = cur;
                samples++;
            }
            if (samples >= 3 && monotonicGrowth && prev > 0) {
                spdlog::warn("leak_watch: collection '{}' is monotonically growing — "
                             "final size {}",
                             name, prev);
            }
        }
    }
    spdlog::info("=== END LEAK WATCH SUMMARY ===");
}

} // namespace kmp::leak_watch
