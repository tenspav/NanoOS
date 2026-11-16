#!/usr/bin/env python3
"""C current-map materialization and checkpoint RAM planner vs Python oracle."""
from pathlib import Path
import ctypes as C,importlib.util,struct,subprocess,bisect,random,json
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1]
def load(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
fmt=load('cpformat',HERE/'checkpoint.py');mapping=load('mapping_types',HERE/'test_storage.py')
class Closed(C.Structure):_fields_=[('block',C.c_uint32),('tail',C.c_uint32),('role',C.c_uint32),('statistics',C.c_uint32*4)]
class Plan(C.Structure):_fields_=[(n,C.c_uint32) for n in ('phase','runs','nodes','checks','changed_blocks','first_gen','first_key','last_gen','last_key')]+[('refs',C.c_uint32*2084)]
def main():
 build=HERE/'build';common=['cc','-std=c99','-O1','-g','-Wall','-Wextra','-Werror']
 subprocess.run(common+['-shared','-fPIC',str(HERE/'sftl.c'),str(HERE/'checkpoint_plan.c'),'-o',str(build/'checkpoint_plan.dylib')],check=True)
 lib=C.CDLL(str(build/'checkpoint_plan.dylib'));source=ROOT/'hw-reports/run57-metadata'
 if source.exists():data=(source/'checkpoint.bin').read_bytes();records=(source/'checkpoint-records.bin').read_bytes()
 else:
  fixture=load('cp_fixture',HERE/'test_checkpoint.py');data,records=fixture.fixture();data=bytearray(data);struct.pack_into('<I',data,32768+10*8,1);data=bytes(data)
 c=fmt.decode(data,records)
 mask=(C.c_uint8*2084)(*([15]*2084));base=(mapping.Run*8192)();updates=(mapping.Update*8192)();m=mapping.Map();assert not lib.ns_sftl_init(C.byref(m),2084,c['sectors'],mask,base,8192,updates,8192,*c['last'])
 for i,(lba,vba,count) in enumerate(c['runs']):base[i]=mapping.Run(lba,vba,count)
 m.nruns=len(c['runs']);assert not lib.ns_sftl_finish(C.byref(m))
 raw=C.create_string_buffer(data);meta=(C.c_uint32*192).from_buffer_copy(records);out=(C.c_uint32*49152)();outmeta=(C.c_uint32*192)();runs=(mapping.Run*8192)();work=(mapping.Update*8192)();p=Plan()
 beforebase=bytes(base);beforeupdates=bytes(updates)
 def plan(closed=None,n=0):return lib.ns_checkpoint_plan(C.byref(p),C.byref(m),raw,meta,closed,n,out,outmeta,runs,8192,work,8192)
 rc=plan();assert not rc and p.phase==2 and not p.changed_blocks,(rc,p.phase,p.runs,p.changed_blocks)
 d=fmt.decode(bytes(out),bytes(outmeta));assert d['refs']==c['refs'] and d['roots']==c['roots']
 assert bytes(base)==beforebase and bytes(updates)==beforeupdates and m.ready
 # Four new scratch-sector mappings into an explicitly closed former READY.
 target=next(i for i,s in enumerate(c['sb']) if i>=4 and s[0]&255==1)
 g,k=c['last'];new={};epoch=min(29,g);lba_start=0x30c084 if c['sectors']>0x30c087 else 0
 for i in range(4):
  g,k=fmt.advance(g,k);lba=lba_start+i;new[lba]=target*2048+i
  assert not lib.ns_sftl_replay(C.byref(m),new[lba],(C.c_uint32*4)(k<<16|1,g,lba,epoch))
 assert not lib.ns_sftl_finish(C.byref(m))
 assert plan()==-4 and m.ready # Unclosed READY with live mappings is refused.
 closed=(Closed*1)(Closed(target,8,0,(C.c_uint32*4)(epoch,epoch,epoch,1)))
 assert not plan(closed,1) and p.phase==2 and p.changed_blocks>=1 and p.checks>=10000+2*len(c['runs'])
 d=fmt.decode(bytes(out),bytes(outmeta));assert d['sb'][target][0]&65535==0x1003 and d['refs'][target]==4
 starts=[r[0] for r in c['runs']];dst=[r[0] for r in d['runs']]
 def lookup(runs,starts,lba):
  first,v,count=runs[bisect.bisect_right(starts,lba)-1];return v if v==fmt.HOLE else v+lba-first
 rng=random.Random(0x4350504c)
 for lba in [*new,*[rng.randrange(c['sectors']) for _ in range(20000)]]:assert lookup(d['runs'],dst,lba)==new.get(lba,lookup(c['runs'],starts,lba))
 # Alias/space/bad root/count/TLV controls preserve the source map.
 savedmap=bytes(m);savedbase=bytes(base);savedupdates=bytes(updates)
 assert lib.ns_checkpoint_plan(C.byref(p),C.byref(m),out,meta,closed,1,out,outmeta,runs,8192,work,8192)==-1
 assert lib.ns_checkpoint_plan(C.byref(p),C.byref(m),raw,meta,closed,1,out,outmeta,runs,8192,work,3)==-3
 for offset,value in [(16380,2088),(0,33),(16392,0xffffffff),(32768+target*8,1|1<<16)]:
  changed=bytearray(data);struct.pack_into('<I',changed,offset,value);bad=C.create_string_buffer(bytes(changed))
  assert lib.ns_checkpoint_plan(C.byref(p),C.byref(m),bad,meta,closed,1,out,outmeta,runs,8192,work,8192)!=0
 assert bytes(m)==savedmap and bytes(base)==savedbase and bytes(updates)==savedupdates and m.ready
 assert not plan(closed,1)
 (build/'checkpoint-current-plan.bin').write_bytes(bytes(out));(build/'checkpoint-current-plan-records.bin').write_bytes(bytes(outmeta))
 (build/'checkpoint-plan-result.json').write_text(json.dumps({'evidence':'OFFLINE ONLY; C planner on physical run57 base plus simulated scratch updates; output must never be flashed','pass':True,'independent_lookup_checks':20004,'source_unchanged':True},indent=2)+'\n')
 subprocess.run(['arm-none-eabi-gcc','-std=c99','-O2','-Wall','-Wextra','-Werror','-mcpu=cortex-a5','-mthumb','-ffreestanding','-c',str(HERE/'checkpoint_plan.c'),'-o',str(build/'checkpoint-plan-arm.o')],check=True)
 print('PASS C checkpoint RAM planner: physical template validation, complete replay materialization, per-block counts, closed flags,20004independent current-map lookups; alias/space/corrupt guards leave source unchanged; ARM build')
if __name__=='__main__':main()
