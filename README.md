# HS_Coop: two-player co-op for Heat Signature

An unofficial mod that lets two players, each on their own PC with their own copy of
[Heat Signature](https://store.steampowered.com/app/268130/Heat_Signature/), play in the same galaxy: same stations,
same ships, same guards, each other's missions.

> **Status: playable, with bugs.** It has been tested a lot between two copies of the game on one PC, and in a few
> real sessions over the internet. Expect rough edges and occasional desyncs. Bug reports are very welcome (see below).

## What works
- You see your partner on any station or ship you are both on (their look, weapon, pose and shots), plus their pod in
  space and arrows pointing to them.
- Both players can run missions at the same time, and the partner gets a share of the pay.
- Guards are run by one game and mirrored in the other: either player can knock them out or kill them, and they spot,
  chase and shoot either player.
- Doors, crates, items, keycards, barrels, windows, terminals, sentries, explosions and carried bodies are shared.
- Slow-mo and pause are shared. You can rescue a partner drifting in space, and dock behind your partner.
- You connect with a Steam lobby code or a Steam invite, and fall back to an IP-address code if Steam can't connect.
- The partner's game downloads the host's galaxy automatically.

Full feature list and controls: **[PLAYERS.md](PLAYERS.md)**.

## Requirements
- Windows, and Heat Signature **bought on Steam** and started from Steam (each player needs their own copy).
- Game version **2021.1.20.1**, the current Steam version. The mod refuses to run on any other version.
- **Both players install the same release zip.** Different versions refuse to connect to each other.
- An internet connection the first time you install (to download the mod loader, see below).

## Install (both players)
1. On this page, click **[Releases](../../releases)** (right-hand side), and under the newest release download
   `HS_Coop-<version>.zip`. Don't use the green "Code" button: that is the source code, not the mod.
2. Unzip it: right-click the zip, choose **Extract All**, and open the extracted folder. Don't run anything from inside
   the zip.
3. Close Heat Signature.
4. Double-click **Install.bat**. If Windows shows "Windows protected your PC", click **More info**, then **Run anyway**
   (the zip isn't signed with a paid certificate; [SECURITY.md](SECURITY.md) explains what the installer does).
5. A window shows what it did and ends with "Done. The co-op mod is installed." Press Enter to close it.

If the installer can't find the game, it asks you to pick the folder that contains `Heat_Signature.exe` (usually
`C:\Program Files (x86)\Steam\steamapps\common\Heat Signature`).

### What the installer puts on your PC
Three files, all in the Heat Signature game folder, and nothing anywhere else:

| File | What it is |
|---|---|
| `d3d9.dll`, `ModLoader.dll` | The [Heat Signature Mod Loader](https://github.com/piepieonline/HeatSignatureModLoader) by piepieonline, which lets the game load mods. Downloaded from [its release page](https://github.com/piepieonline/HeatSignatureModLoader/releases/tag/v1.1.0) and refused unless both files match fixed checksums. |
| `mods\HS_Coop.dll` | This mod. |

- If a file with the same name was already there, it is renamed to `<name>.coop-backup-<date>`, not deleted.
- While you play, the mod writes its logs (`coop-logs`, `coop-reports`) and a few small `coop-*.txt` files (such as
  the last lobby code) into the game folder.
- **Your saves:** the first time you join a host whose galaxy is different from yours, the host's galaxy is copied
  into a new galaxy slot called "Galaxy Coop", and your game is switched to it. Your own galaxy is kept as it was.
  To switch back to it later, run `advanced\coop_galaxy.py restore` from the zip (needs Python).
- The mod does nothing until you host or join, so solo play is unchanged with it installed.

### Uninstall
Close the game and double-click **Uninstall.bat**. It removes `mods\HS_Coop.dll`, and also removes the mod loader if
it is the exact version the installer put there and no other mods use it. Backups and logs are left alone.

## Playing together
Both players start Heat Signature from Steam and pick a character (different ones). One player hosts; the other joins.
Use one of these three ways.

### 1. Steam invite (easiest, for Steam friends)
1. **Host:** press **Escape**, then **F5**. A lobby code appears in the co-op panel at the bottom left.
2. **Host:** in the Steam friends list, right-click your partner and choose **Invite to Game**. (Or your partner
   right-clicks you and chooses **Join Game**.)
3. **Partner:** accept the invite. If your game is already running with a character picked, you join right away. If
   the game is closed, Steam starts it; you join as soon as you have picked a character.

### 2. Lobby code (anyone, through Steam)
1. **Host:** press **Escape**, then **F5**. The lobby code (15 letters and digits) appears at the bottom left and is
   copied for you. Send it to your partner (Discord, Steam chat, anything).
2. **Partner:** copy the code, then press **Escape**, then **F6**. If nothing was copied, type the code or press
   Ctrl+V, then Enter. Capitals and dashes don't matter, and a typo is refused.

Both of these go through Steam: no IP addresses and no router settings.

### 3. IP-address code (if Steam won't connect)
While hosting, the host presses **F6** to switch to a 28-character IP-address code, and the partner joins with it
the same way. On the same home network it always works. Over the internet the host's router must let the
connection in. The mod tries to open the port itself; if that fails, forward UDP port 27015 to the host's PC, or both
use a VPN such as Tailscale. Anyone with this code can see the host's IP address, so only send it to your partner.

### The first time you connect
If your galaxies differ, the partner's game downloads the host's galaxy, and a message says to restart the game.
Restart Heat Signature, then join again (F6 offers the last code; an invite works too).

### Once you're in
- The partner arrives at the host's station.
- **F8** opens the co-op menu: join your partner, call them to you, go to the mission ship, save a bug report.
- A blue arrow always points to your partner. When your partner sends a request, press **F9** to accept it.

The full guide, with every key and feature: **[PLAYERS.md](PLAYERS.md)**.

## Updating
The mod never updates itself. When a new release comes out, both players download the new zip and run Install.bat
again.

## Known issues
- Guards who arrive mid-mission (defenders) while only one player is aboard exist only in that player's game, and can
  have different names on the two screens.
- Passing traffic ships far from the host can differ between the two games.
- Shop purchases, money and station capture belong to each player's own save.
- The partner's pod is drawn but has no collision and can't be boarded.

## Reporting a bug
Right after it happens, press **F8**, then **8**. That saves both players' logs and a screenshot into the
`coop-reports` folder in the game folder. Open an [issue](../../issues) describing what you did and what you saw, and
attach the newest report folder (zipped). If the game crashed, attach the newest file from `coop-logs` instead.
Lobby codes and IP addresses are masked in the logs. The screenshot in a report can still show a lobby code, so crop
it if you were hosting.

## Building from source / forking
See **[CONTRIBUTING.md](CONTRIBUTING.md)**.

## Security
What the mod sends over the network, what it trusts, and how to report a security problem:
[SECURITY.md](SECURITY.md).

## Credits and license
- Built on the **[Heat Signature Mod Loader](https://github.com/piepieonline/HeatSignatureModLoader) by piepieonline**
  (CC BY-NC 4.0). It isn't included here: the installer downloads it from the author's release. This mod is free and
  must not be sold.
- Heat Signature is by Suspicious Developments. This is a fan project, not affiliated with or endorsed by them. No game
  files are included; you need your own copy of the game.
- HS_Coop's own code is under the [MIT license](LICENSE). Third-party notices: [NOTICE.md](NOTICE.md).
