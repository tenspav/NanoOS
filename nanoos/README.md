# NanoOS v0.0.0

Build the freestanding ARM kernel with `make -C nanoos`. Output:
`nanoos/build/kernel.bin`, linked at physical `0x08000000`. Requires
`arm-none-eabi-gcc` and binutils. No libc, OS services, or dynamic allocation.
The MMU and caches stay disabled. The kernel uses a private ARGB framebuffer
at `0x08200000`; the linker reserves the preceding 2 MiB for code/BSS/stack.

Run `nanoos/run-in-nanoemu.sh` from any directory for the automated emulator
boot and UI regression (optional argument: `STOCK`, `SAFE`, or `MAX`;
default `STOCK`). It builds a relocatable **chainloader application**
embedding this kernel, overlays it onto the snapshot's Blocks slot, boots
RetailOS, navigates to Blocks, and exercises the launcher through the existing
shutdown/SRAM handoff path. It never changes the disk image or uses a device.
Probe's original sources and build products remain untouched.

Prerequisites: built `emulator/build/qemu-system-arm`, snap6 dumps, disk and
resource image used by `probe/chainloader/test_roundtrip.py`, Python 3/Pillow,
and NanoApps SDK/tools at `../../NanoApps` (override `NANOAPPS` for make).
After changing emulator sources, rebuild NanoEmu with `emulator/build.sh`.
The emulator PMU key handler includes Left -> volume down, Right -> volume up,
and Enter -> Home aliases. Existing Up/Down and h/Home mappings still work.

Screenshots, emulator log, raw kernel, and chainloader package are under
`nanoos/build/`. Captures are grouped in `build/screens/{stock,safe,max}/`,
with logs in `build/nanoemu-{stock,safe,max}.log`. `test_nanoemu.py` drives QMP headlessly and exits after checks.
`build/chainloader.hbapp` is the wrapper used for this test; a separate
`kernel.hbapp` direct-launch package is not produced.

## Spec clarification and limits

The launcher draws a 432x240 landscape frame with four evenly spaced circles.
No rotation control is confirmed by the existing LCD register definitions, so
`lcd_present()` rotates the logical framebuffer clockwise into the native
240x432 scanout buffer. Panel timing, stride, and window geometry stay native.
The logical buffer lives in kernel BSS; scanout remains at `0x08200000`.
Turn the panel counterclockwise to view the launcher upright. Screenshots
include raw panel captures and upright `*_landscape.png` views for NanoOS.

On every kernel boot, a landscape clock selector defaults to STOCK. Volume
buttons / Left-Right cycle STOCK, SAFE, MAX; Home / Enter confirms. Only
confirmation writes CPU `0x3C500000` and GPU `0x3C50001C`, using the spec
register values. The active profile appears beside the launcher title.
Selection is held in RAM for this boot; it is not persisted.

The built-in fixed 8x8 bitmap cells cover every character used by this UI.
Home uses PMU reg7's active-low level, acknowledges the latched GPIO 0x56
PMU event line, and detects press edges. Volume uses active-low GPIO 0x28/0x29.
Polling I2C transactions have bounded waits. StockOS issues the existing
watchdog/PMGR reset sequence; in NanoEmu this reloads the RetailOS boot state.

Verified in NanoEmu: clock selector navigation/wraparound, all three CPU/GPU
register pairs, chainloader boot, launcher rendering, both navigation
directions/wraparound, all placeholder scenes, Enter/Home confirmation,
repeated Home returns, and reset back to RetailOS. Real hardware display/input
and reset behavior are unverified. No physical device was accessed.
ViceNano, PS1Emu, and Settings are placeholders, as specified; no games load.
