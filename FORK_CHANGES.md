# Fork changes — `mattebin/Kenshi-Online` `stability/upstream-base`

Fork of `The404Studios/Kenshi-Online` carrying stability and correctness
fixes for Kenshi 1.0.68 (Steam, "Newland"). Branch is fast-forward
mergeable into upstream `main`.

This branch incorporates work from two debugging tracks running in parallel
on different forks of upstream — our own original work, and fixes from
`andperks6/Kenshi-Online` that converge on the same root causes. Each
borrowed commit credits the fork it came from in its message.

The commits below are listed newest-first.

---

## Fixes shipped on this branch

| # | Commit | Source | Area | Summary |
|---|---|---|---|---|
| 27 | `2cb41d4` | original | MP correctness | **Two server-side cross-client bugs found and fixed via `KenshiMP.IntegrationTest`.** (1) `HandleDisconnect` was preserving entities for reconnect but never broadcasting `S2C_EntityDespawn` for them — other clients kept ghost squads of disconnected players on screen. Now sends `EntityDespawn` per owned entity before `PlayerLeft`. (2) `HandleBuildRequest` used `BroadcastExcept(player.id, ...)` for `S2C_BuildPlaced` — placer never got their own confirmation. Switched to `Broadcast` (matches inventory/squad/faction patterns). IntegrationTest score: 64/66 → **70/70** after fixes. Both bugs invisible on single-PC, found only by running 2 simulated clients against the same server. |
| 26 | `e40c274` | original | diagnostics | **Three runtime analyzers — `field_diff` + `concurrency_watch` + `leak_watch`.** Close the runtime gaps that install-time analysis can't see. `field_diff` snapshots a pointer arg pre-call and verifies expected fields are non-null post-call (catches AICreate-class "ran but didn't initialise" bugs). `concurrency_watch` is an RAII per-hook depth counter that warns once on same-thread reentrancy or different-thread collision. `leak_watch` snapshots process memory + registered collection sizes every 5 minutes; after ≥3 snapshots, any monotonically-growing collection gets a warn log. All three feed their summaries into the `install_audit::Emit` block so one grep gets you everything. |
| 25 | `6ad2dc4` | original | diagnostics | **Install audit log block.** Emitted once at end of `Core::InitHooks`. For every installed hook: name, target address, RVA, install/enable flags, MovRaxRsp-fix flag, live call/crash counters, prologue hex (8 bytes), prologue-analyzer arg-count + confidence, callsite-analyzer arg-count + confidence. Surrounded by `=== KMP HOOK AUDIT BEGIN ===` / `=== END ===` so it's grep-and-paste for bug reports. |
| 24 | `0a124a4` | original | diagnostics | **Call-site analyzer.** Companion to the prologue analyzer. Walks Kenshi's `.text` for any `call rel32` whose displacement resolves to the hook target. Walks back ~512 bytes from that CALL recognising arg-setup patterns (`mov RCX/RDX/R8/R9`, `lea`, `xor self`, `mov [rsp+0x28+]`). Inferred arg count from the caller side cross-checks the prologue analyzer. When both agree we trust the typedef; when they disagree there's real signal worth investigating. |
| 23 | `9c0c083` | original | diagnostics | **Prologue analyzer for hook arg-count verification.** Static x64 prologue/early-body scanner that infers a function's `__fastcall` arg count by looking for home-space register spills (`mov [rsp+0x08/0x10/0x18/0x20], RCX/RDX/R8/R9`) and stack-arg reads (`mov rXX, [rsp+0x28+]`). At hook install time, every site calls `VerifyArgCount(name, target, expectedCount)` — agreement → info log, high-confidence disagreement → warn log. Would have caught the AI::create 2-vs-6-arg bug at install in one log line, instead of a multi-hour bisect. Wired into AICreate, AIPackages, CharacterDeath, CharacterKO, FactionRelation, ItemPickup, ItemDrop, BuyItem, CharacterCreate. |
| 22 | `d2a7828` | original | MP correctness | **Stop infinite retry on cap-rejected spawns.** The cap-rejected branch in `Hook_CharacterCreate` was calling `RequeueSpawn` without bumping `retryCount` — the spawn manager kept popping the same request, hitting the same full cap, and re-queuing forever. Surface area got bigger when the cap was raised to 32. Now properly increments the retry counter so `MAX_SPAWN_RETRIES` (200, ~10s) eventually fires the drop-with-warn. **Not in andperks6 or muddxyii — original.** |
| 21 | `1d7abf0` | original | MP scaling | **Raise per-player spawn cap from 4 to 32 (configurable).** Upstream `MAX_SPAWNS_PER_PLAYER = 4` (commit `ef8d242`) silently dropped everything past the first 4 chars per remote player — catastrophically low for the project's 16-player co-op goal since vanilla Kenshi squads regularly exceed 4. New default is 32 (above vanilla squad cap of 30, leaves a safety margin). Exposed as `ClientConfig::maxSpawnsPerPlayer` in `client.json`, refreshed on every `ResumeForNetwork` so retuning takes effect on reconnect. Clamped to `[1, 256]`. **Not in andperks6 or muddxyii — original.** |
| 20 | `cdd9209` | borrowed (andperks6 `3d5bdfc`) | crash safety | **Permanent CharacterCreate passthrough.** The MovRaxRsp naked detour wrapping `CharacterCreate` is unsafe for sustained runtime interception. andperks6 bisect traced intermittent zone-stream NPC crashes to the post-load full-body re-enable. Hook now stays in lightweight passthrough (timestamp + counter + factory capture) for the entire DLL lifetime. char_tracker_hooks (animation tick) covers active-character discovery from a stable game-tick context that doesn't go through MovRaxRsp. |
| 19 | `e903674` | borrowed (andperks6 `0385189`) | MP correctness | **Position read fallback chain.** AnimClass-chain position read returns zero on the first ~1-2s after world load while animClass is still being populated. Sending a zero position made the server interpret it as a teleport. Now reads `char + offsets.character.position` first (works frame 1), falls back to AnimClass chain. |
| 18 | `4d47939` | borrowed (andperks6 `2d1a04c`) | MP correctness | **OIS keyDown/keyUp swallow.** Fixes the double-input bug where typing in chat or interacting with the multiplayer menu also drove Kenshi's game actions. WndProc handles overlay input, but Kenshi reads keyboard separately through OIS — both consumers received the same keystroke without this hook. |
| 17 | `8d7e8d3` | borrowed-arch (andperks6 `71b6dd2`) | diagnostics | **`KMP_DISABLE_HOOKS` runtime gate.** Per-hook bypass switch via `kmp_disable_hooks.txt` file or env var. Lets the next person bisecting a crash flip individual hooks (or all of them) without recompiling. Saved several hours during the AICreate bisect; preserved here for future debugging. Implementation is a clean module under `sys/hook_gate.{h,cpp}` rather than inlined in core.cpp. |
| 16 | `e1bea58` | borrowed (andperks6 `f5330f9`) | combat | **AI::create is 6-arg, not 3-arg.** Our previous 3-arg fix forwarded RCX/RDX/R8 but left R9 + stack args 5/6 as garbage. The function uses arg 5 / arg 6 to populate `this+0x318` / `this+0x10`, and AI scoring later dereferences `this+0x318` — `null` causes the deferred crash at `game+0x59820D`. andperks6 went deeper into the binary than we did and found the full signature. |
| 15 | `de03769` | original | docs | README fork-notice section linking to `FORK_CHANGES.md`. |
| 14 | `c933ab5` | original | docs | `FORK_CHANGES.md` summary doc for upstream review. |
| 13 | `6737f12` | original | combat | First proper `ai_hooks` fix — 3-arg signature + install+disable+ResumeForNetwork lifecycle + minimal pass-through bodies. **Superseded by `e1bea58` (6-arg).** Kept in history so the bisect trail is reviewable. |
| 12 | `ff4edd0` | original | combat | Initial workaround for the AICreate bug (skip install). **Superseded by `6737f12` and then `e1bea58`.** Kept in history for traceability. |
| 11 | `5e6d2a9` | original | diagnostics | **Watcher subsystem.** Coarse, flush-forced `WATCH/HOOK`, `WATCH/PKT`, `WATCH/TICK`, `WATCH/SYNC`, `WATCH/POS` markers. Designed to localize "where were we when Kenshi died" when the host process is terminated outside in-process exception coverage. Gated on `verboseWatchLog` config flag. |
| 10 | `23c8ef8` | original | MP correctness | **Faction-pointer identity, not name.** Avoids the 18-character `Player 1` name-collision swap in the kenshi-online.mod placeholder squad. Discovery converts the well-known `Player 1` / `Player 2` names into faction pointers once; subsequent re-validation uses `FindByPtr` instead of `FindByName`. |
| 9 | `8d59ee8` | original | Steam compat | **Auto-discover `CharacterHuman` backpointer offset.** GOG had it at `+0x2D8`; Steam (and likely other current builds) has it elsewhere. Walks candidate slots and validates pointer-shape, vtable in module range, and `+0x18` `std::string` size; falls back to GOG `+0x2D8` if no candidate validates so GOG behavior is unchanged. (andperks6 has independent equivalent in their char_tracker_hooks.cpp; ours predates theirs.) |
| 8 | `b661c66` | original | scanner | **`allowUnaligned` pattern flag.** `CharAnimUpdate`'s pattern targets `mov rcx,[rbx+0x320]; mov [rbx+0x37C],sil` *inside* the function body — the resolved address is intentionally non-16-byte-aligned. Standard alignment check rejected it as "likely mid-function hit." (andperks6 has equivalent in commit `787ed96`.) |
| 7 | `f902236` | original | crash safety | **`LOCK` prefix on MovRaxRsp wrapper depth counter.** Without `LOCK`, plain `inc/dec dword [mem]` is atomic on a single core but not across cores — two threads racing on the reentrancy counter could both observe `depth==1`, both take the wrapper's normal-path, both write to the per-hook global save slots, and one then `RET`s to a corrupted return address. Silent fast-fail termination, no exception path catches it. **Not in the andperks6 fork.** |
| 6 | `2b80dac` | original | MP correctness | **Strip `.mod` suffix from server-sent faction strings.** Server sends `"10-kenshi-online.mod"` because Kenshi internally addresses faction records by their full mod-qualified name; lookup tables only matched the unsuffixed forms. Every real session got `Unknown faction` until this fix. Log-once instead of per-frame. (andperks6 has equivalent embedded in `0385189`.) |
| 5 | `bac5445` | original | crash safety | **Engine null-deref VEH recovery.** Pattern-scans `kenshi_x64.exe` at init for `movss xmm0,[rax+0x90]; mulss xmm0,[rax+0x34]` — the recurring engine null-deref. VEH redirects RAX to a static zero buffer when the AV hits and resumes execution. Pattern is rebuilt at runtime so the fix works across Kenshi binary revisions. **Not in the andperks6 fork.** |
| 4 | `ad37352` | original | crash safety | Defer `CharacterCreate` hook arming until spawn manager is ready. |
| 3 | `95d4525` | original | crash safety | Keep `CharacterCreate` hook bypassed during preset/race selection. |
| 2 | `f068805` | original | MP correctness | Resume sync after main-menu join + world load (was deferred forever). |
| 1 | `9646bf9` | original | crash safety | Port "safe local testing" guards from old branch. |

