# NanoOS Loader Verification Report

**Date:** 2026-10-15
**Tester:** automated subagent
**Status:** WORKS — the loader loads `kernel.bin` and successfully hands off to the bare-metal NanoOS kernel in NanoEmu.

## What was tested

- App: `apps/NanoOSLoader/`
- Kernel: `nanoos/build/kernel.bin`
- Test script: `apps/NanoOSLoader/test_nanoemu.py`
- Emulator: NanoEmu (snap6 resume, Game Boy slot replaced with the loader, kernel overlaid on `/Apps/Data/Blocks/world.bin`)

## Test run

```sh
python3 apps/NanoOSLoader/test_nanoemu.py
```

Result: **PASS** (wall time ~43 s).

Key output:

```
  03_loader
Volume navigation (wraps both ways)...
  warm-up press registered
  ...
  14_busy
  BUSY frame captured
  15_nanoos_after_handoff
  PC sampler: last 4 samples in NanoOS (0x08000000..0x08000f8c, SVC): ['0x800067c', '0x800072c', '0x80006bc', '0x8000768']
PASS: loader launcher, volume nav, Home->RetailOS, ViceNano+Home -> BUSY -> kernel load -> shutdown + SRAM handoff -> NanoOS running
```

## Evidence

- Screenshots saved in `apps/NanoOSLoader/build-emu/screens/`:
  - `03_loader.png` / `03_loader_landscape.png` — loader launcher visible.
  - `14_busy.png` / `14_busy_landscape.png` — BUSY/progress frame captured.
  - `15_nanoos_after_handoff.png` / `15_nanoos_after_handoff_landscape.png` — NanoOS's first frame after handoff.
- NanoEmu log: `apps/NanoOSLoader/build-emu/nanoemu.log`.
- The 4 Hz PC sampler confirmed the CPU was executing inside the loaded kernel image (`0x08000000..0x08000f8c`) in SVC mode immediately after the handoff.

## Loader handoff path (`loader.c`)

1. On launch, `hb_raw_init()` opens the raw framebuffer and draws the launcher.
2. When the user selects an icon other than StockOS and presses Home, `launch()` sets state `BOOT_READ` and draws `BUSY...`.
3. `boot_read()` calls:
   - `hb_fs_size(NANOOS_KERNEL_PATH)` to get the kernel size.
   - `hb_os_alloc(n)` to allocate an OS-heap staging buffer.
   - `hb_fs_read(NANOOS_KERNEL_PATH, kernel, n)` to read the whole kernel into DRAM.
4. After `SETTLE_FRAMES` frames (4 frames) to let the filesystem/FMC idle, state reaches `BOOT_HANDOFF`.
5. `hw_shutdown_all()` stops display pushes, FMC/NAND, DMA, timers, and masks the VIC.
6. `handoff_to_payload(kernel, kernel_len)` (`probe/chainloader/handoff.S`) takes over:
   - copies a position-independent trampoline into SRAM at `0x22000000`;
   - copies a warm-reset stub to `0x22008100`;
   - writes the handoff ABI block (`struct n7_handoff`) to `0x22009000`;
   - escalates to SVC mode via the RetailOS hook at `0x08917714`;
   - disables interrupts, cleans/invalidates caches, turns off MMU/caches/branch prediction;
   - copies the payload from the staging buffer to physical DRAM `0x08000000`;
   - sets boot registers (`r0=0`, `r1=0x1097`, `r2=0x22009000`, `sp=0x22010000`, `lr=0x22008100`);
   - branches to `0x08000000`.

## Notes / limitations observed

- The loader does **not** show the NanoOS v0.0.1 beta launcher spec screen in this build: NanoOS currently boots to a different first screen, so the test script skipped the NanoOS-side StockOS/reset round trip and reported:
  ```
  NanoOS shows a screen other than the v0.0.1 beta launcher (nanoos/ changed); NanoOS-side StockOS round trip skipped
  ```
  This is expected per `README.md` when `nanoos/` changes.
- The handoff itself is fully verified: the CPU is running the loaded kernel at `0x08000000` in SVC mode with no reset.
- No errors, no resets during the handoff, and no QEMU keypresses were lost in this run.

## Files referenced

- `NanoRework/apps/NanoOSLoader/loader.c`
- `NanoRework/apps/NanoOSLoader/test_nanoemu.py`
- `NanoRework/probe/chainloader/handoff.S`
- `NanoRework/probe/chainloader/handoff.h`
- `NanoRework/probe/chainloader/shutdown.h`
- `NanoRework/apps/NanoOSLoader/build-emu/screens/`
- `NanoRework/apps/NanoOSLoader/build-emu/nanoemu.log`
