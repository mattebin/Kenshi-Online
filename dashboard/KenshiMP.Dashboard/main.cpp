// =========================================================================
//                       KenshiMP.Dashboard
// =========================================================================
// The unified out-of-game client.  Phase 1 scope: a single Win32
// window with three panels:
//
//   ┌─────────────────────────────────────────────────────────────────┐
//   │  KenshiMP — Dashboard                                           │
//   ├─────────────────────────────────────────────────────────────────┤
//   │  Status                                                         │
//   │   ✓ kenshi_x64.exe   (PID 1234)     ✓ KenshiMP.MasterServer    │
//   │   ✓ KenshiMP.Server  (PID 5678)     ✗ kenshi_x64 SafeAddon log │
//   ├─────────────────────────────────────────────────────────────────┤
//   │  Actions                                                        │
//   │   [ Launch Game ]  [ Start Server ]  [ Stop Server ]            │
//   │   [ Start Master ] [ Stop Master ]   [ Open Folder ]            │
//   ├─────────────────────────────────────────────────────────────────┤
//   │  Logs (last 200 lines)                                          │
//   │  [client] 22:00:22  Core: faction-signext-rescue armed          │
//   │  [server] 22:00:23  GameServer: Listening on port 27800         │
//   │  ...                                                            │
//   └─────────────────────────────────────────────────────────────────┘
//
// Refreshes every 1 s.  No IPC yet — pure observer + child-process
// launcher.  The named-pipe channel to KenshiMP.SafeAddon.dll lands
// in phase 2; this binary is the long-term home for everything that
// shouldn't live inside Kenshi's address space.
//
// See ../docs/ARCHITECTURE_v2.md for the full migration plan.
#include <Windows.h>
#include <CommCtrl.h>
#include <Shlwapi.h>
#include <ShellAPI.h>      // ShellExecuteW
#include <stdio.h>
#include <string>
#include <vector>
#include "process_status.h"
#include "log_aggregator.h"
#include "spawn_queue.h"

#pragma comment(lib, "comctl32.lib")

