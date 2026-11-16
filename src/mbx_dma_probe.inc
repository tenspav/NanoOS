/* RAM-only capability probe. No GPU/NAND/peripheral payload destination.
 * Native engine enable (+0x30=1): pinned firmware0x080a309c..0x080a31a0.
 * Borrow idle channel0, restore its registers and the original engine state. */
#ifndef MBX_DMA_PROBE
#define MBX_DMA_PROBE 0
#endif
uint32_t mbx_dma_probe_result[36];
#ifndef MBX_DMA_MATRIX
#define MBX_DMA_MATRIX 0
#endif
/* Diagnostic records: cfg,count,source-offset,fixed,errors,usec,ctrl,err. */
uint32_t mbx_dma_matrix_result[196];
#if MBX_DMA_PROBE
#ifndef MBX_DMA_PROBE_BASE
#define MBX_DMA_PROBE_BASE 0x38700000u
#endif
#ifndef MBX_DMA_PROBE_ENABLE
#define MBX_DMA_PROBE_ENABLE 0
#endif
#ifndef MBX_DMA_PROBE_UNGATE
#define MBX_DMA_PROBE_UNGATE 0
#endif
#if MBX_DMA_PROBE_BASE == 0x38700000u
#define PROBE_GATE_MASK 0x1000u
#elif MBX_DMA_PROBE_BASE == 0x38200000u
#define PROBE_GATE_MASK 0x0800u
#else
#error unsupported DMA probe engine
#endif
#define PROBE_GATE (*(volatile uint32_t *)0x3c500048u)
#define PROBE_DMA(o) (*(volatile uint32_t *)(MBX_DMA_PROBE_BASE+(o)))
#if MBX_DMA_MATRIX
#define PROBE_WORDS 4100u
#else
#define PROBE_WORDS 64u
#endif
static uint32_t probe_source[PROBE_WORDS] MM_DMA_BUF __attribute__((aligned(64)));
static uint32_t probe_dest[PROBE_WORDS] MM_DMA_BUF __attribute__((aligned(64)));
static void probe_barrier(void){__asm__ volatile("dsb sy" ::: "memory");}
static int probe_wait(void){
 uint32_t start=USEC;
 for(uint32_t spins=0;spins<20000u*64u;++spins){
  if(!(PROBE_DMA(0x110)&1u))return 0;
  if(USEC-start>=20000u)return -1;
 }
 return -1;
}
#if MBX_DMA_MATRIX
static int probe_matrix(void){
#if defined(MBX_DMA_MATRIX_SET) && MBX_DMA_MATRIX_SET == 1
 static const uint32_t configs[4]={0x86489000u,0x87489000u,0x85480000u,0x8548a000u};
#else
 static const uint32_t configs[4]={0x84489000u,0x8448b000u,0x85489000u,0x8548b000u};
#endif
 uint32_t *m=mbx_dma_matrix_result;m[0]=1;m[1]=0;
 for(unsigned p=0;p<4;++p)for(unsigned test=0;test<6;++test){
  uint32_t n=test<2?32u:test<4?4092u:4091u,off=test>=4, fixed=test&1u;
  uint32_t *r=m+4+(p*6+test)*8;
  r[0]=configs[p]|(fixed?0:0x08000000u);r[1]=n;r[2]=off;r[3]=fixed;
  for(unsigned i=0;i<PROBE_WORDS;++i){probe_source[i]=0x12340000u+i*0x10203u;probe_dest[i]=0xa5a5a5a5u;}
  probe_barrier();PROBE_DMA(0x110)=0;
  PROBE_DMA(0x100)=(uint32_t)(probe_source+off);PROBE_DMA(0x104)=(uint32_t)(probe_dest+2);
  PROBE_DMA(0x108)=0;PROBE_DMA(0x10c)=r[0];PROBE_DMA(0x114)=n;PROBE_DMA(0x08)=1;probe_barrier();
  uint32_t start=USEC;PROBE_DMA(0x110)=0x8001u;probe_barrier();int rc=probe_wait();
  r[5]=USEC-start;r[6]=PROBE_DMA(0x110);r[7]=PROBE_DMA(0x0c);
  if(rc){r[4]=0xffffffffu;PROBE_DMA(0x110)=0;probe_barrier();if(probe_wait())return -1;}
  else{
   probe_barrier();for(unsigned i=0;i<PROBE_WORDS;++i){uint32_t want=0xa5a5a5a5u;
    if(fixed){if(i==2)want=probe_source[off+n-1];}else if(i>=2&&i<2+n)want=probe_source[off+i-2];
    r[4]+=probe_dest[i]!=want;
   }
  }
  if(r[4]||r[7])++m[1];
  PROBE_DMA(0x08)=1;PROBE_DMA(0x10)=1;probe_barrier();
 }
 return 0;
}
#endif
static void run_mbx_dma_probe(void){
 uint32_t *r=mbx_dma_probe_result;r[0]=1;uint32_t gate_before=PROBE_GATE;r[34]=gate_before;
 if(MBX_DMA_PROBE_UNGATE){PROBE_GATE=gate_before&~PROBE_GATE_MASK;probe_barrier();if(PROBE_GATE&PROBE_GATE_MASK){r[1]=14;r[35]=PROBE_GATE;return;}}
 uint32_t global=PROBE_DMA(0x30);r[20]=global;
 if(global&~1u){r[1]=12;goto restore_gate;}
 if(MBX_DMA_PROBE_ENABLE){PROBE_DMA(0x30)=global|1u;probe_barrier();}
 r[32]=PROBE_DMA(0x30);
 if(MBX_DMA_PROBE_ENABLE&&!(r[32]&1u)){r[1]=12;goto restore_global;}
 for(unsigned i=0;i<8;++i)if(PROBE_DMA(0x110+0x20*i)&1u){r[1]=2;goto restore_global;}
 uint32_t saved[6];for(unsigned i=0;i<6;++i)r[12+i]=saved[i]=PROBE_DMA(0x100+4*i);
 r[18]=PROBE_DMA(0x04);r[19]=PROBE_DMA(0x0c);
 if((r[18]|r[19])&1u){r[1]=3;goto restore_global;}
 for(unsigned pass=0;pass<2;++pass){
  for(unsigned i=0;i<32;++i)probe_source[i]=0x12340000u+i*0x10203u;
  for(unsigned i=0;i<64;++i)probe_dest[i]=0xa5a5a5a5u;
  probe_barrier();PROBE_DMA(0x110)=0;
  PROBE_DMA(0x100)=(uint32_t)probe_source;PROBE_DMA(0x104)=(uint32_t)(probe_dest+2);
  PROBE_DMA(0x108)=0;PROBE_DMA(0x10c)=pass?0x84489000u:0x8c489000u;PROBE_DMA(0x114)=32;
  PROBE_DMA(0x08)=1;probe_barrier();
  for(unsigned i=0;i<6;++i)r[24+i]=PROBE_DMA(0x100+4*i);
  r[30]=*(volatile uint32_t*)0x3c500048u;r[31]=*(volatile uint32_t*)0x3c50004cu;
  uint32_t start=USEC;PROBE_DMA(0x110)=0x8001u;probe_barrier();
  int rc=probe_wait();r[4+pass]=USEC-start;r[6+pass]=PROBE_DMA(0x110);r[8+pass]=PROBE_DMA(0x04);r[10+pass]=PROBE_DMA(0x0c);
  if(rc){r[1]=4+pass;PROBE_DMA(0x110)=0;probe_barrier();if(probe_wait()){r[1]=8;return;}break;}
  probe_barrier();uint32_t errors=0;
  for(unsigned i=0;i<64;++i){uint32_t expected=0xa5a5a5a5u;if(pass){if(i==2)expected=probe_source[31];}else if(i>=2&&i<34)expected=probe_source[i-2];errors+=probe_dest[i]!=expected;}
  r[2+pass]=errors;PROBE_DMA(0x08)=1;probe_barrier();
  if(errors||r[10+pass]){r[1]=10+pass;break;}
 }
#if MBX_DMA_MATRIX
 if(!r[1]&&probe_matrix()){r[1]=8;return;}
#endif
 PROBE_DMA(0x110)=0;probe_barrier();if(probe_wait()){r[1]=8;return;}
 for(unsigned i=0;i<6;++i)PROBE_DMA(0x100+4*i)=saved[i];
 PROBE_DMA(0x08)=1;probe_barrier();r[21]=PROBE_DMA(0x110);r[22]=PROBE_DMA(0x04);r[23]=PROBE_DMA(0x0c);
 if(!r[1])r[1]=1;
restore_global:
 if(MBX_DMA_PROBE_ENABLE){PROBE_DMA(0x30)=global;probe_barrier();}
 r[33]=PROBE_DMA(0x30);
restore_gate:
 if(MBX_DMA_PROBE_UNGATE){PROBE_GATE=(PROBE_GATE&~PROBE_GATE_MASK)|(gate_before&PROBE_GATE_MASK);probe_barrier();}
 r[35]=PROBE_GATE;
 mbx_dma_matrix_result[2]=gate_before;mbx_dma_matrix_result[3]=PROBE_GATE;
}
#undef PROBE_DMA
#endif
