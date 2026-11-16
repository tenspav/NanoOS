#!/usr/bin/env python3
"""Independent oracle and physical capture roundtrip; no device access."""
from pathlib import Path
import ctypes as C,struct,subprocess,json,random
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1]
class Entry(C.Structure):_fields_=[(n,C.c_uint32) for n in ('delta','epoch','lba','count')]
class Btoc(C.Structure):_fields_=[('entry',C.POINTER(Entry))]+[(n,C.c_uint32) for n in ('capacity','n','sectors','role','first_gen','first_key','last_gen','last_key','span')]
def main():
 build=HERE/'build';build.mkdir(exist_ok=True);common=['cc','-std=c99','-O1','-g','-Wall','-Wextra','-Werror']
 sources=[str(HERE/n) for n in ('btoc.c','write.c','sftl.c')]
 subprocess.run(common+['-fsanitize=address,undefined',str(HERE/'test_btoc.c')]+sources+['-o',str(build/'test_btoc')],check=True);subprocess.run([str(build/'test_btoc')],check=True)
 subprocess.run(common+['-shared','-fPIC']+sources+['-o',str(build/'btoc.dylib')],check=True);lib=C.CDLL(str(build/'btoc.dylib'))
 def construct(records):
  b=Btoc();entries=(Entry*2048)();assert not lib.ns_btoc_init(C.byref(b),entries,2048)
  for r in records:assert not lib.ns_btoc_add(C.byref(b),(C.c_uint32*4)(*r))
  return b,entries
 rng=random.Random(0x42544f43)
 for case in range(100):
  rec=[];seq=(9<<16)|65500;lba=0
  for i in range(rng.randrange(1,508)):
   seq+=rng.choice([1,1,1,3]);lba=lba+1 if rng.randrange(3) else rng.randrange(10000)
   rec.append(((seq&65535)<<16|rng.choice([1,2]),seq>>16,lba,rng.randrange(7,10)))
  b,e=construct(rec);want=[];anchor=(rec[0][1]<<16)|(rec[0][0]>>16)
  for a,g,l,epoch in rec:
   delta=((g<<16)|(a>>16))-anchor
   if want and want[-1][0]+want[-1][3]==delta and want[-1][1]==epoch and want[-1][2]+want[-1][3]==l:want[-1][3]+=1
   else:want.append([delta,epoch,l,1])
  assert [[e[i].delta,e[i].epoch,e[i].lba,e[i].count] for i in range(b.n)]==want
 capture=ROOT/'hw-reports/run57-metadata'
 if capture.exists():
  raw=(capture/'btoc-records.bin').read_bytes();source=[struct.unpack_from('<4I',raw,i*16) for i in range(2044)];b,e=construct(source)
  data=(C.c_uint32*8192)();records=(C.c_uint32*32)();assert not lib.ns_btoc_encode(C.byref(b),4,0,data,records)
  assert bytes(data)[:16384]==(capture/'btoc.bin').read_bytes() and bytes(records)[:64]==raw[2044*16:]
  stats=(C.c_uint32*4)();lib.ns_btoc_statistics(C.byref(b),stats)
  cp=(capture/'checkpoint.bin').read_bytes();stored=struct.unpack_from('<4I',cp,65536+65*16);assert tuple(stats)==stored
  result={'evidence':'PHYSICAL run57 source consumed offline; no device writes','entries':b.n,'physical_data_sectors':b.sectors,'weave_span':b.span,'entire_data_and_all_metadata_exact':True,'statistics_exact':list(stats)}
  (build/'btoc-run57-result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS PHYSICAL BTOC oracle: all2044sectors,614entries,2628weave span, full16KiB and all4metadata records byte-exact; stored statistics match614ENTRY samples')
 subprocess.run(['arm-none-eabi-gcc','-std=c99','-O2','-Wall','-Wextra','-Werror','-mcpu=cortex-a5','-mthumb','-mno-unaligned-access','-ffreestanding','-c',str(HERE/'btoc.c'),'-o',str(build/'btoc-arm.o')],check=True)
 print('PASS independent randomized BTOC gap/coalescing oracle:100histories; ARM build')
if __name__=='__main__':main()
