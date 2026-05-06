// KenshiMP.LogTail
// =================
// Live multi-stream tail viewer for Kenshi-Online.
//
// Why this exists
// ---------------
// During development the user has to flip between three log files in two
// different directories to understand what just happened: the in-process
// client log (`KenshiOnline_<pid>.log`), the dedicated server log
// (`KenshiOnline_Server.log`), and the master server log
// (`KenshiMP_Master.log`).  Plus the crash log (`KenshiOnline_CRASH.log`).
//
// LogTail watches all of them in one console window, prefixes each line with
// its source, and color-codes by severity.  Both `KenshiMP.Injector.exe`
// (when launching the game) and `KenshiMP.Server.exe` (when starting) spawn
// LogTail automatically — first one to start wins, the others detect an
// existing instance via a single mutex and exit silently.
//
// The viewer is process-isolated: when Kenshi or the server dies, LogTail
// keeps streaming until the user closes its window.  The last 200 lines of
// each log stay visible on the user's screen while they read what
// happened, instead of disappearing into a closed Kenshi process.
//
// No external deps — pure Win32, single translation unit, builds in
// seconds.  Lives next to the rest of the build artifacts so the injector
// can locate it via `GetModuleFileName(NULL)` + `..\KenshiMP.LogTail.exe`.

#include <Windows.h>
#include <Shlwapi.h>
#include <shellapi.h>     // CommandLineToArgvW
#include <stdio.h>
#include <string>
#include <vector>
#include <unordered_map>

// ── Colour helpers ──
// FOREGROUND_* constants picked to be readable on the default console
// background.  We never fall through to the user's existing palette so
// the output is visually consistent across Win10 / Win11 / classic
// console host.
namespace col {
    static constexpr WORD GRAY    = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    static constexpr WORD WHITE   = GRAY | FOREGROUND_INTENSITY;
    static constexpr WORD CYAN    = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    static constexpr WORD YELLOW  = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    static constexpr WORD RED     = FOREGROUND_RED | FOREGROUND_INTENSITY;
    static constexpr WORD GREEN   = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    static constexpr WORD MAGENTA = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
}

static HANDLE g_console = INVALID_HANDLE_VALUE;

static void writeColored(const std::string& s, WORD attr) {
    SetConsoleTextAttribute(g_console, attr);
    DWORD written = 0;
    WriteFile(g_console, s.data(), (DWORD)s.size(), &written, nullptr);
    SetConsoleTextAttribute(g_console, col::GRAY);
}

// ── Tailed-file state ──
// One TailedFile per log we watch.  We hold the file open with
// FILE_SHARE_READ | FILE_SHARE_WRITE so a different process — Kenshi,
// the server, or our own crash dumper — can keep appending while we read.
struct TailedFile {
    std::wstring path;
    std::string  prefix;     // e.g. "[client]"
    WORD         prefixColor;
    HANDLE       handle = INVALID_HANDLE_VALUE;
    LARGE_INTEGER offset{};  // current read position
    std::string  partial;    // bytes we've read but no newline yet
    FILETIME     lastWrite{}; // last seen write time, used to detect
                              // truncation/rotation when the underlying
                              // file shrinks (test runs roll new logs)
};

static bool openTailed(TailedFile& tf, bool seekToEnd) {
    if (tf.handle != INVALID_HANDLE_VALUE) return true;
    tf.handle = CreateFileW(
        tf.path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (tf.handle == INVALID_HANDLE_VALUE) return false;

    // On first open, jump to end so we only show new lines (matches `tail
    // -f` UX).  On reopen after rotation we start from offset 0.
    LARGE_INTEGER size{};
    GetFileSizeEx(tf.handle, &size);
    tf.offset.QuadPart = seekToEnd ? size.QuadPart : 0;
    SetFilePointerEx(tf.handle, tf.offset, nullptr, FILE_BEGIN);

    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(tf.handle, &info)) {
        tf.lastWrite = info.ftLastWriteTime;
    }
    return true;
}

static void closeTailed(TailedFile& tf) {
    if (tf.handle != INVALID_HANDLE_VALUE) {
        CloseHandle(tf.handle);
        tf.handle = INVALID_HANDLE_VALUE;
    }
    tf.offset.QuadPart = 0;
    tf.partial.clear();
}

