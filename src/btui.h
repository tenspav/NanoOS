#ifndef NANOOS_BTUI_H
#define NANOOS_BTUI_H
/* Settings > Bluetooth Central (btui.c), the launcher's scene 3 */
void btui_enter(void);
/* every main-loop pass while open: keys (input + swipes), tap = a touch tap
 * at touch.x / .y; services BT, draws when the LCD is ready; returns
 * BUTTON_HOME to go back (Vol- + Vol+ or a tap on the top strip), else 0 */
unsigned btui_tick(unsigned keys, unsigned tap);
#endif
