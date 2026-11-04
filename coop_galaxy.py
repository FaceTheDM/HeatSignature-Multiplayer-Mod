"""Share one galaxy between co-op players (both must play in the same galaxy).

  HOST:   python coop_galaxy.py export                 -> writes coop-galaxy.zip (the galaxy and its crew)
  CLIENT: python coop_galaxy.py import coop-galaxy.zip -> installs it as the galaxy slot "Galaxy Coop" and selects it
  CLIENT: python coop_galaxy.py restore                -> switches back to the galaxy that was selected before the import

Options: --save-dir DIR (default %APPDATA%/Heat_Signature), --keep-characters (import: copy your current crew into the
co-op galaxy as extra characters). Close Heat Signature first. Everything replaced is backed up next to
the save as *.coop-backup-<time>."""
import argparse,os,pathlib,re,shutil,sys,time,zipfile
SLOT='Galaxy Coop'
p=argparse.ArgumentParser(); p.add_argument('action',choices=['export','import','restore']); p.add_argument('zip',nargs='?')
p.add_argument('--save-dir',default=os.path.join(os.environ.get('APPDATA',''),'Heat_Signature')); p.add_argument('--keep-characters',action='store_true')
a=p.parse_args()
save=pathlib.Path(a.save_dir); options=save/'Options.txt'
if not options.exists(): sys.exit(f'No Options.txt in {save}; pass --save-dir')
stamp=time.strftime('%Y%m%d-%H%M%S')
text=options.read_text()
current=(re.search(r'GalaxyFolder = (.*)',text) or [None,'Galaxy 1\\'])[1].strip()
current_dir=save/current.rstrip('\\/')
def select(folder):
    shutil.copy2(options,options.with_name(f'Options.txt.coop-backup-{stamp}'))
    options.write_text(re.sub(r'GalaxyFolder = .*',lambda m:f'GalaxyFolder = {folder}\\',text))
if a.action=='export':
    out=pathlib.Path(a.zip or 'coop-galaxy.zip')
    # The galaxy file refers to the crew's character files (personal missions are loaded from them), so the crew is
    # part of the galaxy: without it the partner's game cannot open the mission computer.
    files=[f for f in current_dir.rglob('*') if f.is_file()]
    if not any(f.name=='Galaxy.txt' for f in files): sys.exit(f'{current_dir} has no Galaxy.txt; play once in that galaxy first')
    with zipfile.ZipFile(out,'w',zipfile.ZIP_DEFLATED) as z:
        for f in files: z.write(f,f.relative_to(current_dir).as_posix())
    print(f'Exported {len(files)} world files of "{current}" to {out.resolve()}. Send it to your partner.')
elif a.action=='import':
    if not a.zip: sys.exit('import needs the zip from the HOST')
    target=save/SLOT
    if target.exists(): shutil.move(str(target),str(save/f'{SLOT}.coop-backup-{stamp}'))
    target.mkdir()
    with zipfile.ZipFile(a.zip) as z:
        for n in z.namelist():
            if n.startswith('/') or '..' in n.split('/') or '\\' in n or ':' in n: sys.exit(f'unexpected entry {n} in zip; refusing')
        z.extractall(target)
    if a.keep_characters and (current_dir/'Characters').exists(): shutil.copytree(current_dir/'Characters',target/'Characters',dirs_exist_ok=True)
    # Re-importing while the co-op galaxy is selected must keep the player's own galaxy as the one to restore.
    if current.rstrip('\\/')!=SLOT: (save/'.coop-previous-galaxy').write_text(current)
    select(SLOT)
    print(f'Imported the HOST galaxy as "{SLOT}" and selected it (previous: "{current}"). Start Heat Signature normally.')
else:
    prev=save/'.coop-previous-galaxy'
    if not prev.exists(): sys.exit('nothing to restore')
    select(prev.read_text().strip().rstrip('\\/'))
    print(f'Selected "{prev.read_text().strip()}" again. The co-op galaxy stays in "{SLOT}".')
