#ifndef NANOOS_CLK_H
#define NANOOS_CLK_H
#include <stdint.h>
/* CLKCON 0x3c500000: clock profiles and the rates read back from the live
 * registers (kHz). Cortex-A5 has no hardware divide and we link without
 * libgcc: udiv() is the shared software one. */
uint32_t udiv(uint32_t n, uint32_t d);
/* source 0 = 24 MHz oscillator, 1..3 = PLL-A/B/C; an implausible PMS word
 * (NanoEmu's regfile fill) gives the documented stock rate and sets *nominal */
uint32_t clk_src_khz(unsigned src, int *nominal);
uint32_t clk_cpu_div(void);   /* +0x00 divisor x +0x04 CPU divider */
uint32_t clk_cpu_khz(int *nominal);
uint32_t clk_gpu_khz(int *nominal);
/* Profile 0 STOCK / 1 SAFE / 2 MAX. STOCK writes no CLKCON register;
 * SAFE/MAX: CPU (PLL-A retune, see clk.c), then the GPU rate field
 * +0x1c[29:16] = gpu. Logs before/after to report R86. Returns
 * clk_result: 0 stock, 1 CPU raised, < 0 CPU left or rolled back (clk.c). */
int clk_apply(unsigned profile, uint32_t gpu);
extern int clk_result;
#endif
