# Speed / time sync — RE_Kenshi-derived lead

**Status:** parked, not implemented. Game speed remains locked at 1× across all
clients on 1.0.68 (see README "Known limits"). This doc captures the missing
pieces for whoever picks it up next.

**TL;DR:** RE_Kenshi already reads/writes the live game-speed value from
`GameWorld + 0x700`. Earlier in this fork we tried that offset and got garbage
— almost certainly because we held the wrong `GameWorld*`. The win condition
is "find a valid `GameWorld*` for 1.0.68 Steam Newland"; the offsets are then
known.

## Confirmed offsets (from BFrizzleFoShizzle/KenshiLib, branch `RE_Kenshi_mods`)

Source: `Include/kenshi/GameWorld.h` in the KenshiLib submodule that RE_Kenshi
links against. Byte offsets are present as inline comments in that header.

| Offset | Type | Field | What it is |
|---|---|---|---|
| `+0x008` | `float` | `tempSpawnsDisableTimer` | Internal NPC-spawn cooldown |
| `+0x00C` | `bool`  | `initialized` | Has the world finished init |
| `+0x4F0` | `bool`  | `steamEnabled` | Steam vs GOG branch — useful sanity check |
| `+0x580` | `PlayerInterface*` | `player` | Singleton arrow into per-player state |
| **`+0x700`** | **`float`** | **`frameSpeedMult`** | **Live speed multiplier (1×, 2×, 3×, fractional with custom-speeds toggled)** |
| `+0x8A0` | `SimpleTimeStamper` | `timeStamper` | Live time-of-day clock (16 bytes) |
| `+0x8B0` | `ZoneManager*` | `zoneMgr` | Sanity check (vtable in module range) |
| `+0x8B8` | `bool` | `debugFlag` | |
| **`+0x8B9`** | **`bool`** | **`paused`** | **Game pause flag** |
| `+0x8BA` | `bool` | `gameResetting` | Game reset flag |
| `+0x8C0` | `AudioSystemGlobal*` | `audioThread` | Sanity check |

KenshiLib's reversing notes say the layout was captured for **Kenshi v1.0.51**
(see `Source/Kenshi.cpp` header comment). Our target is **1.0.68 Steam Newland**.
The offsets likely still hold — RE_Kenshi as currently shipped on Nexus
advertises support for `Kenshi 1.0.x` and the speed feature works on current
builds — but nothing in this doc has been *runtime-validated against 1.0.68 by
this fork yet*.

## Acquiring the `GameWorld*` — three paths, in increasing reliability

### 1. Static RVA (cheapest, version-fragile)

KenshiLib `Source/Kenshi.cpp`:

```cpp
Kenshi::GameWorld& Kenshi::GetGameWorld()
{
    static RVAPtr<GameWorld> c_inst(0x001AAE060);
    return *c_inst.GetPtr();
}
```

That `0x001AAE060` is the absolute *RVA* of the global `GameWorld` singleton
pointer in **1.0.51**. On 1.0.68 the data layout of the binary has shifted; do
NOT trust this RVA blindly. Use it as a starting point and validate the result
(see "Validation" below).

### 2. Hook `GameWorld::initModsList` (RE_Kenshi's approach)

RE_Kenshi `Plugins.cpp`:

```cpp
void (*initModsList_orig)(GameWorld*);
void preload_init_hook(GameWorld* thisptr) { ... }

KenshiLib::AddHook(
    KenshiLib::GetRealAddress(&GameWorld::initModsList),
    preload_init_hook, &initModsList_orig);
```

`initModsList` is called once during world load with a valid `GameWorld*` as
its first arg. Cache that pointer, use it forever. This is how RE_Kenshi
actually does it — and it's why their speed read works while ours didn't:
they get the pointer from the engine instead of guessing.

For our fork the equivalent is to scan for `initModsList`'s prologue (or any
other GameWorld member function with a stable signature) and install a
MinHook that captures `RCX` (the `this` pointer in `__fastcall` x64).

### 3. Walk PlayerInterface backwards

We already have a working hook on the player-character path (`save_sync` /
`character_accessors`). `Character → CharacterController → PlayerInterface →
GameWorld` is reachable; `PlayerInterface*` lives at `GameWorld + 0x580` so
the inverse walk needs a known offset from `PlayerInterface` back to
`GameWorld`. Less elegant than path 2; mention only as a fallback.

## Validation routine (do this before trusting anything)

When you have a candidate `GameWorld*`, run this gauntlet before reading
`frameSpeedMult`:

1. `*(uintptr_t*)gw == <vtable in kenshi_x64.exe .rdata>` — vtable check
2. `*(bool*)(gw + 0x4F0) == steamEnabled_expected` — Steam vs GOG branch
3. `*(ZoneManager**)(gw + 0x8B0)` — non-null and vtable in module range
4. `*(float*)(gw + 0x700)` is in `[0.05f, 10.0f]` — sane speed range
5. While paused, `*(bool*)(gw + 0x8B9) == true`