// Pick a colour for a line based on its severity tag.  spdlog writes
// `[level]` somewhere in the line; we substring-search for the common
// ones.  Falls through to the file's own prefix colour for "info" lines.
static WORD colorForLine(const std::string& line, WORD defaultAttr) {
    if (line.find("[error]")     != std::string::npos) return col::RED;
    if (line.find("[critical]")  != std::string::npos) return col::RED;
    if (line.find("[warning]")   != std::string::npos) return col::YELLOW;
    if (line.find("[debug]")     != std::string::npos) return col::GRAY;
    if (line.find("[trace]")     != std::string::npos) return col::GRAY;
    if (line.find("KMP RECOVER") != std::string::npos) return col::GREEN;
    if (line.find("KMP RESCUE")  != std::string::npos) return col::GREEN;
    if (line.find("KMP VEH CRASH") != std::string::npos) return col::MAGENTA;
    if (line.find("UNHANDLED")   != std::string::npos) return col::MAGENTA;
    return defaultAttr;
}

// Read whatever new bytes are available, split on \n, and write each
// complete line to the console with the file's prefix.
static void pumpFile(TailedFile& tf) {
    if (tf.handle == INVALID_HANDLE_VALUE) {
        if (!openTailed(tf, /*seekToEnd=*/false)) return;
    }

    // Detect log rotation: when a new test starts the old log may stay
    // and a new one is created with a new name, but in some flows the
    // server is restarted and it truncates its log to zero.  If our
    // current offset exceeds the current file size, reopen from start.
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(tf.handle, &size)) {
        closeTailed(tf);
        return;
    }
    if (size.QuadPart < tf.offset.QuadPart) {
        // File was truncated.  Start over.
        closeTailed(tf);
        if (!openTailed(tf, /*seekToEnd=*/false)) return;
        GetFileSizeEx(tf.handle, &size);
    }

    while (tf.offset.QuadPart < size.QuadPart) {
        char buf[4096];
        DWORD toRead = (DWORD)std::min<long long>(
            sizeof(buf), size.QuadPart - tf.offset.QuadPart);
        DWORD got = 0;
        if (!ReadFile(tf.handle, buf, toRead, &got, nullptr) || got == 0) {
            break;
        }
        tf.offset.QuadPart += got;
        tf.partial.append(buf, got);

        // Emit each complete line we now have.
        size_t start = 0;
        while (true) {
            size_t nl = tf.partial.find('\n', start);
            if (nl == std::string::npos) break;
            std::string line = tf.partial.substr(start, nl - start);
            // strip CR
            if (!line.empty() && line.back() == '\r') line.pop_back();
            start = nl + 1;

            // Skip empty lines from the prefix path, otherwise the
            // console fills with blank rows during quiet periods.
            if (line.empty()) continue;

            std::string prefixed = tf.prefix + " " + line + "\n";
            writeColored(prefixed, colorForLine(line, tf.prefixColor));
        }
        tf.partial.erase(0, start);
    }
}

