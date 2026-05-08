#include "log_aggregator.h"
#include <Shlwapi.h>
#include <algorithm>

namespace kmp::dash {

void LogAggregator::EnsureStream(const std::wstring& filename,
                                  const std::string& prefix) {
    std::wstring full = m_dir + L"\\" + filename;
    if (!PathFileExistsW(full.c_str())) return;
    for (auto& s : m_streams) if (s.path == full) return;

    StreamState s;
    s.path = full;
    s.prefix = prefix;
    s.handle = CreateFileW(full.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s.handle == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    GetFileSizeEx(s.handle, &size);
    s.offset.QuadPart = size.QuadPart;  // tail-style: skip past existing
    SetFilePointerEx(s.handle, s.offset, nullptr, FILE_BEGIN);
    m_streams.push_back(std::move(s));
}

void LogAggregator::RebindClientStream() {
    // Find newest KenshiOnline_<pid>.log in the watch dir; re-bind
    // the [client] stream to it if it differs from what we have.
    WIN32_FIND_DATAW fd{};
    std::wstring pat = m_dir + L"\\KenshiOnline_*.log";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::wstring newest;
    FILETIME newestTime{};
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        // Skip the named special-case files which already have their
        // own dedicated streams below.
        std::wstring name = fd.cFileName;
        if (name == L"KenshiOnline_Server.log"     ||
            name == L"KenshiOnline_Master.log"     ||
            name == L"KenshiOnline_CRASH.log"      ||
            name == L"KenshiOnline_Cartographer.log"||
            name == L"KenshiOnline_SafeAddon.log"  ||
            name == L"KenshiOnline_Probe.log") continue;
        if (CompareFileTime(&fd.ftLastWriteTime, &newestTime) > 0) {
            newestTime = fd.ftLastWriteTime;
            newest = m_dir + L"\\" + name;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (newest.empty() || newest == m_currentClientPath) return;

    // Close any existing [client] stream and replace.
    for (auto it = m_streams.begin(); it != m_streams.end();) {
        if (it->prefix == "[client]") {
            if (it->handle != INVALID_HANDLE_VALUE) CloseHandle(it->handle);
            it = m_streams.erase(it);
        } else ++it;
    }
    m_currentClientPath = newest;
    StreamState s;
    s.path = newest;
    s.prefix = "[client]";
    s.handle = CreateFileW(newest.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s.handle == INVALID_HANDLE_VALUE) return;
    // Start at top of new client log so the user sees the session boot.
    s.offset.QuadPart = 0;
    SetFilePointerEx(s.handle, s.offset, nullptr, FILE_BEGIN);
    m_streams.push_back(std::move(s));
}

void LogAggregator::PumpStream(StreamState& s) {
    if (s.handle == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(s.handle, &size)) return;
    if (size.QuadPart < s.offset.QuadPart) {
        // truncated — restart from the top
        s.offset.QuadPart = 0;
        SetFilePointerEx(s.handle, s.offset, nullptr, FILE_BEGIN);
        s.partial.clear();
    }
    while (s.offset.QuadPart < size.QuadPart) {
        char buf[4096]; DWORD got = 0;
        DWORD toRead = (DWORD)std::min<long long>(
            sizeof(buf), size.QuadPart - s.offset.QuadPart);
        if (!ReadFile(s.handle, buf, toRead, &got, nullptr) || got == 0) break;
        s.offset.QuadPart += got;
        s.partial.append(buf, got);

        size_t start = 0;
        while (true) {
            size_t nl = s.partial.find('\n', start);
            if (nl == std::string::npos) break;
            std::string line = s.partial.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            start = nl + 1;
            if (line.empty()) continue;

            AggregatedLine ag;
            ag.source = s.prefix;
            ag.content = std::move(line);
            ag.seen = std::chrono::system_clock::now();
            {
                std::lock_guard lk(m_bufMutex);
                m_buffer.push_back(std::move(ag));
                if (m_buffer.size() > kMaxBuffer) m_buffer.pop_front();
            }
            m_totalIngested.fetch_add(1);
        }
        s.partial.erase(0, start);
    }
}

void LogAggregator::Tick() {
    if (m_dir.empty()) return;
    EnsureStream(L"KenshiOnline_Server.log",       "[server]");
    EnsureStream(L"KenshiMP_Master.log",           "[master]");
    EnsureStream(L"KenshiOnline_Master.log",       "[master]");
    EnsureStream(L"KenshiOnline_CRASH.log",        "[CRASH]");
    EnsureStream(L"KenshiOnline_Cartographer.log", "[carto]");
    EnsureStream(L"KenshiOnline_SafeAddon.log",    "[safe]");
    EnsureStream(L"KenshiOnline_Probe.log",        "[probe]");
    RebindClientStream();
    for (auto& s : m_streams) PumpStream(s);
}

std::vector<AggregatedLine> LogAggregator::Recent(size_t n) const {
    std::lock_guard lk(m_bufMutex);
    if (m_buffer.size() <= n) {
        return std::vector<AggregatedLine>(m_buffer.begin(), m_buffer.end());
    }
    return std::vector<AggregatedLine>(m_buffer.end() - n, m_buffer.end());
}

} // namespace kmp::dash
