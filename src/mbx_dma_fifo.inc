/* Experimental DMA feeding of the already-owned MBX FIFO. Disabled by default.
 * Live-pinned firmware maps GPU request30 to DMA engine2 request1 and clockID6.
 * Start with capacity-reserved32word transfers, preserving every packet bell.
 * Initial implementation waits synchronously; it does not claim a free CPU. */
#ifndef MBX_FIFO_DMA
#define MBX_FIFO_DMA 0
#endif
#ifndef MBX_FIFO_DMA_PORT
#define MBX_FIFO_DMA_PORT 0x3b800000u
#endif
#ifndef MBX_FIFO_DMA_CFG
#define MBX_FIFO_DMA_CFG 0x84489000u
#endif
#ifndef MBX_FIFO_DMA_TUNE
#define MBX_FIFO_DMA_TUNE 0
#endif
#ifndef MBX_FIFO_DMA_BUS_TUNE
#define MBX_FIFO_DMA_BUS_TUNE 0
#endif
#ifndef MBX_FIFO_DMA_POLL_NOPS
#define MBX_FIFO_DMA_POLL_NOPS 0u
#endif
uint32_t mbx_fifo_dma_stats[42];
#if MBX_FIFO_DMA
#define FDMA(o) (*(volatile uint32_t *)(0x38700000u+(o)))
#define FDMA_GATE (*(volatile uint32_t *)0x3c500048u)
static uint32_t fdma_owned,fdma_ready,fdma_saved[6],fdma_global,fdma_gate;
static uint32_t fdma_frame,fdma_profile;
static void fdma_begin(void){
 static const uint32_t delays[4]={0,16,64,256};
 static const uint32_t configs[4]={0x84489000u,0x85489000u,0x86489000u,0x87489000u};
 fdma_profile=(MBX_FIFO_DMA_TUNE||MBX_FIFO_DMA_BUS_TUNE)?(fdma_frame/16u)%4u:0u;
 mbx_fifo_dma_stats[20]=MBX_FIFO_DMA_TUNE?delays[fdma_profile]:MBX_FIFO_DMA_POLL_NOPS;
 if(MBX_FIFO_DMA_BUS_TUNE&&fdma_owned){FDMA(0x10c)=configs[fdma_profile];mbx_fifo_dma_stats[16]=configs[fdma_profile];}
 mbx_fifo_dma_stats[21]=++fdma_frame;
 fdma_ready=fdma_owned;
}
static void fdma_barrier(void){__asm__ volatile("dsb sy" ::: "memory");}
static uint32_t fdma_active(void){uint32_t bits=0;for(unsigned i=0;i<8;++i)bits|=(FDMA(0x110+32*i)&1u)<<i;return bits;}
static int fdma_wait_idle(uint32_t us){uint32_t start=USEC;for(uint32_t spins=0;spins<us*64u;++spins){if(!(FDMA(0x110)&1u))return 0;if(USEC-start>=us)break;}return -1;}
static void fdma_restore_gate(void){FDMA_GATE=(FDMA_GATE&~0x1000u)|(fdma_gate&0x1000u);fdma_barrier();mbx_fifo_dma_stats[11]=FDMA_GATE;}
static void fdma_close(void){
 fdma_ready=0;if(!fdma_owned)return;
 FDMA(0x110)=0;fdma_barrier();
 if(fdma_wait_idle(20000)){++mbx_fifo_dma_stats[5];FDMA(0x30)=0;fdma_barrier();}
 for(unsigned i=0;i<6;++i){FDMA(0x100+4*i)=fdma_saved[i];}
 FDMA(0x08)=1;fdma_barrier();
 if(!(fdma_active()&~1u)){FDMA(0x30)=fdma_global;fdma_barrier();mbx_fifo_dma_stats[15]=FDMA(0x30);fdma_restore_gate();}
 else ++mbx_fifo_dma_stats[5];
 fdma_owned=0;
}
static void fdma_open(void){
 uint32_t*r=mbx_fifo_dma_stats;r[0]=1;r[12]=MBX_FIFO_DMA_PORT;r[13]=0x8001u;r[16]=MBX_FIFO_DMA_CFG;
 fdma_gate=FDMA_GATE;r[10]=fdma_gate;FDMA_GATE=fdma_gate&~0x1000u;fdma_barrier();
 if(FDMA_GATE&0x1000u){r[1]=2;fdma_restore_gate();return;}
 fdma_global=FDMA(0x30);r[14]=fdma_global;
 if(fdma_global&~1u){r[1]=3;fdma_restore_gate();return;}
 FDMA(0x30)=fdma_global|1u;fdma_barrier();
 if(!(FDMA(0x30)&1u)||fdma_active()||((FDMA(0x04)|FDMA(0x0c))&1u)){r[1]=4;FDMA(0x30)=fdma_global;fdma_barrier();fdma_restore_gate();return;}
 for(unsigned i=0;i<6;++i)fdma_saved[i]=FDMA(0x100+4*i);
 fdma_owned=1;fdma_ready=0;
 FDMA(0x104)=MBX_FIFO_DMA_PORT;FDMA(0x108)=0;FDMA(0x10c)=MBX_FIFO_DMA_CFG;fdma_barrier();r[1]=1;
}
static int fdma_span(const void*source,uint32_t words){
 uint32_t*r=mbx_fifo_dma_stats;uint32_t entered=USEC;uintptr_t a=(uintptr_t)source,end=a+words*4u;
 if(!fdma_owned||!fdma_ready||!words||words>123u||(a&3u)||a<MM_DRAM||end<a||end>MM_DRAM+0x04000000u)return -1;
 /* Clean source cache lines for the DMA master. No source pointer is retained
  * after completion/abort. CPU-only FIFO submission does not need this. */
 for(uintptr_t line=a&~31u;line<end;line+=32){__asm__ volatile("mcr p15,0,%0,c7,c10,1"::"r"(line):"memory");++r[9];}
 fdma_barrier();r[18]+=USEC-entered;
 if(FDMA(0x110)&1u){++r[5];return -1;}
 if((CORE(0x12c)&0x20u)||(FDMA(0x0c)&1u)){++r[5];return -1;}
 FDMA(0x08)=1;FDMA(0x100)=(uint32_t)a;
#if MBX_FIFO_DMA_CFG & 0x08000000u
 /* Experimental incrementing aperture access: never advance outside this
  * capacity-reserved group, and restart at the original port each time. */
 FDMA(0x104)=MBX_FIFO_DMA_PORT;
#endif
 FDMA(0x114)=words;fdma_barrier();
 uint32_t start=USEC,polls=0,pause_count=r[20];r[17]+=start-entered;FDMA(0x110)=0x8001u;fdma_barrier();int rc=0;
 for(uint32_t spins=0;;++spins){
  if(!(FDMA(0x110)&1u))break;
  ++polls;
  /* Do not contend for the peripheral bus with three MMIO polls per word.
   * Deadline/error checks remain bounded even if the timer stops. */
  if(!(spins&15u)&&((FDMA(0x0c)&1u)||USEC-start>=20000u)){rc=-1;break;}
  if(spins>=20000u*64u){rc=-1;break;}
  for(uint32_t pause=0;pause<pause_count;++pause)__asm__ volatile("nop" ::: "memory");
 }
 uint32_t status=CORE(0x12c),occ=status>>16&255u;if(occ>r[6])r[6]=occ;
 if((status&0x20u)||(FDMA(0x0c)&1u))rc=-1;
 uint32_t elapsed=USEC-start;r[4]+=elapsed;r[19]+=polls;r[7]=FDMA(0x110);r[8]=FDMA(0x04);
 uint32_t *profile=r+22+fdma_profile*5u;++profile[0];profile[1]+=words;profile[2]+=elapsed;profile[3]+=start-entered;profile[4]+=polls;
 if(rc){++r[5];FDMA(0x110)=0;fdma_barrier();if(fdma_wait_idle(20000)){FDMA(0x30)=0;fdma_barrier();}return -1;}
 FDMA(0x08)=1;fdma_barrier();++r[2];r[3]+=words;return 0;
}
#else
static void fdma_open(void){mbx_fifo_dma_stats[0]=0;}
static void fdma_close(void){}
#endif
