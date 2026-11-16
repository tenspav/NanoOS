#!/usr/bin/env python3
"""Actual ARM regression for reset/reprobe configuration lifetime."""
from pathlib import Path
import importlib.util,json,os,struct,subprocess,sys
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1];SDK=HERE.parent/'sdk/nanoos-example'
def main():
 build=HERE/'build/reprobe';build.mkdir(parents=True,exist_ok=True)
 arch=['-mcpu=cortex-a5','-mthumb','-mfloat-abi=soft','-mno-unaligned-access']
 flags=arch+['-std=c99','-O2','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-tree-loop-distribute-patterns','-I'+str(ROOT/'NanoOS/src')]
 for name,source in (('app',HERE/'test_reprobe_arm.c'),('start',SDK/'start.S')):
  subprocess.run(['arm-none-eabi-gcc']+flags+['-c',str(source),'-o',str(build/(name+'.o'))],check=True)
 elf=build/'reprobe.elf';subprocess.run(['arm-none-eabi-gcc']+arch+['-nostdlib','-Wl,--gc-sections,-T,'+str(SDK/'app.ld')]+[str(build/(n+'.o')) for n in ('start','app')]+['-o',str(elf)],check=True)
 raw=build/'reprobe.bin';game=build/'game.bin';subprocess.run(['arm-none-eabi-objcopy','-O','binary',str(elf),str(raw)],check=True)
 subprocess.run([sys.executable,str(SDK/'mkgame.py'),str(raw),str(game)],check=True,stdout=subprocess.DEVNULL)
 spec=importlib.util.spec_from_file_location('pack_reprobe',ROOT/'bundle_native.py');pack=importlib.util.module_from_spec(spec);spec.loader.exec_module(pack)
 image=build/'image.bin';image.write_bytes((ROOT/'NanoOS/build/kernel.bin').read_bytes()+pack.make_pack(game.read_bytes()))
 store=build/'seeded.nand';media=bytearray()
 for bank in range(4):media+=struct.pack('<20I',0x4750374e,bank//2,(bank%2)<<19|7<<7,15,*([0x11223344]*16))+bytes(48)+struct.pack('<I',0x12340000|bank)*4096
 store.write_bytes(media);symbols=subprocess.check_output(['arm-none-eabi-nm','-S',str(elf)],text=True)
 addr,size=next((int(w[0],16),int(w[1],16)) for line in symbols.splitlines() if (w:=line.split())[-1]=='ns_reprobe_stats')
 out=build/'result';trace=build/'trace.txt'
 subprocess.run([sys.executable,str(ROOT/'emutests/direct_boot.py'),'--image',str(image),'--nand-store',str(store),'--checkpoint-game','--seconds','60','--out',str(out),'--capture',f'ns_reprobe_stats:{addr:#x}:{size}'],env=dict(os.environ,NANO7_FMC_N31_PARAMS='1',NANO7_FMC_REQUIRE_POWER2='1',NANO7_FMC_TRACE=str(trace)),check=True,stdout=subprocess.DEVNULL)
 stats=struct.unpack('<5I',(out/'ns_reprobe_stats.bin').read_bytes());print('reprobe stats',stats)
 result={'evidence':'ACTUAL ARM MODEL: reset drops per-CE configuration','stats':stats,'pass':stats==(0,3,12,12,1)}
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');assert result['pass'],result
 print('PASS actual ARM reprobe: three dual-CE reset cycles, twelve stale-config reads rejected transactionally, twelve restored CE/CAU reads exact; no array mutation')
if __name__=='__main__':main()
