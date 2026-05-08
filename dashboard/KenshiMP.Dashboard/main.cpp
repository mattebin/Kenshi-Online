// =========================================================================
//                       KenshiMP.Dashboard
// =========================================================================
// Centralised v2 client.  Two modes (Host / Player) selected by a big
// red toggle bar at the top.  Each mode shows a numbered vertical
// workflow column — `1. ...`, `2. ...`, etc. — so an LLM-driven UI
// agent can find buttons reliably by caption prefix.
//
// Layout
// ------
//   ┌──────────────────────────────────────────────────────────────┐
//   │  KenshiMP — Dashboard                                        │
//   ├──────────────────────────────────────────────────────────────┤
//   │  [  HOST  ]   [ Player ]      ACTIVE MODE: PLAYER  (red)     │
//   ├──────────────────────────────────────────────────────────────┤
//   │  Workflow                                                    │
//   │   [ 1. Launch Game            ]  status...                   │
//   │   [ 2. View Logs              ]  status...                   │
//   │   [ 3. Stop Game              ]  status...                   │
//   │   [ 4. Disconnect / Exit      ]  status...                   │
//   ├──────────────────────────────────────────────────────────────┤
//   │  Tools                                                       │
//   │   [ Brainer ] [ Probe ] [ CrashWatchdog ] [ LogTail ]        │
//   │   [ Open Folder ] [ Edit server.json ]                       │
//   ├──────────────────────────────────────────────────────────────┤
//   │  Logs (last 100 lines)                                       │
//   │  ...                                                         │
//   └──────────────────────────────────────────────────────────────┘
//
// LLM-friendly conventions
// ------------------------
// - Every button caption starts with its workflow number ("1. ", "2. ")
//   or its tool name verbatim ("Brainer", "Probe", etc.).
// - HMENU ids are stable and meaningful (IDC_HOST_1_START_MASTER, etc.).
// - Buttons are wide (240 px) and tall (34 px) so click targets are
//   generous regardless of which automation tool is driving.
// - The active mode is BOTH text-coded ("ACTIVE MODE: HOST") AND
//   colour-coded (red WM_CTLCOLORSTATIC) so an LLM can confirm state
//   from either signal.
#include <Windows.h>
#include <CommCtrl.h>
#include <Shlwapi.h>
#include <ShellAPI.h>
#include <stdio.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "process_status.h"
#include "log_aggregator.h"
#include "spawn_queue.h"
#include "bg_fader.h"

#pragma comment(lib, "comctl32.lib")

