#include "audio.h"
#include "spi.h"
#include "timer.h"
/* Codec on SPI0 (NanoEmu nano7_codec.c:12-101): 5-byte frames
 *   write  6C msb lsb 00 data      read  6D msb lsb 00 xx -> rx[4]
 * The model has no ID register; reg 0x002f always reads with bit7 set.
 * The probe only reads: writing unknown registers of the real codec is
 * avoided.
 *
 * I2S0 0x3ca00000 (nano7_i2s.c): +0x00 bit0 enable, +0x40 clock divisor
 * (12 MHz / div, 272 = 44.1 kHz; left as RetailOS set it), +0x10 TX FIFO,
 * s16 left then right. The model has no back-pressure and no FIFO status,
 * so the CPU feeds samples paced by USEC at 44.1 kHz, a bounded batch per
 * pass. On the device the FIFO depth is unknown: DMA (engine 1 channel 2,
 * destination 0x3ca00010) is the real path, still to do. */
#define I2S(o) (*(volatile uint32_t *)(0x3ca00000u + (o)))
#define I2S_TX (*(volatile uint16_t *)0x3ca00010u)
#define RATE 44100u
#define HALF_PERIOD 50u        /* 44100 / 440 / 2 */
#define AMPLITUDE 6000
#define BATCH 1024u
struct audio_state audio;
static uint32_t t0, total;
int audio_codec_read(unsigned reg) {
    uint8_t tx[5] = {0x6d, (uint8_t)(reg >> 8), (uint8_t)reg, 0, 0}, rx[5];
    return spi_xfer(SPI_CODEC, tx, rx, 5, 2000) ? rx[4] : -1;
}
void audio_tone(uint32_t ms) {
    I2S(0x00) |= 1u;
    audio.ms = ms;
    audio.frames = 0;
    total = ms * (RATE / 100) / 10;
    t0 = USEC;
    audio.playing = 1;
}
void audio_service(void) {
    if (!audio.playing) return;
    uint32_t us = USEC - t0;
    if (us > audio.ms * 1000u) us = audio.ms * 1000u;
    uint32_t due = us / 100u * (RATE / 100) / 100u;   /* frames by now */
    if (due > total) due = total;
    for (unsigned n = 0; audio.frames < due && n < BATCH; ++n, ++audio.frames) {
        int16_t s = (audio.frames / HALF_PERIOD) & 1 ? -AMPLITUDE : AMPLITUDE;
        I2S_TX = (uint16_t)s;
        I2S_TX = (uint16_t)s;
    }
    if (audio.frames >= total) audio.playing = 0;
}
