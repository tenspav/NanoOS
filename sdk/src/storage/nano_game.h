#ifndef NS_NANO_GAME_H
#define NS_NANO_GAME_H
#include "game.h"
struct nanoos_api;
/* ABI19 adapter. Mount performs reads/features only. Program/erase can occur
 * only through an explicit save_store on a prepared file. Kernel ABI unchanged.
 * Call from a game with exclusive NAND ownership; do not mix direct raw writes.
 * State is caller-owned in DRAM; no process-global mounted singleton. */
int ns_nano_game_mount(struct ns_game *,const struct nanoos_api *);
#endif