namespace {

// ── Mode state ──
enum class Mode { Host, Player };
static Mode g_mode = Mode::Player;

// ── Window handles ──
HWND g_hwnd = nullptr;
HWND g_btnModeHost = nullptr;
HWND g_btnModePlayer = nullptr;
HWND g_lblActiveMode = nullptr;     // "ACTIVE MODE: PLAYER" — red when active

// Workflow buttons + status labels — five per mode max.  Re-bound on
// mode toggle.
struct WorkflowSlot {
    HWND btn = nullptr;
    HWND status = nullptr;
};
static constexpr int kMaxWorkflowSteps = 6;
WorkflowSlot g_workflow[kMaxWorkflowSteps]{};

// Tool row (always visible).
HWND g_btnBrainer = nullptr;
HWND g_btnProbe = nullptr;
HWND g_btnWatchdog = nullptr;
HWND g_btnLogTail = nullptr;
HWND g_btnOpenFolder = nullptr;
HWND g_btnEditServerJson = nullptr;

// Logs are painted directly onto the bg-compositing buffer in
// WM_ERASEBKGND so the background image bleeds through them.  No
// EDIT control — just a paint rect.  Trade-off: the text is not
// selectable / copyable.  Accepted "good enough" 2026-05-06 after
// the WS_EX_COMPOSITED + transparent EDIT attempt broke the layout.
RECT g_logRect{ 12, 0, 772, 0 };
HFONT g_uiFont = nullptr;
HFONT g_uiFontBold = nullptr;
HBRUSH g_redBrush = nullptr;

kmp::dash::LogAggregator g_logs;
uint64_t g_lastLogIngestSeen = 0;

kmp::dash::StubDispatcher g_spawnDisp;
kmp::dash::SpawnQueue g_spawnQueue(g_spawnDisp);

// Background image fader.  Initialised in WM_CREATE once GDI+ is up.
std::unique_ptr<kmp::dash::BgFader> g_fader;
ULONG_PTR g_gdiPlusToken = 0;

// Refresh timer drives status / log refresh once per second.
// A separate, faster fade timer ticks the background animation.
constexpr UINT_PTR kRefreshTimer = 1;
constexpr int      kRefreshMs    = 1000;
constexpr UINT_PTR kFadeTimer    = 2;
constexpr int      kFadeMs       = 50;     // 20 fps animation

// ── Stable LLM-friendly IDs ──
// Every button has a distinct id with a self-documenting name.  An
// automation agent can map captions ↔ ids without ambiguity.
#define IDC_BTN_MODE_HOST        2001
#define IDC_BTN_MODE_PLAYER      2002

#define IDC_HOST_1_START_MASTER  2101
#define IDC_HOST_2_START_SERVER  2102
#define IDC_HOST_3_LAUNCH_GAME   2103
#define IDC_HOST_4_STOP_ALL      2104
#define IDC_HOST_5_VIEW_LOG      2105

#define IDC_PLAYER_1_LAUNCH_GAME 2201
#define IDC_PLAYER_2_VIEW_LOGS   2202
#define IDC_PLAYER_3_STOP_GAME   2203
#define IDC_PLAYER_4_DISCONNECT  2204

#define IDC_TOOL_BRAINER         2301
#define IDC_TOOL_PROBE           2302
#define IDC_TOOL_WATCHDOG        2303
#define IDC_TOOL_LOGTAIL         2304
#define IDC_TOOL_OPEN_FOLDER     2305
#define IDC_TOOL_EDIT_SERVERJSON 2306

// ── Helpers ──

static std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

static bool SpawnSibling(const wchar_t* exeName,
                          DWORD flags = CREATE_NEW_CONSOLE) {
    std::wstring full = ExeDir() + L"\\" + exeName;
    if (!PathFileExistsW(full.c_str())) return false;
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + full + L"\"";
    BOOL ok = CreateProcessW(nullptr,
        const_cast<LPWSTR>(cmd.c_str()),
        nullptr, nullptr, FALSE, flags,
        nullptr, ExeDir().c_str(), &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return ok != 0;
}

static void KillProcessByName(const wchar_t* name) {
    auto info = kmp::dash::FindProcess(name);
    if (!info.running) return;
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, info.pid);
    if (h) { TerminateProcess(h, 0); CloseHandle(h); }
}

static void StopAllServers() {
    KillProcessByName(L"KenshiMP.Server.exe");
    KillProcessByName(L"KenshiMP.MasterServer.exe");
}

static void OpenWithDefaultApp(const wchar_t* path) {
    ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);
}

// ── Workflow construction ──

struct WorkflowStep {
    int id;
    std::wstring caption;
    std::function<std::wstring()> statusFn; // returns "running" / "..."
    std::function<void()> action;
};

static WorkflowStep MakeHostStep1() {
    return { IDC_HOST_1_START_MASTER, L"1. Start MasterServer",
        []() {
            auto p = kmp::dash::FindProcess(L"KenshiMP.MasterServer.exe");
            return p.running ? std::wstring(L"running (PID ") +
                std::to_wstring(p.pid) + L")"
              : std::wstring(L"not running");
        },
        []() { SpawnSibling(L"KenshiMP.MasterServer.exe"); }
    };
}
static WorkflowStep MakeHostStep2() {
    return { IDC_HOST_2_START_SERVER, L"2. Start Server",
        []() {
            auto p = kmp::dash::FindProcess(L"KenshiMP.Server.exe");
            return p.running ? std::wstring(L"running (PID ") +
                std::to_wstring(p.pid) + L")"
              : std::wstring(L"not running");
        },
        []() { SpawnSibling(L"KenshiMP.Server.exe"); }
    };
}
static WorkflowStep MakeHostStep3() {
    return { IDC_HOST_3_LAUNCH_GAME, L"3. Launch Game",
        []() {
            auto p = kmp::dash::FindProcess(L"kenshi_x64.exe");
            return p.running ? std::wstring(L"game running")
                             : std::wstring(L"game not running");
        },
        []() { SpawnSibling(L"KenshiMP.Injector.exe"); }
    };
}
static WorkflowStep MakeHostStep4() {
    return { IDC_HOST_4_STOP_ALL, L"4. Stop All",
        []() { return std::wstring(L"terminates Server + MasterServer"); },
        []() { StopAllServers(); }
    };
}
static WorkflowStep MakeHostStep5() {
    return { IDC_HOST_5_VIEW_LOG, L"5. View Server Log",
        []() { return std::wstring(L"opens KenshiOnline_Server.log"); },
        []() {
            std::wstring p = ExeDir() + L"\\KenshiOnline_Server.log";
            OpenWithDefaultApp(p.c_str());
        }
    };
}