namespace {

// ── Window state ──
HWND g_hwnd = nullptr;
HWND g_statusEdit = nullptr;     // multi-line readonly edit, status panel
HWND g_logEdit = nullptr;        // multi-line readonly edit, logs panel
HWND g_btnGame = nullptr;
HWND g_btnStartServer = nullptr;
HWND g_btnStopServer = nullptr;
HWND g_btnStartMaster = nullptr;
HWND g_btnStopMaster = nullptr;
HWND g_btnOpenFolder = nullptr;
HFONT g_monoFont = nullptr;

kmp::dash::LogAggregator g_logs;
uint64_t g_lastLogIngestSeen = 0;

// The dashboard-side spawn queue lives here.  Phase 1 ships with the
// stub dispatcher (logs but doesn't actually cross into the game) so
// the queue, retry policy, and counters are all exercisable from the
// Dashboard alone.  Phase 2 swaps the dispatcher to a named-pipe
// client without changing any other code in this binary.
kmp::dash::StubDispatcher g_spawnDisp;
kmp::dash::SpawnQueue g_spawnQueue(g_spawnDisp);

constexpr UINT_PTR kRefreshTimer = 1;
constexpr int      kRefreshMs    = 1000;

#define IDC_BTN_GAME           1001
#define IDC_BTN_START_SERVER   1002
#define IDC_BTN_STOP_SERVER    1003
#define IDC_BTN_START_MASTER   1004
#define IDC_BTN_STOP_MASTER    1005
#define IDC_BTN_OPEN_FOLDER    1006

// ── Helpers ──

static std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

// Spawn a sibling executable with no arguments.  Used by the action
// buttons.  CREATE_NEW_CONSOLE so server / master windows are visible
// on their own; we don't want to capture their stdio inside the
// Dashboard window in this phase.
static bool SpawnSibling(const wchar_t* exeName, DWORD flags = CREATE_NEW_CONSOLE) {
    std::wstring full = ExeDir() + L"\\" + exeName;
    if (!PathFileExistsW(full.c_str())) return false;
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + full + L"\"";
    BOOL ok = CreateProcessW(nullptr,
        const_cast<LPWSTR>(cmd.c_str()),
        nullptr, nullptr, FALSE,
        flags,
        nullptr, ExeDir().c_str(), &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return ok != 0;
}

// Soft-kill a running process by name.  We use TerminateProcess,
// which is brutal but appropriate for development tooling that
// doesn't have a clean-shutdown protocol over a pipe yet.  Future:
// the Dashboard tells the server to shutdown via its own protocol.
static void KillProcessByName(const wchar_t* name) {
    auto info = kmp::dash::FindProcess(name);
    if (!info.running) return;
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, info.pid);
    if (h) {
        TerminateProcess(h, 0);
        CloseHandle(h);
    }
}

// ── Panel paints ──

static void RefreshStatus() {
    auto game   = kmp::dash::FindProcess(L"kenshi_x64.exe");
    auto server = kmp::dash::FindProcess(L"KenshiMP.Server.exe");
    auto master = kmp::dash::FindProcess(L"KenshiMP.MasterServer.exe");
    auto inj    = kmp::dash::FindProcess(L"KenshiMP.Injector.exe");
    auto tail   = kmp::dash::FindProcess(L"KenshiMP.LogTail.exe");
    auto probe  = kmp::dash::FindProcess(L"KenshiMP.Probe.exe");
    auto watch  = kmp::dash::FindProcess(L"KenshiMP.CrashWatchdog.exe");

    char buf[2048]{};
    int pos = 0;
    auto line = [&](const char* label, const kmp::dash::ProcessInfo& p) {
        char l[256]{};
        if (p.running) {
            sprintf_s(l, "  [running]  %s  (PID %u)\r\n", label, p.pid);
        } else {
            sprintf_s(l, "  [ stopped]  %s\r\n", label);
        }
        pos += sprintf_s(buf + pos, sizeof(buf) - pos, "%s", l);
    };
    line("kenshi_x64.exe        ", game);
    line("KenshiMP.Server       ", server);
    line("KenshiMP.MasterServer ", master);
    line("KenshiMP.Injector     ", inj);
    line("KenshiMP.LogTail      ", tail);
    line("KenshiMP.Probe        ", probe);
    line("KenshiMP.CrashWatchdog", watch);

    // Spawn-queue health.  Lives entirely in the Dashboard; no game
    // memory access whatsoever.  Phase 1 always shows "ipc=stub" —
    // that line flips to "ipc=connected" / "ipc=closed" when the
    // SafeAddon named pipe lands in phase 2.
    pos += sprintf_s(buf + pos, sizeof(buf) - pos,
        "\r\nSpawn queue (out-of-game)\r\n"
        "  pending=%zu  completed=%zu  failed=%zu  ipc=%s\r\n",
        g_spawnQueue.PendingCount(),
        g_spawnQueue.CompletedCount(),
        g_spawnQueue.FailedCount(),
        g_spawnDisp.IsConnected() ? "connected" : "stub");

    SetWindowTextA(g_statusEdit, buf);

    // Enable/disable the action buttons based on current state.
    EnableWindow(g_btnStopServer,  server.running ? TRUE : FALSE);
    EnableWindow(g_btnStopMaster,  master.running ? TRUE : FALSE);
    EnableWindow(g_btnStartServer, server.running ? FALSE : TRUE);
    EnableWindow(g_btnStartMaster, master.running ? FALSE : TRUE);
}

static void RefreshLogs() {
    g_logs.Tick();
    if (g_logs.TotalIngested() == g_lastLogIngestSeen) return;
    g_lastLogIngestSeen = g_logs.TotalIngested();
    auto recent = g_logs.Recent(200);
    std::string buf;
    buf.reserve(recent.size() * 96);
    for (auto& l : recent) {
        SYSTEMTIME st{};
        FILETIME ft{};
        SystemTimeToFileTime(&st, &ft); // unused; just want a deterministic repr
        // Format: [source]  content
        buf += l.source;
        buf.push_back(' ');
        buf += l.content;
        buf += "\r\n";
    }
    SetWindowTextA(g_logEdit, buf.c_str());
    // Scroll to bottom
    SendMessage(g_logEdit, EM_LINESCROLL, 0, (LPARAM)recent.size());
}

// ── Win32 plumbing ──

static void CreateMonoFont() {
    LOGFONTW lf{};
    lf.lfHeight = -12;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    wcscpy_s(lf.lfFaceName, L"Consolas");
    g_monoFont = CreateFontIndirectW(&lf);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        CreateMonoFont();

        // Fonts via SendMessage(WM_SETFONT) below.
        auto mkLabel = [&](const wchar_t* text, int x, int y) {
            HWND h = CreateWindowW(L"STATIC", text,
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                x, y, 200, 16, hwnd, nullptr, nullptr, nullptr);
            SendMessage(h, WM_SETFONT, (WPARAM)g_monoFont, TRUE);
        };
        auto mkButton = [&](int id, const wchar_t* text, int x, int y) -> HWND {
            HWND h = CreateWindowW(L"BUTTON", text,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                x, y, 140, 28, hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessage(h, WM_SETFONT, (WPARAM)g_monoFont, TRUE);
            return h;
        };

        mkLabel(L"Status", 12, 8);
        g_statusEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
              WS_VSCROLL | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
            12, 28, 760, 130, hwnd, nullptr, nullptr, nullptr);
        SendMessage(g_statusEdit, WM_SETFONT, (WPARAM)g_monoFont, TRUE);

        mkLabel(L"Actions", 12, 168);
        g_btnGame         = mkButton(IDC_BTN_GAME,         L"Launch Game",   12, 188);
        g_btnStartServer  = mkButton(IDC_BTN_START_SERVER, L"Start Server",  160, 188);
        g_btnStopServer   = mkButton(IDC_BTN_STOP_SERVER,  L"Stop Server",   308, 188);
        g_btnStartMaster  = mkButton(IDC_BTN_START_MASTER, L"Start Master",  456, 188);
        g_btnStopMaster   = mkButton(IDC_BTN_STOP_MASTER,  L"Stop Master",   604, 188);
        g_btnOpenFolder   = mkButton(IDC_BTN_OPEN_FOLDER,  L"Open Folder",   12, 222);

        mkLabel(L"Logs (last 200 lines, refreshed every 1 s)", 12, 264);
        g_logEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
              WS_VSCROLL | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
            12, 284, 760, 280, hwnd, nullptr, nullptr, nullptr);
        SendMessage(g_logEdit, WM_SETFONT, (WPARAM)g_monoFont, TRUE);

