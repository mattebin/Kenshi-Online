# Debugging Kenshi-Online — workflow guide

The codebase carries enough introspection tooling that you should very rarely
need a debugger attached to live Kenshi. This file is the order in which to
use the tools, from "Kenshi crashed" to "I know exactly which hook is wrong."

## When something goes wrong, in order

### 1. Read `KenshiOnline_<pid>.log` for the install audit block

At the end of `Core::InitHooks` we emit a single block:

```
=== KMP HOOK AUDIT BEGIN ===
  hostModuleBase = 0x7FF6...
  hooks total = N
  ----
  [HookName] addr=... rva=... installed=true enabled=true ...
    prologue: 40 57 48 81 EC 90 00 00
    prologue-analyzer: spills 4 reg arg(s), reads up to stack arg 6,
                       inferred 6 args (90% conf)
    callsite-analyzer: callsite @0x... sets reg-mask=0xF (popcount 4),
                       max stack-arg 6, inferred 6 args (...)
    ----
  [...]
=== KMP HOOK AUDIT END ===
```

`grep '=== KMP HOOK AUDIT' -A 200 KenshiOnline_<pid>.log` gets the whole
block. It's the right thing to attach to a bug report — every hook's
install state in one place, with arg-count cross-checks already run.

### 2. Read the watcher trace markers

In `client.json` set `verboseWatchLog: true`. Restart Kenshi. The log will
now contain `WATCH/HOOK`, `WATCH/PKT`, `WATCH/TICK`, `WATCH/SYNC`, `WATCH/POS`
markers. Each is flush-forced so the breadcrumb survives even a process
termination outside our exception coverage. Useful when:

- A crash log shows `RIP=0x0` but you don't know which subsystem was running
- A hang is timing-related and you need to see ordering between sync /
  packet / tick events
- You want to confirm a hook is actually firing in the scenario you think

Off by default in production. Forces flush on every emit, so it does
slow the game a bit — only enable when actively debugging.

### 3. Bisect with `KMP_DISABLE_HOOKS`

Drop a file `kmp_disable_hooks.txt` next to `kenshi_x64.exe` containing
hook names to skip:

```
ai
chartracker
combat
```

Or set the `KMP_DISABLE_HOOKS` environment variable to the same. Set
`*` or `all` to disable everything except `render` (render is required
for chat). The resolved configuration is logged at startup with its
source (`FILE:` / `ENV:` / `NONE:`) and shown as a `[GATE]` HUD line.

This is the fast bisect: when you suspect a hook is the cause, disable
it. Restart. Reproduce. If the bug goes away you've found it; if not
add another hook to the disable list and try again.

### 4. Verify hook arg counts at install time

`prologue_analyzer` and `callsite_analyzer` run automatically during
hook install. If a typedef arg count disagrees with both analyzers'
inference at high confidence, you'll see in the log:

```
[warning] prologue_analyzer[AICreate] @0x... typedef says 2 args but
          inferred 6 (90% conf). Hook may pass garbage in unforwarded
          slots — investigate. Prologue: 40 57 48 81 EC 90 00 00
```

That single line is the smoking gun for the entire class of "calls
original with wrong arg count, function takes the error path, deferred
crash" bugs. The AI::create regression cost a multi-hour bisect; with
the analyzers in place a future regression of the same shape is one
log line.

To add the check on a newly-introduced hook, call

```cpp
prologue_analyzer::VerifyArgCount("HookName", targetAddr, expectedArgs);
callsite_analyzer::VerifyArgCount("HookName", targetAddr, expectedArgs);
```

before `InstallAt`. See `KenshiMP.Core/hooks/ai_hooks.cpp` for examples.

### 5. Read the crash log if Kenshi dies

`KenshiOnline_CRASH.log` accumulates VEH-handled crash dumps. Each has:

- Exception code + RIP (with `game+0xRVA` if the crash was inside Kenshi)
- All 16 GP registers
- `Last CharacterCreate: #N` — how far the loading burst progressed
- `OnGameTick step: K (name)` — how far through the tick loop we were
- AV details: read/write/execute + faulting address
- Stack at RSP: 16 qwords

The RIP RVA combined with the install audit block tells you which Kenshi
function crashed and what state our hooks were in when it happened.

### 6. Engine null-deref recovery

If the log shows `KMP RECOVER #N: game+0xRVA null-deref at +0x90, ...`
the VEH handler caught Kenshi's recurring null-deref and resumed
execution. The pattern is at offset `+0x90` reading from `RAX=0`.
Pattern-scanned at startup so the fix tracks Kenshi binary updates
automatically. If the recover count climbs past a few hundred per
session, that's interesting signal — the engine bug fires more often
under multiplayer load than under normal singleplayer play, so a high
count may indicate a different underlying problem.

