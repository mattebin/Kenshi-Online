# KenshiMP — architecture v2

**Status as of 2026-05-06.** This doc describes the pivot from the
legacy in-game-heavy architecture to the new out-of-game-heavy
architecture. Both paths coexist for now; the old one is preserved as
"legacy" while the new one grows.

## The pivot in one sentence

> Do absolutely everything we can outside the game; only touch the
> game when we have to.

After many sessions of `__fastfail` terminations, MyGUI use-after-free
crashes, MinHook trampolines fighting Kenshi's `mov rax,rsp`
prologues, and silent process deaths that bypassed every user-mode
exception handler — the conclusion is that *living inside Kenshi's
address space is structurally unsafe*. Every meaningful chunk of mod
logic that can run from outside, should.

## The two architectures, side by side

### v1 — legacy (in-game-heavy)

```
                     ┌───────────────────────────────┐
                     │      kenshi_x64.exe           │
                     │   ┌───────────────────────┐   │
                     │   │  KenshiMP.Core.dll    │   │
                     │   │  ──────────────────   │   │
   network ────────► │   │  ENet client          │   │
                     │   │  Spawn pipeline       │   │
                     │   │  Sync orchestrator    │   │
                     │   │  Native HUD (MyGUI)   │   │
                     │   │  ~30 MinHook hooks    │   │
                     │   │  Many SEH wrappers    │   │
                     │   └───────────────────────┘   │
                     └───────────────────────────────┘
                                     │
                                     │ (every crash dies here)
                                     ▼
                              user has to relaunch
```

Files: `KenshiMP.Core/`, `KenshiMP.Common/`, `KenshiMP.Scanner/`, plus
all the historical out-of-process tools (`KenshiMP.Server`,
`KenshiMP.MasterServer`, `KenshiMP.Injector`, `KenshiMP.LogTail`,
`KenshiMP.Cartographer`, `KenshiMP.CrashWatchdog`, `KenshiMP.Probe`,
`KenshiMP.SafeAddon`). All currently still in build.

### v2 — dashboard-heavy (where we're going)

```
   ┌─────────────────────────┐         ┌─────────────────────────────┐
   │  KenshiMP.Dashboard.exe │         │   kenshi_x64.exe            │
   │  ─────────────────────  │         │  ┌───────────────────────┐  │
   │  Win32 GUI              │         │  │ KenshiMP.SafeAddon.dll│  │
   │  Status / Logs / Action │  named  │  │ ─────────────────────  │  │
   │  ENet client            │ ◄──────►│  │ Worker thread          │  │
   │  Save-file management   │  pipe   │  │ Memory-read sampler    │  │
   │  Session orchestration  │         │  │ Spawn shim (RPC only)  │  │
   │  World-state model      │         │  └───────────────────────┘  │
   └─────────────────────────┘         └─────────────────────────────┘
              ▲                                      │
              │                                      │ ONE crash class
              │                                      │ left here
   the Dashboard never crashes        if SafeAddon faults, we restart it,
   when the game does;                game survives, dashboard tells you
   it observes externally
```

Files (new):
- `dashboard/KenshiMP.Dashboard/` — the unified client
- `KenshiMP.SafeAddon/` — already shipped; minimal in-game shim

Files (legacy, kept for now):
- `KenshiMP.Core/` and friends — still build, can still be selected
  via `Plugins_x64.cfg`. Deprecated path; will be removed once
  Dashboard reaches feature parity.

## What lives in the Dashboard (v2)

Everything that doesn't *physically* require running inside Kenshi:

| Concern | Implementation in v2 |
|---|---|
| Server | Spawned/managed as a child process from the Dashboard. Same `KenshiMP.Server.exe` binary. |
| Master server | Same — child process. |
| Lobby | Pure GUI in the Dashboard; ENet client connects to master server, lists active games, joins via the dashboard. |
| Save management | Dashboard reads Kenshi's save folder directly (`%LOCALAPPDATA%\kenshi\save\`), backs up before MP sessions, restores on disconnect. |
| Session orchestration | Dashboard launches Kenshi (via Steam) when the user is ready; Dashboard owns the session lifecycle, not Kenshi. |
| World state | Dashboard maintains its own `SafeWorldSnapshot` model — same data the SafeAddon samples — and uses it as the single source of truth for sync. |
| Network sync | ENet client lives in the Dashboard. Receives remote-player updates; pushes them to the SafeAddon via named pipe. |
| Logging | Dashboard tails every relevant log into one console + a structured panel. (LogTail merges into Dashboard; standalone LogTail.exe stays for users who want a separate window.) |
| Crash forensics | CrashWatchdog merges into Dashboard as a tab. (Standalone watchdog stays.) |
| Memory layout validation | Probe merges into Dashboard as a tab. |

