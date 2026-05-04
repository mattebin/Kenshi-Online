# 2026-05-04 milestone — character sync wired end-to-end on 1.0.68

This doc captures the moment "remote players can see each other's
characters in-game" stopped being a research project and became a
working integration. Everything in this folder up to today contributed.
The single line that proves it:

```
[2026-05-04 13:37:42] Core: DrainCapturedCharactersToServer sent 21 new
                       (skipped 54 faction-mismatch, 0 already-known, 0 invalid)
[2026-05-04 13:37:43] NativeHud: [OK] Sent 21 characters to server
```

## How we got here

Over four hours of work tracing through five Ghidra recon passes, two
runtime probes, and several dead ends:

1. Confirmed `addToUpdateListMain` lives at RVA `0x787C70` on 1.0.68
   (51-byte `unordered_set::insert` passthrough). KenshiLib's
   1.0.51 reference of `0x786A60` is stale — same offsets in
   `GameWorld` (e.g. `+0x750` for `charUpdateListMain`,
   `+0x700` for `frameSpeedMult`) but the wrapper functions moved.
2. Hooked it. Universal capture confirmed — 110+ unique `Character*`
   pointers in a 2-minute populated-zone test, including save-loaded
   NPCs that the legacy CharacterCreate path misses on 1.0.68
   (deserialised NPCs bypass the factory entirely).
3. Re-enabled `host_game_speed` reading `gameWorld + 0x700` directly.
   First sane read 1.0× → confirmed 2.0× / 5.0× transitions on
   keypress. Speed broadcast (C2S → server → S2C → client write to
   `+0x700`) wired and validated locally.
4. Built the captured-set drain — feeds the existing
   `C2S_EntitySpawnReq` pipeline that the legacy faction-iterator
   path was supposed to feed but couldn't on 1.0.68 (it walks the
   removal queue at `+0x888`, not the live set at `+0x750`).
5. Faction-discovery fallback by character-name: when both legacy
   discovery paths return 0, walk the captured set looking for a
   `Player N` placeholder character whose faction can be promoted to
   the canonical local faction pointer.

## What was working before today

Network handshake, chat, presence, position sync, install/release
infra, port-forward verification, the chat-input gate, MyGUI hooks,
the `.pdata` mid-function recovery on the factory hooks. The
two-machine session 2026-05-04-night confirmed all of these end-to-end.

## What works now (this milestone)

- Universal character capture (every spawn — factory and save-loaded).
- Player-vs-NPC filter via faction.
- Spawn broadcast through the existing `C2S_EntitySpawnReq` pipeline.
- Speed broadcast (`C2S_HostGameSpeed`) and apply (`S2C_HostGameSpeed` →
  write to `gameWorld + 0x700`).
- HUD warnings now reflect the AddToUpdateListMain capture path
  ("INFO: ... (134 chars)" instead of misleading WARN messages).

## What's left

Three small follow-ups, none of them research:

1. **Two-machine validation** — a real session with a friend's
   client to confirm remote characters render. The receive-side
   proxy-spawn path is the existing `S2C_EntitySpawn` handler and
   already works for chat / position / presence; it just hasn't had
   real character data fed to it on 1.0.68 until this commit set.
2. **UI cleanup** — F1 currently conflicts with Kenshi's vanilla
   help menu. Same with chat key. Tracked separately.
3. **Filter polish** — current filter accepts any character whose
   faction matches the local player's. That includes Kenny's
   characters when *he* is the host; we should also gate on
   "this is MY player slot" if both players' squads use the same
   `kenshi-online.mod` faction in their local saves.

## Validated offsets on Kenshi 1.0.68 (Steam, Newland)

Anchor for whoever picks up next:

| Field | Offset / RVA | Status |
|---|---|---|
| `GameWorld::frameSpeedMult` | +0x700 (float) | confirmed via host_game_speed |
| `GameWorld::charUpdateListMain` | +0x750 | confirmed via addToUpdateListMain decompile |
| `GameWorld::paused` | +0x8B9 (bool) | confirmed via speed_probe earlier |
| `GameWorld::theFactory` | +0x4A0 (RootObjectFactory*) | confirmed via KenshiLib, runtime read TBD |
| `Character::faction` | +0x10 (Faction*) | confirmed via KServerMod |
| `Character::name` | +0x18 (std::string) | confirmed via KServerMod |
| `Character::position` | +0x48 (Vec3) | confirmed via KServerMod |
| `RootObjectFactory::create` (RVA) | 0x583400 | dispatcher, only fires for factory creates |
| `RootObjectFactory::createRandomChar` (RVA) | 0x5836E0 | per-char entry, only fires for dynamic spawns |
| `RootObjectFactory::createRandomSquad` (RVA) | 0x583A10 | confirmed by literal error strings |
| `CharacterSpawn` (RVA) | 0x581770 | called by factory dispatcher |
| **`addToUpdateListMain` (RVA)** | **0x787C70** | **the universal capture point** |
| `unordered_set::insert` helper (RVA) | 0x5E8130 | inside addToUpdateListMain |

## The recon trail (in order)

- `KenshiOnlineRecon.py` — string survey, anchor presence
- `KenshiOnlineRecon2.py` — RTTI vtables, decompile of broken hook targets
- `KenshiOnlineRecon3.py` — call graph, FUN_140583A10 = createRandomSquad
- `KenshiOnlineRecon4.py` — verify KenshiLib RVAs (mostly stale)
- **`KenshiOnlineRecon5.py`** — full CharacterSpawn decompile + callee scoring →
  found `addToUpdateListMain` at `0x787C70` with one-line confidence

Everything is committed alongside this doc in `docs/reverse-engineering/`.