## When you fix a hook, verify this way

1. The install-audit log block should show your hook with `enabled=true`,
   prologue analyzer agreeing with the typedef, callsite analyzer also
   agreeing, no warnings.
2. Single-PC: launch Kenshi, do the action that exercises the hook,
   confirm no `KMP VEH CRASH` lines and no `[warning] prologue_analyzer`
   lines.
3. Two-PC MP: as documented in `FORK_CHANGES.md` "Verification status",
   the gate test is — Client A hits Client B's character with fists,
   `WATCH/PKT C2S_CombatKO` then `S2C_CombatKO` round-trip in both logs,
   HP drops on both sides. Until that test runs and passes, every MP
   correctness fix is "compiles, looks right in single-PC tests."

## Adding a new analyzer

If you find yourself wanting to add a new "this would have caught my
last bug" tool, follow the existing pattern:

```
KenshiMP.Core/sys/<name>_analyzer.{h,cpp}
```

Wire it into `install_audit::Emit` so its result lands in the bug-report
block alongside the prologue and callsite analyzers. Document the
pattern it catches in the header. Add a one-line invocation in each
hook install site you want covered.

## Runtime checks that catch things install-time analysis can't

### Struct-field corruption (`field_diff`)

Constructor-style hooks (AICreate is one) initialise a `this` struct.
Even with the right arg count, a hook can land in a code path that
fails to write key fields — see the original AI::create bug where
`this+0x318` came back null and crashed AI scoring 5 seconds later.

Inside the hook body:

```cpp
field_diff::Snapshot pre = field_diff::Capture(thisPtr, /*size=*/0x400);
s_origFunction(args...);
static const field_diff::FieldExpectation fields[] = {
    {0x010, "remote-char back-ptr"},
    {0x318, "AI task table"},
};
field_diff::VerifyFieldsWritten(pre, thisPtr, fields, 2, "MyHook");
```

If a listed field comes back zero post-call (and was zero pre-call —
implying the function was supposed to fill it but didn't), the analyzer
logs at warn level. Sample at low rate inside the hook to keep cost down.
The example in `Hook_AICreate` checks every 1000th call after the first
3 — enough coverage to spot a regression, not enough to bloat the log.

### Concurrent reentry (`concurrency_watch`)

Drop `KMP_CONCURRENCY_GUARD("HookName")` at the top of any hook body to
get RAII tracking of:
- How many threads are currently inside the hook (depth)
- Which thread entered first
- Reentry from the same thread (often means the hook called something
  that called the hook — usually a bug, sometimes intentional)
- Collision from a different thread (almost always a bug — that hook is
  probably not thread-safe with itself)

Logs at most once per state to avoid flooding under sustained collisions.
Per-state counters survive the session and get dumped by `EmitSummary`,
which `install_audit::Emit` calls — so the bug-report block always
includes "did anything ever collide."

### Long-session leak detection (`leak_watch`)

Every 5 minutes of game time, `Tick()` (called from `OnGameTick`) takes a
snapshot of process working-set + private bytes plus the size of every
collection that's been registered via `RegisterSize`. After ≥3 snapshots,
any collection whose size is monotonically increasing across all of them
gets logged at warn level — the "you have a leak" signal, much earlier
than someone noticing the process at 4 GiB after a 4-hour session.

To register a new collection (do it in the owning module's `Install`):

```cpp
leak_watch::RegisterSize("hooks::s_myMap", []() {
    std::lock_guard lock(s_myMutex);
    return s_myMap.size();
});
```

Already registered:
- `ai_hooks::s_remoteControlled` — should reset on disconnect.
- `entity_hooks::s_spawnsPerPlayer` — should reset on `ResumeForNetwork`.

Add new ones whenever you write a long-lived collection that *should*
stay bounded. The minute it stops being bounded, leak_watch tells us.

## What's still missing

Honest list of bugs the current tooling would NOT have caught:

- **Cross-fork client desync.** Two clients running different code paths
  can drift over time; nothing here detects that. Would need a periodic
  state-hash exchange and divergence detection — a bigger feature.
- **Use-after-free of game-object pointers.** The codebase uses
  pointer-shape heuristics ("looks like a heap pointer?") in a lot of
  places, but if a freed pointer happens to look heap-shaped we won't
  notice. A real fix requires the engine handing us a refcount or
  generation counter.
- **Network packet validation gaps.** `PacketReader` does bounds checks
  but doesn't have a per-message-type schema. A buggy or malicious
  server could feed structured-but-wrong data and we'd accept it.

When one of these bites, write the analyzer that would have caught it
and add it to this guide.
