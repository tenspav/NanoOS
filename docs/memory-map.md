# NanoOS 64 MB memory map (stage 1 of the Vice City port)

Source of truth: `src/memmap.h` (values), `kernel.ld` (kernel sections), `src/mmu.c` (CPU MMU),
`src/mbx.h` (GPU MMU). Since 2026-10-18 the CPU MMU is on, with a flat 1:1 map of 1 MB sections and
the D-cache enabled. `-DNANOOS_MMU=0` (build_payload.py: `NANOOS_MMU=0`) builds the old MMU-off
kernel.

## Regions

| Range | Size | CPU attribute | Owner / contents |
|---|---|---|---|
| 0x08000000-0x081fffff | 2 MB | Normal WB-WA | Kernel: text+data at 0x08000000 (at most 64 KB, kernel.ld assert), .bss from 0x08010000 (CPU-only buffers, the 16 KB MMU table `ttb`), 16 KB stack, `kalloc` heap up to 0x08200000 (about 1.8 MB, CPU-only) |
| 0x08200000-0x082fffff | 1 MB | Strongly-ordered | LCD scan-out BUF0 0x08200000 and BUF1 0x08280000, 0x65400 B each (lcd.c, display DMA) |
| 0x08300000-0x083fffff | 1 MB | Strongly-ordered | mbx_test render target and trace ring (GPU writes) |
| 0x08400000-0x084fffff | 1 MB | Normal WB-WA | NPAK asset pack, moved here by startup.s (CPU-only reads) |
| 0x08500000-0x087fffff | 3 MB | Strongly-ordered | Kernel `.dma` (`MM_DMA_BUF`): MBX page tables, PB, parameter pool and aux workspace; ViceNano render target and texels; audio DMA passes and LLIs. About 1.66 MB used |
| 0x08800000-0x089fffff | 2 MB | Normal WB-WA | RetailOS leftovers that NanoOS still reads: bt.c's patchram blob 0x0890c884..0x08914203. Nothing is loaded here |
| 0x08a00000-0x09ffffff | 22 MB | Strongly-ordered | Game GPU arena: textures, render targets and MBX pools of the game, GPU VA 0x00a00000..0x02000000 |
| 0x0a000000-0x0affffff | 16 MB | Normal WB-WA | Game heap / streaming arena (CPU-only). Holds the report mailbox and mirrors (see below) |
| 0x0b000000-0x0b9fffff | 10 MB | Normal WB-WA | Game image: header, code, data, BSS, stack (`MM_GAME`; the stub links at 0x0b000000) |
| 0x0ba00000-0x0bffffff | 6 MB | Normal WB-WA | Second CPU-only game heap. Holds 2 report mirrors |
| 0x38000000-0x3fffffff | | Strongly-ordered, XN | MMIO |
| everything else | | Strongly-ordered | Boot ROM, SRAM 0x22000000 (chainloader block 0x22009000), unused |

`MM_UNCACHED` (memmap.h) is the bitmap of uncached DRAM sections. A section's attribute is changed in
that one place.

### Why each choice

- **DMA and GPU buffers are uncached, not cleaned.** Every non-CPU master (MBX, display, audio DMA)
  reads and writes plain DRAM, and the drivers were written for the MMU-off machine, where all memory
  is Strongly-ordered. Some drivers poll GPU-written words: `pool_mark`, `snap_*`, the aux free
  list, the ViceNano sentinel. Grouping these buffers in `.dma` and mapping that section
  Strongly-ordered keeps their behaviour bit-for-bit, so no cache maintenance has to be added to
  tuned driver code. Normal non-cacheable, or write-back plus explicit clean/invalidate, is a later
  optimisation. It needs DSBs before every doorbell, and they need checking on the device.
- **Kernel, heap, stack, pack and game are cached.** They are CPU-only (NAND, USB and the BT UART
  are PIO).
- **The report mailbox survives.** `reset_stockos()` cleans the whole D-cache before the watchdog
  reset, so the RAM reports reach DRAM as before. Every reset path goes through it.
- **The GPU's own MMU is unchanged.** mbx.c still maps GPU VA 0..32 MB to PA 0x08000000..0x0a000000
  identity, and the CPU map is 1:1, so `MBX_VA(p) = p - 0x08000000` still holds. GPU-visible data
  must stay below 0x0a000000.
- **SCTLR is set explicitly.** mmu_init clears A (alignment faults), TRE (TEX remap) and AFE, so
  the TEX/C/B section bits mean what mmu.c writes, whatever RetailOS left there. Unaligned accesses
  are legal on Normal memory. The kernel is still built `-mno-unaligned-access`, because the
  uncached sections are Strongly-ordered.

### Report mailbox windows (report.h)

The main mailbox is at 0x0ae10000..0x0af01000. The mirrors are at 0x0a0d0000, 0x0a300000,
0x0acb0000, 0x0af60000, 0x0ba00000 and 0x0be60000.

