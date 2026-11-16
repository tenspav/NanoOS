#!/usr/bin/env python3
"""Independent portable mapping oracle; no device access."""
from pathlib import Path
import ctypes as C
import random
import struct
import subprocess
HERE=Path(__file__).resolve().parent

class Run(C.Structure):
    _fields_=[(x,C.c_uint32) for x in ('lba','vba','count')]
class Update(C.Structure):
    _fields_=[(x,C.c_uint32) for x in ('lba','vba','generation','key')]
class Address(C.Structure):
    _fields_=[(x,C.c_uint32) for x in ('ce','cau','block','page','sector')]
class Map(C.Structure):
    _fields_=[(x,C.c_uint32) for x in ('blocks','sectors','nruns','nupdates','run_capacity','update_capacity','checkpoint_generation','checkpoint_key','ready')]+[
        ('error',C.c_int),('bank_mask',C.POINTER(C.c_uint8)),('runs',C.POINTER(Run)),('updates',C.POINTER(Update)),('read_only',C.c_uint32)]

def main():
    build=HERE/'build';build.mkdir(exist_ok=True)
    common=['cc','-std=c99','-O1','-g','-Wall','-Wextra','-Werror']
    exe=build/'test_sftl'
    subprocess.run(common+['-fsanitize=address,undefined',str(HERE/'test_sftl.c'),str(HERE/'sftl.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    disk=build/'test_disk'
    subprocess.run(common+['-fsanitize=address,undefined',str(HERE/'test_disk.c'),str(HERE/'disk.c'),str(HERE/'sftl.c'),'-o',str(disk)],check=True)
    subprocess.run([str(disk)],check=True)
    for name,sources in (('mount',['mount.c','disk.c','sftl.c']),('partition',['partition.c']),('write',['write.c','sftl.c']),('fmc_program',['fmc_program.c']),('fmc_erase',['fmc_erase.c']),('reclaim',['reclaim.c','write.c','sftl.c']),('append',['append.c','write.c','sftl.c']),('collect',['collect.c','append.c','write.c','sftl.c','disk.c']),('btoc',['btoc.c','write.c','sftl.c']),('close',['btoc.c','append.c','write.c','sftl.c']),('checkpoint_plan',['checkpoint_plan.c','sftl.c'])):
        test=build/f'test_{name}'
        subprocess.run(common+['-fsanitize=address,undefined',str(HERE/f'test_{name}.c')]+[str(HERE/s) for s in sources]+['-o',str(test)],check=True)
        subprocess.run([str(test)],check=True)
    subprocess.run(['python3',str(HERE/'test_hfs.py')],check=True)
    shared=build/'sftl.dylib'
    subprocess.run(common+['-shared','-fPIC',str(HERE/'sftl.c'),'-o',str(shared)],check=True)
    lib=C.CDLL(str(shared));m=Map();runs=(Run*1024)();updates=(Update*1024)();masks=(C.c_uint8*4)(15,7,3,15)
    def init(sectors=100):
        assert not lib.ns_sftl_init(C.byref(m),4,sectors,masks,runs,1024,updates,1024,9,100)
    def node(first,pairs):
        raw=struct.pack('<2I',first,0xfffffff0)+b''.join(struct.pack('<2I',*p) for p in pairs)
        assert len(raw)<=4096
        return C.create_string_buffer(raw+b'\xff'*(4096-len(raw)))
    def lookup(lba):
        value=C.c_uint32(0xdeadbeef);rc=lib.ns_sftl_lookup(C.byref(m),lba,C.byref(value));return rc,value.value
    init()
    # Independent bank-stripe enumerator, including the four reserved tail slots.
    checks=0
    for mask in range(1,16):
        masks[0]=mask;live=[b for b in range(4) if mask>>b&1]
        stripe=[(b//2,b%2,0,p,s) for p in range(128) for b in live for s in range(4)]
        for v,expected in enumerate(stripe[:-4]):
            out=Address();assert not lib.ns_vfl_address(C.byref(m),v,C.byref(out))
            assert tuple(getattr(out,k) for k,_ in out._fields_)==expected
            checks+=1
        for v in range(len(stripe)-4,2048):
            assert lib.ns_vfl_address(C.byref(m),v,C.byref(Address()))==-2
    masks[0]=15
    rng=random.Random(0x4e414e44)
    for case in range(100):
        init();base=[0x7fffff if i%7==0 else 100+i for i in range(100)]
        # Nodes arrive out of order; every logical position is explicit.
        chunks=[(first,[(v,1) for v in base[first:first+20]]) for first in range(0,100,20)]
        rng.shuffle(chunks)
        for first,pairs in chunks:assert not lib.ns_sftl_node(C.byref(m),node(first,pairs))
        records=[];expected={}
        for i in range(300):
            lba=rng.randrange(100);vba=2048+rng.randrange(1000);key=101+i
            records.append((vba,(key<<16|1,9,lba,8)))
            expected[lba]=vba
        rng.shuffle(records)
        for vba,words in records:
            assert not lib.ns_sftl_replay(C.byref(m),vba,(C.c_uint32*4)(*words))
        assert not lib.ns_sftl_finish(C.byref(m))
        for lba in range(100):
            want=expected.get(lba,base[lba]);rc,got=lookup(lba)
            assert (rc==-6) if want==0x7fffff else (rc==0 and got==want),(lba,rc,got,want)
            checks+=1
        # A separate coalesced snapshot must preserve the source map and agree
        # with the independent per-LBA oracle, before replay compaction.
        snapshot=(Run*1024)();work=(Update*1024)();used=C.c_uint32()
        original_runs=bytes(runs);original_updates=bytes(updates)
        assert not lib.ns_sftl_snapshot(C.byref(m),snapshot,1024,work,1024,C.byref(used))
        expanded={}
        for run in snapshot[:used.value]:
            for j in range(run.count):expanded[run.lba+j]=0x7fffff if run.vba==0x7fffff else run.vba+j
        assert expanded=={lba:expected.get(lba,base[lba]) for lba in range(100)}
        assert bytes(runs)==original_runs and bytes(updates)==original_updates and m.ready
        last=(updates[m.nupdates-1].generation,updates[m.nupdates-1].key)
        assert not lib.ns_sftl_compact(C.byref(m)) and m.nupdates==len(expected)
        assert (updates[m.nupdates-1].generation,updates[m.nupdates-1].key)==last
        for lba in range(100):
            want=expected.get(lba,base[lba]);rc,got=lookup(lba)
            assert (rc==-6) if want==0x7fffff else (rc==0 and got==want)
            checks+=1
    init();assert not lib.ns_sftl_node(C.byref(m),node(0,[(0,100)]))
    assert not lib.ns_sftl_replay(C.byref(m),2048,(C.c_uint32*4)(101<<16|1,9,1,9))
    assert not lib.ns_sftl_replay(C.byref(m),2049,(C.c_uint32*4)(101<<16|1,9,2,9))
    assert lib.ns_sftl_finish(C.byref(m))==-4 and lookup(1)[0]==-5
    for pairs in ([(2043,2)],[(2048+1531,2)],[(0,0)],[(0x7ffffe,1)]):
        init();assert lib.ns_sftl_node(C.byref(m),node(0,pairs))==-2
        assert lib.ns_sftl_finish(C.byref(m))==-2
    init();assert not lib.ns_sftl_node(C.byref(m),node(1,[(0,100)]))
    assert lib.ns_sftl_finish(C.byref(m))==-2
    subprocess.run(['arm-none-eabi-gcc','-std=c99','-O2','-Wall','-Wextra','-Werror','-mcpu=cortex-a5','-mthumb','-mno-unaligned-access','-ffreestanding','-c',str(HERE/'sftl.c'),'-o',str(build/'sftl-arm.o')],check=True)
    subprocess.run(['arm-none-eabi-gcc','-std=c99','-O2','-Wall','-Wextra','-Werror','-mcpu=cortex-a5','-mthumb','-mno-unaligned-access','-ffreestanding','-c',str(HERE/'disk.c'),'-o',str(build/'disk-arm.o')],check=True)
    for name in ('mount','partition','write','fmc_program','fmc_erase','reclaim','append','collect','btoc','checkpoint_plan'):
        subprocess.run(['arm-none-eabi-gcc','-std=c99','-O2','-Wall','-Wextra','-Werror','-mcpu=cortex-a5','-mthumb','-mno-unaligned-access','-ffreestanding','-c',str(HERE/f'{name}.c'),'-o',str(build/f'{name}-arm.o')],check=True)
    print(f'PASS storage mapping: {checks} independent stripe/replay lookups; corruption/conflict/gap/BTOC checks; sanitizer and ARM builds')
if __name__=='__main__':main()
