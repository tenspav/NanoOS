/* polled, no hot path: kernel.bin must fit 64 KB; the plain Makefile
 * link has no memcpy for GCC to turn copy loops into */
#pragma GCC optimize("Os", "no-tree-loop-distribute-patterns")
#include "bt.h"
#include "timer.h"
#include "fmt.h"
#include "report.h"
/* Bluetooth: controller bring-up and gamepad (HID) host over its HCI (H4) UART.
 *
 * Controller: Broadcom BCM2076B1 (patchram string at 0x0890cbfa). UART 1 at
 * 0x3db00000 (Samsung UART; NanoEmu nano7_btuart.c:2205-2310): +0x00 3,
 * +0x04 0x405, +0x08 7 (FIFOs), +0x0c 1 (RTS), +0x10 bit0 RX data ready
 * (W1C pending 0x170), +0x18 bit9 TX FIFO full, +0x20 TX byte, +0x24 RX
 * byte, +0x28 divisor. H4: 01 = command, 02 = ACL data, 04 = event.
 *
 * Power-up = RetailOS's sphwBluetooth_Init 0x0854f56c (chip type 5), RE'd
 * 2026-10-17. RetailOS's BT-off path (0x084f63a0) leaves PMU reg 0x4b =
 * 0x09, the chip held in reset: that was the device's "ERR at reset FF".
 *   GPIO 70 output high + pull-up (BT_WAKE) first; PMU (I2C1 0x73, via
 *   IRAM 0x220002b2) 0x52 = 0xEA, 0x56 = 0x49; 10 ms; GPIO 70 high again;
 *   50 ms; 0x4b = 0x09 (reset); 50 ms; 0x4b = 0x0B (bit 1 = release)
 *   (round 8, order and delays per Codex RE of 0x0854f57c..0x0854f59a; a
 *   failed PMU ID read or write ends the bring-up as "ERR pmu Ex"); UART1 gate id 16 (CLKCON +0x4c bit 29,
 *   +0x6c bit 8) on; pins 78/79 (TX/RX), 80/81 (RTS/CTS) function 2,
 *   pull-ups on 78/81; UART at 115200, auto flow control; 5 ms; HCI Reset.
 *   (fix3: I2C1 transfer reworked, see i2c_xfer)
 * GPIO function write (0x0842a580): 0x3cf001e0 = bank << 16 | bit << 8 |
 * cmd, cmd 0xF = output high, 2 = UART; every cmd but input (function 0)
 * sets the bank +0x14 bit; pull-up = bank +0x0c bit.
 * Divisor (0x08001c40, 16x oversampling): round(clk / (16 baud)) - 1,
 * clk = "clock 5" (CLKCON +0x10).
 * Patchram: after the first HCI Reset, RetailOS's own BCM2076B1 patch is
 * replayed from its resident copy in DRAM (RetailOS image 0x0890c884..
 * 0x08914203: 151 Write_RAM records + Launch_RAM, the same bytes in every
 * RAM snapshot) when its FNV-1a matches (the GPU window covers it, so it
 * can be overwritten): Download_Minidriver, 50 ms, each record waits for
 * the Command Complete of ITS opcode (others are ignored; a record that
 * would run past PATCH_END fails as "ERR patch FB"), Launch_RAM (0xfc4e),
 * 250 ms, Reset again (BT_PATCH is left there). At 115200 (no
 * fc18 baud change) it takes ~3 s; the main loop keeps running.
 *
 * Gamepad host (2026-10-17): Write_Simple_Pairing_Mode -> Inquiry (first
 * Class of Device major 0x05 = peripheral/gamepad, or page BT_PAD_ADDR) ->
 * Create_Connection -> Authentication_Requested with SSP "Just Works"
 * (IO capability NoInputNoOutput, user confirmation auto-accepted, link key
 * kept in RAM only: a reconnect this session re-authenticates with it,
 * after a reboot the pad must be paired again) -> encryption ->
 * L2CAP HID control PSM 0x11 + interrupt 0x13. A channel the pad opens
 * itself (NanoEmu's NanoPad does after encryption) is accepted; after
 * 0.5 s without one the host opens it. Reports parsed (report ID after the
 * A1 DATA|Input header): 0x01 DualSense/DS4 simple (lx ly rx ry b0 b1 b2
 * l2 r2), 0x11 DS4 full (same, 2 bytes later), 0x31 DualSense full
 * (lx ly rx ry l2 r2 counter b0 b1 b2), 0x3f Switch Pro / 8BitDo simple
 * (b0 b1 hat lx16 ly16..), and NanoEmu's NanoPad (7 bytes, no report ID).
 * A failure past bring-up disconnects and searches again (bt.retries).
 * Every wait is a step deadline: 1.5 s up to SSP, 12 s after. */
#define UART(o) (*(volatile uint32_t *)(0x3db00000u + (o)))
#define GPIO(o) (*(volatile uint32_t *)(0x3cf00000u + (o)))
#define CLKCON(o) (*(volatile uint32_t *)(0x3c500000u + (o)))
#define BAUD 115200u
#define RESET_TRIES 3u
#define RPT_BT_ID 85u   /* 80 M8, 81 NAND, 82 audio, 83 USB, 84 touch */
/* the pad to page (wire order, e.g. DualSense 12:34:56:78:9A:BC ->
 * {0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12}); all zero = inquiry, first gamepad */
#ifndef BT_PAD_ADDR
#define BT_PAD_ADDR {0, 0, 0, 0, 0, 0}
#endif
/* 0: skip the patchram replay (ROM firmware only), e.g. if "ERR patch" */
#ifndef BT_PATCHRAM
#define BT_PATCHRAM 1
#endif
#define PATCH 0x0890c884u
#define PATCH_END 0x08914203u
#define PATCH_SUM 0x543a9171u   /* FNV-1a (report.h rpt_sum) of the blob */
#define STEP_US 1500000u
#define PAIR_US 12000000u       /* inquiry 10.24 s, page 5.12 s, pairing */
#define OPEN_US 500000u         /* wait for the pad's own L2CAP open */
#define CID_CTRL 0x0041u
#define CID_INTR 0x0042u
#define MAX_PKT 300u
#define RX_BUDGET 512u          /* bytes per pass */
struct bt_state bt;
static uint32_t step_t0;
static const uint8_t *patch;    /* next patchram record */
static uint16_t patch_op;       /* opcode in flight while patching */
static const uint8_t cfg_addr[6] = BT_PAD_ADDR;
static uint8_t ident = 1, found, have_key, key[16];
/* Bluetooth Central: 0 automatic (first gamepad / BT_PAD_ADDR), 1 list
 * every device, 2 page target */