        // Bind log aggregator to the install dir.
        g_logs.SetWatchDir(ExeDir());

        SetTimer(hwnd, kRefreshTimer, kRefreshMs, nullptr);
        return 0;
    }
    case WM_TIMER:
        if (wp == kRefreshTimer) {
            RefreshStatus();
            RefreshLogs();
            // Drive the spawn queue's retry/dispatch loop on the same
            // 1 Hz tick.  Cheap until there's actual work; once IPC
            // lands we'll likely move this onto a 60 Hz worker thread.
            g_spawnQueue.DrainTick();
        }
        return 0;
    case WM_COMMAND: {
        WORD id = LOWORD(wp);
        switch (id) {
        case IDC_BTN_GAME:        SpawnSibling(L"KenshiMP.Injector.exe"); break;
        case IDC_BTN_START_SERVER: SpawnSibling(L"KenshiMP.Server.exe"); break;
        case IDC_BTN_STOP_SERVER:  KillProcessByName(L"KenshiMP.Server.exe"); break;
        case IDC_BTN_START_MASTER: SpawnSibling(L"KenshiMP.MasterServer.exe"); break;
        case IDC_BTN_STOP_MASTER:  KillProcessByName(L"KenshiMP.MasterServer.exe"); break;
        case IDC_BTN_OPEN_FOLDER:  ShellExecuteW(nullptr, L"open", ExeDir().c_str(),
                                                  nullptr, nullptr, SW_SHOWNORMAL); break;
        }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kRefreshTimer);
        if (g_monoFont) { DeleteObject(g_monoFont); g_monoFont = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

} // unnamed namespace

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"KenshiMPDashboard";
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, L"KenshiMPDashboard",
        L"KenshiMP — Dashboard",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 620,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
