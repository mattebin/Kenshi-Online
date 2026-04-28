# Kenshi-Online — `coop-stability-2026-04` fork

Fork of [`The404Studios/Kenshi-Online`](https://github.com/The404Studios/Kenshi-Online) that adds stability fixes for the connect / first-NPC pipeline and a packaged installer for end users. The original repo's [README](README.md) covers the architecture and design.

This branch is **alpha**. The connection layer works end-to-end; the spawn pipeline still has one unresolved engine-side crash documented in [KNOWN_ISSUES.md](KNOWN_ISSUES.md). Read that before opening bugs.

## What works on this build

- DLL builds cleanly from a fresh checkout (`cmake --build build --config Release`).
- 88/88 unit tests pass (`KenshiMP.UnitTest.exe`).
- Mod loads, scanner resolves >540 game functions/offsets without warnings.
- F1 multiplayer panel renders, Tab player list, Insert log panel, in-game chat.
- Auto-connect ~2 s after save load (configurable in `%APPDATA%\KenshiMP\client.json`).
- `kenshi-online.mod` "Singleplayer" start places the `Player 1` / `Player 2` characters.
- Server handshake, ping/name display, time-sync packets.
- The earlier ~30 s idle crash on a connected, gameLoaded session does **not** reproduce here — connecting before that window dodges it.

## What doesn't yet

- Within ~10–30 s of the first connected `CharacterCreate` the engine null-derefs at `game+0x644365` and Kenshi terminates without a Windows error dialog. Reproducible on every test session and on upstream's builds for months. Full repro recipe + register dump in [KNOWN_ISSUES.md](KNOWN_ISSUES.md). Needs a debugger attached to a live Kenshi to make further progress.

## End-user install

If you just want to try it:

1. Download the release zip from the [`Releases`](https://github.com/mattebin/Kenshi-Online/releases) page (or build it yourself, see below).
2. Unzip anywhere.
3. Run `install.bat`. It auto-detects Kenshi via Steam's `libraryfolders.vdf`, falls back to GOG, then to a manual path prompt.
4. Backups go to `<Kenshi>/KenshiMP_backup_<timestamp>/`. To revert: `uninstall.bat` from the same folder.
5. Launch Kenshi from Steam. New Game → "Singleplayer" → world loads → mod auto-connects to `127.0.0.1:27800`.

You'll need to start the bundled `KenshiMP.Server.exe` first (or have someone else host it) for the auto-connect to find anything. The default `server.json` has UPnP off, so for LAN/internet play you forward UDP 27800 yourself.

## Build from source

Requires Visual Studio 2022 / Build Tools 17 with the C++ workload, plus CMake (the VS-bundled one works).

```pwsh
git clone https://github.com/mattebin/Kenshi-Online
cd Kenshi-Online
git checkout coop-stability-2026-04
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\bin\Release\KenshiMP.UnitTest.exe   # should print "All tests PASSED!"
```

The DLL ends up at `build\bin\Release\KenshiMP.Core.dll`. Drop it next to `kenshi_x64.exe` (or run the install.bat in `dist\`). For an existing install, just overwrite `KenshiMP.Core.dll` — Kenshi must be closed during the swap.

## Branch layout

| Branch | Purpose |
| --- | --- |
| `main` | Tracks upstream `The404Studios/Kenshi-Online` |
| `coop-stability-2026-04` | This branch — collected stability fixes + packaging |

Cherry-pick or rebase commits forward as upstream moves. None of the changes here touch the network protocol, so they should stay portable.

## Commit log on this branch

```
249a905  entity_hooks: don't promote runtime NPC fallback faction to local player
9d152fd  shared_save_sync: tolerate ".mod" suffix on faction strings + log-once;
         render_hooks: drop pre-emptive F1 readiness gate
1339241  overlay: honor ClientConfig::autoConnect on first frame
2f37832  WIP: co-op stability investigation — guarded join flow + deferred hook disable
```

## Filing bugs

Please run with at least these env vars and attach the output:

- The full `KenshiOnline_<PID>.log` from `<Kenshi folder>/`
- The `KenshiOnline_CRASH.log` if the session ended in a VEH-caught crash
- Steam Kenshi version (Properties → Betas)
- `KenshiMP.Server.stdout.log` if hosting

Open issues against the upstream repo or this fork's tracker — duplicates are fine.

## License

Same as upstream — see [LICENSE](LICENSE).
