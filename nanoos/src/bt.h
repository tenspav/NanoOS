#ifndef NANOOS_BT_H
#define NANOOS_BT_H
#include <stdint.h>
enum { BT_OFF, BT_RESET, BT_ADDR, BT_CONNECT, BT_CTRL, BT_INTR, BT_HID,
       BT_ERR };
/* step: enum above; failed: the step that timed out or was refused;
 * buttons/axes: last HID input report (bit0 = button 1; X Y Z Rz signed,
 * -127..127, Y -127 = up) */
struct bt_state { unsigned step, failed, status; uint8_t local[6];
                  uint16_t handle; uint32_t events, acl, reports, dropped;
                  uint16_t buttons; int8_t axis[4]; };
extern struct bt_state bt;
void bt_start(void);     /* HCI reset, then connect the gamepad */
void bt_service(void);   /* call every pass: RX, protocol steps, deadlines */
const char *bt_step_name(unsigned step);
#endif
