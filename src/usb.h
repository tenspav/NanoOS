#ifndef NANOOS_USB_H
#define NANOOS_USB_H
#include <stdint.h>
/* snpsid: DWC2 GSNPSID after the clock/PHY bring-up (0x4f54xxxx on a real
 * core), id0: as found before it; vbus: PMU reg5 bit6 (-1 = PMU read
 * failed); core: 1 if a DWC2 core answered; otg: GOTGCTL; waits: bit 0 PHY
 * ready, 1 AHB idle, 2 soft reset done, 3 AHB idle after (1 = ok) */
struct usb_info { uint32_t snpsid, hwcfg2; int vbus, core; uint32_t id0, otg, waits; };
extern struct usb_info usb;
void usb_probe(void);   /* also writes the R83 report */
#endif
