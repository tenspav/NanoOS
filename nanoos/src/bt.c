#include "bt.h"
#include "timer.h"
/* Bluetooth HID host over the BT controller's HCI (H4) UART.
 *
 * UART 1 at 0x3db00000 (Samsung UART; NanoEmu nano7_btuart.c:2205-2310):
 * +0x10 bit0 RX data ready, +0x18 bit9 TX FIFO full, +0x20 TX byte,
 * +0x24 RX byte. RetailOS left the port configured (baud, FIFOs). H4:
 * 01 = command, 02 = ACL data, 04 = event.
 *
 * Sequence (host-initiated, no pairing: the HID peer accepts PSM 0x11 /
 * 0x13 without authentication, as many gamepads in "pairing mode" do):
 *   HCI Reset -> Read_BD_ADDR -> Create_Connection to the pad ->
 *   L2CAP connect PSM 0x11 (HID control), configure both directions ->
 *   L2CAP connect PSM 0x13 (HID interrupt), configure ->
 *   input reports  A1 btnLo btnHi X Y Z Rz  on the interrupt channel.
 * The pad is NanoEmu's fake "NanoPad" (02:4e:50:41:44:01). Inquiry,
 * Secure Simple Pairing / link keys and incoming connections (a paired pad
 * pages the iPod) are the next steps for real pads; see DRIVER_PORT.md. */
#define UART(o) (*(volatile uint32_t *)(0x3db00000u + (o)))
#define STEP_US 1500000u
#define CID_CTRL 0x0041u
#define CID_INTR 0x0042u
#define MAX_PKT 300u
#define RX_BUDGET 512u          /* bytes per pass */
struct bt_state bt;
/* as sent on the wire (nano7_btuart.c:153) */
static const uint8_t pad_addr[6] = {0x01, 0x44, 0x41, 0x50, 0x4e, 0x02};
static uint32_t step_t0;
static uint8_t ident = 1;
/* per channel (0 control, 1 interrupt): remote CID, config done both ways */
static uint16_t remote[2];
static uint8_t peer_cfg[2], our_cfg[2];
/* H4 receive: pkt[0] = type */
static uint8_t pkt[MAX_PKT];
static unsigned pos, need, skip, body_known;

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
static void fail(unsigned status) {
    bt.failed = bt.step;
    bt.status = status;
    bt.step = BT_ERR;
}
static void l2cap_connect(unsigned ch) {
    uint16_t psm = ch ? 0x13 : 0x11, scid = ch ? CID_INTR : CID_CTRL;
    uint8_t d[4] = {(uint8_t)psm, 0, (uint8_t)scid, (uint8_t)(scid >> 8)};
    remote[ch] = 0;
    peer_cfg[ch] = our_cfg[ch] = 0;
    sig(0x02, ident++, d, 4);
    step(ch ? BT_INTR : BT_CTRL);
}
static int channel(uint16_t our_cid) {
    return our_cid == CID_CTRL ? 0 : our_cid == CID_INTR ? 1 : -1;
}
static void channel_progress(unsigned ch) {
    if (!peer_cfg[ch] || !our_cfg[ch]) return;
    if (ch == 0 && bt.step == BT_CTRL) l2cap_connect(1);
    else if (ch == 1 && bt.step == BT_INTR) step(BT_HID);
}
static void signalling(const uint8_t *p, unsigned len) {
    while (len >= 4) {
        uint8_t code = p[0], id = p[1];
        unsigned n = p[2] | p[3] << 8;
        const uint8_t *d = p + 4;
        if (n > len - 4) n = len - 4;
        if (code == 0x03 && n >= 8) {           /* Connection Response */
            int ch = channel((uint16_t)(d[2] | d[3] << 8));
            unsigned result = d[4] | d[5] << 8;
            if (ch >= 0 && result == 0) {
                remote[ch] = (uint16_t)(d[0] | d[1] << 8);
                uint8_t c[4] = {d[0], d[1], 0, 0};  /* our Config Request */
                sig(0x04, ident++, c, 4);
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
        }
        p += 4 + n;
        len -= 4 + n;
    }
}
static void acl(const uint8_t *p, unsigned n) {
    ++bt.acl;
    if (n < 9 || ((p[1] | p[2] << 8) & 0xfff) != bt.handle) return;
    unsigned len = p[5] | p[6] << 8, cid = p[7] | p[8] << 8;
    if (len > n - 9) len = n - 9;
    const uint8_t *d = p + 9;
    if (cid == 0x0001) signalling(d, len);
    else if (cid == CID_INTR && len >= 7 && d[0] == 0xa1) {
        bt.buttons = (uint16_t)(d[1] | d[2] << 8);
        for (unsigned i = 0; i < 4; ++i) bt.axis[i] = (int8_t)d[3 + i];
        ++bt.reports;
    }
}
static void event(const uint8_t *p, unsigned n) {
    ++bt.events;
    if (n < 3) return;
    unsigned code = p[1];
    if (code == 0x0e && n >= 7) {               /* Command Complete */
        unsigned op = p[4] | p[5] << 8, status = p[6];
        if (status) { if (op == 0x0c03 || op == 0x1009) fail(status); return; }
        if (op == 0x0c03 && bt.step == BT_RESET) {
            hci_cmd(0x1009, 0, 0);              /* Read_BD_ADDR */
            step(BT_ADDR);
        } else if (op == 0x1009 && bt.step == BT_ADDR && n >= 13) {
            for (unsigned i = 0; i < 6; ++i) bt.local[i] = p[7 + i];
            uint8_t c[13];
            for (unsigned i = 0; i < 13; ++i) c[i] = i < 6 ? pad_addr[i] : 0;
            c[6] = 0x18; c[7] = 0xcc;           /* DM1..DH5 */
            c[8] = 0x02;                        /* page scan R2 */
            c[12] = 0x01;                       /* allow role switch */
            hci_cmd(0x0405, c, 13);             /* Create_Connection */
            step(BT_CONNECT);
        }
    } else if (code == 0x0f && n >= 7) {        /* Command Status */
        if (p[3]) fail(p[3]);
    } else if (code == 0x03 && n >= 6 && bt.step == BT_CONNECT) {
        if (p[3]) { fail(p[3]); return; }       /* Connection Complete */
        bt.handle = (uint16_t)((p[4] | p[5] << 8) & 0xfff);
        l2cap_connect(0);
    } else if (code == 0x05 && bt.step >= BT_CTRL && bt.step <= BT_HID) {
        fail(n >= 7 ? p[6] : 0xff);             /* Disconnection Complete */
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
void bt_start(void) {
    unsigned spins = 0;
    while ((UART(0x10) & 1) && spins++ < 65536u) (void)UART(0x24);
    pos = skip = 0;
    bt.failed = bt.status = 0;
    bt.events = bt.acl = bt.reports = bt.dropped = 0;
    bt.handle = 0;
    hci_cmd(0x0c03, 0, 0);                      /* Reset */
    step(BT_RESET);
}
void bt_service(void) {
    if (bt.step == BT_OFF) return;
    for (unsigned n = 0; n < RX_BUDGET && (UART(0x10) & 1); ++n)
        rx_byte((uint8_t)UART(0x24));
    if (bt.step != BT_HID && bt.step != BT_ERR && USEC - step_t0 > STEP_US)
        fail(0xff);
}
const char *bt_step_name(unsigned s) {
    static const char *const names[] = {"off", "reset", "addr", "connect",
                                         "hid ctrl", "hid intr", "hid up", "ERR"};
    return s < 8 ? names[s] : "?";
}
