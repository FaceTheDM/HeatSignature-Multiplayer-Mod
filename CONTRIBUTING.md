# Building, changing and forking HS_Coop

## How the mod works (short version)
HS_Coop is a 32-bit DLL loaded by piepieonline's
[Heat Signature Mod Loader](https://github.com/piepieonline/HeatSignatureModLoader). The loader gives it hooks into the
game's compiled GameMaker scripts and access to game variables. Each player runs their own game:

- Ships and stations are generated identically in both games from a shared seed (`Seed.cpp`).
- The game that runs a ship's guards sends snapshots, and the other game mirrors them with the guards' own AI switched
  off (`Guards.cpp`, `Snapshot.cpp`).
- The partner is drawn as a stand-in player object (`CoopProbe.cpp`, `Visuals.cpp`, `Presence.cpp`).
- Shared objects (doors, crates, items, keycards...) are matched by their generation order (`Objects.cpp`).

| File | What it does |
|---|---|
| `src/CoopProbe.cpp` | Plugin entry, executable version check, local player, stand-in, per-frame tick, status line |
| `src/Net.cpp`, `Wire.h` | UDP or Steam transport, HMAC-SHA256 authentication, snapshots, reliable fragmented messages |
| `src/Steam.cpp`, `Lobby.cpp` | Steam lobbies and invites, IP-address codes, UPnP and STUN |
| `src/Seed.cpp` | Generation seed points (stations, mission ship steps) |
| `src/Missions.cpp` | Partner mission ships, ship copies, reward share |
| `src/Guards.cpp`, `Snapshot.cpp` | Guard authority, snapshots, hits, targeting of the stand-in, alarms |
| `src/Objects.cpp`, `Crate.cpp`, `Hull.cpp` | Shared ship objects, explosions, sounds, crates, module losses |
| `src/Presence.cpp`, `Visuals.cpp` | Arrows, pods, rescue, F8 menu, partner look and weapon |
| `src/TimePolicy.cpp`, `Frame.cpp` | Shared slow-mo and pause, shared coordinate frame, galaxy fingerprint |
| `src/GalaxySync.cpp` | Automatic galaxy transfer to the partner |
| `src/Report.cpp` | Logs and F8 → 8 bug reports (log lines are masked in `CoopLog`, `CoopProbe.cpp`) |
| `src/Console.cpp`, `TestMenu.cpp` | Developer console and F10 test menu (only with `debug=1`) |

When fixing a sync bug, read how the game itself handles that mechanic first. The loader's
`docs/known_variable_properties.txt` and a decompiler such as UndertaleModTool help with that. Fixes that copy the
game's own logic hold up much better than workarounds.

## Build
Needs Windows, git and Python 3.10+. Everything else is downloaded into this folder.

```powershell
python setup_build.py   # mod loader source (pinned commit), portable Zig 0.14.1, pefile
python build.py         # -> build\HS_Coop.dll
python check_build.py   # checks architecture, exports, imports and the loader ABI
python package.py       # -> dist\HS_Coop-<build>.zip
```

The mod loader is built against commit `aaded27d253e0158c58bb498d84ff0a8b87781ab`. Players get the loader DLLs from its
v1.1.0 release, which `install.ps1` downloads and checks against pinned SHA-256 values. The loader is never committed
or put in release zips. If you move to a newer loader release, update the URL and both checksums in `install.ps1`.

The **build id** is a hash of `src/`. Two players with different build ids get a warning in game, so any change to the
source, even a comment, means both players need the new zip.

## Testing
- `coop.ini` next to `Heat_Signature.exe` sets up a fixed connection without codes (see `install.py`). Adding `debug=1`
  turns on the F10 test menu, `coop-trace.txt` and the `coop-command.txt` console, and writes lobby codes and IP
  addresses to the log unmasked. `log=verbose` logs every event.
- Console commands (one line in `coop-command.txt`, separated by `;`): `get ID VAR`, `set ID VAR VALUE`,
  `call SCRIPT SELF ARGS...`, `eng BUILTIN SELF ARGS...`, `list OBJECT`, `gget GLOBALID`, `capture`, `play`,
  `walk RADIUS SECONDS`, `goto FORE STB [SPEED]`, `quit`. `@` is the local player, `!` the stand-in, `$` the player's
  parent, `"text"` a string. Wrong argument counts can crash the game.
- To run two games on one PC, use two separate copies of the game folder with separate save folders (the game reads
  `%APPDATA%`). Never test against your real save without a backup.
- `build.py --dev DIR` adds extra developer sources from `DIR` with `HS_COOP_DEVTOOLS` defined. The original author's
  automated test tooling uses this. That tooling isn't public, and release builds never include it.
- The loader can't unload plugins, so restart the game to load a new DLL.

## No automatic updates
Earlier private builds could download a newer signed DLL from the host. That was removed before this repository went
public: an unmaintained self-updater is a way to put code on players' PCs. Don't add one back. Players install new
versions by hand, and both must use the same zip.

## Pull requests
- One fix or feature per pull request, with a description of how you tested it (two copies, or two PCs).
- Never commit game files, saves, logs, `coop.ini`, keys or built DLLs. `.gitignore` covers the usual
  ones.
- Keep code comments about why a thing is done, especially which game script or variable it depends on.
