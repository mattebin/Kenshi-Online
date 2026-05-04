# Playing together — step-by-step

A short recipe for getting a working two-player session on Kenshi 1.0.68
with this mod installed on both machines. Follow the steps in order — the
sync hooks are sensitive to whether you connect before or after loading a
save, and to which scenario you start in.

## 0. One-time setup

Both players need the mod installed. Easiest path: download the latest
`KenshiMP-Setup-*.exe` from the Releases page and run it. The installer
auto-detects Kenshi, takes backups of the files it touches, and copies
the DLL, GUI layouts, and mod files into place.

The host also needs `KenshiMP.Server.exe` (it ships in the same release
zip and is dropped next to `kenshi_x64.exe` by the installer).

## 1. Host: start the server

On the hosting machine, run `KenshiMP.Server.exe` from the Kenshi folder.
A console window opens and prints something like:

```
GameServer: Listening on port 27800 — ready for connections
```

Leave that window open for the whole session — closing it shuts the
server down.

The host shares their public IP and port with the other player. Default
port is `27800` UDP. If the host is over the open internet, port `27800`
needs to be forwarded on the router (UDP only — KenshiMP uses ENet over
UDP, no TCP forward needed). On the same LAN, just use the local IP.

## 2. Each player: launch Kenshi and connect *before* loading a save

This is the only hard rule. **Connect first, load save second.** The
sync hooks need to be live in connected state when the world loads, or
they miss the world-load event.

On each machine independently:

1. Launch Kenshi normally (Steam → Play, or the desktop shortcut).
2. At the main menu, click the new **MULTIPLAYER** button (if missing,
   press F1 to open the same panel).
3. Enter the host's IP and port, pick a player name, hit **Connect**.
4. Wait until the chat / status bar shows **"Connected!"**.

Order between the two players doesn't matter — whoever connects first
just sits at the main menu while the other connects. When both are
connected, the server reports `2 players connected`.

## 3. Both players: load saves around the same time, in a populated area

A few things help here:

- **Same starting zone matters.** Each player loads their own save —
  the mod syncs characters on top of whatever world each of you is in.
  If your saves are in different parts of the map you're literally on
  opposite sides of the world. Pick a save where both of you are in
  the **same town** (Squin, The Hub, World's End, a trader hub —
  anything recognizable). Easiest first-time setup: both start a new
  game with the same scenario (Wanderer puts you both at The Hub).
- **Populated areas help the spawn pipeline warm up.** Right after you
  load in, walk around in a town for ~30 seconds before doing anything
  else. NPCs spawning into your view triggers the engine's character
  factory, which is what the mod's sync layer hooks into.

## 4. Find each other

Open the map. Confirm you both see the same town name. Walk to where
the other player should be. You should see their characters with their
player name above them.

If the other player only shows up in chat and you never see their
characters in-world, that's a known issue being tracked — the spawn
pipeline doesn't always capture on Kenshi 1.0.68. See
`docs/SPEED_SYNC_LEAD.md` for the broader 1.0.68 layout-shift backlog.

## Quick checklist for the host invite message

When inviting someone, send them just enough to act on:

```
Mod installer:  https://github.com/<your-fork>/Kenshi-Online/releases/latest
Server:         <YOUR_PUBLIC_IP>:27800

Recipe:
  1. Run the installer once, accept its auto-detect of your Kenshi folder.
  2. Launch Kenshi.
  3. Main menu → MULTIPLAYER button → paste the IP and port → Connect.
  4. Wait for "Connected!", then load a save (same town as me works
     best — let's both start a new Wanderer game in The Hub).
  5. Walk around for ~30s so the spawn system warms up.
```

## Troubleshooting

- **"Connected!" never appears.** The host hasn't started the server,
  or UDP port `27800` isn't reachable from outside their network. Run
  the `Port Forward Test` workflow from the repo's Actions tab against
  the host's public IP to verify reachability.
- **Connected but can't see the other player's characters.** Spawn
  pipeline didn't capture. Try the populated-area / 30s walk-around
  step. If that still fails, attach the file
  `KenshiOnline_<PID>.log` from your Kenshi folder when reporting.
- **Multiplayer button missing on the main menu.** The GUI layouts
  didn't deploy. Re-run the installer.
- **Game speed feels different.** Speed is locked to 1× for everyone
  on 1.0.68 in this build (documented limit in the README). Pressing
  2× / 3× speeds up your *local* world only.

## Logs to send if something breaks

Send `KenshiOnline_<PID>.log` from the Kenshi install folder. Don't
send `MyGUI.log`, `kenshi.log`, or `KenshiOnline_Server.log` from
the host (those don't contain client-side mod info). The PID is in
the filename — pick the most recent one.
