# KenshiMP Alpha Installer Notes

Release asset name:

```text
KenshiMP-alpha-2026-05-08-installer.zip
```

## What Changed

- Adds an end-user installer package around the current KenshiMP build.
- Installs Core, server, dashboard, fake test client, watchdog/probe/log tools, layouts, and `kenshi-online.mod`.
- Backs up `Plugins_x64.cfg`, main menu layout, `__mods.list`, and existing KenshiMP binaries.
- Detects common Steam/GOG Kenshi paths, including `C:\SteamLibrary\Steam\steamapps\common\Kenshi`.
- Includes passive fake-client test instructions.
- Documents that real remote spawning is still alpha/risky.

## Tested So Far

- Solo host loaded and connected.
- Local player discovery worked from a normal character named `Truth`.
- Passive FakeBob connected as player 2 and received host position updates.
- Passive FakeBob did not spawn a body in Kenshi.
- Server cleaned stale FakeBob entity state after disconnect.

## Not Claimed Stable

- Real remote player spawning.
- Full gameplay co-op.
- Live client time/speed apply.
- Internet play without manual port forwarding.

## Suggested Release Text

This is an alpha installer for people helping test Kenshi-Online. Use it only with Kenshi 1.0.68 x64. The UI, server join, passive fake-client relay, and local host position broadcast have been tested. Real remote player spawning is enabled for testing but may crash, so back up saves first.

Install by extracting the zip and running `install.bat`. First test should be a controlled two-player spawn test using the checklist in `JOINING.md`.

