#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Watcher — coarse, flush-forced trace markers for "where were we when Kenshi
// died" debugging. Unlike spdlog::debug/info from inside hot loops, every call
// here is followed by an explicit logger flush so the breadcrumb cannot be
// lost in a buffered write when the process is terminated outside our
// exception coverage (which is the failure mode we're chasing).
//
// Usage:
//   KMP_WATCH("HOOK", "FactionRelation enter");
//   KMP_WATCH_FMT("PKT", "type={} size={}", type, size);
//
// The category is a short uppercase tag ("HOOK", "TICK", "PKT", "SYNC", ...)
// so a grep of `WATCH/HOOK` etc. surfaces just one subsystem at a time.
//
// Gating: every emit checks `IsEnabled()` first (an inline atomic load).
// The flag defaults OFF so production users get a clean log; flip
// `verboseWatchLog: true` in client.json (or call SetEnabled(true) from
// code) to turn the firehose on.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <spdlog/spdlog.h>

namespace kmp::watcher {

// Process-global enable flag. Set by Core::Initialize after loading the
// client config. Inline atomic load on every emit — cheap enough for the
// hot paths the watcher instruments (50 Hz position broadcast, per-packet
// dispatch, etc.) without measurably affecting framerate.
inline std::atomic<bool> g_enabled{false};

inline bool IsEnabled() noexcept {
    return g_enabled.load(std::memory_order_relaxed);
}

inline void SetEnabled(bool e) noexcept {
    g_enabled.store(e, std::memory_order_relaxed);
}

inline void Mark(const char* category, const char* event) {
    if (!IsEnabled()) return;
    spdlog::info("WATCH/{}: {}", category, event);
    auto logger = spdlog::default_logger();
    if (logger) logger->flush();
}

template <typename... Args>
inline void MarkFmt(const char* category, fmt::format_string<Args...> fmt, Args&&... args) {
    if (!IsEnabled()) return;
    spdlog::info(std::string("WATCH/") + category + ": " + fmt::format(fmt, std::forward<Args>(args)...));
    auto logger = spdlog::default_logger();
    if (logger) logger->flush();
}

} // namespace kmp::watcher

#define KMP_WATCH(cat, evt) ::kmp::watcher::Mark(cat, evt)
// Variadic format flavour. Pass a category string then a fmt::format pattern.
// Note: cannot be used inside __try/__except blocks (fmt::format constructs
// std::string). Use plain KMP_WATCH there.
#define KMP_WATCH_FMT(cat, ...) \
    ::kmp::watcher::MarkFmt(cat, __VA_ARGS__)