---

## What this branch has that andperks6 doesn't

- **Engine null-deref VEH recovery** (`bac5445`) — pattern-scanned, build-agnostic.
- **`LOCK` prefix on MovRaxRsp depth counter** (`f902236`) — fixes a silent cross-core race.
- **Faction-pointer identity tracking** (`23c8ef8`) — avoids `Player 1` name swap.
- **Watcher trace markers** (`5e6d2a9`) — flush-forced "where were we when Kenshi died" breadcrumbs.
- **Auto-discover offset path** (`8d59ee8`) — independent of theirs, slightly older.
- **Per-player spawn cap raised + configurable** (`1d7abf0`) — was 4, now 32 (configurable).
- **Cap-rejected spawn retry budget** (`d2a7828`) — fixes infinite retry loop.
- **Hook prologue analyzer** (`9c0c083`) — would have caught the AICreate 2-vs-6-arg bug at install time.
- **Hook call-site analyzer** (`0a124a4`) — independent cross-check on prologue analysis.
- **Install audit log** (`6ad2dc4`) — one greppable block summarising every hook for bug reports.
- **Field-diff / concurrency-watch / leak-watch analyzers** (`e40c274`) — runtime layer that catches struct-field corruption, unsafe concurrent reentry, and long-session memory leaks.
- **Two server-side MP bugs** (`2cb41d4`) — EntityDespawn-on-disconnect missing, BuildPlaced confirmation missing for placer. Found by IntegrationTest, fixed and verified 70/70.