static WorkflowStep MakePlayerStep1() {
    return { IDC_PLAYER_1_LAUNCH_GAME, L"1. Launch Game",
        []() {
            auto p = kmp::dash::FindProcess(L"kenshi_x64.exe");
            return p.running ? std::wstring(L"game running")
                             : std::wstring(L"game not running");
        },
        []() { SpawnSibling(L"KenshiMP.Injector.exe"); }
    };
}
static WorkflowStep MakePlayerStep2() {
    return { IDC_PLAYER_2_VIEW_LOGS, L"2. View Logs",
        []() {
            auto t = kmp::dash::FindProcess(L"KenshiMP.LogTail.exe");
            return t.running ? std::wstring(L"LogTail running")
                             : std::wstring(L"LogTail not running");
        },
        []() { SpawnSibling(L"KenshiMP.LogTail.exe"); }
    };
}
static WorkflowStep MakePlayerStep3() {
    return { IDC_PLAYER_3_STOP_GAME, L"3. Stop Game",
        []() { return std::wstring(L"sends terminate to kenshi_x64.exe"); },
        []() { KillProcessByName(L"kenshi_x64.exe"); }
    };
}
static WorkflowStep MakePlayerStep4() {
    return { IDC_PLAYER_4_DISCONNECT, L"4. Disconnect / Exit",
        []() { return std::wstring(L"stop game + close LogTail"); },
        []() {
            KillProcessByName(L"kenshi_x64.exe");
            KillProcessByName(L"KenshiMP.LogTail.exe");
        }
    };
}

static std::vector<WorkflowStep> CurrentWorkflow() {
    if (g_mode == Mode::Host) {
        return { MakeHostStep1(), MakeHostStep2(), MakeHostStep3(),
                 MakeHostStep4(), MakeHostStep5() };
    }
    return { MakePlayerStep1(), MakePlayerStep2(),
             MakePlayerStep3(), MakePlayerStep4() };
}

// ── UI plumbing ──

static void CreateFonts() {
    LOGFONTW lf{};
    lf.lfHeight = -14;
    lf.lfWeight = FW_NORMAL;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    g_uiFont = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_BOLD;
    lf.lfHeight = -16;
    g_uiFontBold = CreateFontIndirectW(&lf);
    g_redBrush = CreateSolidBrush(RGB(180, 30, 30));
}

