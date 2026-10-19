#ifndef NANOOS_INPUT_H
#define NANOOS_INPUT_H
#include <stdint.h>
#define BUTTON_HOME 1u
#define BUTTON_LEFT 2u
#define BUTTON_RIGHT 4u
/* set with BUTTON_HOME when the Home edge is the Vol-+Vol+ chord */
#define BUTTON_CHORD 8u
/* PMU reads for the on-screen diagnostic line */
/* ok/err = Home polls with a good / no good reg7 read; stage of the
 * last failure: 1 write address, 2 write data, 3 read address, 4 read
 * data, 5/6 write/read STOP never done (round 10); con/stat/ev = I2C +0x00/+0x04/+0x20 then; retry = register reads
 * started over; timeouts by site: byte done, bus free, STOP done */
/* (round 7) to_stop = STOPs never done (controller re-initialised),
 * to_rdy = STARTs with I2C1 +0x10 still non-zero after 200 us, recs =
 * controller unsticks (input_i2c_recover) */
struct input_debug { unsigned r7, idle, ok, err, stage, con, stat, ev, retry,
                     to_byte, to_free, to_stop, to_rdy, recs; };
/* the boot run's I2C1 evidence (layout: input.c input_i2c_recover) */
#define INPUT_I2C_LOG 29u
extern uint32_t input_i2c_log[INPUT_I2C_LOG];
extern struct input_debug input_debug;
extern unsigned input_i2c_n; /* PMU register reads issued (round 15) */
void input_init(void);
/* I2C1 controller unstick (NACK a pending read byte, STOP, init); input_init
 * runs it before the first PMU transfer, Home polling again after repeated
 * failures (at most 1/s), buttons_init at Drivers entry (before bt.c) */
uint32_t input_i2c_recover(void);
/* round 10: VIC IRQ 22 (I2C1) enable, CPU IRQs stay masked; lg: 7 words or 0 */
void input_i2c_vic(uint32_t *lg);
/* one PMU register read with the usual retries; -1 if the bus failed */
int input_pmu_read(unsigned reg);
/* one PMU register write (no retry); 0 ok, -1 if the bus failed (bt.c) */
int input_pmu_write(unsigned reg, unsigned val);
/* debounced Home level (1 = pressed) */
unsigned input_home_level(void);
unsigned input_poll(void);
/* buttons held at the last sample (levels, not edges) */
unsigned input_held(void);
/* sample during long work; the events are returned by the next input_poll() */
void input_service(void);
#endif
