# Fork changes — `mattebin/Kenshi-Online` `stability/upstream-base`

Fork of `The404Studios/Kenshi-Online` carrying stability and correctness fixes
for Kenshi 1.0.68 (Steam, "Newland"). Branch is fast-forward mergeable into
upstream `main` (no conflicts at the time of writing).

The commits below are listed newest-first.

---

## Fixes shipped on this branch

| # | Commit | Area | Summary |
|---|---|---|---|
| 14 | `6737f12` | combat | **AICreate hook 3-arg signature + lazy enable.** `AI::create` is a member function (`this`, `Character*`, `Faction*`); the upstream typedef was 2-arg, dropping the real Faction* in R8. Original then took its `[AI::create] No faction for` error path on every call → faction-less AI controllers → chars could move but not attack. Hook body reduced to a minimal forwarder; install+disable+`ResumeForNetwork` lifecycle wired to mirror `entity_hooks`. Bisect 2026-05-02 in pure vanilla Kenshi confirmed root cause. |
| 13 | `ff4edd0` | combat | Initial workaround for the AICreate bug — skipped the install entirely. Superseded by `6737f12`. Keep in history for traceability. |
| 12 | `5e6d2a9` | diagnostics | Watcher subsystem — flush-forced `WATCH/HOOK`, `WATCH/PKT`, `WATCH/TICK`, `WATCH/SYNC`, `WATCH/POS` markers. Gated on `verboseWatchLog` config flag. Designed to localize "where were we when Kenshi died" in scenarios where the host process is terminated outside in-process exception coverage. Every emit goes through `spdlog::info` followed by an explicit logger flush so the breadcrumb is on disk before the next instruction. |
| 11 | `23c8ef8` | MP correctness | Identity tracking by faction *pointer*, not name. Discovery converts the well-known `Player 1` / `Player 2` names into faction pointers the first time the tracker has any matching characters; subsequent re-validation uses `FindByPtr` instead of `FindByName`. Avoids the 18-character `"Player 1"` name-collision swap that happens with the kenshi-online.mod placeholder squad. |
| 10 | `8d59ee8` | Steam compat | Auto-discover `CharacterHuman` backpointer offset. GOG had it at `+0x2D8`; Steam (and likely other current builds) has it elsewhere. Discovers the right offset on the first hook call by walking candidate slots and validating: pointer-shaped, vtable inside host module range, `+0x18` `std::string` size in [1, 256]. Caches once locked in. Falls back to the GOG `+0x2D8` offset if no candidate validates, keeping GOG behavior unchanged. |
| 9 | `b661c66` | scanner | `allowUnaligned` flag on `PatternEntry`. `CharAnimUpdate`'s pattern targets `mov rcx,[rbx+0x320]; mov [rbx+0x37C],sil` *inside* the function body — the resolved address is intentionally non-16-byte-aligned. Standard alignment check rejected it as "likely mid-function hit." The flag bypasses the rejection for patterns where mid-function is the design. |
| 8 | `f902236` | crash safety | `LOCK` prefix on MovRaxRsp wrapper depth `inc`/`dec`. Without `LOCK`, plain `inc/dec dword [mem]` is atomic on a single core but not across cores. Two threads racing on the reentrancy counter could both observe `depth==1`, both take the wrapper's normal-path, both write to the per-hook global save slots, and one then `RET` to a corrupted return address — a silent fast-fail termination, no exception path catches it. |
| 7 | `2b80dac` | MP correctness | Strip `.mod` suffix from server-sent faction strings. Server sends faction identifiers as `"10-kenshi-online.mod"` because Kenshi internally addresses faction records by their full mod-qualified name; lookup tables only matched the unsuffixed forms. Every real session got `Unknown faction` until this fix. Also log-once instead of per-frame. |
| 6 | `bac5445` | crash safety | Engine null-deref recovery handler. Pattern-scans `kenshi_x64.exe` at init for `movss xmm0,[rax+0x90]; mulss xmm0,[rax+0x34]` — the exact instruction pair that fires the recurring engine null-deref. VEH redirects RAX to a static zero buffer when the AV hits and resumes execution. Pattern is rebuilt at runtime so the fix works across Kenshi binary revisions where the RVA shifts. |
| 5 | `ad37352` | crash safety | Defer `CharacterCreate` hook arming until spawn manager is ready. Prevents crash from 130+ rapid preset character creates firing through the hook before the spawn path is valid. |
| 4 | `95d4525` | crash safety | Keep `CharacterCreate` hook bypassed during preset/race selection setup. Same class of issue as `ad37352` for the character creation screen. |
| 3 | `f068805` | MP correctness | Resume sync after main-menu join + world load. Was previously deferred forever — `ResumeForNetwork` never fired if the user joined a server before loading a save. |
| 2 | `9646bf9` | crash safety | Port "safe local testing" guards. Prevents crashes when running with a localhost server before any client is connected. |
| 1 | `f902236`'s sibling — already in upstream | — | (only listed here for completeness with the section above) |