static uint8_t central, target[6];
struct bt_dev bt_devs[BT_DEVS];
unsigned bt_ndev;
/* per channel (0 control, 1 interrupt): remote CID, opened, config done
 * both ways */
static uint16_t remote[2];
static uint8_t opened[2], peer_cfg[2], our_cfg[2];
/* H4 receive: pkt[0] = type */
static uint8_t pkt[MAX_PKT];
static unsigned pos, need, skip, body_known;
static uint32_t rxlog[12];      /* first 48 bytes received, for R85 */
static uint32_t clk[2];         /* clock 5 kHz, UART divisor (R85) */
/* PMU bus 1 diagnosis (R85 v5 "bus" words):
 *   0-2 CLKCON +0x14 +0x4c +0x6c as found; 3-11 IIC +0x00..+0x20 as found
 *   (read with the gates opened); 12 unstick: 1 = found busy (IICSTAT bit
 *   5) or pending (IICCON bit 4) | NACKed byte WAIT_* result << 8 (01 ok,
 *   00 timeout) | STOP done << 16; 13-21 IIC +0x00..+0x20 after the unstick
 *   and RetailOS's init; 22 first PMU read: byte | status << 8 (0 ok, 1
 *   address NACK, 2 data NACK, 3 no byte done) | 1 << 16 (done: status kept
 *   apart from the byte, a real FF reads 0x100ff); 23-31 at the first
 *   failed transfer: IICCON IICSTAT +0x20 IICDS +0x10, stage | transfer
 *   << 8, CLKCON +0x4c +0x6c +0x14; 32 STOPs re-issued | STOPs never done
 *   << 16; 33-35 PMU reads before power-up, as word 22: reg 0x00 (ID),
 *   0x07 (input.c's Home), 0x47 (Home per AGY's RE, bit 6) */
#define BUS_N 36
static uint32_t bus[BUS_N], xfers;
static const uint8_t pmu_regs[3] = {0x52, 0x56, 0x4b};
#define I2C(o) (*(volatile uint32_t *)(0x3c900000u + (o)))

static void tx(const uint8_t *p, unsigned n) {
    while (n--) {
        for (unsigned spins = 0; (UART(0x18) & 0x200u) && spins < 100000u; ++spins) {}
        UART(0x20) = *p++;
    }
}
static void hci_cmd(uint16_t op, const uint8_t *par, unsigned n) {
    uint8_t h[4] = {0x01, (uint8_t)op, (uint8_t)(op >> 8), (uint8_t)n};
    tx(h, 4);
    tx(par, n);
}
/* commands whose parameters are the 6-byte address + extra bytes */
static void addr_cmd(uint16_t op, const uint8_t *addr, const uint8_t *x, unsigned n) {
    uint8_t c[22];
    for (unsigned i = 0; i < 6 + n; ++i) c[i] = i < 6 ? addr[i] : x[i - 6];
    hci_cmd(op, c, 6 + n);
}
static void handle_cmd(uint16_t op, unsigned x) {   /* handle (+ one byte) */
    uint8_t c[3] = {(uint8_t)bt.handle, (uint8_t)(bt.handle >> 8), (uint8_t)x};
    hci_cmd(op, c, op == 0x0411 ? 2 : 3);
}
static void acl_send(uint16_t cid, const uint8_t *p, unsigned n) {
    uint8_t h[9] = {0x02, (uint8_t)bt.handle, (uint8_t)(bt.handle >> 8 | 0x20),
                    (uint8_t)(n + 4), (uint8_t)((n + 4) >> 8),
                    (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)cid, (uint8_t)(cid >> 8)};
    tx(h, 9);
    tx(p, n);
}
static void sig(uint8_t code, uint8_t id, const uint8_t *d, unsigned n) {
    uint8_t p[16];
    p[0] = code; p[1] = id; p[2] = (uint8_t)n; p[3] = 0;
    for (unsigned i = 0; i < n && i < 12; ++i) p[4 + i] = d[i];
    acl_send(0x0001, p, 4 + n);
}
static void step(unsigned s) { bt.step = s; step_t0 = USEC; }
static void patch_done(void);
static void report(void);
static void search(void) {
    unsigned any = 0;
    for (unsigned i = 0; i < 6; ++i) any |= bt.peer[i] = central ? target[i] : cfg_addr[i];
    if (central == 1) any = bt_ndev = 0;
    found = 0;
    opened[0] = opened[1] = 0;
    if (any) {
        static const uint8_t c[7] = {0x18, 0xcc, 0x01, 0, 0, 0, 0x01};  /* DM1..DH5, R1, role switch */
        addr_cmd(0x0405, bt.peer, c, 7);        /* Create_Connection */
        step(BT_CONNECT);
    } else {
        static const uint8_t c[5] = {0x33, 0x8b, 0x9e, 0x08, 0x00};     /* GIAC, 10.24 s */
        hci_cmd(0x0401, c, 5);                  /* Inquiry */
        step(BT_INQ);
    }
}
static void fail(unsigned status) {
    bt.failed = bt.step;
    bt.status = status;
    bt.pad = (struct bt_pad){0, 0, 0};
    if (bt.step < BT_INQ) bt.step = BT_ERR;
    else {
        if (bt.handle) handle_cmd(0x0406, 0x13);    /* Disconnect */
        bt.handle = 0;
        ++bt.retries;
        if (central) step(BT_READY); else search();
    }
    report();
}
static void l2cap_connect(unsigned ch) {
    uint16_t psm = ch ? 0x13 : 0x11, scid = ch ? CID_INTR : CID_CTRL;
    uint8_t d[4] = {(uint8_t)psm, 0, (uint8_t)scid, (uint8_t)(scid >> 8)};
    opened[ch] = 1;
    sig(0x02, ident++, d, 4);
}
static int channel(uint16_t our_cid) {
    return our_cid == CID_CTRL ? 0 : our_cid == CID_INTR ? 1 : -1;
}
static void config_req(unsigned ch) {
    uint8_t c[4] = {(uint8_t)remote[ch], (uint8_t)(remote[ch] >> 8), 0, 0};
    peer_cfg[ch] = our_cfg[ch] = 0;
    sig(0x04, ident++, c, 4);
}
static void channel_progress(unsigned ch) {
    if (!peer_cfg[ch] || !our_cfg[ch]) return;
    if (ch == 0 && bt.step == BT_CTRL) step(BT_INTR);
    else if (ch == 1 && bt.step == BT_INTR) { step(BT_HID); report(); }
}
static void signalling(const uint8_t *p, unsigned len) {
    while (len >= 4) {
        uint8_t code = p[0], id = p[1];
        unsigned n = p[2] | p[3] << 8;
        const uint8_t *d = p + 4;
        if (n > len - 4) n = len - 4;
        if (code == 0x02 && n >= 4) {           /* the pad opens a channel */
            unsigned psm = d[0] | d[1] << 8;
            int ch = psm == 0x11 ? 0 : psm == 0x13 ? 1 : -1;
            if (ch >= 0 && bt.step >= BT_AUTH) {
                uint16_t our = ch ? CID_INTR : CID_CTRL;
                uint8_t r[8] = {(uint8_t)our, (uint8_t)(our >> 8), d[2], d[3], 0, 0, 0, 0};
                sig(0x03, id, r, 8);
                remote[ch] = (uint16_t)(d[2] | d[3] << 8);
                opened[ch] = 1;
                config_req((unsigned)ch);
            }
        } else if (code == 0x03 && n >= 8) {    /* Connection Response */
            int ch = channel((uint16_t)(d[2] | d[3] << 8));
            unsigned result = d[4] | d[5] << 8;
            if (ch >= 0 && result == 0) {
                remote[ch] = (uint16_t)(d[0] | d[1] << 8);
                config_req((unsigned)ch);
            } else if (ch >= 0 && result != 1) fail(result);
        } else if (code == 0x04 && n >= 4) {    /* peer's Config Request */
            int ch = channel((uint16_t)(d[0] | d[1] << 8));
            if (ch >= 0 && remote[ch]) {
                uint8_t r[6] = {(uint8_t)remote[ch], (uint8_t)(remote[ch] >> 8), 0, 0, 0, 0};
                sig(0x05, id, r, 6);
                peer_cfg[ch] = 1;
                channel_progress((unsigned)ch);
            }
        } else if (code == 0x05 && n >= 6) {    /* response to ours */
            int ch = channel((uint16_t)(d[0] | d[1] << 8));
            if (ch >= 0 && !(d[4] | d[5] << 8)) {
                our_cfg[ch] = 1;
                channel_progress((unsigned)ch);
            }
        } else if (code == 0x06 && n >= 4) {    /* Disconnection Request */
            sig(0x07, id, d, 4);
            if (bt.step == BT_HID) fail(0x16);
        } else if (code == 0x0a && n >= 2) {    /* Information Request */
            uint8_t r[4] = {d[0], d[1], 1, 0};  /* not supported */
            sig(0x0b, id, r, 4);
        }
        p += 4 + n;
        len -= 4 + n;
    }
}
/* d-pad hat 0..7 (N, NE, .. NW; 8+ = released) -> PAD_UP/RIGHT/DOWN/LEFT >> 13 */
static const uint8_t hat_bits[8] = {1, 3, 2, 6, 4, 12, 8, 9};
/* Switch / NanoPad face buttons (south, east, west, north = B A Y X / a b x y
 * in bits 0 1 2 3) -> PAD_W S E N */
