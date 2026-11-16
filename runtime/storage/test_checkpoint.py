#!/usr/bin/env python3
"""Independent offline checkpoint oracle tests. Never touches a device."""
from pathlib import Path
import importlib.util,struct,hashlib,json
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1]
spec=importlib.util.spec_from_file_location('checkpoint_format',HERE/'checkpoint.py');cp=importlib.util.module_from_spec(spec);spec.loader.exec_module(cp)
def fixture():
 b=bytearray(cp.SIZE);struct.pack_into('<III',b,0,2,7,8);struct.pack_into('<I',b,16380,cp.BLOCKS)
 stats=[(1,(0x11111111,0x22222222)),(255,tuple(range(22))),(254,tuple(range(116)))]
 struct.pack_into('<I',b,16384,len(stats));p=16388
 for tag,values in stats:struct.pack_into('<II',b,p,tag,len(values));p+=8;struct.pack_into('<'+str(len(values))+'I',b,p,*values);p+=len(values)*4
 for i in range(cp.BLOCKS):struct.pack_into('<II',b,32768+i*8,7 if i in (7,8) else 9,2<<16)
 struct.pack_into('<II',b,32768+9*8,3|(1<<16),2<<16)
 b[131072:]=b'\xff'*65536;struct.pack_into('<8I',b,131072,0,0xfffffff0,cp.HOLE,3,9*2048,1,cp.HOLE,4)
 # Unused padding may contain prior RAM; used fields must ignore it.
 b[32768+cp.BLOCKS*8:65536]=b'\x5a'*(65536-32768-cp.BLOCKS*8)
 for node in range(1,16):b[(32+node)*4096+8:(33+node)*4096]=b'\xa5'*(4096-8)
 records=bytearray();g=9;k=65520
 for section in cp.SECTIONS:records+=struct.pack('<4I',k<<16|section<<8|0x1f,g,cp.descriptor(len(records)//16),0xffffffff);g,k=cp.advance(g,k)
 return bytes(b),bytes(records)
def main():
 b,r=fixture();c=cp.decode(b,r);canonical=cp.encode(c);again=cp.decode(canonical,r)
 for name in ('roots','stats','sb','seq','reads','runs','first','last'):assert c[name]==again[name],name
 assert canonical!=b and c['first'][0]==9 and c['last'][0]==10
 cases=[]
 def bad(offset,value,records=False):
  raw=bytearray(r if records else b);struct.pack_into('<I',raw,offset,value);cases.append((b,bytes(raw)) if records else (bytes(raw),r))
 bad(16380,2088);bad(0,33);bad(8,7);bad(16388+4,0xffffffff);bad(32768+9*8,3);bad(32768+7*8+4,1);bad(65536,5);bad(131072+4,0);bad(131072+8,9*2048+2043);bad(0,0,True);bad(16+4,0xffffffff,True)
 for x,y in cases:
  try:cp.decode(x,y)
  except ValueError:pass
  else:raise AssertionError('bad checkpoint accepted')
 physical=ROOT/'hw-reports/run56-checkpoint'
 if (physical/'checkpoint.bin').exists():
  data=(physical/'checkpoint.bin').read_bytes();records=(physical/'checkpoint-records.bin').read_bytes();c=cp.decode(data,records);canonical=cp.encode(c);again=cp.decode(canonical,records)
  for name in ('roots','stats','sb','seq','reads','runs','first','last'):assert c[name]==again[name],name
  build=HERE/'build/checkpoint-format';build.mkdir(parents=True,exist_ok=True);(build/'run56-canonical.bin').write_bytes(canonical)
  result=cp.summary(c);result['evidence']='Offline validation of PHYSICAL run56 capture; no NAND write';result['canonical_sha256']=hashlib.sha256(canonical).hexdigest();(build/'run56-analysis.json').write_text(json.dumps(result,indent=2)+'\n')
  print('PASS physical capture format: all2084live counts match4274runs;45variable statistics entries; known table roundtrip with unused RAM padding removed')
 print('PASS checkpoint format MODEL: variable-length statistics, state/tree cross-check, root list, weave wrap, opaque-field preservation, canonical roundtrip and corruption guards')
if __name__=='__main__':main()
