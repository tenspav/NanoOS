#pragma GCC optimize("Os")   /* no hot path, no divisions: kernel.bin must fit 64 KB */
#include "audio.h"
#include "spi.h"
#include "fmt.h"
#include "asset.h"
#include "report.h"
#include "memmap.h"
/* Codec on SPI port 0 (RetailOS 0x08429fb8 / 0x08429ffe -> 0x083f4ee8
 * port 0, one CS-low transaction; NanoEmu nano7_codec.c):
 *   write  6C msb lsb 00 data      read  6D msb lsb 00 xx -> rx[4]
 * 0x2f is a 16-bit codec register address, not an I2C address. RetailOS
 * reads no ID register (0x4281 is a table key); regs 0..3 read 00 42 A8 10
 * on the device.
 *
 * Bring-up (boot-progress 2026-10-17, rounds 2-4): device R82 v3 shows
 * RetailOS leaves everything the codec path needs on: clock gates (SPI0
 * 0x2b, I2S0 0x26, 0x47), MCLK (CLKCON +0x0c = 0x80000001), the SPI0 pins
 * (functions 4,2,2,2) and the I2S0 pins 7 and 20 (function 3, 0x080ba31c),
 * and SPI0 answers with its set-up as found (spi.c: RetailOS's polled
 * per-byte path; SPI_US is per byte, a read stops at the first timeout).
 * The codec is powered at 44.1 kHz (0x0f = 8A, 0x220 = 28: the mode 3 and
 * rate values); before playing, the mode 4 (play) RMWs of 0x080d053c are
 * replayed (play[]). Answered = reg 0x2f read back and not FF.
 *
 * Playback: I2S0 0x3ca00000 at 44.1 kHz, fed by DMA engine 1 channel 2.
 * Device R82 v3: the beep's channel read 16 bytes (one FIFO fill), then
 * waited forever: its CTRL had dst request 12 (snap6's leftover), which is
 * the I2S port at 0x3d400010. I2S0 TX on engine 1 is request 10 (VERIFIED:
 * 0x080b1cb4 looks it up at 0x890c278 for peripheral index 12 = 0x3ca00010;
 * the device's own leftover CTRL is 0x8a80). NEXT holds the plain
 * descriptor address (0x080b1cb4 strips bit 31, never sets it).
 * Streaming, RetailOS style (the channel reloads a descriptor from memory
 * when a pass ends): two PASS-frame stereo buffers, two descriptors. When
 * pass i starts (channel NEXT = d[i ^ 1]), audio_service() refills the
 * other buffer, points d[i ^ 1] at it with next = d[i], and turns d[i]
 * (already loaded) into a short silent stop pass. So without
 * audio_service() the chain ends within two passes; with it late, the
 * channel stops and is restarted (under). A channel that never gets a
 * request stays on one pass: audio.frames stops at one buffer ahead (2048
 * source frames at 22.05 kHz). Any source rate is resampled to 44.1 kHz by
 * nearest sample (Bresenham, no division).
 * R82 (report.h mailbox): I2S0 and DMA registers and the codec reads.
 * R82 takes a free slot or the last R82, from slot 3 down. */
#define I2S_BASE 0x3ca00000u
#define I2S_FIFO (I2S_BASE + 0x10)
#define I2S(o) (*(volatile uint32_t *)(I2S_BASE + (o)))
#define DMA_BASE 0x38200000u
#define DMA(o) (*(volatile uint32_t *)(DMA_BASE + (o)))
#define CH 0x140u               /* channel 2 = base + 0x100 + 0x20 * 2 */
#define DMA_CFG 0x84249000u     /* RetailOS: 16-bit elements, source increment */
#define DMA_GO 0x8a81u          /* enable | I2S0 TX request 10 << 6 | mem->periph | ITC */
#define RPT_AUDIO_ID 82u
#define PASS 2048u              /* frames per DMA pass: 46 ms */
#define HUSH 64u                /* elements of the silent stop pass */
#define SPI_US 10000u           /* per byte (polled spi_xfer) */
struct lli { uint32_t src, dst, next, cfg, count; };
struct audio_state audio;
/* DMA-read: the uncached kernel .dma (memmap.h) */
static int16_t out[2][PASS * 2] MM_DMA_BUF, hush[HUSH] MM_DMA_BUF;
static volatile struct lli d[2] MM_DMA_BUF;
static const int16_t *pcm;
static uint32_t frames, acc, rate, stereo;
static unsigned cur;            /* descriptor playing */
/* codec registers read: ID area, power (6, 7), rate (f), mute (220),
 * volume (227, 229), 2f */