static unsigned face(unsigned b) { return (b >> 2 & 1) | (b & 3) << 1 | (b & 8); }
static void hid_report(const uint8_t *d, unsigned len) {
    unsigned id = d[1], hat = 8;
    uint32_t btn;
    int lx, ly;
    if (len == 7) {                             /* NanoPad: A1 btnLo btnHi X Y Z Rz */
        unsigned b = d[1] | d[2] << 8;
        btn = face(b) | (b & 0x30) | (b & 0xc0) << 2 | (b & 0x100) << 4;
        lx = (int8_t)d[3];
        ly = (int8_t)d[4];
    } else if (id == 0x3f && len >= 9) {        /* Switch simple: b0 b1 hat lx16 ly16 */
        btn = face(d[2]) | (d[2] & 0xf0) | (d[3] & 0x1f) << 8;
        hat = d[4];
        lx = d[6] - 128;
        ly = 128 - d[8];                        /* up = larger */
    } else {
        const uint8_t *st = d + (id == 0x11 ? 4 : id == 0x31 ? 3 : 2), *b = st + (id == 0x31 ? 7 : 4);
        if ((id != 0x01 && id != 0x11 && id != 0x31) || len < (unsigned)(b - d) + 3) return;
        btn = b[0] >> 4 | b[1] << 4 | (b[2] & 1u) << 12;
        hat = b[0] & 15;
        lx = st[0] - 128;
        ly = st[1] - 128;
    }
    if (hat < 8) btn |= (uint32_t)hat_bits[hat] << 13;
    bt.pad.lx = (int8_t)(lx < -127 ? -127 : lx);
    bt.pad.ly = (int8_t)(ly < -127 ? -127 : ly > 127 ? 127 : ly);
    bt.pad.buttons = btn;
    ++bt.reports;
}
static void acl(const uint8_t *p, unsigned n) {
    ++bt.acl;
    if (n < 9 || !bt.handle || ((p[1] | p[2] << 8) & 0xfff) != bt.handle) return;
    unsigned len = p[5] | p[6] << 8, cid = p[7] | p[8] << 8;
    if (len > n - 9) len = n - 9;
    const uint8_t *d = p + 9;
    if (cid == 0x0001) signalling(d, len);
    else if (cid == CID_INTR && bt.step == BT_HID && len >= 7 && d[0] == 0xa1) hid_report(d, len);
}
/* Central's list: the entry of address a (a new one, up to BT_DEVS, is
 * cleared), its class of device kept when cod is given */
