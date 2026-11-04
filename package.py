"""Build the package to send to a co-op partner: dist/HS_Coop-<build>.zip with the plugin, a one-click
installer (Install.bat / Uninstall.bat; needs nothing but Windows) and the guide.
Both players must use the same zip (the plugin build id must match). Older zips are removed so only one is around."""
import pathlib,re,subprocess,sys,zipfile
ROOT=pathlib.Path(__file__).resolve().parent
out=subprocess.run([sys.executable,str(ROOT/'build.py')],capture_output=True,text=True,cwd=ROOT)
build=re.search(r'Built ([0-9a-f]{8})',out.stdout+out.stderr)
dll=ROOT/'build'/'HS_Coop.dll'
# The build must have succeeded. This is the release build: no developer channel (that is only in build.py --dev).
if not build or not dll.exists(): sys.exit('build failed - nothing packaged:\n'+out.stdout+out.stderr)
# The Heat Signature Mod Loader (piepieonline, CC BY-NC 4.0) is not in the package: install.ps1 downloads it from its
# author's GitHub release and checks its SHA-256.
name=f'HS_Coop-{build.group(1)}'
dist=ROOT/'dist'; dist.mkdir(exist_ok=True)
for old in dist.glob('HS_Coop-*.zip'):
    if old.name!=f'{name}.zip': old.unlink()
target=dist/f'{name}.zip'
with zipfile.ZipFile(target,'w',zipfile.ZIP_DEFLATED) as z:
    z.write(ROOT/'install.bat',f'{name}/Install.bat'); z.write(ROOT/'uninstall.bat',f'{name}/Uninstall.bat')
    z.write(ROOT/'HOW_TO_PLAY.txt',f'{name}/HOW TO PLAY.txt'); z.write(ROOT/'PLAYERS.md',f'{name}/PLAYERS.md')
    z.write(dll,f'{name}/files/HS_Coop.dll'); z.write(ROOT/'install.ps1',f'{name}/files/install.ps1')
    for f in ('install.py','coop_galaxy.py'): z.write(ROOT/f,f'{name}/advanced/{f}')
print('Wrote',target)
