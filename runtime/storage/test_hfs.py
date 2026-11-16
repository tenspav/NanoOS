#!/usr/bin/env python3
from pathlib import Path
import struct,subprocess,hashlib
HERE=Path(__file__).resolve().parent
SIZE=50208
be16=lambda n:struct.pack('>H',n)
be32=lambda n:struct.pack('>I',n)
def fork(size,extents):
 return struct.pack('>QII',size,0,sum(n for start,n in extents))+b''.join(struct.pack('>II',*e) for e in extents).ljust(64,b'\0')
def header():
 node=bytearray(4096);node[8]=1;node[10:12]=be16(3)
 node[14:38]=struct.pack('>HIIIIHHI',1,1,4,1,1,4096,516,2)
 for k,off in enumerate((14,120,248,4088)):node[4094-k*2:4096-k*2]=be16(off)
 return node
def leaf(records):
 b=bytearray(4096);b[8]=255;b[9]=1;b[10:12]=be16(len(records));off=14
 for k,record in enumerate(records):
  b[4094-k*2:4096-k*2]=be16(off);b[off:off+len(record)]=record;off+=len(record)
 b[4094-len(records)*2:4096-len(records)*2]=be16(off)
 return b
def entry(parent,name,ident,extents=None):
 text=name.encode('utf-16-be');key=be16(6+len(text))+be32(parent)+be16(len(text)//2)+text
 record=bytearray(88 if extents is None else 248);record[:2]=be16(1 if extents is None else 2);record[8:12]=be32(ident)
 if extents is not None:record[88:168]=fork(SIZE,extents)+b''
 return key+record
def image():
 b=bytearray(262144);v=bytearray(512);v[:4]=be16(0x482b)+be16(4);v[40:48]=be32(512)+be32(512)
 v[192:272]=fork(8192,[(24,16)]);v[272:352]=fork(8192,[(8,16)]);b[1024:1536]=v
 b[4096:8192]=header();payload=bytes((i*13+i//4096*17+11)&255 for i in range(SIZE))
 # The initial catalog fork describes all99allocated blocks, including91
 # from an overflow record; its inline eight extents occupy only8blocks.
 file_extents=[(40+i*2,1) for i in range(8)]
 records=[entry(1,'Test',2),entry(2,'Apps',16),entry(16,'Icons',17),entry(17,'NanoOS.bin',18,file_extents)]
 records[-1]=bytearray(records[-1]);keylen=int.from_bytes(records[-1][:2],'big')+2
 records[-1][keylen+88+12:keylen+88+16]=be32(99)
 b[8192:12288]=leaf(records);b[12288:16384]=header()
 key=be16(10)+b'\0\0'+be32(18)+be32(8)
 b[16384:20480]=leaf([key+struct.pack('>II',64,91)+bytes(56)])
 for i in range(99):
  block=40+i*2 if i<8 else 64+i-8
  chunk=payload[i*512:(i+1)*512];b[block*512:block*512+len(chunk)]=chunk
 return b,payload
def main():
 build=HERE/'build';build.mkdir(exist_ok=True);exe=build/'test_hfs'
 subprocess.run(['cc','-std=c99','-D_DARWIN_C_SOURCE','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(HERE/'test_hfs.c'),str(HERE/'hfs.c'),'-o',str(exe)],check=True)
 b,payload=image();p=build/'hfs-test.img';p.write_bytes(b)
 output=subprocess.check_output([str(exe),str(p)],text=True).strip();assert output==hashlib.sha256(payload).hexdigest(),output
 cases=[]
 def case(offset,data):
  bad=bytearray(b);bad[offset:offset+len(data)]=data;cases.append(bad)
 case(1024,b'NO');case(1064,be32(513));case(1024+272+16,be32(510));case(8192+8,b'\0');case(8192+4094,be16(4095));case(8192+14,be16(600));case(16384+14+4,be32(19));case(16384+14+12,be32(510))
 for i,bad in enumerate(cases):
  p=build/f'hfs-bad-{i}.img';p.write_bytes(bad);subprocess.run([str(exe),str(p),'reject'],check=True,stdout=subprocess.DEVNULL)
 subprocess.run(['arm-none-eabi-gcc','-std=c99','-O2','-Wall','-Wextra','-Werror','-mcpu=cortex-a5','-mthumb','-mno-unaligned-access','-ffreestanding','-c',str(HERE/'hfs.c'),'-o',str(build/'hfs-arm.o')],check=True)
 print('PASS HFS+: directory path lookup, ASCII case folding, fragmented overflow fork, whole50208byte SHA256, EOF/I/O and8malformed metadata cases; sanitizers/ARM')
if __name__=='__main__':main()