static struct bt_dev *dev_add(const uint8_t *a, const uint8_t *cod) {
    unsigned i = 0, k;
    for (; i < bt_ndev; ++i) {
        for (k = 0; k < 6 && bt_devs[i].addr[k] == a[k]; ++k) ;
        if (k == 6) break;
    }
    if (i == BT_DEVS) return 0;
    struct bt_dev *d = &bt_devs[i];
    if (i == bt_ndev) { ++bt_ndev; d->named = 0; d->name[0] = 0; }
    for (k = 0; k < 6; ++k) d->addr[k] = a[k];
    if (cod) for (k = 0; k < 3; ++k) d->cod[k] = cod[k];
    return d;
}
static void set_name(struct bt_dev *d, const uint8_t *s, unsigned n) {
    unsigned m = 0;
    for (; m < n && m < 17 && s[m]; ++m) d->name[m] = (char)s[m];
    d->name[m] = 0;
    d->named = 1;
}
/* after the inquiry: one Remote_Name_Request at a time, then ready */
static void next_name(void) {
    for (unsigned i = 0; i < bt_ndev; ++i)
        if (!bt_devs[i].named) {
            static const uint8_t c[4] = {0x01, 0, 0, 0};   /* R1, clock offset unknown */
            bt_devs[i].named = 2;
            addr_cmd(0x0419, bt_devs[i].addr, c, 4);
            step(BT_INQ);
            return;
        }
    step(BT_READY);
}
static void event(const uint8_t *p, unsigned n) {
    ++bt.events;
    if (n < 3) return;
    unsigned code = p[1];
    const uint8_t *a = p + 3;                   /* BD_ADDR of the pairing events */
    if (code == 0x0e && n >= 7) {               /* Command Complete */
        unsigned op = p[4] | p[5] << 8, status = p[6];
        if (status) { fail(status); return; }
        if (op == 0x0c03 && bt.step == BT_RESET && !bt.patched) {
            patch = (const uint8_t *)PATCH;
            if (BT_PATCHRAM && *(const volatile uint16_t *)PATCH == 0xfc4c &&
                rpt_sum(patch, PATCH_END - PATCH) == PATCH_SUM) {
                hci_cmd(0xfc2e, 0, 0);          /* Download_Minidriver */
                patch_op = 0xfc2e;
                step(BT_PATCH);
                return;
            }
            bt.patched = 2;
        }
        if (bt.step == BT_PATCH) {              /* minidriver / record accepted */
            if (op != patch_op) return;         /* not the command in flight */
            if (op == 0xfc4e) {                 /* Launch_RAM: leaves BT_PATCH, */
                if (patch == (const uint8_t *)PATCH_END) patch_done();   /* only after */
                else fail(0xfb);                /* the whole blob, as the timeout path */
                return;
            }
            if (op == 0xfc2e) delay_us(50000);
            /* the next record must fit before PATCH_END: else "ERR patch FB" */
            if (patch + 3 > (const uint8_t *)PATCH_END ||
                patch + 3u + patch[2] > (const uint8_t *)PATCH_END) { fail(0xfb); return; }
            static const uint8_t cmd = 0x01;
            patch_op = (uint16_t)(patch[0] | patch[1] << 8);
            tx(&cmd, 1);
            tx(patch, 3u + patch[2]);           /* opcode, length, data */
            patch += 3u + patch[2];
            step(BT_PATCH);
        } else if (op == 0x0c03 && bt.step == BT_RESET) {
            hci_cmd(0x1001, 0, 0);              /* Read_Local_Version */
            step(BT_VER);
        } else if (op == 0x1001 && bt.step == BT_VER && n >= 15) {
            bt.hci_ver = p[7];
            bt.mfr = (uint16_t)(p[11] | p[12] << 8);
            bt.lmp_sub = (uint16_t)(p[13] | p[14] << 8);
            bt.have_ver = 1;
            static const uint8_t on = 1;
            hci_cmd(0x0c56, &on, 1);            /* Write_Simple_Pairing_Mode */
            step(BT_SSP);
        } else if (op == 0x0c56 && bt.step == BT_SSP) search();
        else if (op == 0x0402 && bt.step == BT_INQ && found) {
            static const uint8_t c[7] = {0x18, 0xcc, 0x01, 0, 0, 0, 0x01};
            addr_cmd(0x0405, bt.peer, c, 7);    /* Create_Connection */
            step(BT_CONNECT);
        }
    } else if (code == 0x0f && n >= 7) {        /* Command Status */
        if (p[3]) fail(p[3]);
    } else if ((code == 0x02 || code == 0x22 || code == 0x2f) && n >= 16 &&
               bt.step == BT_INQ && !found) {   /* Inquiry Result (std / RSSI / EIR) */
        if (central) {
            struct bt_dev *d = dev_add(p + 4, p + (code == 0x02 ? 13 : 12));
            /* EIR: elements len, type, data; 08/09 = short/complete name */
            for (unsigned k = 18; d && code == 0x2f && k + 2 < n && p[k]; k += p[k] + 1u)
                if ((p[k + 1] | 1) == 9) { set_name(d, p + k + 2, p[k] - 1u); break; }
        } else if ((p[code == 0x02 ? 14 : 13] & 0x1f) == 0x05) {   /* CoD major: peripheral */
            for (unsigned i = 0; i < 6; ++i) bt.peer[i] = p[4 + i];
            found = 1;
            hci_cmd(0x0402, 0, 0);              /* Inquiry_Cancel, then connect */
        }
    } else if (code == 0x01 && bt.step == BT_INQ && !found) {
        if (central) next_name(); else search();   /* Inquiry Complete, nothing: again */
    } else if (code == 0x07 && n >= 10 && bt.step == BT_INQ) {
        struct bt_dev *d = dev_add(p + 4, 0);  /* Remote_Name_Request_Complete */
        if (d && !p[3]) set_name(d, p + 10, n - 10);
        next_name();
    } else if (code == 0x03 && n >= 6 && bt.step == BT_CONNECT) {
        if (p[3]) { fail(p[3]); return; }       /* Connection Complete */
        bt.handle = (uint16_t)((p[4] | p[5] << 8) & 0xfff);
        handle_cmd(0x0411, 0);                  /* Authentication_Requested */
        step(BT_AUTH);
    } else if (code == 0x17 && n >= 9) {        /* Link_Key_Request */
        if (have_key) addr_cmd(0x040b, a, key, 16);
        else addr_cmd(0x040c, a, 0, 0);
    } else if (code == 0x18 && n >= 25) {       /* Link_Key_Notification */
        for (unsigned i = 0; i < 16; ++i) key[i] = p[9 + i];
        have_key = 1;
    } else if (code == 0x31 && n >= 9) {        /* IO_Capability_Request */
        static const uint8_t c[3] = {0x03, 0x00, 0x04};  /* NoInputNoOutput, no OOB, bonding */
        addr_cmd(0x042b, a, c, 3);
    } else if (code == 0x33 && n >= 9) {        /* User_Confirmation_Request */
        addr_cmd(0x042c, a, 0, 0);              /* Just Works: accept */
    } else if (code == 0x06 && n >= 6 && bt.step == BT_AUTH) {
        if (p[3]) { fail(p[3]); return; }       /* Authentication_Complete */
        handle_cmd(0x0413, 1);                  /* Set_Connection_Encryption on */
    } else if (code == 0x08 && n >= 7 && bt.step == BT_AUTH) {
        if (p[3] || !p[6]) { fail(p[3] ? p[3] : 0xfd); return; }  /* Encryption_Change */
        step(BT_CTRL);
    } else if (code == 0x05 && n >= 7 && bt.handle &&
               ((p[4] | p[5] << 8) & 0xfff) == bt.handle) {
        bt.handle = 0;                          /* Disconnection Complete */
        fail(p[6]);
    }
}
static void rx_byte(uint8_t b) {
    if (skip) { --skip; return; }
    if (!pos) {
        if (b != 0x02 && b != 0x04) { ++bt.dropped; return; }
        need = b == 0x04 ? 3 : 5;
        body_known = 0;
    }
    pkt[pos++] = b;
    if (pos < need) return;
    if (!body_known) {                          /* header in: add the body */
        body_known = 1;
        unsigned body = pkt[0] == 0x04 ? pkt[2] : (unsigned)(pkt[3] | pkt[4] << 8);
        if (need + body > MAX_PKT) { skip = body; pos = 0; ++bt.dropped; return; }
        need += body;
        if (body) return;
    }
    if (pkt[0] == 0x04) event(pkt, pos); else acl(pkt, pos);
    pos = 0;
}
/* "clock 5" (0x0842a1c0): CLKCON +0x10 source [29:28] 0 = 24 MHz, 1/2 =
 * PLL at +0x20/+0x24, / ([19:16]+1)*([23:20]+1); the device runs it from
 * 24 MHz (R85: 24000 kHz, divisor 0x0C), a PLL source keeps the divisor.
 * Divisions: libgcc (both links already pull it in for the GPU code). */