static const uint16_t creg[AUDIO_NREG] = {
    0x0000, 0x0001, 0x0002, 0x0003, 0x0006, 0x0007, 0x000f, 0x0220,
    0x0227, 0x0229, 0x002f,
};
/* mode 4 (play) of 0x080d053c: reg << 16 | mask << 8 | value */
static const uint32_t play[] = {
    0x00074000u, 0x02197878u, 0x0229ff40u, 0x022aff40u, 0x00060100u,
    0x0201e040u, 0xc81fffffu, 0xc85fff0fu, 0xc96fff0eu, 0x0223ff08u,
    0x0224ff09u, 0x0225ff00u, 0x0226ff00u, 0x0227ff40u, 0x0228ff40u,
    0x0229ff41u, 0x022aff41u, 0x00758080u,
};
/* register dump: 8-aligned address | word count */
static const uint32_t dump[] = {
    I2S_BASE | 4, (I2S_BASE + 0x38) | 3,        /* I2S0 +0x00..0c, +0x38..40 */
    DMA_BASE | 2, (DMA_BASE + CH) | 6,          /* engine status, channel 2 */
};
static int xfer(unsigned reg, unsigned cmd, unsigned v) {
    uint8_t tx[5] = {(uint8_t)cmd, (uint8_t)(reg >> 8), (uint8_t)reg, 0, (uint8_t)v}, rx[5];
    return spi_xfer(SPI_CODEC, tx, rx, 5, SPI_US) ? rx[4] : -1;
}
static void hexes(const volatile uint32_t *w, unsigned n) {
    while (n--) { fmt_str(" "); fmt_hex(*w++, 8); }
    fmt_line(0, 0, 0);
}
static void regs(void) {
    for (unsigned i = 0; i < sizeof dump / 4; ++i)
        hexes((const volatile uint32_t *)(dump[i] & ~7u), dump[i] & 7);
}
/* pass i has started: queue the next one behind it, d[i] becomes the stop */
static void advance(unsigned i) {
    volatile struct lli *k = &d[i ^ 1u], *c = &d[i];
    if (audio.frames < frames) {
        int16_t *o = out[i ^ 1u];
        uint32_t pos = audio.frames, a = acc;
        for (unsigned f = 0; f < PASS; ++f, o += 2) {   /* 44.1 kHz stereo */
            int16_t l = 0, r = 0;
            if (pos < frames) {
                const int16_t *s = pcm + (pos << stereo);
                l = s[0]; r = s[stereo];
                for (a += rate; a >= AUDIO_RATE; a -= AUDIO_RATE) ++pos;
            }
            o[0] = l; o[1] = r;
        }
        audio.frames = pos; acc = a;
        __asm__ volatile("dsb sy" ::: "memory");
        k->src = (uint32_t)out[i ^ 1u];   /* this order keeps every mix a valid pass */
        k->count = PASS * 2;
        k->next = (uint32_t)c;
    } else {
        k->next = 0; k->count = HUSH; k->src = (uint32_t)hush;
    }
    c->next = 0; c->count = HUSH; c->src = (uint32_t)hush;
    cur = i;
    ++audio.passes;
}
static void start(void) {
    DMA(CH + 0x10) = 0;         /* abort the channel (old stream) */
    I2S(0x08) &= ~6u;           /* TX off while the channel is set up */
    I2S(0x00) = 1u;             /* RetailOS 0x080bdf38, 0x080ba31c, 0x080d077c */
    I2S(0x04) = 0x03100099u;    /* 16-bit TX */
    I2S(0x30) = 0x1000u;
    I2S(0x40) = 0x110u;         /* 12 MHz / 272 = 44.1 kHz */
    I2S(0x3c) = I2S(0x3c);      /* clear the sticky underrun */
    d[0].dst = d[1].dst = I2S_FIFO;
    d[0].cfg = d[1].cfg = DMA_CFG;
    advance(1);                 /* fills out[0] into d[0] */
    advance(0);                 /* out[1] into d[1]; d[0] -> stop */
    DMA(0x08) = 1u << 2;        /* ack a stale ch2 completion */
    DMA(CH + 0x00) = (uint32_t)out[0];   /* = d[0] before advance(0) */
    DMA(CH + 0x04) = I2S_FIFO;
    DMA(CH + 0x08) = (uint32_t)&d[1];
    DMA(CH + 0x0c) = DMA_CFG;
    DMA(CH + 0x14) = PASS * 2;  /* channel: CTRL at +0x10, COUNT +0x14 */
    DMA(CH + 0x10) = DMA_GO;
    I2S(0x08) |= 6u;            /* TX on */
    audio.playing = 1;
}
int audio_play_pcm(const void *p, uint32_t len, uint32_t r) {
    uint32_t st = r >> 31;
    r &= ~AUDIO_STEREO;
    if (!p || ((uint32_t)p & 1) || !r) return 0;
    pcm = p;
    frames = len >> (1 + st);
    stereo = st;
    rate = r;
    audio.frames = acc = 0;
    start();
    return 1;
}
void audio_stop(void) {
    DMA(CH + 0x10) = 0;
    audio.playing = 0;
}
void audio_service(void) {
    if (!audio.playing) return;
    uint32_t nx = DMA(CH + 0x08), on = DMA(CH + 0x10) & 1;
    unsigned i = nx == (uint32_t)&d[1] ? 0 : nx == (uint32_t)&d[0] ? 1 : 2;
    if (on) {
        if (i < 2 && i != cur) advance(i);
    } else if (audio.frames < frames) {
        ++audio.under;
        start();
    } else audio.playing = 0;
}
void audio_init(void) {
    volatile struct rpt_hdr *h = RPT_HDR;
    unsigned s = 4;
    audio_stop();
    while (--s && h->id[s] && h->id[s] != RPT_AUDIO_ID) ;
    char *p0 = fmt_out = RPT_SLOT(s);
    fmt_end = p0 + 4096;
    fmt_str("R82 audio v4"); fmt_line(0, 0, 0);
    regs();
    int v = 0;
    for (unsigned r = 0; r < AUDIO_NREG; ++r)
        audio.id[r] = v = v < 0 ? -1 : xfer(creg[r], 0x6d, 0);
    audio.w = audio.passes = audio.under = 0;
    for (unsigned i = 0; v >= 0 && v != 0xff && i < sizeof play / 4; ++i) {
        unsigned reg = play[i] >> 16, m = play[i] >> 8 & 0xff;
        int o = m == 0xff ? 0 : xfer(reg, 0x6d, 0);
        if (o < 0 || xfer(reg, 0x6c, ((unsigned)o & ~m) | (play[i] & m)) < 0) break;
        ++audio.w;
    }
    /* codec regs (creg order), play writes */
    hexes((const volatile uint32_t *)audio.id, 6);
    hexes((const volatile uint32_t *)audio.id + 6, 6);
    uint32_t n = 0, len = (uint32_t)(fmt_out - p0);
    fmt_out = 0;
    if (h->magic[0] == RPT_MAGIC0) {        /* no mailbox: no R82 */
        h->id[s] = RPT_AUDIO_ID;
        h->len[s] = len;
        h->sum[s] = rpt_sum(p0, len);
        h->hdr_sum = rpt_hdr_sum();
    }
    if (audio.w) audio_play_pcm(asset_find("test.pcm", &n), n, 22050);
}
