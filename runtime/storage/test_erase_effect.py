#!/usr/bin/env python3
"""Model-only programmed-page clearing/neighbor preservation by ARM backend."""
from pathlib import Path
import importlib.util,json,os,struct,subprocess,sys
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1];SDK=HERE.parent/'sdk/nanoos-example'
def main():
 build=HERE/('build/erase-effect-status50' if os.getenv('NANO7_FMC_ERASE_STATUS50') else 'build/erase-effect');build.mkdir(parents=True,exist_ok=True)
 arch=['-mcpu=cortex-a5','-mthumb','-mfloat-abi=soft','-mno-unaligned-access']
 flags=arch+['-std=c99','-O2','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-ffunction-sections','-fdata-sections','-I'+str(ROOT/'NanoOS/src')]
 for name,source in (('app',HERE/'test_erase_arm.c'),('erase',HERE/'fmc_erase.c'),('start',SDK/'start.S')):
  subprocess.run(['arm-none-eabi-gcc']+flags+['-c',str(source),'-o',str(build/(name+'.o'))],check=True)
 elf=build/'effect.elf';subprocess.run(['arm-none-eabi-gcc']+arch+['-nostdlib','-Wl,--gc-sections,-T,'+str(SDK/'app.ld')]+[str(build/(n+'.o')) for n in ('start','app','erase')]+['-o',str(elf)],check=True)
 raw=build/'effect.bin';game=build/'game.bin';subprocess.run(['arm-none-eabi-objcopy','-O','binary',str(elf),str(raw)],check=True)
 subprocess.run([sys.executable,str(SDK/'mkgame.py'),str(raw),str(game)],check=True,stdout=subprocess.DEVNULL)
 spec=importlib.util.spec_from_file_location('pack_effect',ROOT/'bundle_native.py');pack=importlib.util.module_from_spec(spec);spec.loader.exec_module(pack)
 image=build/'image.bin';image.write_bytes((ROOT/'NanoOS/build/kernel.bin').read_bytes()+pack.make_pack(game.read_bytes()))
 media=bytearray()
 for block in range(7,11):
  for bank in range(4):
   for pg in (0,127):
    row=(bank%2)<<19|block<<7|pg;words=[0x11223344]*16;value=0xab000000|block<<8|bank
    media+=struct.pack('<20I',0x4750374e,bank//2,row,15,*words)+bytes(48)+struct.pack('<I',value)*4096
 store=build/'seeded.nand';store.write_bytes(media)
 symbols=subprocess.check_output(['arm-none-eabi-nm','-S',str(elf)],text=True)
 addr,size=next((int(w[0],16),int(w[1],16)) for line in symbols.splitlines() if (w:=line.split())[-1]=='ns_erase_effect_stats')
 out=build/'result';trace=build/'trace.txt'
 subprocess.run([sys.executable,str(ROOT/'emutests/direct_boot.py'),'--image',str(image),'--nand-store',str(store),'--checkpoint-game','--seconds','60','--out',str(out),'--capture',f'ns_erase_effect_stats:{addr:#x}:{size}'],env=dict(os.environ,NANO7_FMC_N31_PARAMS='1',NANO7_FMC_TRACE=str(trace)),check=True,stdout=subprocess.DEVNULL)
 stats=struct.unpack('<5I',(out/'ns_erase_effect_stats.bin').read_bytes());assert stats==(0,4,512,12,0),stats
 commands=[int(line.split()[-1],16) for line in trace.read_text().splitlines() if ' W 008 ' in line]
 assert commands.count(0xd7)==4 and not set(commands)&{0x8a,0x87,0x17}
 (out/'result.json').write_text(json.dumps({'evidence':'SYNTHETIC MEDIA ONLY','stats':stats,'erases':4,'programs':0,'target':'four seeded blocks, one bank each; other-bank first/last pages retained'},indent=2)+'\n')
 print('PASS actual ARM erase MODEL: four CE/CAU routes clear programmed first/last pages and all128pages; twelve neighboring-bank checks retained; four erases/no programs')
if __name__=='__main__':main()
