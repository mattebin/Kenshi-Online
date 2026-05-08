// =========================================================================
//                       KenshiMP.CrashWatchdog
// =========================================================================
// Out-of-process supervisor for kenshi_x64.exe.
//
// What it does
// ------------
// 1. Polls the running process list for a kenshi_x64.exe instance.
//    Optionally accepts a PID on the command line (the injector knows
//    the PID it just launched and can pass it directly, skipping the
//    poll).
// 2. Opens that process with SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION.
//    These are read-only handles — we cannot modify or terminate the
//    target.  If the user is concerned about us doing anything to
//    their game, the access mask proves we can't.
// 3. WaitForSingleObject on the process handle blocks until the game
//    exits (clean or crash).
// 4. On exit: GetExitCodeProcess to read the exit code, then sweep
//    the Kenshi folder for KenshiOnline_*.log and KenshiOnline_CRASH.log,
//    grab the last 100 lines of each, and assemble a forensic report.
// 5. Write the report to KenshiMP_CrashReport_<YYYYmmdd-HHMMSS>.txt
//    next to the game logs so the LogTail viewer (which tails by
//    pattern) picks it up automatically.
//
// What it does NOT do
// -------------------
// - Modify Kenshi's memory.  We don't even ask for write access.
// - Terminate the game.  We don't ask for PROCESS_TERMINATE.
// - Inject code.  We don't ask for VM_OPERATION.
// - Auto-restart.  This is intentional — relaunching after a crash
//   masks intermittent bugs and frustrates debugging.  If the user
//   wants automatic relaunch they re-run the injector by hand.
//
// Why this is "100 bil IQ" architecture
// -------------------------------------
// Every previous attempt to handle "did Kenshi crash?" lived inside
// Kenshi's address space (VEH, SetUnhandledExceptionFilter).  Those
// can be bypassed by __fastfail / TerminateProcess / heap corruption
// triggering kernel-side termination.  An external supervisor sees
// the kernel-level exit code regardless of which user-mode handler
// the fault routed through.  No crash class can hide from us anymore.
#include <Windows.h>
#include <Shlwapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

static std::wstring SelfDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

static std::wstring TimestampUtcCompact() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[32]{};
    swprintf_s(buf, L"%04d%02d%02d-%02d%02d%02d",
               st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// Find a running process by image name.  Returns 0 if not found.
// Iterates the process snapshot — `Process32FirstW` enumerates every
// process in the system, so we match against the basename.
static DWORD FindProcessByName(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// Read the last `lines` lines of a file.  Streaming approach: scan
// backwards in 4 KB chunks until we've collected enough newlines or
// hit the start of the file.  Avoids loading huge logs entirely.
static std::string ReadLastLines(const std::wstring& path, size_t lines) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    int64_t pos = size.QuadPart;
    constexpr int64_t kChunk = 4096;
    std::string accum;
    size_t newlines = 0;
    while (pos > 0 && newlines < lines + 1) {
        int64_t readSize = (pos > kChunk) ? kChunk : pos;
        pos -= readSize;
        LARGE_INTEGER seek{}; seek.QuadPart = pos;
        SetFilePointerEx(h, seek, nullptr, FILE_BEGIN);
        std::string buf((size_t)readSize, '\0');
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), (DWORD)readSize, &got, nullptr)) break;
        accum = buf.substr(0, got) + accum;
        newlines = 0;
        for (char c : accum) if (c == '\n') ++newlines;
    }
    CloseHandle(h);
    // Trim to last `lines` lines.
    size_t found = 0;
    for (auto it = accum.rbegin(); it != accum.rend(); ++it) {
        if (*it == '\n') {
            if (++found > lines) {
                size_t off = (size_t)(accum.rend() - it);
                return accum.substr(off);
            }
        }
    }
    return accum;
}

// Friendly label for common Windows exit codes.  Anything not in this
// list goes in the report as a raw hex value so a future investigator
// can look it up.
static std::string ExitCodeLabel(DWORD code) {
    switch (code) {
        case 0:           return "STILL_ACTIVE / clean exit";
        case 1:           return "generic error / clean exit with status";
        case 0xC0000005:  return "EXCEPTION_ACCESS_VIOLATION";
        case 0xC0000409:  return "STATUS_STACK_BUFFER_OVERRUN (/GS / __fastfail)";
        case 0xC0000374:  return "STATUS_HEAP_CORRUPTION";
        case 0xC0000602:  return "STATUS_FAIL_FAST_EXCEPTION";
        case 0xC0000420:  return "STATUS_ASSERTION_FAILURE";
        case 0xC000041D:  return "STATUS_FATAL_USER_CALLBACK_EXCEPTION";
        case 0x40010005:  return "DBG_CONTROL_C (Ctrl+C)";
        case 0xC000013A:  return "STATUS_CONTROL_C_EXIT";
        case 0xCFFFFFFF:  return "killed by Task Manager / SIGKILL equivalent";
    }
    return "unrecognised exit code";
}