## What andperks6 has that this branch doesn't (yet)

- **`/probe` command extensions** (`6943ffc`) — debugging aid, list/`<addr>`/iterator fallback. Low priority.
- **Host-flow connect-immediately** (`0e52cc0`) — different angle on our `f068805` resume-sync fix; may be combinable.
- **Mod gamestart cleanup script** (`7c09c87`) — removes broken `Player N` squads from `kenshi-online.mod` so vanilla starts work. We have ad-hoc Python scripts that patch combat stats; theirs is more comprehensive.
- **KenshiLib offset migration plan** (`0ab0974`) — multi-phase rebuild plan to replace hand-copied offsets with KenshiLib typed accessors. Long-term direction; not blocking.
- **Build/install consolidation** (`467146f`) — single canonical install.bat, mod install required, no PowerShell auto-patching.

## What `muddxyii/Kenshi-Online` adds (orthogonal — server hosting)

`muddxyii` is a server-hosting / packaging fork, not engine-side. Their work includes:
- UPnP rewrite without COM dependency (~577 lines, replaces `natupnp.dll` with manual SSDP/HTTP)
- `KENSHI_DIR` cmake cache var so build doesn't assume repo is inside Kenshi install
- Release packaging bundles for end-users
- README rewrite for player-first setup

These are independent improvements that wouldn't conflict with anything in this branch.

