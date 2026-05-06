#pragma once
// LogTailLauncher — single function used by both the injector and the
// dedicated server to start `KenshiMP.LogTail.exe` if (and only if) it
// isn't already running.
//
// LogTail itself enforces single-instance via a Global mutex; this helper
// just performs a best-effort `CreateProcess` + ignore-failure handshake
// so the caller doesn't have to know whether the tail viewer is already
// up.  Both callers run from inside the Kenshi install directory, so
// resolving LogTail's path is just "next to me".
//
// No spdlog dependency on purpose: Common gets included into both
// processes that haven't yet initialised their loggers when this is
// called, and OutputDebugStringA is enough for diagnostics here.

#include <Windows.h>
#include <Shlwapi.h>
#include <string>

namespace kmp {

inline void LaunchLogTailIfAbsent() {
    // Respect KMP_NO_LOGTAIL=1 so CI / scripted runs don't pop a
    // window when nobody's there to look at it.
    char skip[8]{};
    if (GetEnvironmentVariableA("KMP_NO_LOGTAIL", skip, sizeof(skip)) > 0) {
        if (skip[0] == '1') return;
    }

    // Probe the global mutex without acquiring it.  If LogTail is
    // already running, OpenMutex returns a handle and we exit.  We never
    // hold the mutex from the caller — only LogTail itself does.
    HANDLE existing = OpenMutexW(SYNCHRONIZE, FALSE,
                                 L"Global\\KenshiMP.LogTail");
    if (existing != nullptr) {
        CloseHandle(existing);
        return;
    }

    // Resolve LogTail.exe in the same directory as the calling process.
    wchar_t self[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) return;
    PathRemoveFileSpecW(self);
    std::wstring exe = std::wstring(self) + L"\\KenshiMP.LogTail.exe";
    if (!PathFileExistsW(exe.c_str())) {
        OutputDebugStringW((L"KMP: LogTail not found at " + exe + L"\n").c_str());
        return;
    }

    // CREATE_NEW_CONSOLE so the tail window is visually distinct from
    // whatever console (if any) the caller already owns.  DETACHED would
    // hide the window from the user, defeating the point.
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\"";
    BOOL ok = CreateProcessW(nullptr,
        const_cast<LPWSTR>(cmd.c_str()),
        nullptr, nullptr, FALSE,
        CREATE_NEW_CONSOLE,
        nullptr, self, &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

} // namespace kmp
