#include "usb.h"
#include "input.h"
/* USB 2.0 OTG = Synopsys DWC2 at 0x38400000, PHY 0x3c400000
 * (docs/HARDWARE.md; bootrom DFU path, docs/research/bootrom.md). Probe
 * only, read-only: the core ID and VBUS.
 *
 * Blocker: NanoEmu has no DWC2 model (a register file filled with 0xff,
 * GRSTCTL always 0x80000000), so a device-mode driver (core reset, EP0
 * enumeration, bulk endpoints, then USB mass storage BOT/SCSI or a vendor
 * file-transfer class) can't be tested in the emulator yet. That needs a
 * nano7_usb.c model with GSNPSID, W1C GINTSTS, GHWCFG values and SETUP
 * delivery on EP0 first. */
#define OTG(o) (*(volatile uint32_t *)(0x38400000u + (o)))
struct usb_info usb;
void usb_probe(void) {
    usb.snpsid = OTG(0x40);
    usb.hwcfg2 = OTG(0x48);
    usb.core = (usb.snpsid >> 16) == 0x4f54u;
    int r5 = input_pmu_read(5);
    usb.vbus = r5 < 0 ? -1 : (r5 >> 6) & 1;
}
