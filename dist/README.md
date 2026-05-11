# Kenshi-Online Alpha Installer

This package installs the current KenshiMP alpha build for Kenshi 1.0.68 x64.

This is a test build, not a finished co-op release. Connection, server join, UI, chat/status, passive fake-client relay, and basic local host position broadcast have been tested. Real remote player spawning is enabled for testing, but it is still the risky part and can crash Kenshi.

## Install

1. Close Kenshi and all KenshiMP tools.
2. Extract this zip anywhere.
3. Run `install.bat`.
4. Let it auto-detect Kenshi, or paste the Kenshi folder path when asked.
5. The installer also copies the shared test save to `%LOCALAPPDATA%\kenshi\save\123`.
6. The installer creates a neutral `%APPDATA%\KenshiMP\client.json` only if one does not already exist. Existing config is backed up and left alone.
7. Start `KenshiMP.Server.exe`.
8. Launch Kenshi from Steam.
9. Use **Load Game** with save `123`, then join manually from the in-game Multiplayer/F1 menu.

The installer backs up changed files to:

```text
<Kenshi>\KenshiMP_backup_<timestamp>
```

Run `uninstall.bat` to remove the mod and restore backed-up files.

## Joining Modes

This build supports both join flows.

Manual in-game join:

1. Load into the world first.
2. Open the Multiplayer/F1 menu.
3. Choose **JOIN GAME**.
4. Enter the server IP and port, then press **CONNECT**.

Autoconnect:

1. Enable auto-connect in the in-game Multiplayer settings, or set `autoConnect` to `true` in `%APPDATA%\KenshiMP\client.json`.
2. Set `lastServer` and `lastPort` to the server you want.
3. Load into the world and let the client connect automatically.

The dashboard is optional. Use it if you want helper status/log windows; it is not required for manual in-game join.

## Files Included

| File | Purpose |
| --- | --- |
| `KenshiMP.Core.dll` | Main Ogre plugin loaded by Kenshi |
| `KenshiMP.Server.exe` | Local/LAN dedicated server |
| `KenshiMP.Dashboard.exe` | Status/log helper |
| `KenshiMP.TestClient.exe` | Fake client for relay/spawn tests |
| `KenshiMP.Injector.exe` | Legacy launcher/helper |
| `KenshiMP.CrashWatchdog.exe` | Out-of-process crash report helper |
| `KenshiMP.Probe.exe` | Out-of-process read-only probe |
| `KenshiMP.LogTail.exe` | Log tail helper |
| `KenshiMP.Cartographer.exe` | Reverse-engineering helper |
| `KenshiMP.SafeAddon.dll` | Safe addon fallback build |
| `kenshi-online.mod` | Multiplayer start/templates |
| `install.bat` | Installer |
| `uninstall.bat` | Uninstaller |
| `KenshiMP.Restore.bat` | Restore newest binary backups |
| `KenshiMP.SwitchAddon.bat` | Toggle Core/SafeAddon plugin line |

## First Two-Player Test

Use the same package on both PCs.

Host:

1. Run `KenshiMP.Server.exe`.
2. Launch Kenshi.
3. Use **Load Game** with save `123`. Do not use **New Game** for the multiplayer test.
4. Open Multiplayer/F1, choose **JOIN GAME**, and connect to `127.0.0.1:27800`.
5. Wait for player 2 before moving.

Other player:

1. Install the same package.
2. Launch Kenshi.
3. Use **Load Game** with save `123`. Do not use **New Game** for the multiplayer test.
4. Open Multiplayer/F1, choose **JOIN GAME**, and connect to the host IP and port.
5. Wait 20-30 seconds after joining.
6. Move only a few steps for the first test.

Good result: join messages, visible second player, no spawn loop, no duplicate player pile, no crash after small movement.

## Passive Fake-Client Test

This tests networking without spawning a remote body in Kenshi:

```bat
KenshiMP.TestClient.exe 127.0.0.1 27800 FakeBob --passive
```

Expected output includes:

```text
[PASSIVE] Position update from player 1: entity=0 ...
```

In Kenshi, FakeBob should join the server but no FakeBob body should spawn.

## Known Limitations

- Real remote player spawning is experimental and may crash.
- Time/speed sync is not yet proven against live Kenshi state.
- Client-side time apply is not presented as working.
- Server is local/LAN by default; internet play requires UDP `27800` forwarding.
- Both players must use the exact same package/build.
- Autoconnect is supported but not forced by the installer.
- If Kenshi fails to launch after testing, run `uninstall.bat` or verify game files in Steam.

