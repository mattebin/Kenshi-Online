# Joining Test Checklist

Use this for the first real multiplayer spawn test.

This package includes the shared test save `123`. The installer copies it to
`%LOCALAPPDATA%\kenshi\save\123` and backs up any existing save with that name.
The installer creates a neutral `%APPDATA%\KenshiMP\client.json` only if one does
not already exist. Existing config is backed up and left alone.

Use either join mode:

- Manual in-game join: load into the world, open Multiplayer/F1, choose **JOIN GAME**, enter IP and port, then press **CONNECT**.
- Autoconnect: enable auto-connect in the in-game settings or set `autoConnect` to `true` in `client.json` with the wanted `lastServer` and `lastPort`.

## Host

1. Install with `install.bat`.
2. Back up the save you are using.
3. Run `KenshiMP.Server.exe`.
4. Launch Kenshi from Steam.
5. Use **Load Game** and load save `123`. Do not use **New Game** for this test.
6. Open Multiplayer/F1, choose **JOIN GAME**, and connect to `127.0.0.1:27800`.
7. Wait until player 2 joins and spawns.
8. Move a few steps only.
9. Exit cleanly.

## Other Player

1. Install the exact same release package with `install.bat`.
2. Launch Kenshi from Steam.
3. Use **Load Game** and load save `123`. Do not use **New Game**.
4. Open Multiplayer/F1, choose **JOIN GAME**, and connect to the host IP and port.
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

