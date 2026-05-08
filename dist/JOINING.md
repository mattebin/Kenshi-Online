# Joining Test Checklist

Use this for the first real multiplayer spawn test.

## Host

1. Install with `install.bat`.
2. Back up the save you are using.
3. Run `KenshiMP.Server.exe`.
4. Launch Kenshi from Steam.
5. Load the multiplayer save/start.
6. Open Multiplayer and join `127.0.0.1:27800`.
7. Wait until player 2 joins and spawns.
8. Move a few steps only.
9. Exit cleanly.

## Other Player

1. Install the exact same release package with `install.bat`.
2. Launch Kenshi from Steam.
3. Load the same multiplayer save/start.
4. Open Multiplayer and join the host IP on port `27800`.
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

After testing, send the latest `KenshiOnline_<pid>.log`, `KenshiOnline_Server.log`, and any `KenshiMP_CrashReport_*.txt`.