---

## Verification status

All 13 commits ship with the same caveat: **verified on a single PC running
both client and server (sometimes joining its own server).** No two-machine,
two-Steam-account session has happened yet.

The mechanically-verified state on a single PC:

- Client launches via OGRE plugin loader, no injector required (`Plugins_x64.cfg`
  carries `Plugin=KenshiMP.Core`)
- Save loads with the kenshi-online mod (Player 1 squad spawns)
- Connection to a local server completes through handshake → entity hooks
  resume → AI hooks resume
- **SP combat:** chars attack, NPCs fight back, no crashes (this commit)
- **MP combat in single-PC test:** same as SP after connect (this commit)
- Watcher trace markers fire and flush
- Engine null-deref recovery handler catches and recovers
- Faction identity is stable across the 18-character `Player 1` placeholder squad

The first test that actually proves co-op multiplayer functions:

> Two PCs, two Steam accounts. Client A attacks client B's character with
> fists. Watch HP drop on both sides via `WATCH/PKT C2S_CombatKO` →
> `WATCH/PKT S2C_CombatKO` in both clients' logs.

Until that test runs and passes, treat the MP-correctness fixes (`#3`, `#7`,
`#11`) as "compiles, launches, doesn't crash, looks right in single-PC tests."

---

## Open follow-ups (not in this PR)

These are deferred, called out in source comments as `TODO`, and not blocking
this branch's mergeability:

- **`movement_hooks` install.** `CharacterMoveTo` and `CharacterSetPosition`
  both have `mov rax, rsp` prologues *plus* a 5th stack parameter for moveType.
  The current MovRaxRsp wrapper can't forward stack params correctly; the
  hooks are documented as deliberately not installed. Position sync uses
  polling from `OnGameTick` as a workaround. Real fix needs trampoline work.
- **Move `MarkRemoteControlled` into `packet_handler::HandleSpawnEntity`.**
  Currently `ai_hooks::IsRemoteControlled()` has no consumer (only used by
  `movement_hooks` which isn't installed). When `movement_hooks` is fixed, the
  correct place to mark remote chars is at the packet handler — not from
  inside the AI hook. AI hook can stay a minimal forwarder.
- **Verify `AI::loadPackages` 3-arg signature by binary inspection.**
  Currently the typedef mirrors `AI::create` defensively; if the underlying
  function is actually 2-arg, the extra R8 forwarding is harmless (callee
  ignores it), but the hypothesis hasn't been proven the way `AI::create`'s
  has.
- **AIPackages and AICreate hook bodies as minimal forwarders.** The bisect
  showed both SEH `__try`/`__except` wraps and `Core::Get`/`EntityRegistry`
  access from inside these hooks destabilize the engine in MP. The minimal
  forwarders are the safe pattern. If post-call work is ever needed, the
  right model is the `combat_hooks` lock-free ring buffer drained from
  `OnGameTick` — never inline in the hook body.
