// process_status — out-of-process probe for "is this binary running"
//
// The Dashboard's status panel shows ✓/✗ for every component (game,
// server, master, dashboard tools).  This module owns those checks.
// Pure read-only — opens process snapshots, never modifies anything.
#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>

namespace kmp::dash {

struct ProcessInfo {
    bool     running = false;
    uint32_t pid = 0;
};

// Search the running-process list for an executable matching `name`.
// `name` is matched case-insensitively against `szExeFile` so paths
// don't matter — `kenshi_x64.exe` or `Kenshi_x64.EXE` both work.
ProcessInfo FindProcess(const wchar_t* name);

} // namespace kmp::dash
