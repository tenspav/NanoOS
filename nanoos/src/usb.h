#ifndef NANOOS_USB_H
#define NANOOS_USB_H
#include <stdint.h>
/* snpsid: DWC2 GSNPSID (0x4f54xxxx on a real core); vbus: PMU reg5 bit6
 * (-1 = PMU read failed); core: 1 if a DWC2 core answered */
struct usb_info { uint32_t snpsid, hwcfg2; int vbus, core; };
extern struct usb_info usb;
void usb_probe(void);
#endif
