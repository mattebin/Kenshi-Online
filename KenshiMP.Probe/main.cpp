// =========================================================================
//                          KenshiMP.Probe
// =========================================================================
// Out-of-process memory-layout validator for kenshi_x64.exe.
//
// Why this is a separate process
// ------------------------------
// We've spent dozens of test sessions watching the in-process DLL guess
// at memory layouts and crash trying to read them.  ReadProcessMemory
// from a separate process can't crash the target — failed reads return
// false, no AV propagates.  The Probe verifies our offset assumptions
// in real time, and its findings drive corrections in the in-process
// addons safely.
//
// What it walks each tick
// -----------------------
// 1. `kenshi_x64.exe` module base + size
// 2. `PlayerBase` singleton at the historically-validated RVA — the
//    deref's first qword should look like a heap pointer
// 3. `GameWorldSingleton` similarly — the deref should yield a GameWorld
// 4. Within the GameWorld struct, scan for a pointer whose first qword
//    equals the recorded RootObjectFactory vtable RVA
//    (`mod+0x16993B0`).  This is the same scan the in-process
//    SafeAddon performs but here it runs from outside, so even if it
//    fails, the game is unaffected.
//
// Output
// ------
// Streams to `KenshiOnline_Probe.log` next to the other Kenshi-online
// logs.  KenshiMP.LogTail picks it up via its `KenshiOnline_*` glob.
// One line per validation pass; severity escalates from `info` (all
// good) to `warning` (one read failed) to `error` (multiple structures
// invalid simultaneously).
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

namespace {

static std::wstring SelfDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

static DWORD FindKenshiPid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"kenshi_x64.exe") == 0) {
                pid = pe.th32ProcessID; break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// Cached module info for the attached process.  We resolve once and
// re-use; the kenshi_x64.exe module never moves within its own
// process.
struct TargetCtx {
    HANDLE   hProc = nullptr;
    DWORD    pid = 0;
    uint64_t modBase = 0;
    uint64_t modSize = 0;
};

// Read N bytes from the target process.  Wraps ReadProcessMemory
// with size checks; returns false on any failure path.
template <typename T>
static bool ReadAt(const TargetCtx& ctx, uint64_t addr, T& outVal) {
    SIZE_T got = 0;
    if (!ReadProcessMemory(ctx.hProc, (LPCVOID)addr,
                            &outVal, sizeof(T), &got)) return false;
    return got == sizeof(T);
}

// Resolve `kenshi_x64.exe` inside the target.  Iterates loaded
// modules via EnumProcessModules and matches by basename.
static bool ResolveModule(TargetCtx& ctx) {
    HMODULE mods[1024]{};
    DWORD needed = 0;
    if (!EnumProcessModules(ctx.hProc, mods, sizeof(mods), &needed)) return false;
    DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count; ++i) {
        wchar_t name[MAX_PATH]{};
        if (GetModuleBaseNameW(ctx.hProc, mods[i], name, MAX_PATH) == 0) continue;
        if (_wcsicmp(name, L"kenshi_x64.exe") == 0) {
            MODULEINFO mi{};
            if (!GetModuleInformation(ctx.hProc, mods[i], &mi, sizeof(mi))) continue;
            ctx.modBase = (uint64_t)(uintptr_t)mi.lpBaseOfDll;
            ctx.modSize = mi.SizeOfImage;
            return true;
        }
    }
    return false;
}

