# Installing NanoOS + ViceNano M8 on iPod nano 7G

NanoOS v0.0.2 beta is experimental; real hardware operation is unverified.
This guide describes a manual install on an already jailbroken nano 7G
with the NanoApps resident installed. It does not install the jailbreak.
Run host commands from the NanoRework repository root. Device commands
below are instructions only; no physical device is used to prepare or test
this documentation.

## 1. Prepare dependencies and backups

Have `arm-none-eabi-gcc`, `arm-none-eabi-g++`, binutils, make, Python 3,
Pillow, and NanoApps build dependencies available. The installer expects
sibling `../NanoApps` and `../ViceNano/m8` trees; override these with
`NANOAPPS` and `VICENANO_M8`. ViceNano needs its generated `.nantex`/`.dff`
assets in `m8/generated/`.

Before changing device files, retain a known-good disk backup and a complete
copy of its current `/Apps` tree outside the iPod, including both
`AllApps.pack` and `AllApps-B.pack` if present. Record which NanoOS files
already exist. Use your established jailbreak backup/mount procedure;
mount points below are examples, not device identifiers.

## 2. Build the combined payload

```sh
python3 nanoos/installer/build_payload.py
python3 nanoos/installer/list_device_files.py
(cd nanoos/installer/dist && sha256sum -c Apps/NanoOS/SHA256SUMS)
```

The staging tree `nanoos/installer/dist/Apps/` contains:

- `NanoOS/kernel.bin`: full kernel, drivers, DevMode, integrated ViceNano M8.
- `Executables/NanoOS.hbapp`: device loader reading `/Apps/NanoOS/kernel.bin`.
- `NanoOS/assets/vicenano/`: converted assets and aligned `vicenano.pak`.
- `NanoOS/assets/vicenano/vcworld.pak`: runtime pack. The loader appends it to
  `kernel.bin` at boot, and ViceNano's mode 4 (Vice City North Beach street
  corner) draws from it. If it is missing, the kernel still boots and mode 4
  shows "no VC pack: preview".
- `NanoOS/MANIFEST.json` and `NanoOS/SHA256SUMS`: metadata and checksums.
- `Icons/NanoOS.bin`, only if the installer's expected backup icon exists.

Check the printed manifest rather than assuming the icon was produced.
The installer does not generate or update the RetailOS app registration
packs. Files under `dist/emu/` are for NanoEmu, not device installation.

## 3. Prepare a disk-image patch and test in NanoEmu

Build NanoEmu with `./emulator/build.sh` if needed. Supply the existing
snap6 RAM/IRAM/overlay/torn-map dumps, fixups, resource image, and a compatible
FAT32 disk dump. Then run:

```sh
python3 nanoos/installer/patch_disk_emu.py --disk dumps/ipod-disk.dd --mode all
```

This creates `dist/emu/emu-{chainloader,loader,combined}.patch` under the
installer directory. These are `N7PATCH1` overlays; the source disk image
is read-only and remains unchanged. The patch tool can replace existing
files only and cannot grow their slots. An incompatible dump or oversized
payload must be resolved before proceeding.

The combined patch puts the slim embedded chainloader in **Blocks**, and
the full NanoOSLoader in **Game Boy**. The full kernel occupies
`/Apps/Data/Probe/vrom0.bin`; the slim kernel also overlays
`/Apps/Data/Blocks/world.bin`. These paths are emulator slot substitutions.

Launch with the command printed by the patch script, or use:

```sh
./emulator/run.sh \
  --ramfull dumps/snap6/ram-full.bin --iram dumps/snap6/iram.bin \
  --ctx emulator/fixups/snap6-ctx.bin --overlay dumps/snap6/overlay.bin \
  --os-poke-file emulator/fixups/snap6-resume.poke \
  --torn-map dumps/snap6/torn-map.bin \
  --nand-miss oracle --nand-disk dumps/ipod-disk.dd --nand-disk-offset 49216 \
  --nand-rsrc emulator/rsrc/rsrc-1.1.2.img1 \
  --nand-patch nanoos/installer/dist/emu/emu-combined.patch \
  -M nano7,mmu=0x08b28000,torn-ignore-pc=0x093d2a80:0x093d4100
```

Open Game Boy to exercise the full loader's disk-read/handoff path. In its
launcher, select ViceNano and press Home to boot NanoOS. Confirm STOCK at
the kernel clock selector, select ViceNano again, and check M8's title and
available GPU/world previews. Verify Home returns to NanoOS, DevMode pages
open, and StockOS returns to RetailOS. Blocks tests the slim direct path,
which omits the Drivers page. Test SAFE/MAX separately in the emulator;
emulator success does not establish hardware stability.

## 4. Register a fresh NanoOS menu entry

Skip this step if NanoOS is already registered. Use the existing NanoApps
packaging workflow documented in
[the loader install guide](../apps/NanoOSLoader/INSTALL.md):

