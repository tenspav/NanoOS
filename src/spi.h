#ifndef NANOOS_SPI_H
#define NANOOS_SPI_H
#include <stdint.h>
/* Serial ports in SPI mode: SPI0 0x3c300000 = audio codec, port 2
 * 0x3d200000 = multitouch controller (NanoEmu nano7_codec.c /
 * nano7_touch.c; the codec model still only has the START path). */
#define SPI_CODEC 0x3c300000u
#define SPI_TOUCH 0x3d200000u
/* One full-duplex transfer of n bytes on RetailOS's polled path, byte by
 * byte: send tx[i], read rx[i]. us = timeout of each wait (per byte).
 * 1 when done, 0 on timeout (counted in spi_timeouts). */
int spi_xfer(uint32_t base, const uint8_t *tx, uint8_t *rx, unsigned n,
             uint32_t us);
extern uint32_t spi_timeouts;
#endif
