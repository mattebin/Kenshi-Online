# Known Issues — Kenshi-Online (`coop-stability-2026-04` branch)

This file is the handoff for whoever attacks the remaining bugs next. Everything here was reproduced on this fork's build of [`The404Studios/Kenshi-Online`](https://github.com/The404Studios/Kenshi-Online) at upstream commit `6afb163` against Kenshi v1.0.65 (Steam, English).

## 1. Silent termination on first connected `CharacterCreate`

**Severity:** blocker for actual play.
**Reproduced:** four separate sessions on this fork, multiple sessions on upstream before that. 100 % rate.

### Symptom

- Mod loads, hooks install cleanly, save loads, auto-connect succeeds, HUD shows player name + ping.
- Within 10–30 s of connecting, Kenshi disappears from the task list with no Windows error dialog.
- `KenshiOnline_<PID>.log` truncates **immediately after** a single line of the form:
  ```
  SpawnManager: VALIDATED template '<NPC NAME>' from char+0x40 = 0x... (mgr=0x7FF70F254130)
  ```
  The trailing NPC name varies (`Hungry bandit leader`, `Slavemonger`, `Nomad Animal Trader`, …) — whatever the engine spawned first after the multiplayer hook re-enabled. The terminator is not the NPC; it's *whatever happens next on the same call stack*.
- `KenshiOnline_CRASH.log` has **no VEH entry** for the affected sessions. The fault path bypasses the in-process `AddVectoredExceptionHandler` chain — almost certainly `__fastfail` / structured fail-fast or a deferred async crash on a thread without VEH.

### Where to look

The crash is somewhere in the call stack:

```
Hook_CharacterCreate                       (entity_hooks.cpp ~line 540)
  ├─ CallOriginalCreate(factory, td)       MovRaxRsp wrapper, returns the new char
  ├─ SEH_FeedSpawnManager(factory,td,c)    -> SpawnManager::OnCharacterCreate
  │    └─ logs "VALIDATED template '...'"  ← LAST LINE BEFORE TERMINATION
  ├─ (NPC hijack path — usually skipped, no queued spawn req for first NPC)
  ├─ SEH_FeedSpawnManager (again, harmless)
  ├─ SEH_ConnectedPostProcess              -> SEH_ReadAndRegisterEntity
  └─ s_hookDepth--; return character;      then back through the wrapper to Kenshi
```

Two earlier same-signature crashes (sessions PID 20516 and PID 39408) **were** caught by VEH, with these registers:

```
RIP=0x00007FF70D764365 (game+0x644365)
AV: READ at 0x0000000000000090
RAX=0x0000000000000000  RCX=0x000000018C1004C8 / 0x0000000060D10BE0
Last CharacterCreate: #0   ← so this fired BEFORE our hook even triggered
OnGameTick step: -1 (init), tick #0
```

`RIP=game+0x644365` plus `AV READ at 0x90` from a null `RAX` is a Kenshi engine null-deref reading offset `0x90` of an object that was supposed to be live. The same site is what's terminating us silently in the post-spawn path — once the mod's `SpawnManager` has touched its first connected NPC, some Kenshi-side accessor at this site reads through a now-stale pointer.

### Things already tried on this fork (didn't fix it)

| Attempt | Result |
| --- | --- |
| Defer `HookManager::Disable("CharacterCreate")` to `OnGameTick` instead of calling it from inside the live detour stack | No effect — crash still fires. (The hook isn't the problem; it's what runs after.) |
| F1 readiness gate in `render_hooks.cpp` | Cosmetic. Removed in a later commit because it just hid the multiplayer panel; codex's button-level `CanUseJoinFlow()` is the actual safety gate. |
| `shared_save_sync` faction-suffix `.mod` strip + log-once | Fixes a 33 k-line/session log spam. Unrelated to the crash. |
| Stop promoting runtime NPC fallback faction onto `PlayerController::localFactionPtr` | Closes a wrong-faction trap (was binding the local player to e.g. a Slavemonger). Unrelated to the crash itself; crash continues without it. |

