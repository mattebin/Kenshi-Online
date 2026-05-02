# Test plan — exercise the introspection tooling

How to confirm each analyzer actually does what it claims, on a single PC.
Each test is an action plus an exact log-grep with pass/fail criteria.

The log file is `<Kenshi dir>/KenshiOnline_<pid>.log`. PID changes each run;
get the latest with:

```
ls -t "<Kenshi dir>"/KenshiOnline_*.log | head -1
```

Run all tests with `verboseWatchLog: false` first (production config).
Re-run T11 with it set to `true`.

---

## T1. install_audit fires once at startup

**Action.** Launch Kenshi. Don't connect to a server. Don't start a save.
Just sit on the main menu for 10 seconds, then exit.

**Grep.**

```
grep -A 200 "=== KMP HOOK AUDIT BEGIN" KenshiOnline_<pid>.log
```

**Pass.** One block, bounded by `BEGIN` / `END` markers. Lists every
installed hook with: name, addr, rva, installed/enabled flags, prologue
hex, prologue-analyzer summary, callsite-analyzer summary. Block ends
with `=== KMP CONCURRENCY WATCH SUMMARY ===` and a `leak_watch:` baseline
snapshot.

**Fail.** Block missing — `Core::InitHooks` exited early before reaching
the audit emit. Look earlier in the log for `[ERR]` lines.

---

## T2. prologue_analyzer agrees with current typedefs

**Action.** Same as T1. Read the audit block.

**Grep.**

```
grep "prologue_analyzer\[" KenshiOnline_<pid>.log
```

**Pass.** Every line is `[info]`, format `typedef N args matches inferred —
spills … reads up to stack arg N, inferred N args (NN% conf)`. Confidence
should be >= 60% for AICreate, ApplyDamage, CharacterDeath, CharacterKO,
FactionRelation, ItemPickup, ItemDrop, BuyItem, CharacterCreate.

**Fail.** Any `[warning]` line — current typedef is wrong. Open Ghidra at
the reported RVA and reconcile.

---

## T3. prologue_analyzer catches a deliberate mismatch

This is the regression test. Confirms the analyzer would catch a future
AICreate-class bug.

**Setup.** Edit `KenshiMP.Core/hooks/ai_hooks.cpp`, find the
`prologue_analyzer::VerifyArgCount("AICreate", ..., 6)` call. Change
`6` to `3`. Rebuild.

**Action.** Launch Kenshi.

**Grep.**

```
grep "prologue_analyzer\[AICreate\]" KenshiOnline_<pid>.log
```

**Pass.** Single line at `[warning]` level: `typedef says 3 args but
inferred 6 (90% conf). Hook may pass garbage in unforwarded slots —
investigate.`

**Cleanup.** Revert the change back to `6`. Rebuild.

---

## T4. callsite_analyzer agrees with prologue_analyzer

**Action.** Same as T1. Read the audit block.

**Grep.**

```
grep "callsite_analyzer\[" KenshiOnline_<pid>.log
```

**Pass.** Every line either:
- `[info]` "typedef N args matches caller — callsite @0x… sets reg-mask=…"
- `[debug]` "no `call rel32` to target found in .text" (function only
  called via vtable — not a bug, just no signal)

**Fail.** `[warning]` lines saying "typedef N but caller sets M" with M > N.
That's an arg-count regression — investigate.

---

## T5. field_diff stays quiet when AICreate is healthy

**Action.** Connect to local server, load a save, walk around for 30
seconds (forces several CharacterCreate → AICreate calls).

**Grep.**

```
grep "field_diff\[AICreate\]" KenshiOnline_<pid>.log
```

**Pass.** No matches. Silent run = AICreate fields are populated as
expected post-call (this+0x010 and this+0x318 are non-null after the
6-arg call, as documented by andperks6 commit f5330f9).

