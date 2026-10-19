#ifndef NANOOS_GAME_H
#define NANOOS_GAME_H
/* Large game image (gameabi.h, docs/memory-map.md). game_boot() runs once,
 * right after mmu_init() and before anything else writes DRAM above the
 * kernel: it copies NanoOSLoader's game.bin chunks to the image's load
 * address and checks the image; 1 = a game is ready (the launcher shows
 * its icon). game_run() launches it (BSS zeroed each time; the game's
 * .data is not restored, so it re-initialises its own state) and returns
 * when the game does. */
int game_boot(void);
void game_run(void);
#endif
