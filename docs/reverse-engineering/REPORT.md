# Ghidra recon report — 2026-05-04

A four-pass static analysis of `kenshi_x64.exe` (1.0.68 Steam Newland)
in Ghidra 12.0.4. Every recon-pass output is in this folder
(`01-...txt`..`04-...txt`) plus the Python scripts that produced them
(`KenshiOnlineRecon{,2,3,4}.py`). Run them yourself via
`pyghidraRun.bat -H <projectDir> <projectName> -process kenshi_x64.exe
-noanalysis -scriptPath <scripts> -postScript <name>.py`.

The whole motivation for this round was: our v0.1.0 mod connects two
players, syncs chat, and renders position — but **no remote characters
ever appear** in either player's world. We wanted a ground-truth answer
for "what function does Kenshi 1.0.68 call when a character spawns" so
we could hook it.

## TL;DR

- `RootObjectFactory::createRandomSquad` is at RVA `0x583A10` on
  1.0.68 (same as 1.0.51 — confirmed by literal error-message strings
  in the function body).
- Our existing factory hooks at `0x583400` / `0x5836E0` are **on the
  right functions**, but those functions only fire for
  dynamically-created characters (new game, recruitment, dynamic
  spawns) — **NOT for characters loaded from save**. That's why the
  hook never fires when Kenny streams into Squin: 1.0.68 deserialises
  those NPCs from save data, bypassing the factory entirely.
- The right capture point is **`addToUpdateListMain`**, the function
  that adds any character (factory-spawned or save-loaded) to
  GameWorld's live update set. Every character — without exception —
  flows through it.
- KenshiLib's reference RVA for `addToUpdateListMain` (`0x786A60`,
  captured against 1.0.51) **doesn't match 1.0.68**. The function has
  moved and we don't yet know its new RVA.
- The unordered_set the function writes to (`charUpdateListMain`)
  appears to still live at `GameWorld + 0x750` on 1.0.68 based on the
  pattern of nearby small set-walkers — but its internal MSVC STL
  layout is different from the libstdc++ shape our `game_world_iter`
  codes for, which is why that walker returns 0 even when characters
  are present.
- Next concrete step: **ReClass.NET at runtime** to map the actual
  MSVC `std::unordered_set` layout used by Kenshi's MSVC build, then
  fix `game_world_iter.cpp` to walk it correctly. Static analysis took
  us as far as it can go alone.

## What we definitively confirmed

### `createRandomSquad` is `FUN_140583A10` on 1.0.68

Strings inside the function body (extracted by `KenshiOnlineRecon3.py`,
section 4) are literal error messages with the function's own name:

```
'[RootObjectFactory::createRandomSquad] Missing squad leader data'
'[RootObjectFactory::createRandomSquad] Missing squad squad data'
'[RootObjectFactory::createRandomSquad] Missing squad animals data'
'[RootObjectFactory::createRandomSquad] Missing squad slaves data'
'[RootObjectFactory::createRandomSquad] Missing squad prisoners data'
```

Body size 10989 bytes. Same RVA as KenshiLib 1.0.51 reference. Calls
`FUN_1405836e0` (createRandomChar) multiple times for each squad
member.

### `createRandomChar` is `FUN_1405836e0`

Same RVA as KenshiLib reference. Decompile (in
`02-decompile-broken-hook-targets.txt`) shows it calls
`thunk_FUN_140583400` (the generic factory dispatcher) and uses
strings `"random character base-copy"`, `"unique"`,
`"unique replacement spawn"`. This is the per-character creation
path inside a squad spawn.

### `RootObjectFactory::create` is `FUN_140583400`

The 730-byte multi-purpose factory dispatcher we've been hooking. Has
three branches inside: characters (calls `thunk_FUN_140581770` =
CharacterSpawn), nodes ("is node"), and foliage ("foliage"). It IS
the right function for what KenshiLib called `RootObjectFactory::create`,
but on 1.0.68 the engine doesn't always go through this for character
creation — see "What we discovered" below.

### `?ou@@3PEAVGameWorld@@EA` is NOT exported on 1.0.68

The mangled `ou` global GameWorld pointer that RE_Kenshi resolves via
`GetProcAddress` was stripped from 1.0.68's exports. We already knew
this from runtime probing; Ghidra confirms it's not in the symbol
table either.

### Speed-setter constants — 86 candidates, no clear winner

`KenshiOnlineRecon2.py` looked for functions reading the float `5.0f`
constant pool entry (RE_Kenshi's evidence that pressing "3×" sets
`frameSpeedMult = 5.0`). 86 functions read it. Most are unrelated —
expf calls, terrain math, animation timers. None of the short ones we
decompiled matched the "write 5.0 to a fixed offset of arg0" pattern
we'd expect for `setFrameSpeedMultiplier`. Either the speed setter
loads 5.0 indirectly (e.g. from a key-binding lookup table) or the
heuristic missed it. Cheat Engine's "value scan + change-and-rescan"
will identify it definitively in 5 minutes.

## What we discovered (and why our spawn hook never fires)

The two-machine session log showed:

- Engine creating 50+ NPCs in Kenny's view (visible on screen,
  `tracked: 59` in HUD)
- `[Pipeline] CharacterCreate: 0 calls after 31s` — our
  `RootObjectFactory::create` hook saw exactly zero invocations

Combined with the Ghidra decompile, the explanation is:

> Characters loaded from a save are **deserialised**, not factory-created.
> The factory functions (`createRandomSquad`, `createRandomChar`, `create`)
> are only invoked for characters that didn't exist in the save —
> recruitment, dynamic random spawns, new-game character generation.
> When you walk into Squin and the engine streams in 50 saved NPCs,
> none of those go through the factory. They go through whatever path
> handles `Character` deserialisation, then get added to
> `GameWorld::charUpdateListMain` via `addToUpdateListMain`.

Static call-graph analysis can't trace this: both `CharacterSpawn`
(0x581770) and `createRandomSquad` (0x583A10) report **"Direct callers:
0 function(s)"** because they're virtual methods on `RootObjectFactory`,
called via vtable dispatch (`gameWorld->theFactory->createRandomSquad(...)`).
Ghidra's static call graph doesn't see virtual calls.

## What's left to nail down

### 1. The actual `addToUpdateListMain` RVA on 1.0.68

`FUN_1407869e0` (RVA `0x7869E0`, 89 bytes — closest function to
KenshiLib's expected `0x786A60`) is a **hash-set walker** that touches
`param_1 + 0x768`, `+0x770`, `+0x788` — looks like an iterator over
buckets. Probably a destructor or a "process all characters" loop, not
the inserter we want. The actual `addToUpdateListMain` is somewhere
nearby and has signature `void(GameWorld*, Character*)` — but we don't
know its exact RVA yet.

A more aggressive Ghidra script could enumerate every function with
that signature and check which one calls into MSVC's
`unordered_set::_Insert_node_at` or similar. Roughly an evening of
work; deferred.

### 2. The actual offset of `charUpdateListMain` on 1.0.68

The set's bucket-array pointer appears to be at `GameWorld + 0x788`
based on `FUN_1407869e0`'s access pattern. KenshiLib 1.0.51 said the
unordered_set itself (its first byte) starts at `+0x750`. The deltas
to internal fields:

| Field | KenshiLib 1.0.51 says | What FUN_1407869e0 reads on 1.0.68 |
|---|---|---|
| set start | `+0x750` | unknown |
| set internal "head" or count | `+0x18` from set start = `+0x768` | reads `+0x770` (could be size_t count) |
| bucket count or index | `+0x20` from set start = `+0x770` | reads `+0x768` (could be index) |
| bucket array ptr | `+0x38` from set start = `+0x788` | reads `+0x788` |

The +0x788 match is suspicious-good. So the **set most likely still
starts at +0x750** but the internal libstdc++ shape we coded for in
`game_world_iter.cpp` doesn't match MSVC's actual layout. Need ReClass
to confirm.

### 3. The actual offset of `frameSpeedMult` on 1.0.68

KenshiLib says +0x700. Our `speed_probe` runs (in `SPEED_SYNC_LEAD.md`)
showed garbage there on 1.0.68 — value `~3.3e35` instead of a sane
multiplier. The float either moved or the GameWorld pointer we
dereferenced was wrong (the slot-vs-instance double-dereference). 5
min in Cheat Engine settles it.

## Tools used + run cost

| Tool | Time | Note |
|---|---|---|
| Ghidra 12.0.4 headless import + auto-analysis | 8 min one-time | imports `kenshi_x64.exe`, auto-finds 77,508 functions |
| KenshiOnlineRecon.py | ~10 sec | string survey + xref enumeration |
| KenshiOnlineRecon2.py | ~5 min | RTTI vtables + decompiler dumps for short candidates |
| KenshiOnlineRecon3.py | ~3 min | call graph for spawn entry + decompile of huge function |
| KenshiOnlineRecon4.py | ~2 min | per-function offset extraction from decompile |

Re-running everything from scratch is ~20 min on this machine. Each
script is self-contained and committed to this folder.

## Next session — concrete plan

1. **`KenshiOnlineRecon5.py` — narrow down `addToUpdateListMain`** (~15 min):
   Static analysis still has one targeted use left before going dynamic.
   Decompile `FUN_140581770` (CharacterSpawn, 6410 bytes) in full and
   look at the tail of the function — wherever it calls a small helper
   that writes to a `+0x7??` offset of a GameWorld pointer, that helper
   IS `addToUpdateListMain`. Cross-check against any function whose body
   is ~80-150 bytes and ends with an MSVC `std::unordered_set::_Insert_*`
   call. Should give us the exact RVA on 1.0.68.

2. **ReClass.NET pass on the live GameWorld pointer** (~30 min):
   - Launch Kenshi, load a save in a populated zone (NPCs visible).
   - Resolve GameWorld pointer using our existing function-disasm
     fallback path (the slot RVA we already log on every run).
     Dereference once for the live struct address.
   - Open ReClass.NET, attach to `kenshi_x64.exe`, paste the live
     GameWorld address, walk to `+0x750`.
   - Right-click each 8-byte slot → identify type (pointer / size_t /
     vtable). The MSVC `std::unordered_set` layout will be obvious
     after 5-6 fields are typed. v5's findings tell us where in the
     struct to focus.
   - Save the layout as JSON. Same trick for `frameSpeedMult` at
     `+0x700`.

3. **Update `KenshiMP.Core/game/game_world_iter.cpp`** with the verified
   MSVC layout offsets — single-file change, build, drop DLL into
   Kenshi folder, retest. The `Count()` log line should jump from 0 to
   the same number Kenshi's HUD shows.

4. **Add a periodic snapshot+diff** of the set in `core.cpp`
   (~50 lines). New characters since last tick → broadcast spawn to
   other players. Removed → broadcast despawn.

5. Re-run the two-player test. Remote characters should now render.

Steps 1-4 are realistic for one focused evening. Step 5 confirms.