If 1-4 all pass on idle and 5 flips with the pause key, the layout matches
1.0.68 and the offsets are good. If any fail, the GameWorld layout shifted
between 1.0.51 → 1.0.68 and `Include/kenshi/GameWorld.h` needs to be
re-derived from RE_Kenshi's current source (Nexus says the mod still
supports 1.0.x, so they almost certainly have an updated header in a branch
we haven't read yet).

## Sync design — what to do once it works

This is the protocol shape, agreed during the previous hunt and still valid:

- **Authoritative source:** the host's `frameSpeedMult` and `timeStamper`.
- **Wire format:** new `S2C_TimeSpeed { uint32 day; float secondsOfDay;
  float speedMult; }`, broadcast from the server every 1s and on any change.
- **Client side:** on receive, write to local `gw + 0x700` and the
  `timeStamper` slot. Skip the write if the local value is already within an
  epsilon (avoid fighting the engine on the same frame it set them).
- **Pause:** broadcast `paused` too, client mirrors it; this lets the host
  pause for everyone, which is the actual co-op feature most players want.
- **Edge case:** zone-load gaps. While the client is still loading, defer
  writes — `gw` may be mid-reconstruction. The existing `IsGameLoaded()` gate
  in `core.cpp` is the right place to short-circuit.

## Why this is parked

- The previous hunt for these values consumed a multi-day session and ended
  with `time_discovery.cpp` + `hud_time_probe.cpp` in the codebase as ~660
  lines of dead probes (now removed in commit `3773b20`).
- Speed-locked-at-1× is documented in the README as a known limit; nobody is
  blocked on this.
- Resuming cold from this doc is cheap — about an evening of work to test
  the validation routine on 1.0.68. Resuming cold from "where were we" was
  expensive, hence this file.

## 2026-05-04 attempt — outcome

Spent an evening trying to runtime-validate the offsets above on 1.0.68
Steam Newland with a background-thread probe (`KenshiMP.Core/sys/speed_probe.{h,cpp}`,
env-gated by `KMP_SPEED_PROBE=1`, inert otherwise — left in tree for future
attempts). Findings:

- **`?ou@@3PEAVGameWorld@@EA` is NOT exported on 1.0.68.**
  `GetProcAddress(kenshi_x64.exe, "?ou@@3PEAVGameWorld@@EA")` returns NULL.
  Lo-Fi stripped exports between 1.0.51 (when KenshiLib captured them) and
  the current build. Scratch the cleanest GameWorld-acquisition path.

- **Our existing `GameWorldSingleton` resolver also fails on 1.0.68.**
  String-xref via `dayTime` returns no functions; direct .rdata search for
  the string returns nothing; the prologue-RVA fallback lands on `FF FF FF FF`.
  The static address path is dead too.

- **.data-section scan finds GameWorld-shaped candidates but none have
  `frameSpeedMult` near `+0x700`.** Two candidates consistently surface:
  one at a typical heap address with score 4/5 (vtable + zoneMgr +
  audioThread + paused all valid, but `+0x700` reads uninit garbage like
  `~3.3e35`), and one at a DLL-data-segment-shaped address that's almost
  certainly a false positive. Across multiple in-game tests with the user
  pressing speed hotkeys, NO float in `±0x800` of `+0x700` on either
  candidate transitioned in a way matching the speed change. Either the
  field moved more than `±0x800` away on 1.0.68, or our candidate isn't
  the live GameWorld.

- **No other 1.0.68-targeted mod reads or writes `frameSpeedMult` that we
  can crib from.** RE_Kenshi advertises support for "Kenshi 1.0.x" on
  Nexus but its public source still references the 1.0.51 KenshiLib
  layout. Either Lo-Fi's binary diff is small enough that RE_Kenshi just
  works without code changes (and we're missing something simple about
  resolution), or RE_Kenshi has private updates not committed upstream.

**Conclusion for this branch:** parked indefinitely until either (a) Lo-Fi
ships an exports update, (b) RE_Kenshi publishes an updated KenshiLib
header for the current build, or (c) someone with IDA / Ghidra rebuilds
the GameWorld layout for 1.0.68 from the binary. Probing blindly past this
point is a known-bad investment.

## Sources

- RE_Kenshi (Nexus 847, GitHub `BFrizzleFoShizzle/RE_Kenshi`): live mod
  reading/writing `frameSpeedMult` on current Kenshi.
- KenshiLib (`BFrizzleFoShizzle/KenshiLib`, branch `RE_Kenshi_mods`):
  `Include/kenshi/GameWorld.h` is the layout reference; `Source/Kenshi.cpp`
  is the static-RVA accessor.
- Earlier session-summary in this fork's history (commit `3773b20` and the
  removed `time_discovery.cpp` / `hud_time_probe.cpp`): catalogues every
  approach that *didn't* work last time, useful for not repeating mistakes.