static uint32_t uart_khz(void) {   /* 24 MHz source only (the device's): else 0 */
    uint32_t c = CLKCON(0x10);
    return c >> 28 & 3 ? 0 : 24000u / (((c >> 16 & 15) + 1) * ((c >> 20 & 15) + 1));
}
/* as RetailOS's pin helper 0x0842a580 (0x0842a5be): every cmd sets the
 * bank's +0x14 bit (output 0xE/0xF and function n alike) except input
 * (its 0xFFFE, written as function 0), which clears it */
static __attribute__((noinline)) void pin(unsigned p, unsigned cmd) {
    uint32_t bit = 1u << (p & 7);
    volatile uint32_t *oe = &GPIO((p >> 3) * 0x20 + 0x14);
    *oe = cmd ? *oe | bit : *oe & ~bit;
    GPIO(0x1e0) = (p >> 3) << 16 | (p & 7) << 8 | cmd;
    __asm__ volatile("dsb sy" ::: "memory");
}
/* PMU transfer on I2C1 (0x3c900000) as RetailOS's IRAM driver does it
 * (start 0x22000652, ISR 0x22001214, STOP 0x220013a2), fix3 2026-10-17
 * after AGY's RE. The PMU bus has dedicated pads: pins 97/98/119 are the
 * BCM2078's FM I2S (0x08158e1c, "BT_KEY_FM_RADIO_POWER"), so the earlier
 * mux, level reads ("io 00") and pulses there never touched it. What
 * wedged the controller instead:
 *  - the clock gated off (+0x4c bit 6, +0x6c bit 12) after every transfer,
 *    also after a STOP that had not completed; the gates now stay open,
 *    with id 11 (CLKCON +0x14 bit 31, RetailOS 0x08001802) as well (the
 *    device found it gated: 0x80002200). Kernel f5b9a0db, which never
 *    gated, read the PMU 83..100 times per run;
 *  - IICCON bit 4 (pending: SCL held) never cleared before a START: START
 *    now writes IICCON whole (0x2181: ACK, events 0x2100, prescaler 1 as
 *    RetailOS's init 0x080017b4);
 *  - STOP given 1 ms once: RetailOS re-issues it for up to 100 ms; here 5
 *    times 1 ms, then the controller is idled (IICSTAT 0x10).
 * byte done = +0x20 bit 8 only (never IICCON bit 4 alone: a stale pending
 * bit ends the wait with IICDS still holding the address, the device's
 * 0xE7); the last byte's ACK is not checked. addr 0xe6: send b[0..n-1];
 * 0xe7: read b[0] (NACKed). 0 ok, else the stage. */
static int i2c_byte(void) { return wait_any(&I2C(0x20), 0x100u, 10000u) == WAIT_OK; }
static void gates_on(void) {
    CLKCON(0x14) &= ~0x80000000u;
    CLKCON(0x4c) &= ~0x40u;
    CLKCON(0x6c) &= ~0x1000u;
    __asm__ volatile("dsb sy" ::: "memory");
}
/* resume as input.c resume_ack() and the ISR (0x220013e2..0x2200141a):
 * IICCON with bit 4 clear and the ACK bit as given, +0x20 W1C, IICCON
 * written again */
static void i2c_next(uint32_t ack) {
    uint32_t c = (I2C(0x00) & ~0x90u) | ack;
    I2C(0x00) = c;
    I2C(0x20) = 0x100u;
    I2C(0x00) = c;
}
/* RetailOS's I2C init 0x080017b4 (+0x08 = 0x40: own address, 0x080017c0
 * -> 0x08002228; +0x20: no stale events) */