### What hasn't been tried and probably should be

1. **Attach a debugger to a fresh Kenshi process before hitting Connect.** WinDbg or x64dbg with the project PDBs from `build-codex/bin/Release/`. Set a write breakpoint on `*entity_hooks::s_pendingCreateDisable`, a code breakpoint on `game+0x644365`, and a code breakpoint at the end of the MovRaxRsp wrapper's normal-path `RET` (offset `OFF_NAKED_STUB + ~0x40` in the per-hook page reported in the log as `MovRaxRspFix: 'CharacterCreate' naked detour at 0x<page>`). The single-step output around the moment of termination should disambiguate engine deref vs. wrapper exit corruption in seconds.
2. **AddVectoredExceptionHandler with `FIRST_HANDLER` + an `UnhandledExceptionFilter` Win32 callback.** VEH only catches what the OS exception dispatcher delivers; fail-fast (`__fastfail` / `RaiseFailFastException`) explicitly skips that pipeline. Pair the two so we catch *something* the next time it dies.
3. **Diff against running with no save / a fresh `Singleplayer` start with only `kenshi-online.mod` enabled vs. the same start with a vanilla character template.** The crash signature only ever appears once a Kenshi-side `CharacterCreate` has fired in the connected branch. If a save with no NPC spawning ever crashes, that rules in/out the spawn pipeline.
4. **Audit the MovRaxRsp wrapper's exit when bypass is set mid-call.** The bypass byte flip during the same call is theoretically safe (the wrapper checks bypass on entry, not exit) but worth confirming under a debugger that `OFF_SAVED_GAME_RET` is intact at the `RET` for the first connected create.

### What this fork *does* fix

| Bug | Fix | Commit |
| --- | --- | --- |
| Server tries UPnP on every start (slow, often noisy) | Off by default, opt-in via `enablePortForwarding` in `server.json` | codex pre-fork work, preserved here |
| Open-menu button dispatched into a half-initialized engine | New `SpawnManager::HasSpawnPathReady()` + `NativeMenu::CanUseJoinFlow()` block Host/Join until the spawn pipeline has captured something | codex pre-fork work, preserved here |
| Auto-connect was hardcoded off in `overlay.cpp` first-frame init | Honors `ClientConfig::autoConnect` | `1339241` |
| `shared_save_sync` flooded the log with 33 000+ "Unknown faction '12-kenshi-online.mod'" lines per session, making the game stutter | Strip the `.mod` suffix in `NormalizeFactionKey` + warn-once-per-distinct-value | `9d152fd` |
| F1 menu refused to open when `HasSpawnPathReady()` was false → mod looked entirely missing to the user | F1 always opens the panel; per-button guards are sufficient | `9d152fd` |
| Local player faction was getting bound to whichever random NPC spawned first after connect (Slavemonger, etc.), breaking every "is this entity mine?" filter for the rest of the session | Only promote `s_earlyPlayerFaction` to `PlayerController` when `s_earlyFactionLocked` is set; runtime NPC fallback is no longer written through | `249a905` |

### Telemetry to capture on the next attempt

To pinpoint the silent termination, the *minimum* useful diagnostic is to log a flush-forced marker on every line in the post-`VALIDATED` path. Suggested instrumentation (not yet committed):

```cpp
// at the end of SEH_FeedSpawnManager
spdlog::default_logger()->flush();
spdlog::info("entity_hooks: SEH_FeedSpawnManager returned (char=0x{:X})", (uintptr_t)character);
spdlog::default_logger()->flush();
```

Repeat at every internal boundary inside `SEH_ConnectedPostProcess`, `SEH_ReadAndRegisterEntity`, and right before `return character;` at the end of `Hook_CharacterCreate`. spdlog's `flush_on(debug)` is set in `core.cpp:305` but explicit flush before suspect calls eliminates any "buffered, lost on terminate" doubt.
