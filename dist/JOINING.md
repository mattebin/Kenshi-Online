# Joining Test Checklist

Use this for the first real multiplayer spawn test.

This package includes the shared test save `123`. The installer copies it to
`%LOCALAPPDATA%\kenshi\save\123` and backs up any existing save with that name.
Autoconnect is enabled by default for the current public test server. The installer
backs up and rewrites `%APPDATA%\KenshiMP\client.json` for this test.

## Host

1. Install with `install.bat`.
2. Back up the save you are using.
3. Run `KenshiMP.Server.exe`.
4. Launch Kenshi from Steam.
5. Use **Load Game** and load save `123`. Do not use **New Game** for this test.
6. Wait for autoconnect after the save loads.
7. Wait until player 2 joins and spawns.
8. Move a few steps only.
9. Exit cleanly.

## Other Player

1. Install the exact same release package with `install.bat`.
2. Launch Kenshi from Steam.
3. Use **Load Game** and load save `123`. Do not use **New Game**.
4. Wait for autoconnect to `31.208.67.17:27802` after the save loads.
5. Wait 20-30 seconds after joining.
6. Confirm whether the host and your character are visible.
7. Move a few steps only.
8. Exit cleanly.

## Watch For

Good:

- Join messages appear.
- Both players stay connected.
- A second player appears.
- Small movement syncs.
- No crash during the first minute.

Bad:

- Instant crash on join.
- Duplicate `Player 1` pile.
- Spawn loop.
- Frozen UI.
- Server log repeatedly prints entity spawn requests.
- One tester used New Game while the other loaded an existing save.

After testing, send the latest `KenshiOnline_<pid>.log`, `KenshiOnline_Server.log`, and any `KenshiMP_CrashReport_*.txt`.