---

## Verification status

Every commit has been verified single-PC (one Kenshi client + local server,
sometimes joining own server). **No two-machine, two-Steam-account session
has happened yet.**

Mechanically-verified state on a single PC:
- Client launches via OGRE plugin loader (`Plugins_x64.cfg` carries
  `Plugin=KenshiMP.Core`)
- Save loads with the kenshi-online mod (Player 1 squad spawns)
- Connection to a local server completes through handshake → entity hooks
  resume → AI hooks resume
- **SP combat works**, NPCs fight back normally
- **MP combat works** for the player attacking NPCs in single-PC test
- Watcher trace markers fire and flush
- Engine null-deref recovery handler catches and recovers
- Faction identity is stable across the 18-character `Player 1` placeholder
  squad

**Automated cross-client MP coverage:**
> `KenshiMP.IntegrationTest.exe` spawns its own server, connects two
> simulated clients, and runs **70 assertions** covering handshake,
> entity spawn + broadcast, position sync (both directions), chat
> relay, disconnect cleanup (incl. EntityDespawn), time sync, multi-
> entity per player, inventory add/remove, trade accept, squad
> creation broadcast, faction-relation sync, building placement +
> dismantle, server-browser query, and full end-to-end session.
> Currently passing 70/70 against the head of `stability/upstream-base`.

**Gate test for declaring co-op multiplayer functions in real
two-machine play:**
> Two PCs, two Steam accounts. Client A attacks client B's character with
> fists. Watch HP drop on both sides via `WATCH/PKT C2S_CombatKO` →
> `WATCH/PKT S2C_CombatKO` in both clients' logs.

Until that test runs and passes, treat the MP-correctness fixes as
"compiles, launches, doesn't crash, looks right in single-PC tests +
70/70 on automated 2-client integration tests."

---

## Open follow-ups (not in this PR)

These are deferred, called out in source comments as `TODO`, and not
blocking this branch's mergeability:

- **`movement_hooks` install.** `CharacterMoveTo` and `CharacterSetPosition`
  both have `mov rax, rsp` prologues *plus* a 5th stack parameter for
  moveType. The current MovRaxRsp wrapper can't forward stack params
  correctly; the hooks are deliberately not installed. Position sync uses
  polling from `OnGameTick` as a workaround.
- **Move `MarkRemoteControlled` into `packet_handler::HandleSpawnEntity`.**
  Currently `ai_hooks::IsRemoteControlled()` has no consumer (only used by
  `movement_hooks` which isn't installed). When `movement_hooks` is fixed,
  the correct place to mark remote chars is at the packet handler — not
  from inside the AI hook.
- **Migrate input_hooks RVAs to string-xref pattern.** The current
  hardcoded RVAs (`0x00360680` keyDown, `0x003608F0` keyUp) work for
  Kenshi 1.0.68 Newland; future builds with shifted RVAs will fail to
  install (caught by the warn-and-degrade path, no crash).
- **Borrow `/probe` extensions and host-flow connect-immediately from
  andperks6.** Both are useful but neither blocks this branch.
- **KenshiLib offset migration.** The longer-term direction is to replace
  hand-copied offsets with typed accessors from `BFrizzleFoShizzle/KenshiLib`.
  andperks6 fork has a multi-phase plan in `docs/plans/`. Worth aligning
  with eventually.
