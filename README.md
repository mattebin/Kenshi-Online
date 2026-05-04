# Kenshi-Online

**16-player co-op multiplayer mod for Kenshi**

Kenshi-Online adds seamless multiplayer to Kenshi using native MyGUI integration, ENet networking, and Ogre plugin injection. Players can explore, fight, build, and trade together in the open world of Kenshi.

---

## Quick install (just want to play)

Latest stable build for **Kenshi 1.0.68 (Steam, Newland)**:

> **[Download the latest release](https://github.com/mattebin/Kenshi-Online/releases/latest)** → grab **`KenshiMP-Setup-*.exe`** (one-click installer, recommended) or `KenshiMP-stability-1.0.68.zip` (manual extract).

### Recommended: installer (.exe)

1. Make sure Kenshi isn't running (Steam can stay open).
2. Run **`KenshiMP-Setup-*.exe`**. It auto-detects your Kenshi folder via Steam's library list, takes backups of `Plugins_x64.cfg` / `__mods.list` / `Kenshi_MainMenu.layout` into `<KenshiDir>\KenshiMP_backup\`, copies the DLL + server + Injector + GUI layouts + mod files into the right places, and adds `Plugin=KenshiMP.Core` to `Plugins_x64.cfg`.
3. Launch Kenshi normally from Steam.

To remove cleanly: Windows Settings → Apps → Kenshi-Online → Uninstall. The uninstaller restores the original files from the backups it took.

### Alternative: zip (manual)

1. Right-click Kenshi in Steam → Manage → Browse local files. Extract the zip there. Then run `install.bat` once. Same end-state as the installer.

### 👉 First time playing with someone? Read this.

> **[`docs/PLAYING_TOGETHER.md`](docs/PLAYING_TOGETHER.md)** — step-by-step recipe for the join order (connect first, load save second), picking the same starting zone, and warming up the spawn pipeline so other players' characters render.

### Hosting

Run `KenshiMP.Server.exe` from the Kenshi folder (the installer drops it there). Default port is **`27800` UDP**.

- **Same network (LAN):** friends connect to your local IP (run `ipconfig` → `IPv4 Address`, usually `192.168.x.x`) on `27800`. No router config needed.
- **Over the internet:** forward **UDP `27800`** on your router to your PC (KenshiMP uses ENet over UDP — TCP forwarding isn't needed). Friends connect to your public IP on `27800`. Pin your PC's local IP via DHCP reservation so the rule doesn't break on reboot.
- **Verify reachability:** the repo's Actions tab has a `Port Forward Test` workflow — runs an ENet handshake from a GitHub-hosted runner against your IP. Click "Run workflow", pass your public IP, get a definitive yes/no in ~30 seconds.

> Don't trust online port checkers like yougetsignal for UDP — they false-negative on ENet because the server only replies to a valid handshake, not random probes. Use the workflow or have someone actually connect.

### Known limits on 1.0.68

Game speed is locked at 1× for everyone. Pressing 2× / 3× in-game speeds up your *local* world only — keep everyone on 1× for clean sync. Remote players' characters may not render in your world even though chat / presence / position sync work — both gated on the same 1.0.68 binary-layout problem.

Why and how to fix: [`docs/SPEED_SYNC_LEAD.md`](docs/SPEED_SYNC_LEAD.md), [`docs/NEXT_STEPS.md`](docs/NEXT_STEPS.md) (P0 backlog + reverse-engineering tools).

For the technical details and what changed vs. upstream, keep reading.

---

## Fork notice — `mattebin/stability/upstream-base`

> This fork carries 27 stability, correctness, and diagnostic improvements for Kenshi 1.0.68 (Steam, "Newland") that aren't in upstream `main`. Branch is fast-forward mergeable into upstream — no conflicts at the time of writing.
>
> **`KenshiMP.IntegrationTest` automated 2-client suite passes 70/70** against this branch. Two real cross-client server bugs (EntityDespawn-on-disconnect and BuildPlaced-confirmation-for-placer) were found and fixed via the suite during development.
>
> Borrows several engine-level fixes from the parallel `andperks6/Kenshi-Online` fork (each commit credits the source). Combined with original work here on crash recovery, faction identity, and diagnostics.
>
> **Full per-commit details: [`FORK_CHANGES.md`](FORK_CHANGES.md).**

Highlights:

| Area | What this fork fixes |
|---|---|
| Combat | `AI::create` is a 6-arg constructor, not 2-arg as upstream typedef'd it. Forwarding only RCX/RDX leaves `this+0x318` null and AI scoring crashes later at `game+0x59820D`. Fixed by passing all 6 args + minimal hook bodies + lazy enable on connect. |
| Crash safety | VEH-based recovery for the recurring engine null-deref at `[rax+0x90]`. `LOCK` prefix on the MovRaxRsp wrapper depth counter (silent cross-core race). `CharacterCreate` kept in permanent passthrough mode (full-body re-enable was the source of zone-stream crashes). |
| Steam compat | Auto-discover `CharacterHuman` backpointer offset (Steam ≠ GOG `+0x2D8`). `allowUnaligned` flag for patterns like `CharAnimUpdate` that intentionally land mid-function. |
| MP correctness | Faction-pointer identity (avoids 18-char `Player 1` name-collision swap). Strip `.mod` suffix from server-sent faction strings. Resume sync after main-menu join + world load. Position read fallback chain (char-direct + AnimClass) for the first ~1-2s after world load. OIS keyDown/keyUp swallow when chat/menu modal is open (fixes double-input bug). |
| MP scaling | Per-player spawn cap raised from upstream's hardcoded 4 to a configurable default of 32 (matches vanilla squad sizes). Infinite-retry bug fixed where cap-rejected spawn requests could spin in the spawn manager forever without ever bumping `retryCount`. |
| Diagnostics | Watcher trace markers (`WATCH/HOOK`, `WATCH/PKT`, `WATCH/TICK`, `WATCH/SYNC`, `WATCH/POS`) — flush-forced, gated on a config flag. `KMP_DISABLE_HOOKS` runtime gate (file or env var) for per-hook bypass without recompiling. **Static hook arg-count verification** at install time via prologue + call-site analyzers — would have caught the AI::create 2-vs-6-arg bug from one log line. **Single-block install audit** dump for bug-report attachment. **Runtime analyzers** for struct-field corruption (`field_diff`), unsafe concurrent hook re-entry (`concurrency_watch`), and long-session memory leaks (`leak_watch`). |

**Verification status:** all fixes verified on a single PC running both client and server. No two-machine, two-Steam-account session has been run yet — the gate test for declaring MP combat works is documented in `FORK_CHANGES.md`.

---

## Features

- **Up to 16 players** on a single server
- **Dedicated server** with persistence and console commands
- **Master server** with centralized server browser (auto-discovery)
- **Full network replication** - characters, NPCs, combat, buildings, items
- **Zone-based sync** - efficient bandwidth usage with interest management
- **Server-authoritative** combat and world state
- **Native MyGUI HUD** - status bar, chat with timestamps, player list, debug log
- **Client commands** - `/tp`, `/time`, `/kick`, `/announce`, `/connect`, `/disconnect`, `/pos`, `/players`, `/status`, `/entities`, `/ping`, `/debug`, `/help`
- **One-time install + Injector launcher** - Ogre plugin injection, no DLL injectors or process attach

## Architecture

```
KenshiMP.Injector.exe    -> Modifies Plugins_x64.cfg, launches Kenshi
KenshiMP.Core.dll        -> Loaded by Ogre as a plugin, hooks game functions
KenshiMP.Server.exe      -> Dedicated server (host on VPS or locally)
KenshiMP.MasterServer.exe-> Centralized server browser registry (port 27801)
KenshiMP.Common.lib      -> Shared types, protocol, serialization
KenshiMP.Scanner.lib     -> Pattern scanning, MinHook wrapper
```

## Install & Play (Players)

You do **not** need to build anything. Grab the latest prebuilt zip:

1. **Download** the latest `Kenshi-Online-vX.Y.Z.zip` from the
   [Releases page](../../releases/latest).
2. **Extract** it anywhere (Desktop is fine).
3. **Run `install.bat`** once. It auto-detects your Kenshi install,
   backs up the files it touches, and copies the DLL, GUI layouts, and
   `kenshi-online.mod` into place. Set `KENSHI_DIR` first if you want
   to override auto-detection.
4. **Launch with `KenshiMP.Injector.exe`**. Set your player name and
   server address, click **PLAY**, and Kenshi starts with multiplayer
   enabled. You can also launch Kenshi normally and use the
   **MULTIPLAYER** button on the main menu.

To undo everything, run `uninstall.bat` — it restores the vanilla files
from the backups created during install.

> **Why two scripts?** `install.bat` handles first-time setup
> (GUI layouts, backups, mod-list edits). `KenshiMP.Injector.exe` is the
> day-to-day launcher (player name, server picker, Plugins_x64.cfg
> management, launches Kenshi). The Injector will absorb the installer
> over time; until then, run `install.bat` once and use the Injector
> after that.

For full in-game controls, commands, hosting tips, and troubleshooting,
see [`dist/JOINING.md`](dist/JOINING.md) (also bundled in the release zip).

## Hosting a Server

Anyone can host. Run `KenshiMP.Server.exe` on your PC or a VPS.

1. Copy `KenshiMP.Server.exe` (and optionally `server.json`) to the host
   machine.
2. Create or edit `server.json`:
```json
{
  "serverName": "My Kenshi Server",
  "port": 27800,
  "maxPlayers": 16,
  "pvpEnabled": true,
  "gameSpeed": 1.0
}
```
3. Run: `./KenshiMP.Server.exe`
4. Forward port **27800 UDP** on your router/firewall (or rely on UPnP).
5. Players connect via your IP, or find you in the in-game server browser.

### Server Commands
```
status    - Show server info
players   - List connected players
kick <id> - Kick a player
say <msg> - Broadcast system message
save      - Save world state
stop      - Shutdown server
```

## Building from Source (Developers)

> Only needed if you're hacking on the mod. End users should use the
> prebuilt zip from the [Releases page](../../releases/latest).

### Requirements
- **Visual Studio 2022** (or 2019) with **Desktop development with C++** workload
- **CMake 3.20+** ([download](https://cmake.org/download/) or `winget install Kitware.CMake`)
- **Git** (for submodules)

No vcpkg needed -- all dependencies are bundled as git submodules.

### One-Click Build

```bash
git clone --recursive https://github.com/mattebin/Kenshi-Online.git
cd Kenshi-Online
build.bat
```

That's it. `build.bat` detects your Visual Studio version, configures CMake, builds all targets, and runs unit tests.

### Open in Visual Studio

**Option A -- CMake native (recommended):**
1. Open Visual Studio 2022
2. File > Open > CMake...
3. Select `CMakeLists.txt` in the project root
4. VS reads `CMakePresets.json` and configures automatically
5. Select **x64-release** preset from the toolbar
6. Build > Build All (Ctrl+Shift+B)

**Option B -- Solution file:**
```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
start build\KenshiMP.sln
```
Set configuration to **Release** and build.

### Manual (Command Line)

```bash
# Clone with submodules
git clone --recursive https://github.com/mattebin/Kenshi-Online.git
cd Kenshi-Online

# If you forgot --recursive:
git submodule update --init --recursive

# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build
cmake --build build --config Release

# Run tests
build\bin\Release\KenshiMP.UnitTest.exe
```

### Output

```
build/bin/Release/
    KenshiMP.Core.dll           # Client plugin (auto-deployed to Kenshi dir)
    KenshiMP.Server.exe         # Dedicated server (auto-deployed to Kenshi dir)
    KenshiMP.Injector.exe       # Launcher / installer
    KenshiMP.MasterServer.exe   # Server browser registry
    KenshiMP.TestClient.exe     # Fake player for testing
    KenshiMP.IntegrationTest.exe
    KenshiMP.UnitTest.exe
```

### Dependencies (bundled as submodules in `lib/`)
- [ENet 1.3.x](https://github.com/lsalzman/enet) -- reliable UDP networking
- [MinHook 1.3.3](https://github.com/TsudaKageyu/minhook) -- x64 API hooking
- [nlohmann/json](https://github.com/nlohmann/json) -- JSON for C++
- [spdlog](https://github.com/gabime/spdlog) -- fast logging
- [Dear ImGui](https://github.com/ocornut/imgui) -- debug overlay (optional)

## Controls (In-Game)

| Key | Action |
|-----|--------|
| F1 | Open/close multiplayer menu |
| Insert | Toggle debug/loading log panel |
| Enter | Open/close chat |
| Tab | Toggle player list |
| ` (backtick) | Toggle debug info |
| Escape | Close all panels |

## Network Protocol

- **Port**: 27800 UDP (ENet)
- **Channels**: 3 (reliable ordered, reliable unordered, unreliable sequenced)
- **Tick Rate**: 20 Hz (50ms)
- **Max Players**: 16

### Synced State
- Player character positions, rotations, animations
- NPC positions and AI states (zone-based)
- Combat: attacks, damage, deaths, knockouts
- Buildings: placement, construction, destruction
- Items: pickup, drop, inventory transfers
- Time of day, weather, game speed
- Chat messages

## Project Structure

```
KenshiMP/
+-- KenshiMP.Common/          # Shared library
|   +-- include/kmp/
|       +-- types.h           # Vec3, Quat, EntityID, ZoneCoord
|       +-- constants.h       # Tick rate, max players, port
|       +-- messages.h        # Network message structs
|       +-- protocol.h        # Packet reader/writer
|       +-- compression.h     # Delta compression
|       +-- config.h          # Client/server config
|
+-- KenshiMP.Scanner/         # Pattern scanner library
|   +-- include/kmp/
|       +-- scanner.h         # IDA-style pattern matching
|       +-- patterns.h        # Known Kenshi signatures
|       +-- memory.h          # Safe memory read/write
|       +-- hook_manager.h    # MinHook wrapper
|
+-- KenshiMP.Core/            # Ogre plugin DLL
|   +-- dllmain.cpp           # Plugin entry
|   +-- core.cpp              # Master initialization
|   +-- hooks/                # Game function hooks (14 modules)
|   +-- game/                 # Reconstructed game types
|   +-- net/                  # ENet client
|   +-- sync/                 # Entity registry, interpolation
|   +-- ui/                   # Native MyGUI overlay + menu
|
+-- KenshiMP.Server/          # Dedicated server
|   +-- main.cpp              # Console entry + commands
|   +-- server.cpp            # Game state, networking
|
+-- KenshiMP.MasterServer/    # Server browser registry
|   +-- main.cpp              # ENet master server (port 27801)
|
+-- KenshiMP.Injector/        # Launcher
    +-- main.cpp              # Win32 GUI
    +-- injector.cpp          # Plugins_x64.cfg modifier
    +-- process.cpp           # Game launcher
```

## Technical Details

### Injection Method
Uses the Ogre3D plugin system (proven by RE_Kenshi). The injector modifies
`Plugins_x64.cfg` to add `Plugin=KenshiMP.Core`, and Ogre loads our DLL
automatically during engine initialization. No process injection or manual
DLL loading required.

### Pattern Scanner
Scans kenshi_x64.exe in-memory using IDA-style byte patterns with wildcards.
Resolves RIP-relative addresses for x64 code. Falls back to known pointer chains
from Cheat Engine community.

### State Synchronization
- **Entity ownership**: Each player owns their squad; server owns NPCs
- **Interpolation**: 100ms buffer with hermite spline for smooth remote movement
- **Zone interest**: 3x3 zone grid around each player (only sync nearby entities)
- **Delta compression**: float16 position deltas, smallest-three quaternion encoding

## Credits

Built on community reverse engineering work:
- [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) - Ogre plugin injection system
- [KenshiLib](https://github.com/KenshiReclaimer/KenshiLib) - Game structure definitions
- [Kenshi Online](https://github.com/The404Studios/Kenshi-Online) - Memory addresses reference
- [OpenConstructionSet](https://github.com/lmaydev/OpenConstructionSet) - Game data SDK

## License

MIT License