static void i2c_init(void) {
    static const uint16_t init[][2] = {
        {0x08, 0x40}, {0x14, 1}, {0x18, 0}, {0x04, 0x80}, {0x00, 0}, {0x04, 0},
        {0x0c, 0x40}, {0x00, 0x181}, {0x04, 0x10}, {0x28, 0}, {0x20, 0x2100},
    };
    for (unsigned i = 0; i < sizeof init / sizeof init[0]; ++i) I2C(init[i][0]) = init[i][1];
}
/* STOP as input.c stop(): STOP-done W1C, IICCON bit 13 on; then up to 5 x
 * (IICSTAT = mode, resume, 1 ms for +0x20 bit 13 or IICSTAT bit 5 clear);
 * done: IICCON &= ~0x2010, W1C, IICCON again (0x220013b8); never done:
 * the controller is initialised again. Each wait also has a spin backstop. */
static int i2c_stop(unsigned mode) {
    I2C(0x20) = 0x2000u;
    I2C(0x00) |= 0x2000u;
    for (unsigned t = 0; t < 5; ++t) {
        I2C(0x04) = mode;                       /* bit 5 clear: STOP */
        i2c_next(0x80u);
        uint32_t t0 = USEC;
        for (uint32_t n = 0; n < 1000u * 64u && USEC - t0 < 1000u; ++n)
            if ((I2C(0x20) & 0x2000u) || !(I2C(0x04) & 0x20u)) {
                uint32_t c = I2C(0x00) & ~0x2010u;
                I2C(0x00) = c;
                I2C(0x20) = 0x2100u;
                I2C(0x00) = c;
                return 1;
            }
        ++bus[32];
    }
    bus[32] += 0x10000u;
    i2c_init();
    return 0;
}
/* Before BT's first transfer: snapshot, and a controller left busy or
 * pending (handoff mid-transfer, or an earlier timeout) gets its byte
 * finished NACKed (9 clocks: a slave holding SDA lets go) and a STOP;
 * then RetailOS's init 0x080017b4. Bounded: 10 ms + 5 ms. */
static void i2c_open(void) {
    bus[0] = CLKCON(0x14); bus[1] = CLKCON(0x4c); bus[2] = CLKCON(0x6c);
    gates_on();
    delay_us(50);
    for (unsigned i = 0; i < 9; ++i) bus[3 + i] = I2C(4 * i);
    if ((I2C(0x04) & 0x20u) || (I2C(0x00) & 0x10u)) {
        i2c_next(0);
        bus[12] = 1 | (uint32_t)i2c_byte() << 8;
        bus[12] |= (uint32_t)i2c_stop(I2C(0x04) & 0xd0u) << 16;
    }
    i2c_init();
    delay_us(50);
    for (unsigned i = 0; i < 9; ++i) bus[13 + i] = I2C(4 * i);
}
static unsigned i2c_xfer(unsigned addr, uint8_t *b, unsigned n) {
    unsigned mode = addr & 1 ? 0x90u : 0xd0u, st = 0;
    gates_on();                                 /* and never off again */
    I2C(0x20) = 0x2100u;
    I2C(0x00) = 0x2181u;                        /* pending bit 4 clear */
    I2C(0x04) = mode;
    I2C(0x0c) = addr;
    I2C(0x04) = mode | 0x20u;                   /* START + address */
    for (unsigned i = 0;; ++i) {
        if (!i2c_byte()) { st = 3; break; }
        if (mode == 0x90u && i) { b[0] = (uint8_t)I2C(0x0c); break; }
        if (mode == 0xd0u && i == n) break;
        if (I2C(0x04) & 1u) { st = i ? 2 : 1; break; }
        if (mode == 0x90u) { i2c_next(0); continue; }   /* NACK the only byte */
        I2C(0x0c) = b[i];
        i2c_next(0x80u);
    }
    ++xfers;
    if (st && !bus[28]) {                       /* first failure: the raw controller state */
        static const uint32_t at[9] = {0x3c900000u, 0x3c900004u, 0x3c900020u, 0x3c90000cu,
                                       0x3c900010u, 0, 0x3c50004cu, 0x3c50006cu, 0x3c500014u};
        for (unsigned i = 0; i < 9; ++i)
            bus[23 + i] = at[i] ? *(volatile uint32_t *)at[i] : st | xfers << 8;
    }
    i2c_stop(mode);
    return st;
}
static int pmu_get(unsigned reg) {
    uint8_t b = (uint8_t)reg;
    unsigned st = i2c_xfer(0xe6, &b, 1);        /* register pointer, STOP */
    if (!st) st = i2c_xfer(0xe7, &b, 1);
    bt.i2c_st = (uint8_t)st;
    if (!bus[22]) bus[22] = b | st << 8 | 1u << 16;
    return st ? -1 : b;
}
/* write until the register reads back the value (16 tries: a verified
 * write needs 3 address phases, and the device NACKed ~half of them);
 * the reads go to *rb (0xff = never read) */
static void got(uint8_t *rb, int r) {          /* bt.pmu_ok: which bytes were read */
    unsigned m = 1u << (rb - bt.pmu[0]);
    *rb = (uint8_t)r;
    bt.pmu_ok = (uint8_t)(r < 0 ? bt.pmu_ok & ~m : bt.pmu_ok | m);
}
static int pmu_set(unsigned reg, unsigned val, uint8_t *rb) {
    for (unsigned n = 0; n < 16; ++n) {
        uint8_t b[2] = {(uint8_t)reg, (uint8_t)val};
        int r = (bt.i2c_st = (uint8_t)i2c_xfer(0xe6, b, 2)) ? -1 : pmu_get(reg);
        got(rb, r);
        if (r == (int)val) return 1;
        ++bt.pmu_err;
    }
    return 0;
}
/* pull-up on, as 0x084165a8(pin, 1) (its config word bit 4 = bank +0x0c,
 * 0x080066e0) */
static __attribute__((noinline)) void pull_up(unsigned p) { GPIO((p >> 3) * 0x20 + 0x0c) |= 1u << (p & 7); }
/* RetailOS's order for chip type 5 (0x0854f57c..0x0854f59a): GPIO70 high
 * + pull-up, PMU power 52/56 + 10 ms, GPIO70 high again + 50 ms, reset
 * 4b=09, 50 ms, 4b=0b, then the UART. 0 = done, else (nothing after it,
 * no HCI) 0xe1 PMU ID read failed, 0xe2 power write, 0xe3 reset write. */