// Streaming logger writing spdlog-shaped lines.
struct ProbeLog {
    std::ofstream f;
    bool open(const std::wstring& path) {
        f.open(path, std::ios::out | std::ios::trunc);
        return f.is_open();
    }
    void write(const char* level, const std::string& msg) {
        if (!f.is_open()) return;
        SYSTEMTIME st{}; GetLocalTime(&st);
        char ts[40]{};
        sprintf_s(ts, "[%04d-%02d-%02d %02d:%02d:%02d.%03d]",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        f << ts << " [probe] [" << level << "] " << msg << "\n";
        f.flush();
    }
};

// Validation pass — reads each candidate structure and writes one
// summary line.  Doesn't fail the program on any individual read
// failure; logs the failure and moves on.
static void RunPass(TargetCtx& ctx, ProbeLog& log) {
    constexpr uint64_t kPlayerBaseRva       = 0x28FF908; // observed in
                                                          // multiple sessions
    constexpr uint64_t kRootObjectFactoryVt = 0x16993B0; // recorded in
                                                          // re_kenshi 2/manual_findings

    char buf[512]{};

    // Heap-shape predicate: pointer is in user-mode range AND not
    // inside the module.  Used to weed out garbage values.
    auto looksHeap = [&](uint64_t p) {
        return p >= 0x10000ULL &&
               p <  0x00007FFFFFFFFFFFULL &&
               !(p >= ctx.modBase && p < ctx.modBase + ctx.modSize);
    };

    // 1. PlayerBase singleton dereference
    uint64_t playerCharPtr = 0;
    bool playerOk = ReadAt(ctx, ctx.modBase + kPlayerBaseRva, playerCharPtr) &&
                     looksHeap(playerCharPtr);
    sprintf_s(buf, "PlayerBase singleton at mod+0x%llX deref=0x%llX heap-shape=%s",
              (unsigned long long)kPlayerBaseRva,
              (unsigned long long)playerCharPtr,
              playerOk ? "yes" : "NO");
    log.write(playerOk ? "info" : "warning", buf);

    // 2. Player position triplet — only if PlayerBase looked heap-y
    if (playerOk) {
        float px = 0, py = 0, pz = 0;
        bool posOk = ReadAt(ctx, playerCharPtr + 0x48, px) &&
                     ReadAt(ctx, playerCharPtr + 0x4C, py) &&
                     ReadAt(ctx, playerCharPtr + 0x50, pz);
        sprintf_s(buf,
            "Player position chars+0x48 = (%.1f, %.1f, %.1f) %s",
            px, py, pz, posOk ? "OK" : "FAIL");
        log.write(posOk ? "info" : "warning", buf);
    }

    // 3. GameWorld singleton — RVA for this differs across runs; we
    //    report the candidates that look valid.  GameWorld lives
    //    somewhere in the .data section; without the in-process
    //    resolver we'd have to scan, which is too noisy here.  Instead
    //    we walk all heap-shaped qwords in a candidate window and
    //    look for one that, when deref'd, gives a struct containing
    //    a pointer to the RootObjectFactory vtable.
    //    Window: a slice of .data we've historically observed
    //    GameWorld pointers to live in.
    constexpr uint64_t kDataScanStart = 0x28FF000;
    constexpr uint64_t kDataScanLen   = 0x20000;
    bool factoryFound = false;
    uint64_t factoryAddr = 0;
    uint64_t expectedVt = ctx.modBase + kRootObjectFactoryVt;
    for (uint64_t off = 0; off < kDataScanLen; off += sizeof(uint64_t)) {
        uint64_t cand = 0;
        if (!ReadAt(ctx, ctx.modBase + kDataScanStart + off, cand)) continue;
        if (!looksHeap(cand)) continue;
        // Each heap-shaped pointer is a GameWorld* candidate.  Walk
        // its first 0x1000 bytes for a sub-pointer whose deref equals
        // the factory vtable.
        for (uint64_t inner = 0; inner < 0x1000; inner += sizeof(uint64_t)) {
            uint64_t innerCand = 0;
            if (!ReadAt(ctx, cand + inner, innerCand)) continue;
            if (!looksHeap(innerCand)) continue;
            uint64_t vtCheck = 0;
            if (!ReadAt(ctx, innerCand, vtCheck)) continue;
            if (vtCheck == expectedVt) {
                factoryFound = true;
                factoryAddr = innerCand;
                sprintf_s(buf,
                    "factory found via .data scan: GameWorld@0x%llX + 0x%llX -> 0x%llX (vtable matches mod+0x%llX)",
                    (unsigned long long)cand,
                    (unsigned long long)inner,
                    (unsigned long long)innerCand,
                    (unsigned long long)kRootObjectFactoryVt);
                log.write("info", buf);
                break;
            }
        }
        if (factoryFound) break;
    }
    if (!factoryFound) {
        sprintf_s(buf,
            "factory NOT found by .data scan (expected vtable at mod+0x%llX = 0x%llX)",
            (unsigned long long)kRootObjectFactoryVt,
            (unsigned long long)expectedVt);
        log.write("warning", buf);
    }
}

} // unnamed namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleTitleW(L"KenshiMP — Probe");
    std::wcout << L"KenshiMP.Probe starting\n";

    // Open the log next to the game's logs.
    ProbeLog log;
    std::wstring logPath = SelfDir() + L"\\KenshiOnline_Probe.log";
    if (!log.open(logPath)) {
        std::wcerr << L"could not open " << logPath << L" for writing\n";
        return 2;
    }
    log.write("info", "Probe starting");

    // Resolve PID — argv[1] preferred, else poll.
    DWORD pid = (argc >= 2) ? (DWORD)_wtoi(argv[1]) : 0;
    if (pid == 0) {
        for (int i = 0; i < 60 && pid == 0; ++i) {
            pid = FindKenshiPid();
            if (pid == 0) std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (pid == 0) {
            log.write("error", "kenshi_x64.exe not found within 60 s — exiting");
            return 3;
        }
    }
    char buf[256]{};
    sprintf_s(buf, "attached to PID %u", pid);
    log.write("info", buf);

    TargetCtx ctx{};
    ctx.pid = pid;
    ctx.hProc = OpenProcess(PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_READ |
                              SYNCHRONIZE, FALSE, pid);
    if (!ctx.hProc) {
        sprintf_s(buf, "OpenProcess failed: 0x%lX", GetLastError());
        log.write("error", buf);
        return 4;
    }
    if (!ResolveModule(ctx)) {
        log.write("error", "could not resolve kenshi_x64.exe module bounds");
        CloseHandle(ctx.hProc);
        return 5;
    }
    sprintf_s(buf, "kenshi_x64.exe modBase=0x%llX modSize=0x%llX",
              (unsigned long long)ctx.modBase,
              (unsigned long long)ctx.modSize);
    log.write("info", buf);

    // Sample loop — every 2 seconds.  Bail when the process exits
    // (the OpenProcess handle becomes signaled; we use it as our
    // sleep abort flag).
    while (true) {
        DWORD wait = WaitForSingleObject(ctx.hProc, 2000);
        if (wait == WAIT_OBJECT_0) {
            log.write("info", "target process exited — Probe shutting down");
            break;
        }
        RunPass(ctx, log);
    }

    CloseHandle(ctx.hProc);
    return 0;
}