**Fail.** Any `[warning]` line. Either we regressed the 6-arg signature
or the offsets in `Hook_AICreate`'s `kFields` array are wrong for this
Kenshi build. Run T3-style test to confirm.

---

## T6. field_diff catches deliberate corruption

This is the runtime regression test.

**Setup.** Edit `KenshiMP.Core/hooks/ai_hooks.cpp`. Change the typedef
back to 3-arg:

```cpp
using AICreateFn = void(__fastcall*)(void* ai, void* character, void* arg3);
```

And the hook body:

```cpp
static void __fastcall Hook_AICreate(void* ai, void* character, void* arg3,
                                     void* arg4, void* arg5, void* arg6) {
    KMP_CONCURRENCY_GUARD("AICreate");
    static std::atomic<uint64_t> s_callCount{0};
    const uint64_t n = s_callCount.fetch_add(1, std::memory_order_relaxed);
    const bool checkFields = (n < 3) || (n % 1000 == 0);
    field_diff::Snapshot pre;
    if (checkFields && ai) pre = field_diff::Capture(ai, 0x400);
    s_origAICreate(ai, character, arg3);   // ← only 3 args; arg4..arg6 dropped
    if (checkFields && pre.valid) {
        static const field_diff::FieldExpectation kFields[] = {
            { 0x010, "this+0x10 (stack arg 6 sink)" },
            { 0x318, "this+0x318 (stack arg 5 sink)" },
        };
        field_diff::VerifyFieldsWritten(pre, ai, kFields, 2, "AICreate");
    }
}
```

Rebuild. The build will fail because `s_origAICreate` is still 6-arg —
that's expected, it confirms the typedef was the protection. Don't
actually run the broken build (it would crash).

**Pass.** Compile error proves typedef-to-call-site coupling holds. If
the build succeeds, something is missing the protection — investigate.

**Cleanup.** Revert.

---

## T7. concurrency_watch reports clean run

**Action.** Same as T5 — load save, walk around, do some combat.

**Grep.**

```
grep "concurrency_watch\[" KenshiOnline_<pid>.log
```

**Pass.** No `[warning]` lines. The end-of-audit summary block lists each
hook with `entries=N (no concurrent reentry observed)`.

**Fail.** Any line with `COLLISION` (multi-thread) or `REENTRANT`
(same-thread) — that's a real concurrency bug, not a false positive.
Note the hook name and the involved TIDs; that's the smoking gun.

---

## T8. leak_watch fires baseline snapshot at startup

**Action.** Same as T1. Read the audit block tail.

**Grep.**

```
grep "leak_watch:" KenshiOnline_<pid>.log
```

**Pass.** First line is `leak_watch: workingSet=… privateBytes=… (baseline)`.
Subsequent lines list each registered collection's size. Currently:
`ai_hooks::s_remoteControlled` and `entity_hooks::s_spawnsPerPlayer`,
both should be `size=0` at startup.

**Fail.** Snapshot missing — `install_audit::Emit` didn't reach the
`leak_watch::SnapshotNow` call. Look for an earlier crash in InitHooks.

---

## T9. leak_watch fires periodic snapshot

**Action.** Launch Kenshi. Sit at main menu (or in a save) for 6 minutes.
Exit.

**Grep.**

```
grep "leak_watch: workingSet" KenshiOnline_<pid>.log
```

**Pass.** At least 2 snapshot lines. Second one shows a delta from
baseline, format `workingSet=2.34 GiB (+1234 B) privateBytes=2.55 GiB
(+1234 B)`. For a quiet main-menu session the deltas should be small
(a few KiB to a few MiB at most).

**Fail.** Only the baseline. `OnGameTick` isn't firing or `leak_watch::Tick`
isn't being called from it.

---

## T10. KMP_DISABLE_HOOKS file gate

**Action.** Create `<Kenshi dir>/kmp_disable_hooks.txt` with one line:

```
ai
```

Launch Kenshi.

**Grep.**