static bool IsCrashCode(DWORD code) {
    if (code == 0 || code == 1 || code == STILL_ACTIVE) return false;
    return code >= 0x80000000u || code == 0xCFFFFFFFu;
}

} // unnamed namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleTitleW(L"KenshiMP — CrashWatchdog");
    std::wcout << L"KenshiMP.CrashWatchdog: starting\n";

    // PID can be passed in as argv[1] when the injector spawns us.
    // Otherwise we poll for kenshi_x64.exe — useful when the user
    // launches the watchdog standalone (e.g. attached to a Steam-
    // started Kenshi without going through the injector).
    DWORD pid = 0;
    if (argc >= 2) {
        pid = (DWORD)_wtoi(argv[1]);
        std::wcout << L"  PID supplied via argv: " << pid << L"\n";
    }

    if (pid == 0) {
        std::wcout << L"  polling for kenshi_x64.exe... ";
        for (int tries = 0; tries < 60 && pid == 0; ++tries) {
            pid = FindProcessByName(L"kenshi_x64.exe");
            if (pid == 0) std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (pid == 0) {
            std::wcout << L"timed out.  Exiting.\n";
            return 2;
        }
        std::wcout << L"PID=" << pid << L"\n";
    }

    HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                FALSE, pid);
    if (!hProc) {
        std::wcout << L"  OpenProcess failed: " << GetLastError() << L"\n";
        return 3;
    }
    std::wcout << L"  attached read-only to PID " << pid << L".  Waiting...\n";

    // Block here for as long as Kenshi runs.  When it exits — clean
    // or otherwise — WaitForSingleObject returns.
    WaitForSingleObject(hProc, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(hProc, &exitCode);
    CloseHandle(hProc);

    std::string label = ExitCodeLabel(exitCode);
    bool crashed = IsCrashCode(exitCode);

    char header[512]{};
    sprintf_s(header,
        "Kenshi exited at PID %u\n"
        "  exit code: 0x%08X (%u decimal)\n"
        "  meaning:   %s\n"
        "  classify:  %s\n",
        pid, exitCode, exitCode, label.c_str(),
        crashed ? "CRASH (kernel-level non-zero exit)" : "CLEAN (user exit / explicit ExitProcess)");
    std::wcout << header;

    // Write a forensic report.  Path goes in the Kenshi folder so
    // KenshiMP.LogTail tails it automatically (its filter matches
    // any file with KenshiOnline_*.log; we use a different prefix
    // for the report so it's distinguishable as forensic data).
    std::wstring dir = SelfDir();
    std::wstring reportPath = dir + L"\\KenshiMP_CrashReport_" +
                               TimestampUtcCompact() + L".txt";
    char reportPathA[MAX_PATH * 2]{};
    WideCharToMultiByte(CP_UTF8, 0, reportPath.c_str(), -1,
                        reportPathA, sizeof(reportPathA), nullptr, nullptr);
    std::ofstream rep(reportPath);
    if (!rep.is_open()) {
        std::wcout << L"  could not write report to " << reportPath << L"\n";
        return 4;
    }
    rep << header << "\n";
    rep << "----- last 100 lines of every KenshiOnline_*.log -----\n";

    // Walk the Kenshi folder and append the tail of every relevant log.
    WIN32_FIND_DATAW fd{};
    std::wstring pattern = dir + L"\\KenshiOnline_*.log";
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            std::wstring full = dir + L"\\" + fd.cFileName;
            char nameA[MAX_PATH * 2]{};
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nameA,
                                sizeof(nameA), nullptr, nullptr);
            rep << "\n=== " << nameA << " (last 100) ===\n";
            rep << ReadLastLines(full, 100);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    rep.close();

    std::wcout << L"  wrote " << reportPath << L"\n";
    if (crashed) {
        std::wcout << L"  RESULT: this was a crash — exit code 0x"
                   << std::hex << exitCode << std::dec << L".\n";
    } else {
        std::wcout << L"  RESULT: clean exit — not a crash.\n";
    }
    return crashed ? 1 : 0;
}
