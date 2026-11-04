"""Install (or disable) the HS_Coop co-op plugin in a Heat Signature installation.

  python install.py host   --password SECRET [--port 27015]             (the player who hosts)
  python install.py client --password SECRET --peer HOST_ADDRESS[:PORT] (the partner)
  python install.py disable                                             (turn co-op off again, e.g. for solo play)

(This is the fixed setup with a coop.ini. Most players only need Install.bat and a lobby code: see PLAYERS.md.)

Options: --game DIR (default: found through Steam's library folders, or the folder this zip was unpacked next to),
--dll PATH (default: HS_Coop.dll next to this script, else build/HS_Coop.dll),
--time independent (each player keeps their own slow-mo even on the same ship).
Requires piepieonline's Heat Signature Mod Loader (d3d9.dll + ModLoader.dll in the game folder). Anything replaced is
backed up with a .coop-backup-<time> suffix."""
import argparse,hashlib,os,pathlib,re,shutil,sys,time
HERE=pathlib.Path(__file__).resolve().parent
SUPPORTED_EXE_SHA256='a5f8befe28786206ed1e2a52385051049524fea51e9e80660e3115e7126274d2'   # Steam build 6124596 (2021.1.20.1)
p=argparse.ArgumentParser(); p.add_argument('action',choices=['host','client','disable'])
p.add_argument('--password'); p.add_argument('--peer'); p.add_argument('--port',type=int,default=27015)
p.add_argument('--game'); p.add_argument('--dll'); p.add_argument('--time',choices=['shared','independent'],default='shared')
a=p.parse_args()

def steam_game_folders():
    """Heat Signature in every Steam library (registry SteamPath + steamapps/libraryfolders.vdf)."""
    roots=[]
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,r'Software\Valve\Steam') as k: roots.append(pathlib.Path(winreg.QueryValueEx(k,'SteamPath')[0]))
    except Exception: pass
    roots+= [pathlib.Path(os.environ.get('ProgramFiles(x86)','C:/Program Files (x86)'))/'Steam']
    libraries=[]
    for r in roots:
        libraries.append(r)
        vdf=r/'steamapps'/'libraryfolders.vdf'
        if vdf.exists():
            for m in re.finditer(r'"path"\s+"([^"]+)"',vdf.read_text(errors='ignore')): libraries.append(pathlib.Path(m.group(1).replace('\\\\','\\')))
    return [l/'steamapps'/'common'/'Heat Signature' for l in libraries]

candidates=[pathlib.Path(a.game)] if a.game else [pathlib.Path.cwd(),HERE.parent,HERE,*steam_game_folders()]
game=next((c for c in candidates if (c/'Heat_Signature.exe').exists()),None)
if not game: sys.exit('Heat_Signature.exe not found; pass --game "C:/.../steamapps/common/Heat Signature"')
print(f'Heat Signature found in {game}')
mods=game/'mods'; stamp=time.strftime('%Y%m%d-%H%M%S')
target=mods/'HS_Coop.dll'
def backup(path):
    if path.exists(): shutil.move(str(path),str(path.with_name(path.name+f'.coop-backup-{stamp}')))
if a.action=='disable':
    if target.exists(): target.rename(target.with_name('HS_Coop.dll.disabled-'+stamp)); print('Co-op plugin disabled (play solo as usual; run install.py again to turn it back on).')
    else: print('Co-op plugin was not installed.')
    sys.exit()
exe_hash=hashlib.sha256((game/'Heat_Signature.exe').read_bytes()).hexdigest()
if exe_hash!=SUPPORTED_EXE_SHA256:
    sys.exit('This Heat_Signature.exe is a different version than HS_Coop supports (Steam build 6124596, patch 2021.1.20.1).\n'
             'The plugin would refuse to run on it. Make sure Steam has the game fully updated (or on the same version as your partner).')
if not (game/'ModLoader.dll').exists() or not (game/'d3d9.dll').exists():
    sys.exit('The Heat Signature Mod Loader is not installed in this game folder (need d3d9.dll and ModLoader.dll).\n'
             'Get it from https://github.com/piepieonline/HeatSignatureModLoader/releases and copy both files next to Heat_Signature.exe.')
if not a.password or len(a.password)<8: sys.exit('--password must be at least 8 characters, and the same for both players')
if any(ch.isspace() or ch in '#;' for ch in a.password): sys.exit('--password must not contain spaces, # or ; (pick letters, digits and other symbols)')
if a.action=='client' and not a.peer: sys.exit('client needs --peer HOST_ADDRESS (e.g. the host\'s Tailscale/ZeroTier/Hamachi, LAN or public IP)')
dll=pathlib.Path(a.dll) if a.dll else next((d for d in (HERE/'HS_Coop.dll',HERE/'build'/'HS_Coop.dll') if d.exists()),None)
if not dll or not dll.exists(): sys.exit('HS_Coop.dll not found; pass --dll')
mods.mkdir(exist_ok=True)
backup(target); shutil.copy2(dll,target)
peer=a.peer if (not a.peer or ':' in a.peer) else f'{a.peer}:{a.port}'
ini=[f'role={"HOST" if a.action=="host" else "CLIENT"}',f'password={a.password}',f'time={a.time}']
ini+= [f'bind=0.0.0.0:{a.port}'] if a.action=='host' else ['bind=0.0.0.0:0',f'peer={peer}']
backup(game/'coop.ini'); (game/'coop.ini').write_text('\n'.join(ini)+'\n')
print(f'Installed {dll.name} into {mods} and wrote {game/"coop.ini"} ({a.action}).')
if a.action=='host': print(f'Allow UDP port {a.port} through this PC\'s firewall. Easiest: both install Tailscale or ZeroTier and use that address;\n'
                           f'otherwise forward UDP {a.port} on your router to this PC.')