static HWND MakeButton(HWND parent, int id, const wchar_t* text,
                       int x, int y, int w, int h, HFONT font = nullptr) {
    HWND h2 = CreateWindowW(L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        x, y, w, h, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessage(h2, WM_SETFONT, (WPARAM)(font ? font : g_uiFont), TRUE);
    return h2;
}

static HWND MakeStatic(HWND parent, const wchar_t* text,
                        int x, int y, int w, int h, HFONT font = nullptr,
                        UINT extraStyle = 0) {
    HWND s = CreateWindowW(L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT | extraStyle,
        x, y, w, h, parent, nullptr, nullptr, nullptr);
    SendMessage(s, WM_SETFONT, (WPARAM)(font ? font : g_uiFont), TRUE);
    return s;
}

// Destroy any existing workflow buttons + status labels.  Called on
// mode toggle before re-rendering.
static void ClearWorkflow() {
    for (auto& slot : g_workflow) {
        if (slot.btn)    { DestroyWindow(slot.btn);    slot.btn = nullptr; }
        if (slot.status) { DestroyWindow(slot.status); slot.status = nullptr; }
    }
}

static void RenderWorkflow() {
    ClearWorkflow();
    auto steps = CurrentWorkflow();
    int y = 110;
    constexpr int kRowH = 38;
    constexpr int kBtnW = 240;
    constexpr int kStatusX = 260;
    constexpr int kStatusW = 460;
    int slot = 0;
    for (auto& s : steps) {
        if (slot >= kMaxWorkflowSteps) break;
        g_workflow[slot].btn = MakeButton(g_hwnd, s.id, s.caption.c_str(),
                                          12, y, kBtnW, 34);
        g_workflow[slot].status = MakeStatic(g_hwnd, L"...",
                                              kStatusX, y + 8,
                                              kStatusW, 22);
        ++slot;
        y += kRowH;
    }
}

static void UpdateModeUiHighlight() {
    // Re-caption the mode toggle to indicate which is active.  Active
    // mode wraps with stars; inactive is plain.
    SetWindowTextW(g_btnModeHost,
        g_mode == Mode::Host ? L"★ HOST ★" : L"HOST");
    SetWindowTextW(g_btnModePlayer,
        g_mode == Mode::Player ? L"★ PLAYER ★" : L"PLAYER");
    SetWindowTextW(g_lblActiveMode,
        g_mode == Mode::Host ? L"  ACTIVE MODE: HOST  "
                              : L"  ACTIVE MODE: PLAYER  ");
    InvalidateRect(g_lblActiveMode, nullptr, TRUE);
}

static void RefreshWorkflowStatus() {
    auto steps = CurrentWorkflow();
    for (size_t i = 0; i < steps.size() && i < kMaxWorkflowSteps; ++i) {
        if (!g_workflow[i].status) continue;
        std::wstring s = steps[i].statusFn ? steps[i].statusFn() : L"";
        SetWindowTextW(g_workflow[i].status, s.c_str());
    }
}

// Pull new log lines and trigger a repaint of the log rect when
// there's new content.  No EDIT control to update — the painter
// re-reads g_logs.Recent on every WM_ERASEBKGND.
static void RefreshLogs() {
    g_logs.Tick();
    if (g_logs.TotalIngested() == g_lastLogIngestSeen) return;
    g_lastLogIngestSeen = g_logs.TotalIngested();
    InvalidateRect(g_hwnd, &g_logRect, TRUE);
}

// Draw the recent log lines onto a memory DC as a translucent
// overlay.  Caller has already painted the bg fade on `memDc` —
// this composites on top.  GDI+ alpha-blended backdrop + cleartype
// text in one pass; the whole composited buffer is BitBlt'd to the
// window.  No flicker.
static void PaintLogsOverlay(HDC memDc, const RECT& rect) {
    Gdiplus::Graphics g(memDc);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    Gdiplus::SolidBrush dim(Gdiplus::Color(150, 0, 0, 0));
    g.FillRectangle(&dim,
        rect.left, rect.top,
        rect.right - rect.left, rect.bottom - rect.top);

    Gdiplus::Pen frame(Gdiplus::Color(180, 220, 220, 220), 1.0f);
    g.DrawRectangle(&frame,
        rect.left, rect.top,
        rect.right - rect.left - 1, rect.bottom - rect.top - 1);

    Gdiplus::FontFamily fam(L"Consolas");
    Gdiplus::Font font(&fam, 9.0f, Gdiplus::FontStyleRegular,
                       Gdiplus::UnitPoint);
    Gdiplus::SolidBrush text(Gdiplus::Color(230, 230, 230, 230));

    // Clip text drawing to inside the rect so long lines don't bleed
    // out the right edge.  GDI+ wraps when given a layoutRect.
    int padX = 8, padY = 6;
    Gdiplus::RectF layout(
        (float)(rect.left + padX),
        (float)(rect.top  + padY),
        (float)((rect.right  - rect.left) - padX * 2),
        (float)((rect.bottom - rect.top)  - padY * 2));

    int lineH = 13;
    int maxLines = ((rect.bottom - rect.top) - padY * 2) / lineH;
    if (maxLines < 1) return;
    auto recent = g_logs.Recent((size_t)maxLines);
    int y = rect.top + padY;
    for (auto& l : recent) {
        std::string utf8 = l.source + " " + l.content;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                        (int)utf8.size(), nullptr, 0);
        std::wstring wide(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                             (int)utf8.size(), wide.data(), wlen);

        // Per-line layout rect with NoWrap so we draw a single line
        // truncated by the rect width — this prevents the right-edge
        // overflow without word-wrapping the whole multi-line buffer.
        Gdiplus::RectF lineRect(layout.X, (float)y, layout.Width,
                                 (float)lineH);
        Gdiplus::StringFormat fmt;
        fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        g.DrawString(wide.c_str(), wlen, &font, lineRect, &fmt, &text);
        y += lineH;
    }
}

