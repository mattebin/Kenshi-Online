#pragma once
//
// Hook-disable gate. Lets us bisect which hook crashes the game without
// recompiling. Read once and cached at first use.
//
// Two sources, checked in order:
//   1. File:  <Kenshi dir>\kmp_disable_hooks.txt   (preferred — deterministic)
//   2. Env:   KMP_DISABLE_HOOKS                    (fallback)
//
// File contents = single line, comma-separated, # for line comments. Examples:
//   ai
//   ai,combat,inventory
//   *
//   all
//
// Wildcard "*" or "all" disables every hook EXCEPT render — render is
// required for the chat overlay; without it slash commands cannot be entered.
//
// Hook names: render, entity, combat, squadspawn, chartracker, inventory,
//             faction, time, ai, movement, squad, resource
//
// On first use, the resolved value is logged loudly with its source
// (FILE / ENV / NONE), surfaced as a `[GATE]` HUD line, and emitted to
// `OutputDebugString` so the gate is visible without tailing logs.
//
// Architecture-borrowed from andperks6/Kenshi-Online (commit 71b6dd2);
// extracted here into a dedicated module for cleaner separation from core.cpp.

#include <string>

namespace kmp::hook_gate {

// Returns true if the named hook should be skipped. Logs the resolved gate
// configuration on its very first call.
bool IsDisabled(const char* name);

// Force the cache to populate and emit its loud startup banner. Call this
// once during Core::InitHooks before checking individual hooks so the
// diagnostic banner lands at the top of the log instead of next to the
// first skipped hook.
void EnsureLoaded();

} // namespace kmp::hook_gate
