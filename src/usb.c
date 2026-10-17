#pragma GCC optimize("Os")   /* probe + report, no hot path: kernel.bin must fit 64 KB */
#include "usb.h"
#include "input.h"
#include "timer.h"
#include "fmt.h"
#include "report.h"
/* USB 2.0 OTG = Synopsys DWC2 at 0x38400000, PHY 0x3c400000 (docs/HARDWARE.md,
 * docs/research/bootrom.md). The old probe read GSNPSID with the clocks
 * gated (device: 0x80000000). Bring-up as the bootrom does it (0x20003c94,
 * PHY 0x20009fc8, core 0x2000a864), the gates confirmed in RetailOS
 * (0x08168dd0 -> 0x0841ab5c: gate ids 42 and 11, table 0x08917860):
 * CLKCON +0x4c bit 3 and +0x48 bit 2, a set bit = clock off. No power
 * domain (neither touches 0x39700000 for USB). The PHY trim from the fuse
 * word 0x3d100014 (PHY +0x40/+0x44/+0x100) is skipped: signal quality only.
 *
 * Device mode, polled, NO enumeration: soft-disconnected (DCTL SftDiscon)
 * so the host sees a plain charger, not a device that never answers. VBUS
 * and charging stay the PMU's. Probe results go to R83.
 *
 * NanoEmu has no DWC2 model (0xff-filled register file), so the emulator
 * shows "no core". A device-mode driver needs a nano7_usb.c model first. */
#define OTG(o) (*(volatile uint32_t *)(0x38400000u + (o)))
#define PHY(o) (*(volatile uint32_t *)(0x3c400000u + (o)))
#define CLK(o) (*(volatile uint32_t *)(0x3c500000u + (o)))
#define RPT_USB_ID 83u
/* The R83 report costs ~0.55 KB of kernel.bin, which is over 64 KB
 * (2026-10-17): off by default (core ID probe only), -DUSB_R83=1 or edit
 * this to 1 once there is room */
#ifndef USB_R83
#define USB_R83 0
#endif
struct usb_info usb;
/* R83 register dump; the first 4 are also logged as found (before) */
static const uint32_t dump[] = {
    0x38400040u, 0x3c500048u, 0x3c50004cu, 0x38400e00u, /* GSNPSID gates PCGCCTL */
    0x3c400028u, 0x38400044u, 0x38400048u, 0x3840004cu, /* PHY stat GHWCFG1-4 */
    0x38400050u, 0x38400000u, 0x38400004u, 0x38400008u, /* GOTGCTL GOTGINT */
    0x3840000cu, 0x38400010u, 0x38400014u, 0x38400800u, /* GAHBCFG GUSBCFG */
    0x38400804u, 0x38400808u,                           /* GRSTCTL GINTSTS DCFG DCTL DSTS */
};
#define NDUMP (sizeof dump / sizeof dump[0])
static int ok(int r) { return r == WAIT_OK; }
static void bringup(void) {
    CLK(0x4c) &= ~0x8u;         /* gate 42 */
    CLK(0x48) &= ~0x4u;         /* gate 11 */
    __asm__ volatile("dsb sy" ::: "memory");
    OTG(0xe00) = 0;             /* PCGCCTL: PHY clock not stopped */
    delay_us(10);
    PHY(0x00) = 0;              /* OPHYPWR: all on */
    delay_us(10);
    PHY(0x08) = 1;              /* ORSTCON: PHY reset pulse */
    delay_us(10);
    PHY(0x08) = 0;
    delay_us(10);
    PHY(0x1c) = 6;
    PHY(0x04) = 1;              /* OPHYCLK */
    PHY(0x34) = 4;
    delay_us(1000);
    /* PHY ready, AHB idle, soft reset done, AHB idle again; the bootrom's
     * polls are unbounded, these are not */
    uint32_t w = ok(wait_until(&PHY(0x28), 0x20u, 0x20u, 10000u));
    w |= ok(wait_until(&OTG(0x10), 1u << 31, 1u << 31, 10000u)) << 1;
    OTG(0x10) = 1;              /* GRSTCTL CSftRst */
    w |= ok(wait_until(&OTG(0x10), 1u, 0, 10000u)) << 2;
    w |= ok(wait_until(&OTG(0x10), 1u << 31, 1u << 31, 10000u)) << 3;
    usb.waits = w;
}
void usb_probe(void) {
    uint32_t pre[4];
    for (unsigned i = 0; i < 4; ++i) pre[i] = *(volatile uint32_t *)dump[i];
    usb.id0 = pre[0];
    bringup();
    usb.snpsid = OTG(0x40);
    usb.hwcfg2 = OTG(0x48);
    usb.core = (usb.snpsid >> 16) == 0x4f54u;
    if (usb.core) {             /* GINTSTS CurMod 0 = device (ID pin high) */
        OTG(0x08) = 0;          /* GAHBCFG: no DMA, no interrupt */
        OTG(0x18) = 0;          /* GINTMSK */
        OTG(0x0c) = 0x1408;     /* GUSBCFG as RetailOS: 16-bit UTMI, TRDT 5 */
        OTG(0x804) = 2;         /* DCTL SftDiscon */
        OTG(0x14) = ~0u;        /* GINTSTS W1C */
    }
    usb.otg = OTG(0x00);        /* GOTGCTL: bit 19 BSesVld, 16 ConIDSts */
    int r5 = input_pmu_read(5);
    usb.vbus = r5 < 0 ? -1 : (r5 >> 6) & 1;
#if USB_R83
    /* R83 (report.h mailbox, exported as R83.txt): a free DevMode slot or
     * the last R83, searched from slot 3 down */
    volatile struct rpt_hdr *h = RPT_HDR;
    unsigned s = 4;
    if (!rpt_valid()) {
        for (uint32_t i = 0; i < sizeof *h / 4; ++i) ((volatile uint32_t *)h)[i] = 0;
        h->slot_size = RPT_SLOT_SIZE;
    }
    while (--s && h->id[s] && h->id[s] != RPT_USB_ID) ;
    char *p0 = RPT_SLOT(s);
    fmt_out = p0;
    fmt_end = p0 + 4096;
    fmt_str("== USB R83 ==\nw "); fmt_hex(usb.waits, 1);
    fmt_str(" core "); fmt_dec((uint32_t)usb.core);
    fmt_str(" vbus "); fmt_sdec(usb.vbus);
    for (unsigned i = 0; i < 4 + NDUMP; ++i) {
        if (!(i & 3)) fmt_line(0, 0, 0);
        fmt_str(" ");
        fmt_hex(i < 4 ? pre[i] : *(volatile uint32_t *)dump[i - 4], 8);
    }
    fmt_line(0, 0, 0);
    fmt_str("== END R83 =="); fmt_line(0, 0, 0);
    uint32_t len = (uint32_t)(fmt_out - p0);
    fmt_out = 0;
    h->id[s] = RPT_USB_ID;
    h->len[s] = len;
    h->sum[s] = rpt_sum(p0, len);
    h->magic[0] = RPT_MAGIC0;
    h->magic[1] = RPT_MAGIC1;
    h->hdr_sum = rpt_hdr_sum();
#endif
}