```
grep -E "Hook-disable gate|GATE|AI hooks SKIPPED" KenshiOnline_<pid>.log
```

**Pass.** Three things land in the log:
1. `Hook-disable gate active` block with `source: FILE: …\kmp_disable_hooks.txt`
   and `value : "ai"`.
2. `[GATE]` HUD line `DISABLE=ai (FILE: …)`.
3. `[SKIP] AI hooks disabled via KMP_DISABLE_HOOKS`.

The audit block at the end should show `installed=false` for AICreate
and AIPackages.

**Cleanup.** Delete `kmp_disable_hooks.txt`.

**Side check.** After deleting, set the env var:

```
set KMP_DISABLE_HOOKS=combat,inventory
```

and re-run. Log source should now be `ENV: KMP_DISABLE_HOOKS` with
value `"combat,inventory"`. Combat and inventory hooks `[SKIP]`'d.

---

## T11. Watcher trace markers when verbose enabled

**Setup.** In `<Kenshi dir>/client.json` set `"verboseWatchLog": true`.

**Action.** Launch, connect to local server, load save, walk for 10s.

**Grep.**

```
grep "WATCH/" KenshiOnline_<pid>.log | head -50
```

**Pass.** Mix of `WATCH/HOOK`, `WATCH/PKT`, `WATCH/TICK`, `WATCH/SYNC`,
`WATCH/POS`. Each emit fires a flush so even on a kill -9 the breadcrumb
survives. Verify by tailing the log live: `tail -F KenshiOnline_<pid>.log`
should show new WATCH lines within 1 second of the action that emits them.

**Cleanup.** Set `"verboseWatchLog": false` again. Verbose mode
significantly slows the game.

---

## T12. VEH null-deref recovery

**Action.** Launch, load save, do anything for 60+ seconds.

**Grep.**

```
grep "KMP RECOVER" KenshiOnline_<pid>.log | wc -l
```

**Pass.** Either zero matches (engine null-deref didn't fire this session
— legit) or N matches (handler caught and continued). Either is OK.
What you DON'T want is `KMP VEH CRASH` lines for the same RIP — that
means the recovery handler isn't matching the pattern this session.

**Side check.**

```
grep "KMP VEH CRASH" KenshiOnline_<pid>.log
```

Should be empty for a clean session. Any matches are real crashes; copy
the RIP RVA and feed it to the install audit block to figure out what
hook was active when it crashed.

---

## T13. End-to-end: full audit block contains all sub-summaries

**Action.** Launch, run for 6 minutes (so leak_watch gets two snapshots).
Exit.

**Grep.**

```
sed -n '/=== KMP HOOK AUDIT BEGIN/,/=== END LEAK WATCH SUMMARY/p' \
    KenshiOnline_<pid>.log | wc -l
```

**Pass.** A single contiguous block containing:
1. `=== KMP HOOK AUDIT BEGIN ===` … `=== END ===`
2. `=== KMP CONCURRENCY WATCH SUMMARY ===` … `=== END CONCURRENCY WATCH SUMMARY ===`
3. `leak_watch: workingSet=… (baseline)` followed by collection size lines

The audit block is roughly 200-400 lines for a normal install — enough
to paste into a bug report verbatim.

---

## Quick run order

For fastest coverage in one session:

1. Set `verboseWatchLog: true`, drop a `kmp_disable_hooks.txt` with
   contents `inventory` (gate test).
2. Launch Kenshi.
3. Wait 10s on main menu (T1, T2, T4, T8, T10, T11).
4. Connect to local server, load save (T5, T7, T11).
5. Walk around for 6 minutes total session length (T9).
6. Exit. Read the log.

Should give you a green tick on T1, T2, T4, T5, T7, T8, T9, T10, T11,
T12, T13 in one session. T3 and T6 require deliberate code changes —
run them once after any major refactor of the analyzers themselves.

If a test fails, the failure mode tells you exactly which subsystem
broke. That's the point.
