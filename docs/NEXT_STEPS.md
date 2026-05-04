# Next steps — backlog and plan

State as of 2026-05-04 after the first real two-machine MP session.
Reading order: P0 items unblock everything; P1 items improve UX once
P0 lands; P2 items are nice-to-have. Effort estimates are honest
"focused evening" units.

## What works (don't regress)

- Network handshake, chat, presence sync, position sync (per HUD).
- Installer + zip, both rebuilt with current DLL, idempotent install.
- Chat-input gate via WndProc + MyGUI hook (verified on Kenny).
- `.pdata` mid-function recovery in factory + OIS hooks.
- `PLAYING_TOGETHER.md` covers the connect-then-load-save flow.
- Cloud port-forward test workflow.
- The `speed_probe` background watcher (env-gated, dormant by default).

## P0 — the single blocker

### Map Kenshi 1.0.68's actual GameWorld layout

This unblocks BOTH remote character spawning AND speed sync. Every
other ambitious feature dead-ends at the same place: we don't have
ground-truth offsets/types for 1.0.68, only KenshiLib's 1.0.51
reference.

**Status as of 2026-05-04 evening:** a four-pass Ghidra static
analysis closed in on the answer but couldn't quite finish (see
[`docs/reverse-engineering/REPORT.md`](reverse-engineering/REPORT.md)).
Confirmed: `RootObjectFactory::createRandomSquad` is at RVA `0x583A10`
on 1.0.68 (matches KenshiLib's 1.0.51 reference), and our existing
hooks at `0x583400` / `0x5836E0` are on the **right functions** —
they just only fire for dynamically-created characters, not for
characters loaded from save (which is most of them). The right
universal capture point is `addToUpdateListMain`, which fires for
both factory- and save-loaded characters. KenshiLib's RVA for that
(`0x786A60`) is wrong on 1.0.68 — function moved. The unordered_set
it writes to (`charUpdateListMain`) probably still starts at
`GameWorld + 0x750`, but its internal MSVC STL layout differs from
what `game_world_iter.cpp` codes for, which is why that walker
returns 0 even when characters are present.

**Next concrete step (one focused evening), in order:**

1. **`KenshiOnlineRecon5.py`** — one more targeted Ghidra pass.
   Decompile `FUN_140581770` (CharacterSpawn) in full and find the
   helper it calls near the end that writes into the `+0x7??` region
   of a GameWorld pointer — that's `addToUpdateListMain` on 1.0.68.
   ~15 min. Tells us the RVA + the exact write offset, narrowing
   ReClass.NET's job.

2. **ReClass.NET** at runtime — point it at the live GameWorld
   instance (we know how to resolve the pointer via the function-disasm
   fallback the orchestrator already uses), navigate to `+0x750`, and
   identify the actual MSVC unordered_set field types one click at a
   time. Same for `+0x700` (frameSpeedMult). With v5's narrowed target,
   this is ~30 min.

3. Update `game_world_iter.cpp` and the speed offsets with the
   verified layout — single-file changes — and the existing
   infrastructure should immediately start producing correct counts +
   sane speed reads.

What we know from session logs:

| Path | Status on 1.0.68 |
|---|---|
| `?ou@@3PEAVGameWorld@@EA` exported symbol | NOT exported (was on 1.0.51) |
| `GameWorldSingleton` slot found via func-disasm `GameFrameUpdate` nth=4 | ✓ works, returns valid slot — dereferenced GameWorld pointer is real |
| Static RVA `0x1AAE060` for `ou` | not validated on 1.0.68 |
| `GameWorld + 0x4A0` (theFactory) | NOT validated — never read directly |
| `GameWorld + 0x700` (frameSpeedMult) | reads garbage `~3.3e35` — offset shifted |
| `GameWorld + 0x750` (charUpdateListMain unordered_set) | `Count()` returns 0 even with characters present — either offset shifted or MSVC STL layout differs from libstdc++ shape we coded for |
| `GameWorld + 0x8B9` (paused), `+0x8B0` (zoneMgr) | look correct in the speed_probe runs (vtables in module range, paused is 0/1) |

So **some** offsets at the back of the struct (0x8B0+) survived; the
middle (0x4A0..0x800) shifted. Likely fields were added or changed type
in a 1.0.51 → 1.0.68 update.

**Subtasks (do in order):**

1. **Pick a static analysis tool and load `kenshi_x64.exe`** (see RE
   tools section below — recommendation: Ghidra, free).
2. **Find `RootObjectFactory::create` / `createRandomChar`.** Search
   for string xrefs to "kenshi-online.mod" or known character template
   names. Walk back from where those are referenced — the calling
   functions are the spawn factory.
3. **Find `GameWorld::charUpdateListMain` access pattern.** Set a
   breakpoint with x64dbg on `GetCharacterUpdateList()` (RVA `0x663BE0`
   on 1.0.51 — won't be at that exact RVA, find by xref) and read off
   what offset the function loads from. That's the actual offset of
   `charUpdateListMain` on 1.0.68.
4. **Verify `frameSpeedMult`'s actual offset** by Cheat Engine
   "value 1.0 → press 2× → value 2.0" scan. Walk back from the slot
   address to find which `GameWorld + N` it lives at on 1.0.68.
5. **Update `KenshiMP.Core/game/game_types.h`** with the verified
   offsets, marked `// Kenshi 1.0.68 Newland — verified <date>`.
6. **Re-test the wired-up `game_world_iter::Count()`** — should
   match what Kenshi's HUD shows once offsets are correct.

Once step 5 is done, spawn sync becomes a small follow-up:

```
diff_t snapshot = game_world_iter::Snapshot();
for each new Character* in snapshot vs last_snapshot:
    extract pos/faction/health/inventory
    send S2C_RemoteCharacterSpawn to other clients
for each removed:
    send S2C_RemoteCharacterDespawn
```

That's an evening of work AFTER the offsets are nailed.

## P1 — UX wins waiting on no blockers

### Phase tracker stuck on `Loading` in single-player

Session 22636 showed `phase=Loading gameLoaded=false` for the entire
2-minute single-player run, even though the user was actually in-game.
The phase only advances cleanly via the multiplayer "connected before
load" path. In single-player launches, `OnGameLoaded` never fires —
which means single-player diagnostics (like the `game_world_iter`
sample we tried to run without connecting) silently never run.

**Fix:** investigate why the Loading→GameReady transition needs the
Connected state. Likely the smooth-frame heuristic in `render_hooks`
defers to `core.IsConnected()` somewhere it shouldn't.

### "[Player 1]" labels on every visible character

Two-player session screenshots show every character in the world
labeled `[Player 1]` — the placeholder squad name from
`kenshi-online.mod` is bleeding through. There IS a fix in
`FORK_CHANGES.md` commit `23c8ef8` ("Faction-pointer identity, not
name") but it's clearly not catching all paths. The label-rendering
path on 1.0.68 isn't going through the faction-by-pointer lookup.

**Fix:** find where character display name is computed (probably a
MyGUI binding to a Character method), hook it, return the right
faction-derived name. Or — simpler — make the kenshi-online.mod
pre-assign per-slot unique names so the placeholder collisions don't
happen at all.

### "Wrong IP" silently disconnects

Kenny's session 28904 log: he typed `31.208.26.17` (typo) instead of
`31.208.67.17`. The connect attempt timed out with
`Disconnected from server (reason: 0)` — no UI feedback that he typed
the wrong IP. Cost him 30 seconds of confusion.

**Fix:** when `NetworkClient::Disconnected (reason: 0)` fires within 5s
of `Async connecting`, surface a chat message: "Couldn't reach <ip>:<port>
— is the IP correct? Is the host's port forwarded?".

### Auto-update detection in the Injector

Right now reinstalling requires re-running the installer manually.
The Injector could check the GitHub Releases API on launch and prompt:
"v0.1.1 is available, install now?". Lower the friction on iterating.

## P2 — nice-to-have

- **Linux server** (cherry-pick andperks6 commit `0469823` — adds
  Linux build + Docker image + Tailscale-friendly compose). Already
  surveyed; clean cherry-pick.
- **Discord rich presence** — show "Playing Kenshi-Online with N
  others on <server name>" in Discord.
- **In-game player name customization beyond Player 1/2** — needs
  the placeholder squad fix from P1 first.
- **Master server browser** — `KenshiMP.MasterServer.exe` is built but
  not deployed. Wire it up so players don't have to share IPs manually.

## P3 — research, no commitments

- **Shared-save sync** (the holy grail): both players in the same
  world, same buildings, same NPC state, same time. Currently each
  player loads their own save. Big lift; needs the P0 work first.

---

# Reverse-engineering tools — pick by what you're doing

For mapping Kenshi 1.0.68's binary layout, you need a static analyzer
and a runtime debugger. All free options are listed first.

## Static analysis (look at the binary)

| Tool | Cost | Best for | Notes |
|---|---|---|---|
| **Ghidra** | Free | Function discovery, decompilation, xref analysis | NSA-released, comparable to IDA for almost everything. Has a built-in C-like decompiler. **Recommended starting point.** |
| **Cutter** | Free | Lighter alternative to Ghidra | Built on rizin/radare2, modern UI. Good if Ghidra feels heavy. |
| **IDA Free** | Free | x86/x64 disassembly | The free version of IDA. No decompiler. |
| **IDA Pro** | $1.5k+/yr | Same as Ghidra but commercial | Industry standard. Decompiler is excellent. Skip unless you're going pro. |
| **Binary Ninja** | $300+ | Mid-tier, modern, scriptable | Cleaner UX than Ghidra. Worth it if you'll do a lot of RE work. |

**Workflow with Ghidra for our case:**
1. File → Import `kenshi_x64.exe`.
2. Run auto-analysis (default options are fine).
3. Search → For Strings → look for `"kenshi-online.mod"`,
   `"frameSpeedMult"`, `"charUpdateListMain"`, etc.
4. Right-click a string → References → see who reads it.
5. Walk the calling function with the decompiler view (Ctrl+E).

## Runtime debugging (watch the binary execute)

| Tool | Cost | Best for | Notes |
|---|---|---|---|
| **x64dbg** | Free | Live debugger, breakpoints, memory inspection | The standard. Attach to `kenshi_x64.exe`, set bp on a function, see register state when it hits. **Required for finding hook targets.** |
| **Cheat Engine** | Free | Runtime value scanning, pointer scanning | THE tool for "what address holds the speed value?" Scan for 1.0 → press 2× → scan for 2.0 → repeat until one address remains. |
| **Process Hacker / System Informer** | Free | Process tree, module list, handles | "What DLLs are loaded? What's the IAT?" Lighter than x64dbg for inspection-only tasks. |
| **API Monitor** | Free | Hook Win32 / COM calls | Useful for seeing what `kenshi_x64.exe` calls into the OS / DirectX / Steam SDK. |

## Struct mapping (turn pointers into typed views)

| Tool | Cost | Best for | Notes |
|---|---|---|---|
| **ReClass.NET** | Free | Inferring struct fields at runtime | Point it at a known `GameWorld*` and it shows you the bytes as a navigable struct. Click a slot → "this is a pointer" / "this is a float" → save the struct definition. **The fastest path for our `GameWorld + 0x???` layout problem.** |
| **PE-bear / CFF Explorer** | Free | PE inspection (sections, imports, exports) | Useful for confirming "is `ou` exported?" and finding section RVAs. |

## Recommended order for our backlog

For step 1-4 of the P0 plan above:

1. **Ghidra** to find function entry points and search by string xref.
2. **Cheat Engine** to nail `frameSpeedMult`'s exact offset by
   value-scan-and-change.
3. **ReClass.NET** pointed at the GameWorld pointer (we know how to
   resolve it via the function-disasm fallback) to map the struct
   layout one field at a time.
4. **x64dbg** for "set a breakpoint on `addToUpdateListMain` and see
   what `RCX`/`RDX` look like when characters spawn." This validates
   that the function we picked is the right one.

All four are free. Ghidra + Cheat Engine alone gets you 80% of the
way. None of this needs to happen tonight.

## Existing reference material in this repo

- `docs/SPEED_SYNC_LEAD.md` — KenshiLib offsets table + dead-end
  catalog from prior speed-sync hunts.
- `KenshiMP.Core/sys/speed_probe.{h,cpp}` — env-gated background
  watcher that polls `gameWorld + 0x000..0x1000` for sane floats.
  Useful for "did my new offset guess pan out?" without rebuilding
  the whole sync layer.
- `KenshiMP.Core/game/game_world_iter.{h,cpp}` — the unordered_set
  walker. Currently returns 0 on 1.0.68 (MSVC vs libstdc++ shape).
  When you fix the offsets/layout, the rest of the code is ready.