static unsigned power_up(void) {
    static const uint8_t probe[3] = {0x00, 0x07, 0x47};
    int id = -1;
    i2c_open();
    for (unsigned i = 0; i < 3; ++i) {
        int v = pmu_get(probe[i]);
        if (!i) id = v;
        bus[33 + i] = (uint8_t)v | (uint32_t)bt.i2c_st << 8 | 1u << 16;
    }
    for (unsigned i = 0; i < 3; ++i) got(&bt.pmu[0][i], pmu_get(pmu_regs[i]));
    if (id < 0) return 0xe1;
    pin(70, 0xf);                               /* BT_WAKE high (0x08178be8) */
    pull_up(70);
    if (!pmu_set(0x52, 0xea, &bt.pmu[1][0]) || !pmu_set(0x56, 0x49, &bt.pmu[1][1])) return 0xe2;
    delay_us(10000);
    pin(70, 0xf);
    delay_us(50000);
    if (!pmu_set(0x4b, 0x09, &bt.pmu[1][2])) return 0xe3;  /* reset held */
    delay_us(50000);
    if (!pmu_set(0x4b, 0x0b, &bt.pmu[1][2])) return 0xe3;  /* reset released */
    /* UART open as 0x08172bcc / 0x08550c44 / 0x08550d4c: TX RX function 2,
     * TX pull-up; RTS driven high while configuring; CTS function 2 +
     * pull-up; UCON 0x405 (RetailOS ORs 0xc in: TX by DMA, not polled),
     * UMCON 0x11 = auto flow control as RetailOS (was 1, RTS only); RTS
     * function 2 last */
    pin(78, 2); pin(79, 2); pull_up(78);
    CLKCON(0x4c) &= ~0x20000000u;
    CLKCON(0x6c) &= ~0x100u;
    __asm__ volatile("dsb sy" ::: "memory");
    pin(80, 0xf);
    pin(81, 2); pull_up(81);
    UART(0x00) = 3;
    UART(0x04) = 0x405;
    UART(0x08) = 7;
    UART(0x0c) = 0x11;
    UART(0x10) = 0x170;
    uint32_t khz = clk[0] = uart_khz();
    /* PLL source or implausible clock (NanoEmu): keep the divisor */
    UART(0x28) = clk[1] = khz >= 2000 && khz <= 300000
                          ? (khz * 1000u + 8 * BAUD) / (16 * BAUD) - 1 : UART(0x28);
    pin(80, 2);
    delay_us(5000);
    return 0;
}
/* R85 (report.h mailbox, NanoOSLoader exports R85.txt): a free slot or
 * the last R85, searched from slot 3 down to 0 (the Drivers page's R81 R82
 * R84 take 1..3 first), else slot 0 (GPU test R50). Written on every
 * failure and when the pad comes up. Header "== BT R85 v5 ==" (v5 =
 * this layout), one line per 6 words, "address: words" (little-endian
 * words):
 *   struct bt_state, one word each (bt.h): step, failed, status, peer[0..3],
 *   peer[4..5] handle, events, acl, reports, dropped, retries, pad lx ly -,
 *   pad buttons, have_ver hci_ver tries patched, mfr lmp_sub, pmu[0][0..2]
 *   pmu[1][0], pmu[1][1..2] i2c_st pmu_ok, pmu_err, rxn; then clock 5 kHz
 *   + divisor, UART +0x00..+0x28 (read at the end: the +0x24 read pops one
 *   RX byte), bus[48] (above: PMU bus check + first failed transfer),
 *   I2C1 +0x00..+0x10 and +0x20 at the end (gates opened first), GPIO
 *   bank 8 (pin 70), CLKCON +0x10 (clock 5) +0x14, RetailOS's
 *   patchram in DRAM (4cfc..), the first 48 RX bytes */
static const uint32_t dump[][2] = {
    {(uint32_t)&bt, sizeof bt / 4}, {(uint32_t)clk, 2}, {0x3db00000u, 11},
    {(uint32_t)bus, BUS_N}, {0x3c900000u, 5}, {0x3c900020u, 1},
    {0x3cf00100u, 6}, {0x3c500010u, 2}, {0x0890c884u, 2}, {(uint32_t)rxlog, 12},
};
static void report(void) {
    volatile struct rpt_hdr *h = RPT_HDR;
    unsigned s = 4;
    if (!rpt_valid()) {
        for (uint32_t i = 0; i < sizeof *h / 4; ++i) ((volatile uint32_t *)h)[i] = 0;
        h->slot_size = RPT_SLOT_SIZE;
    }
    while (s-- && h->id[s] && h->id[s] != RPT_BT_ID) ;
    if (s > 3) s = 0;
    char *p0 = RPT_SLOT(s);
    fmt_out = p0;
    fmt_end = p0 + 4096;
    fmt_str("== BT R85 v5 =="); fmt_line(0, 0, 0);
    gates_on();                                 /* the IIC words below read clocked (they stay on) */
    for (unsigned d = 0; d < sizeof dump / sizeof dump[0]; ++d)
        for (unsigned i = 0; i < dump[d][1]; i += 6) {
            const volatile uint32_t *w = (const volatile uint32_t *)dump[d][0] + i;
            fmt_hex((uint32_t)w, 8); fmt_str(":");
            for (unsigned k = 0; k < 6 && i + k < dump[d][1]; ++k) { fmt_str(" "); fmt_hex(w[k], 8); }
            fmt_line(0, 0, 0);
        }
    fmt_str("== END R85 =="); fmt_line(0, 0, 0);
    uint32_t len = (uint32_t)(fmt_out - p0);
    fmt_out = 0;
    h->id[s] = RPT_BT_ID;
    h->len[s] = len;
    h->sum[s] = rpt_sum(p0, len);
    h->magic[0] = RPT_MAGIC0;
    h->magic[1] = RPT_MAGIC1;
    h->hdr_sum = rpt_hdr_sum();
}
static void reset_cmd(void);
static void patch_done(void) {                  /* Launch_RAM: the patched firmware boots */
    patch_op = 0;
    delay_us(250000);
    bt.patched = 1;
    reset_cmd();
}
static void reset_cmd(void) {
    unsigned spins = 0;
    while ((UART(0x10) & 1) && spins++ < 65536u) (void)UART(0x24);
    pos = skip = 0;
    ++bt.tries;
    hci_cmd(0x0c03, 0, 0);                      /* Reset */
    step(BT_RESET);
}
void bt_start(void) {
    for (unsigned i = 0; i < sizeof bt / 4; ++i) ((uint32_t *)&bt)[i] = 0;
    for (unsigned i = 0; i < BUS_N; ++i) bus[i] = 0;
    have_key = 0;
    xfers = 0;
    unsigned e = power_up();
    if (e) {                                    /* "ERR ERR pmu Ex": no HCI */
        bt.failed = BT_PMU;
        bt.status = e;
        bt.step = BT_ERR;
        report();
        return;
    }
    reset_cmd();
}
void bt_service(void) {
    if (bt.step == BT_OFF || bt.step == BT_ERR) return;
    for (unsigned n = 0; n < RX_BUDGET && (UART(0x10) & 1); ++n) {
        uint8_t b = (uint8_t)UART(0x24);
        if (bt.rxn < sizeof rxlog) ((uint8_t *)rxlog)[bt.rxn] = b;
        ++bt.rxn;
        rx_byte(b);
    }
    uint32_t t = USEC - step_t0;
    unsigned ch = bt.step - BT_CTRL;
    if (ch < 2 && !opened[ch] && t > OPEN_US) l2cap_connect(ch);   /* the pad did not */
    if (bt.step < BT_HID && t > (bt.step < BT_INQ ? STEP_US : PAIR_US)) {
        if (bt.step == BT_RESET && bt.tries < RESET_TRIES) reset_cmd();
        else if (bt.step == BT_PATCH && patch == (const uint8_t *)PATCH_END && patch_op == 0xfc4e)
            patch_done();                       /* Launch_RAM without a Command Complete */
        else fail(0xff);
    }
}
int bt_pad_state(struct bt_pad *p) {
    *p = bt.pad;
    return bt.step == BT_HID;
}
int bt_pad_poll(struct bt_pad *p) {
    if (bt.step == BT_OFF) { central = 0; bt_start(); }
    bt_service();
    return bt_pad_state(p);
}
void bt_disconnect(void) {
    if (bt.handle) handle_cmd(0x0406, 0x13);    /* Disconnect, remote user */
    bt.handle = 0;
    bt.pad = (struct bt_pad){0, 0, 0};
    if (bt.step > BT_INQ && bt.step <= BT_HID) step(BT_READY);
}
/* manual mode m: started if off (search follows the bring-up), else
 * searched from ready or a link (dropped first); a search is cancelled for
 * a connect (Inquiry_Cancel's Command Complete pages, as the automatic
 * mode); during bring-up or a connect only the mode changes */