## What MUST stay in the game (the irreducible v2 in-game footprint)

The SafeAddon is the only in-game DLL in the v2 path. Its job is the
RPC gateway for things that physically cannot run from outside:

| Operation | Why it needs in-game |
|---|---|
| Spawn a Character | Calls `RootObjectFactory::*` which lives in the game's heap and walks game-allocator data structures. We can read the factory pointer from outside, but invoking it requires being a thread inside Kenshi. |
| Write to game memory under SEH | Cross-process `WriteProcessMemory` works but is sequential and expensive. In-process writes with SEH wrappers are acceptable and cheap. |
| Modify a Character's faction / position / orders | Same as above — requires running on a thread that can hold the game's locks. |
| Receive Ogre frame callbacks | If we ever need Kenshi's frame timing exactly. (For now we use an independent clock.) |

## The IPC contract (Dashboard ↔ SafeAddon)

A single named pipe: `\\.\pipe\KenshiMP`.
- Dashboard creates the pipe (server side).
- SafeAddon connects on `dllStartPlugin`.
- Both sides exchange length-prefixed JSON messages.
- Connection drops are treated as "the other side died", trigger a
  graceful local shutdown of remaining work but do NOT terminate.

Initial command set (V1 of the protocol):

```jsonc
// Dashboard → SafeAddon
{ "op": "ping" }
{ "op": "snapshot" }                        // request a current world snapshot
{ "op": "spawn", "template": "Wanderer",
  "x": 1234.0, "y": 56.0, "z": 7890.0,
  "factionId": 12 }                          // queue a character spawn
{ "op": "writePos", "entityId": 42,
  "x": 1234.0, "y": 56.0, "z": 7890.0 }     // teleport an entity

// SafeAddon → Dashboard
{ "op": "pong", "frame": 12345 }
{ "op": "snapshot",
  "frame": 12345,
  "localPlayer": { ... },
  "entities": [ ... ] }
{ "op": "spawnResult", "ok": true,
  "entityAddr": "0x1A2B3C4D" }
{ "op": "log", "level": "warn",
  "msg": "..."}                              // forwarded internal log lines
```

## Migration plan

1. **Phase 1 (this session):** ship `KenshiMP.Dashboard.exe` with the
   Win32 shell — Status / Actions / Logs panels. No IPC yet; the
   Dashboard observes processes and tails logs. Legacy stack still
   works alongside it.
2. **Phase 2:** wire the named-pipe IPC. SafeAddon becomes a
   ping/pong + snapshot responder. Dashboard displays live world
   state as it samples.
3. **Phase 3:** move ENet client from `KenshiMP.Core` into the
   Dashboard. SafeAddon takes spawn / write commands from the
   Dashboard. Legacy `KenshiMP.Core.dll` becomes optional.
4. **Phase 4:** delete `KenshiMP.Core/` once all functionality has
   moved out. Repo layout becomes:
   - `dashboard/` — the v2 client
   - `KenshiMP.SafeAddon/` — the in-game shim
   - `KenshiMP.Common/` — shared protocol types
   - `tools/` — Cartographer, Brainer outputs, etc.
   - `legacy/` (optional historical reference)

## What "this session" delivers

- This document
- `dashboard/KenshiMP.Dashboard/` directory + CMake target
- `KenshiMP.Dashboard.exe` — minimum-viable Win32 GUI: a single
  window, three group boxes (Status / Actions / Logs), 1 Hz refresh.
  No IPC yet; pure observer + child-process launcher. Provides the
  user-facing entry point that becomes the long-term home for
  everything.
