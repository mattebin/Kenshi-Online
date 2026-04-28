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
// ─────────────────────────────────────────────────────────────────────────────

#include <spdlog/spdlog.h>

namespace kmp::watcher {

inline void Mark(const char* category, const char* event) {
    spdlog::info("WATCH/{}: {}", category, event);
    auto logger = spdlog::default_logger();
    if (logger) logger->flush();
}

template <typename... Args>
inline void MarkFmt(const char* category, fmt::format_string<Args...> fmt, Args&&... args) {
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
