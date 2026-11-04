# Heat Signature co-op (HS_Coop) — player guide

Two players, each on their own PC, each with their own copy of Heat Signature on Steam (patch 2021.1.20.1; the mod
refuses to run on any other version). One player hosts and gets a lobby code, the other enters it. The connection goes
through Steam, so there are no addresses or router settings; an IP-address code is the fallback.

## What you get

The aim is that what one of you sees, the other sees.

**You and your partner**
- Same galaxy, same stations, same missions. Your partner appears as their own character (look, weapon, pose, shots)
  on any station or ship you are both aboard; in their pod you see their pod; drifting in space you see them floating.
- A blue arrow always points to your partner. It flashes red, with a timer at the bottom of the screen, when they are
  drifting in space or bleeding out. An orange arrow points to your partner's mission ship.
- The status line at the bottom left says where your partner is, both of your missions, alarms and shared slow-mo.

**Missions**
- Both of you can run your own missions at the same time; your partner's mission ship exists in your game too.
- When a mission is completed, the partner gets half its pay (at least 10). If you are still aboard your partner's
  mission ship when they finish, you come back with them.
- Kills, knockouts, witnesses and alarms count for the mission owner, whoever caused them.

**On a ship you share**
- Guards are run by one game (the host's when you are both aboard; otherwise the game of whoever is there first) and
  look and act the same on both screens: movement, punches, aim lasers, being knocked out, killed, carried, searched.
  Either of you can knock out or kill guards; guards spot, chase and shoot either of you. Noises either of you makes
  (punches, thrown items) alert them.
- Doors, crates, money, items on the floor (including dropped weapons and items either of you drops), keycards, fuel
  barrels, windows, terminals and sentries are shared: picked up, opened, broken or unlocked for one is so for both.
  Keycards: whichever of you gets one, both have it. Crates open for both, and what is inside goes to whoever opened
  it. While they choose, your game stands still and shows their choices: click one to suggest it (it is marked in their
  menu). Drop an item to give it to your partner. Airlock doors match when you both docked at the same airlock (F9 behind
  your partner); otherwise each game shows its own airlock.
- Explosions and bullets (yours, your partner's, the guards') are seen in both games; damage is only dealt once.
- Gadget effects (glitch, swapper, teleports) and your partner's sounds are seen and heard.
- Slow-mo: when either of you slows time, both games slow down, with the slow-mo screen effect.
- Your partner's slipstream shows as a trail behind their pod.
- Pause: when either of you opens the pause menu (Escape), both games stop; the other screen says who paused.

**Space**
- Passing traffic ships are the same for both of you (spawned by the host's game; far from the host, by the partner's).
- Mission ships fly the same in both games.
- **Rescue**: fly your pod to a partner drifting in space. When you reach them they ride along in your pod (their view
  follows you) until you arrive at a station or ship; they can press F9 to take their own pod instead.
- **Docking behind your partner**: when your partner's pod is docked at a ship and you fly your pod close to it, press
  F9 to dock at the same airlock.

## The co-op menu (F8)

| Key | Action |
|---|---|
| 1 | Join your partner (with your pod, docked at their ship; or your pod next to theirs in space) |
| 2 | Ask your partner to come to you (they press F9 to accept) |
| 3 | Return to your starting station |
| 4 | Go to the host's starting station, and ask your partner to come too |
| 5 | Cancel your current mission |
| 6 | Go to the active mission ship with your pod, and ask your partner to come too |
| 7 | (partner only) Download the host's galaxy (then restart the game) |
| 8 | Save a bug report: your log, your partner's log and a screenshot, in the `coop-reports` folder of the game |
| 9 | Close |

Requests from your partner (invites, "come to the station", "come to the mission ship") appear at the top of the screen;
press **F9** within 30 seconds to accept. Nothing moves you without your OK.

## Install (both players)

1. Unzip the whole package anywhere.
2. Close Heat Signature and double-click **Install.bat**. It finds the game through Steam, checks its version and
   puts three files into the game folder: the Heat Signature Mod Loader by piepieonline (`d3d9.dll`,
   `ModLoader.dll`, downloaded from [its release page](https://github.com/piepieonline/HeatSignatureModLoader/releases/tag/v1.1.0)
   and checked against a fixed checksum) and the mod (`mods\HS_Coop.dll`). Without internet access, download
   `HeatSignatureModLoader.zip` from that page yourself and put it next to Install.bat. If Windows shows "Windows protected your PC", click "More info",
   then "Run anyway".

Both players must use the **same zip**. The mod never updates itself: for a new version, both download the new zip
and run Install.bat again. **Uninstall.bat** removes the mod again. For a solo session nothing needs
removing: the mod does nothing until you host or join.

## Connecting

1. Both start the game from Steam and pick a character.
2. **Host:** press Escape, then **F5**. A lobby code appears in the co-op panel at the bottom left and is copied to the
   clipboard. Send it to your partner.
3. **Partner:** copy the code, press Escape, then **F6**. With nothing copied, type the code or press Ctrl+V, then
   Enter. Capitals and dashes do not matter; a typo is refused.
4. The partner is brought to the host's station.

**Steam invite instead of a code:** once the host has pressed F5, the host can invite the partner from the Steam friends
list ("Invite to Game"), or the partner can choose "Join Game" on the host. Accepting joins the host's game as soon as
the partner is in the game with a character picked; if the partner's game is closed, Steam starts it first.

**If Steam does not connect you:** while hosting, the host presses **F6** to switch to an IP-address code (28
characters). The games then talk directly. On the same home network that always works. Over the internet the host's
router has to let the connection in: the mod asks the router itself (UPnP) and the panel says whether that worked;
if not, forward UDP port 27015 to the host's PC, or both use a VPN such as Tailscale or Radmin VPN.

A fixed setup with a `coop.ini` file (address and password, no codes) is still possible: see `advanced/install.py`
(needs Python).

## Sharing the galaxy

You must play in the same galaxy. When you connect and your galaxies differ, the partner's game downloads the host's
galaxy (and crew) automatically into a galaxy slot named "Galaxy Coop" (your own galaxy is kept) and the status line
says to restart the game. To pick up the host's newer progress later, use F8 → 7 and restart.

Manual alternative (game closed, needs Python; the script is in `advanced/`): host `python coop_galaxy.py export` → send `coop-galaxy.zip`; partner
`python coop_galaxy.py import coop-galaxy.zip`. The export includes the host's crew; `--keep-characters` also copies
the partner's own crew in. `python coop_galaxy.py restore` switches back to your own galaxy.

Don't both play the same character: pick different ones (the status line warns you).

## Playing

After connecting with a lobby code the partner arrives at the host's station. Later, if you are apart, use F8 → 1 or
2 to meet, or fly over.

## Known limits

- Guards who arrive mid-mission (defenders) while only one of you is aboard exist only in that player's game.
- Shop purchases, money and station capture belong to each player's own save.
- Your partner's pod is a picture: no collision, and you cannot board it.

**Found a bug?** Press F8, then 8, right after it happens. Send the newest folder from `coop-reports` (in the game
folder). If the game stopped with an error, send the newest file from `coop-logs` instead: the last five sessions are
kept there.

To remove the mod, run **Uninstall.bat**.