```sh
./apps/NanoOSLoader/package.sh
```

This stages a registered loader in `/tmp/hbapps/Apps/`, but also builds a
standalone kernel. Use the integrated kernel from step 2 for the actual
transfer. Review the generated app pack with your NanoApps workflow so it
preserves existing apps; do not blindly replace a populated device's packs
with a single-app pack. Keep the resident's active pack and fallback pack
consistent with that workflow. Preserve or generate the NanoOS icon here
if step 2 could not supply one. The installer scripts alone cannot register
a fresh Home-screen entry.

## 5. Transfer the files

Return to RetailOS and mount the device FAT32 volume using your established
procedure. Confirm `/mnt/ipod` is the intended mounted iPod volume and that
the backup from step 1 is complete. For a fresh install, first deploy the
reviewed registration pack(s), loader, and icon through the NanoApps
workflow from step 4. Then copy the integrated bundle over it:

```sh
sudo mkdir -p /mnt/ipod/Apps
sudo cp -r nanoos/installer/dist/Apps/. /mnt/ipod/Apps/
python3 nanoos/installer/list_device_files.py --check-dir /mnt/ipod/Apps
(cd /mnt/ipod && sha256sum -c Apps/NanoOS/SHA256SUMS)
sync
sudo umount /mnt/ipod
```

`--check-dir` compares files and reports registration read-only; it does not
install anything. Check every expected file matches and NanoOS is registered
before unmounting. The copy merges the staged tree and does not register the
app. Transfer uses RetailOS/jailbreak disk access: NanoOS's USB probe is not
a file-transfer service. Never write an emulator `.patch` to the iPod or
replace device apps with emulator slot builds.

After safe eject, open NanoOS on the RetailOS Home screen. Select ViceNano
in the loader and press Home to hand off. Confirm STOCK, then launch
ViceNano from the NanoOS menu. Home (or holding Vol- + Vol+) returns
from M8; a GPU failure reboots into RetailOS on its own. Hardware behavior and
recovery through the StockOS entry remain unverified.

### Pulling the GPU report (R80) without the loader

ViceNano keeps its GPU diag in the RAM mailbox at 0x0ae10000
(`src/report.h`). Round 12: that address is a free block of a RetailOS
allocator, so it survives the reset back into RetailOS only if RetailOS
has not allocated over it yet. NanoOS therefore also writes six small R80
mirrors in other regions (`rpt_mirror_base` in report.h).
`installer/pull_reports.py` reads all of them over NanoApps' SCSI peek (the
same channel as `start peek`), which works on the Home screen:

1. ViceNano fails. Do nothing. The red screen shows "RSN n", the code in
   words, a yellow probe line (TA never/late, id, 4c/6c, gap), and
   "R80 saved - REBOOT IN n". About 5 s later NanoOS stops the GPU, waits
   0.5 s and warm-resets into RetailOS by itself. If frames keep
   half-failing, the reboot comes 30 s after the first failure. Photograph
   the red screen. Never hold Power to switch off: that wipes the report.
2. At the RetailOS Home screen, plug the cable into the Linux machine.
3. Run `python3 nanoos/installer/pull_reports.py`. sudo may ask for a
   password. It first peeks RetailOS kernel text: "PEEK CHANNEL DEAD"
   means the transport failed (sudo, device), not the report. It then
   prints the raw header words and the mirror survival map.
4. If it still finds no report, unplug and open the NanoOS app: the loader
   exports the main mailbox or, failing that, the first intact mirror to
   `/Apps/NanoOS/reports/R80.txt`. In NanoOS, hold Vol- + Vol+ twice
   (clock profile, then StockOS) to get back to RetailOS.

The script writes `nanoos/reports/R80.txt` (plus R50..R53 if present) on
the host and prints R80. A different older copy is moved to `R80.prev.txt`.
A checksum mismatch goes to `R80.bad.txt`. The script writes nothing to the
iPod. `--remote user@host` works as it does for `start`.

## 6. Revert

For NanoEmu, stop the emulator and relaunch without `--nand-patch`; the
original disk image needs no restoration.

For a device update, return to RetailOS (use the device's normal forced
restart procedure if NanoOS hangs), remount the FAT volume, and restore
the backed-up NanoOS kernel, loader, icon, assets, and any changed app
packs. For a fresh install, restore the pre-install registration pack(s)
and remove only the newly added `Apps/NanoOS/`,
`Apps/Executables/NanoOS.hbapp`, and `Apps/Icons/NanoOS.bin` after confirming
they were absent from the backup. Do not remove pre-existing files or the
NanoApps resident. Sync, unmount, and restart into RetailOS. If normal disk
access is unavailable, use your established jailbreak recovery procedure
and known-good backup; these installer scripts provide no recovery flasher.
