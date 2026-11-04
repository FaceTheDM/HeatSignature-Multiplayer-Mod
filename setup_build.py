"""Fetches everything needed to build HS_Coop into this folder (nothing is installed system-wide):

  vendor/HeatSignatureModLoader   piepieonline's mod loader source (headers + variable table), pinned commit
  tools/zig-x86_64-windows-0.14.1 portable Zig compiler, checked against its official SHA-256 (pinned below)
  tools/python                    pefile, used by check_build.py

  python setup_build.py

Needs git and Python 3.10+ on PATH. Safe to run again: finished steps are skipped."""
import hashlib, pathlib, subprocess, sys, urllib.request, zipfile

ROOT = pathlib.Path(__file__).resolve().parent
LOADER_REPO = 'https://github.com/piepieonline/HeatSignatureModLoader.git'
LOADER_COMMIT = 'aaded27d253e0158c58bb498d84ff0a8b87781ab'
ZIG_VERSION = '0.14.1'
ZIG_URL = f'https://ziglang.org/download/{ZIG_VERSION}/zig-x86_64-windows-{ZIG_VERSION}.zip'
ZIG_SHA256 = '554f5378228923ffd558eac35e21af020c73789d87afeabf4bfd16f2e6feed2c'   # as published in ziglang.org/download/index.json

def fetch(url):
    return urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'HS_Coop-build-setup'})).read()

# 1. Mod loader source at the commit the mod is built against.
loader = ROOT / 'vendor' / 'HeatSignatureModLoader'
if not (loader / '.git').exists():
    subprocess.run(['git', 'clone', LOADER_REPO, str(loader)], check=True)
subprocess.run(['git', '-C', str(loader), 'checkout', '--quiet', LOADER_COMMIT], check=True)
print('Mod loader source ready at', LOADER_COMMIT[:7])

# 2. Portable Zig, verified against the official download index.
tools = ROOT / 'tools'; tools.mkdir(exist_ok=True)
zig_dir = tools / f'zig-x86_64-windows-{ZIG_VERSION}'
if not (zig_dir / 'zig.exe').exists():
    archive = tools / 'zig.zip'
    if not archive.exists():
        print('Downloading Zig', ZIG_VERSION, flush=True)
        archive.write_bytes(fetch(ZIG_URL))
    if hashlib.sha256(archive.read_bytes()).hexdigest() != ZIG_SHA256:
        archive.unlink(); sys.exit('Zig download did not match its published SHA-256; deleted it, run again')
    with zipfile.ZipFile(archive) as z: z.extractall(tools)
print('Zig ready')

# 3. pefile for check_build.py.
if not (tools / 'python' / 'pefile.py').exists():
    subprocess.run([sys.executable, '-m', 'pip', 'install', '--target', str(tools / 'python'), 'pefile==2024.8.26'], check=True)
print('pefile ready')
print('Done. Next: python build.py')