They hold data only from a NanoOS report write until the next RetailOS launch of NanoOSLoader, which
exports and clears them. The game image region (0x0b000000..0x0ba00000) contains none of them, so a
report written by M8 or DevMode while a game is resident never corrupts the game. The game heaps do
contain them: running the game after a failing M8 session, but before exporting, can overwrite R80.

## Large game load: route (a), NanoOSLoader

1. NanoOSLoader, under RetailOS, reads kernel.bin (and the asset pack). It then reads
   `/Apps/NanoOS/game.bin` one 256 KB OS-heap chunk per frame, shown as a KB counter on the BUSY
   screen. No single free block is that large: the RetailOS heap's largest free block was about
   1.9 MB in the RAM snapshots (docs/research/ram-map.md).
2. The loader accepts only chunks in [MM_STAGE, MM_DRAM_END) outside the image region. A misplaced
   block is held, so the heap hands out another one. The loader also avoids hb_fs.c's fixed scratch
   at 0x09118000. The chunk list (`struct nboot_game`, gameabi.h) goes to the chainloader's
   `handoff_to_payload_initrd()`, which stores it in `n7_handoff.initrd_start/initrd_size` at
   0x22009000. That field existed in the ABI and was always 0 before.
3. The SRAM trampoline turns RetailOS's MMU off and copies the kernel and pack to 0x08000000, as
   before. Heap VA equals PA, which the kernel staging buffer already relies on.
4. `kernel_main` calls `mmu_init()` and then `game_boot()` before anything else writes high DRAM.
   game_boot validates the handoff block, the descriptor and the `ngame_hdr`, copies the chunks to
   the header's load address (they can never overlap it), and checks the image CRC-32. On success
   the launcher gains a sixth icon, "GM". With no game the launcher is unchanged, five icons, so
   the existing harnesses are unaffected.
5. Game icon: `game_run()` zeroes the game BSS, cleans the D-cache, invalidates the I-cache and
   branch predictor, and calls `entry(&nanoos_api)`. The game draws through the kernel's LCD driver
   and returns to the launcher. The kernel does not restore the game's .data between launches: the
   reVC image links a load copy of .data inside the image and copies it to its live address after
   the BSS at every entry (games/reVC_nanoos/game.ld).

### Game services table 2 (2026-10-19, Vice City stage 2)

`struct nanoos_api` gained a v2 block at its end:

- `usec` (the 1 MHz counter);
- `dcache_clean`;
- `input_held` and `input_service`;
- the MBX driver: `mbx_init`, `mbx_begin`, `mbx_submit`, `mbx_set_background`, `mbx_render`,
  `mbx_wait_done`.

The v1 prefix is unchanged. The table version (`NGAME_API_VERSION` = 2) is in `api->flags` bits
16..23, read with `NGAME_API_TABLE(api)`; a v1 kernel leaves it 0. `NGAME_ABI` (= 1) is unchanged in
two places:

- **Image format:** header, chunk list and entry. NanoOSLoader checks this value, so every loader,
  including the installed ones, loads both games.
- **`api->abi`:** v1 games test it for equality. The stage-1 stub does, and it still runs.

The reVC game checks for table 2 and on an older kernel shows a message instead. GPU-visible game buffers must lie in `MM_GAME_GPU` or be
`lcd_back()`.

### reVC stage-2 usage (games/reVC_nanoos, memreport.py)

| Region | Used |
|---|---|
| MM_GAME image | 0x0b000000..0x0b080af8: image 246,864 B (code 25.8 KB, NPAK v2 pack 217 KB, .data load copy), then .bss 18 KB, live .data 1 KB, stack 256 KB |
| MM_GAME_GPU | 0x08a00000 + 64 KB: 32 texture slots of 32x32x16 bit (Morton order, as M8) |
| MM_GAME_HEAP | nos_heap 0x0a400000..0x0ac00000 (8 MB): clear of the mailbox (0x0ae10000) and the 0x0a0d0000/0x0a300000/0x0acb0000 mirrors, so a resident game never overwrites a report |
| MM_GAME_HEAP2 | unused |

### Route (b), later

A kernel FAT32 reader, on top of a read-only FTL or a launch-time extent manifest, will fill
`MM_GAME` directly and set the same `ngame_hdr` checks. game_boot's descriptor path is then just
skipped. The image format, ABI, launcher entry and memory map stay the same. Route (b) is required
for the full game: route (a) is bounded by RetailOS's free heap (a few MB in the snapshots) and by
`NBOOT_CHUNKS` (64 chunks of 256 KB = 16 MB).

## Planned split for reVC (targets, not measurements; roadmap /tmp/revc_roadmap.txt)

- Image, 10 MB: code plus read-only tables (about 6 MB), globals, pools and stack.
- Heap and streaming, 16 + 6 MB, CPU-only: world placement, collision, animation, scripts, decoded
  assets, I/O and decompression scratch.
- GPU arena, 22 MB: textures (about 8 MB), display targets and MBX pools (about 2 MB), dynamic render
  assets. Its attribute changes to Normal NC or WB+clean once the driver DSB audit is done.
- Kernel: 8 MB in total, including the uncached DMA region and the RetailOS keep area (reclaimable
  once bt.c copies its blob at boot).
