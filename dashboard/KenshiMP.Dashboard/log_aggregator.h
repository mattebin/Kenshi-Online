// log_aggregator — keeps the last N lines of every Kenshi-online log
// file in memory for the Dashboard's "Logs" panel.
//
// Reads files lazily on each refresh tick.  We re-open files instead
// of holding handles open so the writers (game DLL, servers) can
// rotate / truncate them without our locks getting in the way.
#pragma once
#include <Windows.h>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace kmp::dash {

struct AggregatedLine {
    std::string source;   // [client], [server], [master], [safe], etc.
    std::string content;  // raw line, unescaped
    std::chrono::system_clock::time_point seen;
};

class LogAggregator {
public:
    void SetWatchDir(const std::wstring& dir) { m_dir = dir; }

    // Pull any new bytes from the watched files and append to the
    // shared buffer.  Caller invokes this from a UI timer.
    void Tick();

    // Snapshot of the most recent N lines (newest last).  Cheap; we
    // hold the buffer under mutex but copy a small contiguous range.
    std::vector<AggregatedLine> Recent(size_t n) const;

    // Total number of lines ever ingested.  Used as a "did anything
    // change since last paint" hint — the UI re-renders only when
    // this counter advances.
    uint64_t TotalIngested() const { return m_totalIngested.load(); }

private:
    struct StreamState {
        std::wstring path;
        std::string  prefix;     // e.g. "[client]"
        HANDLE       handle = INVALID_HANDLE_VALUE;
        LARGE_INTEGER offset{};
        std::string  partial;    // bytes seen but no newline yet
    };

    void EnsureStream(const std::wstring& filename, const std::string& prefix);
    void RebindClientStream();   // points [client] at newest log
    void PumpStream(StreamState& s);

    std::wstring                  m_dir;
    std::vector<StreamState>      m_streams;
    std::wstring                  m_currentClientPath;
    mutable std::mutex            m_bufMutex;
    std::deque<AggregatedLine>    m_buffer;
    static constexpr size_t       kMaxBuffer = 5000;
    std::atomic<uint64_t>         m_totalIngested{0};
};

} // namespace kmp::dash
