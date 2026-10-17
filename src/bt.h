#ifndef NANOOS_BT_H
#define NANOOS_BT_H
#include <stdint.h>
enum { BT_OFF, BT_RESET, BT_PATCH, BT_VER, BT_SSP, BT_INQ, BT_CONNECT, BT_AUTH,
       BT_CTRL, BT_INTR, BT_HID, BT_ERR, BT_READY,
       BT_PMU /* only as bt.failed: power_up's PMU step failed, no HCI */ };
/* Gamepad state, the same for every pad (bt.c maps each report format):
 * sticks -127..127 (ly -127 = up), buttons PAD_* */
struct bt_pad { int8_t lx, ly; uint32_t buttons; };
#define PAD_W      0x00001u  /* square   / Switch Y / NanoPad x */
#define PAD_S      0x00002u  /* cross    / Switch B / NanoPad a (action) */
#define PAD_E      0x00004u  /* circle   / Switch A / NanoPad b */
#define PAD_N      0x00008u  /* triangle / Switch X / NanoPad y */
#define PAD_L1     0x00010u
#define PAD_R1     0x00020u
#define PAD_L2     0x00040u
#define PAD_R2     0x00080u
#define PAD_SELECT 0x00100u  /* Create/Share / minus */
#define PAD_START  0x00200u  /* Options / plus */
#define PAD_L3     0x00400u
#define PAD_R3     0x00800u
#define PAD_HOME   0x01000u  /* PS / Home */
#define PAD_UP     0x02000u  /* d-pad (hat) */
#define PAD_RIGHT  0x04000u
#define PAD_DOWN   0x08000u
#define PAD_LEFT   0x10000u
/* step: enum above; failed/status: the step that timed out or was refused
 * and why (0xff = timeout); retries: searches restarted after a failure
 * past bring-up; peer: the pad's BD_ADDR (wire order) */
struct bt_state { unsigned step, failed, status; uint8_t peer[6];
                  uint16_t handle; uint32_t events, acl, reports, dropped, retries;
                  struct bt_pad pad;
                  /* Read_Local_Version (valid once have_ver); PMU regs 0x52
                   * 0x56 0x4b before / after power-up (bt.c's own I2C
                   * reads), failed PMU attempts (transfer error or wrong
                   * read-back) and the stage of the last one (1 address
                   * NACK, 2 data NACK, 3 no byte done), HCI Reset tries,
                   * patchram (1 loaded, 2 skipped: RetailOS's copy in DRAM
                   * failed the checksum), raw bytes received; pmu_ok:
                   * bit 3k+i = pmu[k][i] is a byte read (else FF = failed) */
                  uint8_t have_ver, hci_ver, tries, patched;
                  uint16_t mfr, lmp_sub;
                  uint8_t pmu[2][3], i2c_st, pmu_ok; uint32_t pmu_err, rxn; };
extern struct bt_state bt;
void bt_start(void);     /* power up, HCI reset + version, then find the pad */
void bt_service(void);   /* call every pass: RX, protocol steps, deadlines */
/* ViceNano: starts BT on the first call (power-up blocks ~0.2-0.5 s), then
 * services it; fills *p, returns 1 while a pad is connected (else 0, *p
 * zeroed) */
int bt_pad_poll(struct bt_pad *p);
const char *bt_step_name(unsigned step);
/* Bluetooth Central (btui.c, Settings): manual mode. bt_power(1) brings
 * the controller up and scans; bt_scan lists every device answering an
 * inquiry (name from EIR, else a remote name request after it) instead of
 * taking the first gamepad; bt_connect pages one of them (the same HID
 * path: SSP Just Works, L2CAP 0x11/0x13, every pad format); a failure or
 * bt_disconnect ends in BT_READY, no search again. bt_power(0): link
 * dropped, PMU 0x4b = 0x09 (reset held, as RetailOS's BT off), BT_WAKE
 * low, UART1 clock gated. ViceNano's bt_pad_poll starts in the old
 * automatic mode. */
struct bt_dev { uint8_t addr[6], cod[3], named; char name[18]; };
#define BT_DEVS 10
extern struct bt_dev bt_devs[BT_DEVS];
extern unsigned bt_ndev;
void bt_power(int on);
void bt_scan(void);
void bt_connect(const uint8_t *addr);
void bt_disconnect(void);
int bt_pad_state(struct bt_pad *p);   /* *p = the pad, 1 while connected */
void bt_line(void);      /* the Drivers page line, into fmt's buffer */
#endif
