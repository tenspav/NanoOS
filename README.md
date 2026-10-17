# NanoOS v0.0.2 beta

Freestanding ARM kernel for iPod nano 7G, linked at `0x08000000`, with MMU
and caches disabled. The 432x240 landscape UI rotates into the native
240x432 LCD scanout at `0x08200000`. Requires the ARM bare-metal toolchain,
Python 3/Pillow, and the sibling NanoApps SDK.

## Drivers and DevMode

Hardware driver implementations cover LCD, touch, buttons, audio, NAND,
USB, Bluetooth HID, and MBX GPU. Drivers are polled with bounded waits.
Audio provides a codec probe and CPU-fed tone; NAND supports raw reads and
refuses writes. USB is probe-only (NanoEmu has no DWC2 model); Bluetooth
currently targets the emulator's known HID peer, without general pairing.
MBX provides scene submission and GPU test support. These are development
interfaces, not a complete peripheral stack; see [DRIVER_PORT.md](DRIVER_PORT.md).

Every boot starts with a STOCK/SAFE/MAX clock selector. Nominal CPU/GPU
rates are 342/133, 342/171, and 342/266 MHz respectively. Confirmation
changes only the GPU rate fields at `0x3C50001C`, preserving gates and the
memory domain; CPU clocks remain stock. Selection lasts for this boot only.
The profile names do not establish hardware stability.

Volume down/up selects; Home (or both volume buttons together) confirms.
NanoEmu also maps Left/Right and Enter to these controls. The five-entry
menu contains StockOS, ViceNano, PS1Emu, Settings, and DevMode. Home returns
from a scene; StockOS requests a reset to RetailOS. DevMode shows clocks,
memory, FPS, and diagnostics; Vol+ opens Drivers, then the MBX GPU test;
Vol- returns to stats.

## Integrated ViceNano M8 build

From the repository root:

```sh
python3 nanoos/installer/build_payload.py
python3 nanoos/installer/patch_disk_emu.py --mode all
python3 nanoos/installer/list_device_files.py
```

The installer links ViceNano M8 into the full NanoOS kernel and stages the
loader, kernel, and converted assets under `nanoos/installer/dist/Apps/`.
Select **ViceNano** in the NanoOS menu to enter M8 (title, MBX triangle,
and world preview); Home exits to NanoOS. PS1Emu and Settings remain
placeholders. This is not a complete game port.

`make -C nanoos` builds the standalone kernel, whose ViceNano scene is a
placeholder. `nanoos/run-in-nanoemu.sh STOCK` (also SAFE/MAX) runs its
emulator regression; it does not test the integrated installer bundle.
NanoEmu needs a built `emulator/build/qemu-system-arm`, snap6 dumps, disk
and resource images. Rebuild emulator changes with `emulator/build.sh`.

See [INSTALL_IPOD.md](INSTALL_IPOD.md) for integrated bundle testing,
device file transfer, and rollback. Validation described here is emulator
validation only. Real hardware operation, driver behavior, clock stability,
and reset behavior remain unverified.
