# Known Issues

Status for this alpha package, tested around Kenshi 1.0.68 x64.

## Remote Player Spawning

Real remote player body spawning is still experimental. A passive fake client can connect and receive host position updates without spawning a body. A non-passive fake client can request a spawn and make a visible FakeBob body, but an earlier run ended with heap corruption after that path.

Treat real two-player spawning as the next test target, not as stable gameplay.

## Time / Speed Sync

The old `TIME_UPDATE`, `GAME_FRAME_UPDATE`, `TimeManager+0x08/+0x10`, and `GameWorld+0x700` paths are not proven live on Kenshi 1.0.68. Server time packets may still be generated independently by the server. Client-side time apply is not presented as working.

## Save / World State

The server persists `world.kmpsave`. If a test run crashes while a spawned remote player exists, the next run may load stale player-owned entities. Current server filtering skips many stale/ownerless entities, but bad saves should still be backed up before testing.

## UI / Shutdown

Logs may contain `MyGuiBridge: setVisible crashed` during shutdown. Recent CrashWatchdog reports classified this as a clean user exit when the process ended normally.

## Networking

The server listens locally/LAN by default. For internet tests, forward UDP `27800` to the host PC. UPnP is disabled by default.

## Required Test Logs

For useful bug reports, keep:

- `<Kenshi>\KenshiOnline_<pid>.log`
- `<Kenshi>\KenshiOnline_Server.log`
- `<Kenshi>\KenshiMP_CrashReport_<timestamp>.txt`
- The TestClient console output if using FakeBob

