# NanoOS v0.0.3 beta

Freestanding ARM kernel for the iPod nano 7G, linked at `0x08000000`, with
MMU and D-cache enabled (flat 1:1 map; LCD, GPU test buffers, DMA arena and
all MMIO stay uncached/strongly-ordered). The 432x240 landscape UI rotates
into the native 240x432 LCD scanout at `0x08200000`. Requires the ARM
bare-metal toolchain, Python 3/Pillow, and the sibling NanoApps SDK.

## Drivers and DevMode

Hardware driver implementations cover LCD, touch, buttons, audio, NAND,
USB, Bluetooth HID, and MBX GPU, all verified on the real device:

- **Touch** works on hardware, including after the MMU/D-cache enable, via
  SPI chip-select pacing (setup/hold/high delays) and a query-to-fetch gap
  on the Grape touch controller. The DevMode Drivers page shows live touch
  counters (queries, ATN-low polls, reports, timeouts) for diagnosis.
- **Bluetooth** pairs with a real DualSense gamepad; the pad drives the
  ViceNano scene on the device.
- **Audio** probes the codec and feeds a PCM tone at 44.1 kHz.
- **USB** reads the OTG core ID.
- **NAND** (Apple PPN via the FMC at `0x38a00000`) is in bring-up: on most
  boots the FMC register space is unpowered/unclocked after the RetailOS
  handoff, so the driver now runs a staged recovery (controller reset,
  FMC clock enable, PSC power-domain wake, candidate clock gates) and
  reports which step relatched the block. Refuses flash writes.
- **MBX GPU** renders scenes on hardware; see below.

Drivers are polled with bounded waits. These are development interfaces,
not a complete peripheral stack; see [DRIVER_PORT.md](DRIVER_PORT.md).

Every boot starts with a STOCK/SAFE/MAX clock selector. Nominal CPU/GPU
rates are 342/133, 342/171, and 342/266 MHz respectively. Selection lasts
for this boot only. The profile names do not establish hardware stability.

Volume down/up selects; Home (or both volume buttons together) confirms.
NanoEmu also maps Left/Right and Enter to these controls. The menu contains
StockOS, ViceNano, PS1Emu, Settings, and DevMode. Home returns from a
scene; StockOS requests a reset to RetailOS. DevMode shows clocks, memory,
FPS, and diagnostics; Vol+ opens Drivers, then the MBX GPU test; Vol-
returns to stats.

## ViceNano M8 on hardware

The integrated build links ViceNano M8 into the full NanoOS kernel. On the
real iPod the MBX renders the M8 scenes (clear, triangle, textured quad,
and the VC street-corner world preview) at about 70 fps with the MMU and
D-cache on; a hardware report (`R80.txt`) is written to the iPod disk after
each run. The scene is controllable with the DualSense gamepad or touch.

```sh
python3 nanoos/installer/build_payload.py
python3 nanoos/installer/patch_disk_emu.py --mode all
python3 nanoos/installer/list_device_files.py
```

## Game loader (reVC stage 2)

NanoOS also loads standalone game binaries (NGM1 images) through the
NanoOSLoader RetailOS app: the loader scatters 256 KB chunks through the
RetailOS heap into the game window at `0x08800000`-`0x0c000000`, then the
kernel hands over a versioned services table (clock, cache maintenance,
input, MBX driver). The first real game is a carved reVC (Vice City)
build; see the ViceNano repository. The stage-1 stub and old loaders stay
compatible.

`make -C nanoos` builds the standalone kernel, whose ViceNano scene is a
placeholder. `nanoos/run-in-nanoemu.sh STOCK` (also SAFE/MAX) runs its
emulator regression; it does not test the integrated installer bundle.
NanoEmu needs a built `emulator/build/qemu-system-arm`, snap6 dumps, disk
and resource images. Rebuild emulator changes with `emulator/build.sh`.

See [INSTALL_IPOD.md](INSTALL_IPOD.md) for integrated bundle testing,
device file transfer, and rollback. Emulator validation is continuous;
hardware validation covers the drivers listed above. Clock stability beyond
stock profiles and NAND data reads remain under active bring-up.

NanoOS by Tenspav. Requires the community untethered jailbreak to install.