static void DispatchWorkflowAction(int id) {
    auto steps = CurrentWorkflow();
    for (auto& s : steps) {
        if (s.id == id) { if (s.action) s.action(); return; }
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                 WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        CreateFonts();

        // Mode toggle bar (top).  Two big buttons.
        g_btnModeHost   = MakeButton(hwnd, IDC_BTN_MODE_HOST,
                                      L"HOST",   12, 12, 200, 44, g_uiFontBold);
        g_btnModePlayer = MakeButton(hwnd, IDC_BTN_MODE_PLAYER,
                                      L"★ PLAYER ★", 224, 12, 200, 44, g_uiFontBold);
        g_lblActiveMode = MakeStatic(hwnd, L"  ACTIVE MODE: PLAYER  ",
                                      436, 22, 280, 26, g_uiFontBold,
                                      SS_CENTER);

        // Workflow heading.
        MakeStatic(hwnd, L"Workflow (numbered, click in order)",
                    12, 80, 360, 18, g_uiFontBold);

        RenderWorkflow();

        // Tools row — always visible.
        int toolsY = 360;
        MakeStatic(hwnd, L"Tools (always available)",
                    12, toolsY, 360, 18, g_uiFontBold);
        toolsY += 26;
        constexpr int kToolW = 130, kToolH = 32, kToolGap = 8;
        int x = 12;
        g_btnBrainer       = MakeButton(hwnd, IDC_TOOL_BRAINER,
                                         L"Brainer",      x, toolsY, kToolW, kToolH);
        x += kToolW + kToolGap;
        g_btnProbe         = MakeButton(hwnd, IDC_TOOL_PROBE,
                                         L"Probe",        x, toolsY, kToolW, kToolH);
        x += kToolW + kToolGap;
        g_btnWatchdog      = MakeButton(hwnd, IDC_TOOL_WATCHDOG,
                                         L"CrashWatchdog",x, toolsY, kToolW, kToolH);
        x += kToolW + kToolGap;
        g_btnLogTail       = MakeButton(hwnd, IDC_TOOL_LOGTAIL,
                                         L"LogTail",      x, toolsY, kToolW, kToolH);
        x += kToolW + kToolGap;
        g_btnOpenFolder    = MakeButton(hwnd, IDC_TOOL_OPEN_FOLDER,
                                         L"Open Folder",  x, toolsY, kToolW, kToolH);
        x = 12; toolsY += kToolH + 6;
        g_btnEditServerJson = MakeButton(hwnd, IDC_TOOL_EDIT_SERVERJSON,
                                          L"Edit server.json", x, toolsY, kToolW + 40, kToolH);

        // Logs (bottom).  Painted directly onto the bg-compositing
        // buffer in WM_ERASEBKGND — see PaintLogsOverlay.  No EDIT
        // control; trade-off is no text selection.
        int logsY = toolsY + kToolH + 18;
        MakeStatic(hwnd, L"Logs (last lines, transparent overlay)",
                    12, logsY, 460, 18, g_uiFontBold);
        logsY += 22;
        g_logRect = RECT{ 12, logsY, 772, logsY + 220 };

        g_logs.SetWatchDir(ExeDir());

        // Initialise the crossfading background.  Folder is the user-
        // specified pictures dir; if it doesn't exist or has no usable
        // images, BgFader.ImageCount() == 0 and we fall back to a
        // solid colour fill in WM_ERASEBKGND.
        g_fader = std::make_unique<kmp::dash::BgFader>(
            L"C:\\Users\\Matte\\Pictures\\Kenshi rolling backround for client",
            /*dwellMs=*/4000, /*fadeMs=*/2000);

        UpdateModeUiHighlight();
        SetTimer(hwnd, kRefreshTimer, kRefreshMs, nullptr);
        SetTimer(hwnd, kFadeTimer,    kFadeMs,    nullptr);
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC dc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return 1;

        // Single composite buffer — bg paint + log overlay land on
        // the same memory DC, then we BitBlt once.  Everything
        // composes flicker-free in one go.
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);

        if (g_fader && g_fader->ImageCount() > 0) {
            g_fader->PaintToMemDc(mem, w, h);
        } else {
            // No images — flat fill so the overlay still reads.
            HBRUSH bg = CreateSolidBrush(RGB(30, 30, 36));
            FillRect(mem, &rc, bg);
            DeleteObject(bg);
        }

        // Translucent log overlay on top.  Reads g_logs every paint
        // so it always shows the latest lines without an EDIT
        // control mediating.
        PaintLogsOverlay(mem, g_logRect);

        BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        return 1;
    }
    case WM_TIMER:
        if (wp == kRefreshTimer) {
            RefreshWorkflowStatus();
            RefreshLogs();
            g_spawnQueue.DrainTick();
        } else if (wp == kFadeTimer) {
            if (g_fader && g_fader->Tick()) {
                // Repaint only the bg area; child controls handle
                // their own redraw lazily.  RDW_INVALIDATE +
                // RDW_ERASE forces a fresh WM_ERASEBKGND.
                RedrawWindow(hwnd, nullptr, nullptr,
                              RDW_INVALIDATE | RDW_ERASE | RDW_NOCHILDREN);
            }
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        // Paint the active-mode label in red.
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        if (ctl == g_lblActiveMode) {
            SetTextColor(dc, RGB(255, 255, 255));
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, RGB(180, 30, 30));
            return (LRESULT)g_redBrush;
        }
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    case WM_COMMAND: {
        WORD id = LOWORD(wp);
        switch (id) {
        case IDC_BTN_MODE_HOST:
            if (g_mode != Mode::Host) {
                g_mode = Mode::Host;
                UpdateModeUiHighlight();
                RenderWorkflow();
                RefreshWorkflowStatus();
            }
            return 0;
        case IDC_BTN_MODE_PLAYER:
            if (g_mode != Mode::Player) {
                g_mode = Mode::Player;
                UpdateModeUiHighlight();
                RenderWorkflow();
                RefreshWorkflowStatus();
            }
            return 0;
        case IDC_TOOL_BRAINER:        SpawnSibling(L"KenshiMP.Cartographer.exe"); return 0;
        case IDC_TOOL_PROBE:          SpawnSibling(L"KenshiMP.Probe.exe"); return 0;
        case IDC_TOOL_WATCHDOG:       SpawnSibling(L"KenshiMP.CrashWatchdog.exe"); return 0;
        case IDC_TOOL_LOGTAIL:        SpawnSibling(L"KenshiMP.LogTail.exe"); return 0;
        case IDC_TOOL_OPEN_FOLDER:
            ShellExecuteW(nullptr, L"open", ExeDir().c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case IDC_TOOL_EDIT_SERVERJSON: {
            std::wstring p = ExeDir() + L"\\server.json";
            OpenWithDefaultApp(p.c_str());
            return 0;
        }
        default:
            // Workflow buttons span 2101..2105 (host) and 2201..2204 (player).
            if ((id >= 2101 && id <= 2199) || (id >= 2201 && id <= 2299)) {
                DispatchWorkflowAction(id);
                return 0;
            }
            break;
        }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kRefreshTimer);
        KillTimer(hwnd, kFadeTimer);
        g_fader.reset();
        if (g_uiFont)     { DeleteObject(g_uiFont);     g_uiFont = nullptr; }
        if (g_uiFontBold) { DeleteObject(g_uiFontBold); g_uiFontBold = nullptr; }
        if (g_redBrush)   { DeleteObject(g_redBrush);   g_redBrush = nullptr; }
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

    // GDI+ for the BgFader.  Lifetime spans the whole window — we
    // shut it down after the message loop returns.
    g_gdiPlusToken = kmp::dash::BgFader::Startup();

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // hbrBackground = NULL so Windows never paints its own erase
    // before our WM_ERASEBKGND handler runs.  Without this we'd
    // briefly flash COLOR_BTNFACE behind every fade frame.
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"KenshiMPDashboard";
    RegisterClassW(&wc);

    // WS_CLIPCHILDREN keeps the bg-fader's WM_ERASEBKGND painting
    // strictly inside the gaps between child controls.  Without it,
    // each fade tick draws the image *over* the buttons and labels.
    g_hwnd = CreateWindowExW(0, L"KenshiMPDashboard",
        L"KenshiMP — Dashboard",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 760,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    kmp::dash::BgFader::Shutdown(g_gdiPlusToken);
    g_gdiPlusToken = 0;
    return 0;
}