static void central_go(unsigned m) {
    central = (uint8_t)m;
    if (bt.step == BT_OFF || bt.step == BT_ERR) bt_start();
    else if (bt.step == BT_INQ && m == 2) {     /* searching: cancel, then page */
        for (unsigned i = 0; i < 6; ++i) bt.peer[i] = target[i];
        found = 1;
        hci_cmd(0x0402, 0, 0);
    } else if (bt.step >= BT_HID) { bt_disconnect(); search(); }
}
void bt_scan(void) { central_go(1); }
void bt_connect(const uint8_t *a) {
    for (unsigned i = 0; i < 6; ++i) target[i] = a[i];
    central_go(2);
}
void bt_power(int on) {
    if (on) { bt_scan(); return; }
    if (bt.step == BT_OFF) return;
    bt_disconnect();
    pmu_set(0x4b, 0x09, &bt.pmu[1][2]);         /* reset held (16 tries at most) */
    pin(70, 0xe);                               /* BT_WAKE low */
    CLKCON(0x4c) |= 0x20000000u;                /* UART1 gate id 16 off */
    CLKCON(0x6c) |= 0x100u;
    bt.step = BT_OFF;
}
const char *bt_step_name(unsigned s) {
    static const char *const names[] = {"off", "reset", "patch", "ver", "ssp", "inq",
                                         "conn", "auth", "ctrl", "intr", "hid up", "ERR", "ready", "pmu"};
    return s < 14 ? names[s] : "?";
}
/* NAME STATUS details:
 *   pad up:  BT OK PAD ok <mac> rep N
 *   else:    BT <OK|--|ERR> <step> [<failed> SS] hci HH lmp SSSS mf MMMM r N
 *            or, before the version: ... pm XX e N s N
 *   OK = the controller answered (version read); failed SS = the last
 *   failure (step, HCI status, FF = timeout); hci = HCI version, lmp = LMP
 *   subversion, mf = manufacturer (Broadcom 000F); r = searches restarted;
 *   pm = PMU reg 0x4b read back (0B = reset released, FF = never read),
 *   e = failed PMU attempts, s = stage of the last I2C failure, id = PMU
 *   reg 0x00 read before power-up (status << 8 | byte: 0xx = a real
 *   read), S = IICSTAT at the first failed transfer (R85 bus words 33,
 *   24) */
void bt_line(void) {
    fmt_str("BT    ");
    if (bt.step == BT_HID) {
        fmt_str("OK PAD ok ");
        for (unsigned i = 6; i--;) { fmt_hex(bt.peer[i], 2); if (i) fmt_str(":"); }
        fmt_str(" rep "); fmt_dec(bt.reports);
        return;
    }
    fmt_str(bt.have_ver ? "OK " : bt.step == BT_ERR ? "ERR " : "-- ");
    fmt_str(bt_step_name(bt.step));
    if (bt.status) {
        fmt_str(" "); fmt_str(bt_step_name(bt.failed)); fmt_str(" "); fmt_hex(bt.status, 2);
    }
    if (bt.have_ver) {
        fmt_str(" hci "); fmt_hex(bt.hci_ver, 2); fmt_str(" lmp "); fmt_hex(bt.lmp_sub, 4);
        fmt_str(" mf "); fmt_hex(bt.mfr, 4); fmt_str(" r "); fmt_dec(bt.retries);
    } else {
        fmt_str(" pm "); fmt_hex(bt.pmu[1][2], 2);
        fmt_str(" e "); fmt_dec(bt.pmu_err); fmt_str(" s "); fmt_dec(bt.i2c_st);
        fmt_str(" id "); fmt_hex(bus[33] & 0x3ff, 3);
        fmt_str(" S "); fmt_hex(bus[24], 2);
    }
}
