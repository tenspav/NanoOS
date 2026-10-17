#ifndef NANOOS_INPUT_H
#define NANOOS_INPUT_H
#define BUTTON_HOME 1u
#define BUTTON_LEFT 2u
#define BUTTON_RIGHT 4u
/* PMU reads for the on-screen diagnostic line */
/* ok/err = Home polls with a good / no good reg7 read; stage of the
 * last failure: 1 write address, 2 write data, 3 read address, 4 read
 * data; con/stat/ev = I2C +0x00/+0x04/+0x20 then; retry = register reads
 * started over; timeouts by site: byte done, bus free, STOP done */
struct input_debug { unsigned r7, idle, ok, err, stage, con, stat, ev, retry,
                     to_byte, to_free, to_stop; };
extern struct input_debug input_debug;
extern unsigned input_i2c_n; /* PMU register reads issued (round 15) */
void input_init(void);
/* one PMU register read with the usual retries; -1 if the bus failed */
int input_pmu_read(unsigned reg);
/* debounced Home level (1 = pressed) */
unsigned input_home_level(void);
unsigned input_poll(void);
/* sample during long work; the events are returned by the next input_poll() */
void input_service(void);
#endif
