import pathlib,subprocess,sys
ROOT=pathlib.Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'tools/python'))
import pefile
zig=ROOT/'tools/zig-x86_64-windows-0.14.1/zig.exe'
exe=ROOT/'build/abi_smoke.exe'
subprocess.run([str(zig),'c++','-target','x86-windows-gnu','-std=c++17','-O2','-I'+str(ROOT/'vendor/HeatSignatureModLoader/ModLoader'),str(ROOT/'diagnostics/abi_smoke.cpp'),'-o',str(exe)],check=True)
dll=ROOT/'build/HS_Coop.dll'
pe=pefile.PE(str(dll))
assert pe.FILE_HEADER.Machine==0x14c
names={e.name for e in pe.DIRECTORY_ENTRY_EXPORT.symbols}
assert {b'ModInit',b'ModApiVersion'}<=names
print('Imports:',[e.dll.decode() for e in pe.DIRECTORY_ENTRY_IMPORT])
r=subprocess.run([str(exe),str(dll)],check=True,capture_output=True,text=True)
print(r.stdout)
(ROOT/'build/build-check.txt').write_text(r.stdout)
