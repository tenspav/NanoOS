# NanoOS Loader

A RetailOS homebrew app (NanoApps SDK, raw surface) that shows the NanoOS
launcher from `docs/nanoos-launcher-spec.md` and boots the bare-metal NanoOS
kernel. It reads the kernel from `/Apps/NanoOS/kernel.bin`, then calls
`probe/chainloader`'s `hw_shutdown_all()` and `handoff_to_payload()`: SVC hook,
MMU and caches off, kernel copied to 0x08000000, jump. Nothing on NAND is
modified, and a reset brings RetailOS back.

## Controls

| Input | Action |
|---|---|
| Volume Down / Up | move the selection left / right (wraps around) |
| Home on StockOS | RetailOS's own Home handling returns to the Home screen |
| Home on ViceNano / PS1Emu / Settings | boot NanoOS |
| tap an icon, then tap it again | same as above (best effort, see the limits) |

The launcher has the same layout, colours and font as NanoOS (landscape,
rotated like `nanoos/src/lcd.c`), plus a hint line at the bottom. The boot shows
`BUSY...` the moment Home is seen, then the loaded byte count and a progress
bar. The last "handing off" frame stays on the panel until NanoOS draws.

## Build

```
make -C apps/NanoOSLoader            # -> build/NanoOSLoader.hbapp (device)
make -C nanoos                       # -> nanoos/build/kernel.bin
```

This needs `arm-none-eabi-gcc` and the NanoApps SDK at `../../../NanoApps`
(override with `NANOAPPS=`). The app links in
`probe/chainloader/{shutdown.c,handoff.S}` and `nanoos/src/font.c` from their
own trees, so it always matches them.

Build options:
- `KERNEL_PATH=/other/path` loads the kernel from a different path.
- `BUILD=dir` keeps a build variant in its own directory.
- `LOADER_DEFS=...` passes extra `-D` flags.

`handoff.S` embeds an (empty, generated) `payload.bin` as the chainloader
fallback. The loader never uses it.

## Install on a real iPod

See [`INSTALL.md`](INSTALL.md) for the exact build/package/install steps.

Quick path:

```sh
./package.sh              # stages /tmp/hbapps with app + kernel
# copy /tmp/hbapps/Apps to the mounted iPod, or use NanoApps' ./start install NanoOSLoader
```

The loader reads the kernel from `/Apps/NanoOS/kernel.bin`; `package.sh` places it
there in the staging tree. `Info.plist` registers the app as **HBAppKind 1**
(custom screen). Registered as kind 2 (GL) the entry gets no framebuffer and the
loader draws nothing.

## Emulator test (no device)

```
python3 apps/NanoOSLoader/test_nanoemu.py             # full path, ~2 min
python3 apps/NanoOSLoader/test_nanoemu.py --missing   # device build, no kernel
```

The test resumes snap6 the same way as `probe/chainloader/test_roundtrip.py`
and `nanoos/test_nanoemu.py`. Nothing under `dumps/` is written. Overlays go
through a `--nand-patch` file:

- the loader goes in the **Game Boy** slot, because Blocks is a GL (kind 2)
  slot;
- the dumped disk has no `/Apps/NanoOS`, and `disk_patch.py` can only overlay
  existing files. So the emulator build (`build-emu/`) reads
  `/Apps/Data/Blocks/world.bin`, with `kernel.bin` laid over it (zero-padded
  to 24608 bytes).

What the test checks:
1. The loader launcher is drawn.
2. Volume navigation works in both directions and wraps.
3. Home on StockOS leaves the app for the RetailOS Home screen, with no reset.
4. After a relaunch, ViceNano + Home starts the boot. The `BUSY...` frame is
   captured when the screen polling catches it: it is on screen for only a few
   frames, and the report says whether it was caught.
5. NanoOS then draws its own frame.
6. The handoff proof: the 4 Hz PC sampler's last 4 samples all fall inside the
   loaded kernel image (0x08000000 + size) in SVC mode, with no reset in
   between.
7. If NanoOS shows the v0.0.1 beta launcher, the test also checks it pixel for pixel
   against the loader's (above the hint line). It then walks NanoOS'
   placeholder, StockOS and reset back to RetailOS. `nanoos/` is under active
   development, and with any other first screen this step is skipped and the
   skip is reported.

RetailOS navigation waits on screen state (page dots, stable frames), not on
fixed sleeps. Every key press is confirmed in the PMU log. Under host load,
QEMU sometimes never delivers a `sendkey` to the machine; such a press is
resent and counted in the final report. A press that is delivered but ignored
by the app still fails the test.

`--missing` checks that the error screen appears and RetailOS carries on
without a handoff or reset. The test uses a private `state-dir` and
`screens-dir`: the shared `emulator/state` holds the boot state a guest reset
reloads, and other NanoEmu runs use it too. Artifacts go to
`build-emu/{screens,screens-missing,nanoemu*.log}`.

## Limits (found in NanoEmu)

- **Home is RetailOS's.** RetailOS also acts on the Home press that starts a
  boot and closes the app a moment later. The boot therefore hands off within
  a few frames of the press, and so far it has always won in the emulator. If
  the boot fails (no kernel), the error screen shows until that close.
- **Play/Pause is not used.** RetailOS starts music and opens Now Playing on
  it.
- **RetailOS draws its volume overlay** and changes the volume on Vol -/+. The
  app samples buttons once per frame (the SDK reads the GPIO level), and RetailOS
  can stall the app's frame callback for 100-400 ms. That happens especially on
  the first volume press after launch, while it builds the overlay. A short
  press inside a stall is missed, so press a bit longer. The test sends one
  warm-up press, then requires every later press to register.
- **Taps:** the SDK surface touch read saw no taps in NanoEmu with the snap6
  disk's resident (the OS touch list is drained first, and there is no touch
  mailbox), so the buttons are the supported input. RetailOS gives apps no
  exit call, so a tapped StockOS shows "press Home".
- **Not verified on hardware.** As with the chainloader itself: the shutdown
  runs from user mode (MMIO writes land, but CPSID is a no-op until the
  trampoline), and the OS-heap staging buffer is assumed identity-mapped,
  like all app memory.