// Find the most-recently-modified file in `dir` whose name matches
// `prefix*`.  Used to follow `KenshiOnline_<pid>.log` even though the
// PID changes every launch — we want the freshest one.
static std::wstring newestMatching(const std::wstring& dir,
                                   const std::wstring& prefix,
                                   const std::wstring& suffix) {
    WIN32_FIND_DATAW fd{};
    std::wstring pattern = dir + L"\\" + prefix + L"*" + suffix;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring best;
    FILETIME bestTime{};
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0) {
            bestTime = fd.ftLastWriteTime;
            best = dir + L"\\" + fd.cFileName;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

static std::wstring resolveKenshiDir() {
    // Priority order:
    //  1. argv[1] if user passed an explicit dir
    //  2. KENSHI_DIR env var
    //  3. Steam default install
    //  4. The directory the LogTail .exe lives in (works because the
    //     injector copies LogTail into the Kenshi folder)
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring fromArg;
    if (argv && argc >= 2) fromArg = argv[1];
    if (argv) LocalFree(argv);
    if (!fromArg.empty() && PathFileExistsW(fromArg.c_str())) return fromArg;

    wchar_t env[1024]{};
    if (GetEnvironmentVariableW(L"KENSHI_DIR", env, 1024) > 0) {
        if (PathFileExistsW(env)) return env;
    }

    static const wchar_t* steam =
        L"C:\\SteamLibrary\\steamapps\\common\\Kenshi";
    if (PathFileExistsW(steam)) return steam;

    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    PathRemoveFileSpecW(self);
    return self;
}

int main() {
    // Single-instance guard — first invocation wins.  When the injector
    // and the server both spawn us, only the earliest survives; the
    // others quietly exit.  Mutex name is process-global (no Local\
    // prefix) so it works across user sessions.
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Global\\KenshiMP.LogTail");
    if (mtx == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    SetConsoleTitleW(L"KenshiMP — Live Logs");
    g_console = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(g_console, col::GRAY);

    // Header
    writeColored(
        "KenshiMP LogTail — live tail of client/server/master logs.\n"
        "Close this window to stop tailing; the game and server keep running.\n"
        "\n",
        col::WHITE);

    std::wstring dir = resolveKenshiDir();
    {
        char dbuf[MAX_PATH * 2]{};
        WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, dbuf, sizeof(dbuf),
                            nullptr, nullptr);
        std::string msg = std::string("Watching: ") + dbuf + "\n\n";
        writeColored(msg, col::CYAN);
    }

    std::vector<TailedFile> files;
    {
        TailedFile f;
        f.path = L""; // resolved dynamically per tick to follow newest pid
        f.prefix = "[client]";
        f.prefixColor = col::WHITE;
        files.push_back(f);
    }
    {
        TailedFile f;
        f.path = dir + L"\\KenshiOnline_Server.log";
        f.prefix = "[server]";
        f.prefixColor = col::CYAN;
        files.push_back(f);
    }
    {
        // Master server log filename has flipped between two names across
        // the project history.  Try both; whichever exists wins on first
        // open.
        TailedFile f;
        f.path = dir + L"\\KenshiMP_Master.log";
        if (!PathFileExistsW(f.path.c_str())) {
            f.path = dir + L"\\KenshiOnline_Master.log";
        }
        f.prefix = "[master]";
        f.prefixColor = col::MAGENTA;
        files.push_back(f);
    }
    {
        TailedFile f;
        f.path = dir + L"\\KenshiOnline_CRASH.log";
        f.prefix = "[CRASH]";
        f.prefixColor = col::MAGENTA;
        files.push_back(f);
    }
    {
        // Cartographer streams its function-safety classifier verdicts
        // here.  When the user runs KenshiMP.Cartographer.exe the LogTail
        // window starts showing per-row [GOOD/BAD] decisions live.
        TailedFile f;
        f.path = dir + L"\\KenshiOnline_Cartographer.log";
        f.prefix = "[carto]";
        f.prefixColor = col::GREEN;
        files.push_back(f);
    }
    {
        // SafeAddon (out-of-instrumentation Ogre plugin) heartbeat.
        TailedFile f;
        f.path = dir + L"\\KenshiOnline_SafeAddon.log";
        f.prefix = "[safe]";
        f.prefixColor = col::GREEN;
        files.push_back(f);
    }
    {
        // Probe (out-of-process memory validator) results.  Crash and
        // instrumentation streams have their own colours; the probe
        // gets cyan so a column scan distinguishes it at a glance.
        TailedFile f;
        f.path = dir + L"\\KenshiOnline_Probe.log";
        f.prefix = "[probe]";
        f.prefixColor = col::CYAN;
        files.push_back(f);
    }

    // Loop forever.  100ms tick is responsive enough to feel "live" but
    // doesn't peg a CPU when no one is logging.  Each file is pumped
    // independently so a slow producer can't block a fast one.
    std::wstring lastClientPath;
    while (true) {
        // Refresh the [client] file binding to whatever's newest.
        std::wstring fresh = newestMatching(dir, L"KenshiOnline_", L".log");
        // Filter out the special log filenames we already track.
        if (!fresh.empty()) {
            const wchar_t* suffix = PathFindFileNameW(fresh.c_str());
            std::wstring fname = suffix ? suffix : L"";
            if (fname == L"KenshiOnline_Server.log"
             || fname == L"KenshiOnline_Master.log"
             || fname == L"KenshiOnline_CRASH.log") {
                fresh.clear();
            }
        }
        if (!fresh.empty() && fresh != lastClientPath) {
            // New session log — close previous, attach new.
            closeTailed(files[0]);
            files[0].path = fresh;
            lastClientPath = fresh;
            char nbuf[MAX_PATH * 2]{};
            WideCharToMultiByte(CP_UTF8, 0, fresh.c_str(), -1,
                                nbuf, sizeof(nbuf), nullptr, nullptr);
            std::string msg = std::string("\n--- new client log: ") + nbuf + "\n";
            writeColored(msg, col::CYAN);
            // Start from the top of the new log so the user sees the
            // session boot sequence.
            openTailed(files[0], /*seekToEnd=*/false);
        }

        for (auto& tf : files) {
            if (!tf.path.empty()) pumpFile(tf);
        }
        Sleep(100);
    }
    return 0;
}
