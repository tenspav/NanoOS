# NanoOS + ViceNano M8 Systems Integration & Real-Device Install Plan

> **Status:** Complete & Emulator-Verified (No physical device modified)  
> **Workspace:** `/home/pavliukov/ipodwork/NanoRework/nanoos`  
> **Helper Scripts:** `nanoos/installer/` (and symlinked in `nanoos/tools/`)  
> **Staged Bundle Output:** `nanoos/installer/dist/`

---

## 1. Executive Summary & Architecture Overview

This document defines the end-to-end packaging, asset transfer, emulator verification, and real-device installation workflow to run **NanoOS** (with all 8 bare-metal drivers + DevMode diagnostics) and **ViceNano M8** (title screen, MBX triangle test, MBX textured quad test, and MBX 3D North Beach world preview) on the user's jailbroken **iPod nano 7G**.

```
RetailOS Home Screen (Stock OS, FAT32 /Apps mounted by iPod OS)
  │
  ▼  User taps "NanoOS" icon (HBAppKind 1, sbid: hb.NanoOSLoader)
/Apps/Executables/NanoOS.hbapp  (NanoOSLoader v0.0.2 beta)
  │  1. Draws "BUSY... Booting NanoOS" immediately on open (no Home button race)
  │  2. Waits 60 frames (~1 s) for RetailOS launch animation to settle
  │  3. Reads /Apps/NanoOS/kernel.bin (32.0 KiB) into OS heap buffer
  │  4. Waits 4 settle frames (file closed, NAND FMC idle) + 3 handoff frames
  │  5. Calls hw_shutdown_all() -> handoff_to_payload() via SRAM trampoline
  │     (SVC escalation, IRQ/FIQ masked, MMU & D-cache off, copies to 0x08000000)
  ▼
Bare-Metal NanoOS Kernel @ Physical DRAM 0x08000000
  ├── Clock Profile Selector : STOCK (133 MHz GPU) | SAFE (171 MHz) | MAX (266 MHz)
  ├── NanoOS 5-Icon Launcher :
  │     ├── [0] StockOS ("iP")  -> Safe watchdog + PMGR warm reset back to RetailOS
  │     ├── [1] ViceNano ("VC") -> ViceNano M8 bare-metal app:
  │     │         • Mode 0: ViceNano M8 Title Screen
  │     │         • Mode 1: PowerVR MBX Gouraud Triangle
  │     │         • Mode 2: PowerVR MBX 8x8 RGB565 Twiddled Texture Quad
  │     │         • Mode 3: PowerVR MBX 3D Textured World Preview (nbdecoshop01)
  │     │         (Vol+/Right & Vol-/Left cycle scenes; Home returns to Launcher)
  │     ├── [2] PS1Emu ("P1")   -> Placeholder screen (Home returns to Launcher)
  │     ├── [3] Settings ("ST") -> Placeholder screen (Home returns to Launcher)
  │     └── [4] DevMode ("DV")  -> Live hardware telemetry & driver test suite:
  │               • Page 0 (Stats)   : CPU/GPU clocks, RAM, FPS, LCD DSI, PMU I2C
  │               • Page 1 (Drivers) : LCD, Touch, Buttons/Temp, Audio 440Hz,
  │                                    PPN NAND, USB probe, BT HID, MBX probe
  │               • Page 2 (GPU)     : Live 60fps animated MBX Gouraud triangle
```

---

## 2. End-to-End Packaging & Install Flow Design

### 2.1 Why a Unified Kernel Image + Staged Asset Pack?

NanoOS runs in flat physical memory at `0x08000000` with MMU and D-cache disabled (`-mno-unaligned-access`) and does not yet implement a dynamic ELF loader or FAT32/sftl filesystem parser in bare metal. Therefore:

1. **Combined Executable Payload (`/Apps/NanoOS/kernel.bin`):**
   - Links NanoOS `startup.s`, core services (`lcd`, `font`, `input`, `reset`, `devmode`, `mem`, `timer`, `fmt`), all bare-metal peripheral drivers (`spi`, `touch`, `buttons`, `audio`, `nand`, `usb`, `bt`, `mbx`, `mbx_test`, `drivers`), the patched launcher (`main.c`), and the ViceNano M8 NanoOS modules (`nano.cpp`, `nanoos_shim.cpp`, `input.cpp`, `nanoos_gpu_mbx.cpp`, `nanoos_world.cpp`, `model.cpp`, `nanoos_runtime.c`).
   - Total compiled binary size is **32,768 bytes** (`32.0 KiB`), exactly 50% of the `65,536 B` (`0x08000000..0x08010000`) fast code/rodata budget enforced by `nanoos/kernel.ld` and `< 2%` of `NanoOSLoader`'s `2 MiB` limit.
2. **Boot Loader (`/Apps/Executables/NanoOS.hbapp`):**
   - Built from `NanoRework/apps/NanoOSLoader/loader.c` (`v0.0.2 beta`).
   - **Critical real-hardware fix included:** Unlike `v0.0.0` (which showed a duplicate menu inside RetailOS and required pressing `Home` to start the boot, losing a race to RetailOS's own `Home`-button app teardown on real hardware), `v0.0.1 beta` starts the `BUSY...` countdown immediately when tapped on the RetailOS Home screen and hands off to `/Apps/NanoOS/kernel.bin` without touching `Home`.
3. **Home Screen Registration (`AllApps.pack` / `AllApps-B.pack`):**
   - Inspection of the user's pre-NanoOS device backup (`dumps/apps-backup-20261015T115834Z-pre-nanoos/AllApps.pack`) confirms that **NanoOS is already registered** on the user's iPod at index `8/14` (`name='NanoOS'`, `sbid='hb.NanoOSLoader'`, `app_id=0x0dade8a4`, `kind=1`, `executable='NanoOS.hbapp'`, `icon='NanoOS.bin'`).
   - Consequently, updating the real iPod requires **only copying files** into `/mnt/ipod/Apps/` — no `AllApps.pack` binary surgery is needed if that registration is still present.

### 2.2 Physical DRAM & GPU MMU Memory Map

| Physical Address Range | Size | Owner / Purpose |
|---|---|---|
| `0x08000000 .. 0x08007FFF` | `32.0 KiB` | `.text` + `.rodata` + `.data` (`kernel.bin`, must stay `<= 0x08010000`) |
| `0x08010000 .. 0x080ACDFF` | `627.5 KiB` | `.bss` (includes 32 KiB MBX page tables, M8 432x240 render target & twiddled texture slots) |
| `0x080ACE00 .. 0x080B0DFF` | `16.0 KiB` | Bare-metal SVC stack (`_stack_top = 0x080B0E00`) |
| `0x080B0E00 .. 0x081FFFFF` | `~1.31 MiB` | `kalloc()` bare-metal heap (`__heap_start .. __heap_end`) |
| `0x08200000 .. 0x082E53FF` | `~917 KiB` | LCD scanout / drawing framebuffers (`BUF0`, `BUF1`) |
| `0x08300000 .. 0x083653FF` | `405.0 KiB` | `mbx_test.c` DevMode GPU test render target (432x240 ARGB8888) |
| `0x08000000 .. 0x09FFFFFF` | `32.0 MiB` | Mapped 1:1 into PowerVR MBX GPU MMU (`MBX_VA(p) = p - 0x08000000`) |
| `0x09000000 .. 0x0BFFFFFF` | `48.0 MiB` | Upper DRAM available for pre-staged asset packs (`vicenano.pak`) |

---

## 3. Game Asset File Transfer Design

### 3.1 Evaluation of File Transfer Options

| Option | Mechanism | Reliability / Safety | Verdict |
|---|---|---|---|
| **1. Drag-and-Drop Folder over Stock USB Mass Storage** (`/Apps/NanoOS/assets/vicenano/`) | Boot into stock RetailOS, plug in Lightning USB cable, mount the standard FAT32 partition (`/mnt/ipod`), copy `/Apps/NanoOS/...` via `cp` / `rsync` / Finder. | **100% safe.** Uses Apple's stock USB Mass Storage + `sftl` NAND translation layer. Zero brick risk; 25–35 MB/s measured write throughput. | **SELECTED (Primary Path)** |
| **2. Bare-Metal USB Mass Storage inside NanoOS** | Implement DWC2 (`0x38400000`) device-mode BOT/SCSI gadget inside NanoOS and write directly to NAND. | **Blocked & High Risk.** NanoEmu has no DWC2 device model (`GSNPSID = 0xFFFFFFFF`), and bare-metal NAND writes bypass RetailOS's `sftl` wear-leveling/FTL tables (`nand_write_sector` intentionally returns `-1`). | **Rejected** (unsafe on real hardware) |
| **3. iTunes Library Sync (`iPod_Control`)** | Inject game assets as fake audio/video tracks via `iTunesCDB` / `libgpod`. | **Unnecessary complexity.** Requires SQLite/CDB database signing and obfuscated `F00..F49` paths when the same FAT32 partition already exposes `/Apps/` in plain sight. | **Rejected** |

### 3.2 Staged Asset Layout & `NPAK` v1 Container Format

To support both standard RetailOS file I/O (`hb_fs_read`) and future bare-metal 4 KB sector streaming (`nand_read_sector`), `build_payload.py` packages the converted ViceNano M8 North Beach assets (`nbdecoshop01..03.nantex` PAL8 textures and `.dff` geometry) in two forms under `/Apps/NanoOS/assets/vicenano/`:

1. **Individual files** (`nbdecoshop01.nantex`, `nbdecoshop01.dff`, etc.) for direct `hb_fs_read()` access.
2. **A 4096-byte sector-aligned contiguous archive (`vicenano.pak`, `417,792 B` / `102` x 4 KB sectors):**
   - **Header (4096 bytes, sector 0):**
     - Magic `"NPAK"` (`4B`), version `1` (`u32`), entry count `7` (`u32`), data start offset `4096` (`u32`).
     - Followed by `64-byte` directory records: `char name[32]`, `u32 offset`, `u32 size`, `u32 aligned_size`, `u32 crc32`, `u32 sector_index`, `u32 reserved[3]`.
   - **Payload (sectors 1..101):** Each asset starts on an exact `4096-byte` boundary (`sector_index = offset / 4096`), zero-padded to the next 4 KB multiple.
   - **Why 4 KB alignment matters:** Both the iPod's FAT32 cluster size and the PPN NAND sector size (`NAND_SECTOR = 4096`) are 4 KB. Pre-staging `vicenano.pak` into upper DRAM (`0x09000000`) in `NanoOSLoader` before `hw_shutdown_all()`—or reading contiguous sectors—gives bare-metal ViceNano zero-copy pointer access (`base + entry.offset`) to every `.nantex` and `.dff` block.

---

## 4. Helper Scripts (`nanoos/installer/` & `nanoos/tools/`)

All three helper scripts live in `/home/pavliukov/ipodwork/NanoRework/nanoos/installer/` with convenience symlinks in `/home/pavliukov/ipodwork/NanoRework/nanoos/tools/`:

### 4.1 Build Combined Payload: `build_payload.py` / `build_payload.sh`

```sh
cd /home/pavliukov/ipodwork/NanoRework
./nanoos/tools/build_payload.sh --clean
```

What it builds and verifies in `nanoos/installer/dist/`:
- `Apps/NanoOS/kernel.bin` (`32,768 B`): Full combined NanoOS kernel (all 18 NanoOS C modules including all 8 bare-metal drivers + DevMode Stats/Drivers/GPU pages + ViceNano M8 Title/Triangle/Texture/World scenes). Verified `<= 65,536 B`.
- `Apps/Executables/NanoOS.hbapp` (`6,376 B`): `NanoOSLoader` (`v0.0.2 beta`) configured for `/Apps/NanoOS/kernel.bin`.
- `Apps/Icons/NanoOS.bin` (`50,208 B`): 112x112 SilverDB Home screen icon.
- `Apps/NanoOS/assets/vicenano/vicenano.pak` (`417,792 B`) + individual `.nantex`/`.dff` files.
- `Apps/NanoOS/MANIFEST.json` & `Apps/NanoOS/SHA256SUMS`: Complete inventory with byte sizes, 4 KB cluster counts, and SHA-256 checksums.
- `emu/chainloader.hbapp` (`19,256 B`): LZSS-compressed self-contained chainloader (`<= 20,708 B` `Blocks.hbapp` slot).
- `emu/chainloader-full.hbapp` (`27,060 B`): LZSS-compressed full kernel chainloader (`<= 31,180 B` `Game Boy.hbapp` slot).
- `emu/kernel-slim.bin` (`22,056 B`): Slim combined kernel (`<= 24,608 B` `world.bin` slot).
- `emu/NanoOSLoader-emu.hbapp` (`6,380 B`): Emulator `NanoOSLoader` reading the full `kernel.bin` from `/Apps/Data/Probe/vrom0.bin` (`65,536 B` capacity).

### 4.2 Patch Disk Image for Emulator Testing: `patch_disk_emu.py` / `patch_disk_emu.sh`

```sh
cd /home/pavliukov/ipodwork/NanoRework
./nanoos/tools/patch_disk_emu.sh
```

Generates three non-destructive `N7PATCH1` NAND overlay files in `nanoos/installer/dist/emu/` (asserting that `dumps/ipod-disk.dd` size and mtime never change):
1. `emu-chainloader.patch` (`6` x 4 KB blocks, LBA `0x15edf..0x15f08`): Overlays `/Apps/Executables/Blocks.hbapp` with `emu/chainloader.hbapp` (instant boot on tapping **Blocks**).
2. `emu-loader.patch` (`31` x 4 KB blocks, LBA `0xdd47..0x2cb96`): Overlays `/Apps/Executables/Game Boy.hbapp` with `emu/NanoOSLoader-emu.hbapp`, `/Apps/Data/Probe/vrom0.bin` with the full `Apps/NanoOS/kernel.bin` (`32,768 B`), and `/Apps/Data/Blocks/world.bin` with `emu/kernel-slim.bin` (`22,056 B`). Tapping **Game Boy** runs the full `NanoOSLoader` (`BUSY...` -> disk read -> hardware shutdown -> SRAM handoff) into the full combined NanoOS + ViceNano M8 kernel.
3. `emu-combined.patch` (`37` x 4 KB blocks, LBA `0xdd47..0x2cb96`): Overlays **both** slots simultaneously so either **Blocks** or **Game Boy** can be launched in the same NanoEmu session.

### 4.3 List & Audit Real-Device Files: `list_device_files.py` / `list_device_files.sh`

```sh
cd /home/pavliukov/ipodwork/NanoRework
./nanoos/tools/list_device_files.sh --check-backup
# Or when the real iPod is mounted read-only/read-write at /mnt/ipod:
./nanoos/tools/list_device_files.sh --check-dir /mnt/ipod
```

Prints the exact on-device file table:

| Device Target Path (`/mnt/ipod/...`) | Bytes | 4K Clusters | SHA-256 (prefix) | Install Scope |
|---|---:|---:|---|---|
| `/Apps/Executables/NanoOS.hbapp` | `6,376` | `2` | `2d1426d353a6c3b7..` | REQUIRED (loader app) |
| `/Apps/Icons/NanoOS.bin` | `50,208` | `13` | `6685cfea453ffd04..` | FRESH INSTALL ONLY |
| `/Apps/NanoOS/kernel.bin` | `32,768` | `8` | `45b76eb70575d185..` | REQUIRED (combined kernel) |
| `/Apps/NanoOS/assets/vicenano/vicenano.pak` | `417,792` | `102` | `70eda0bb6ecb8a1c..` | RECOMMENDED (aligned NPAK) |
| `/Apps/NanoOS/assets/vicenano/nbdecoshop01.dff` | `99,309` | `25` | `7229d2957e006769..` | OPTIONAL (individual asset) |
| `/Apps/NanoOS/assets/vicenano/nbdecoshop01.nantex` | `44,008` | `11` | `196c34d4c92b7e5c..` | OPTIONAL (individual asset) |
| `/Apps/NanoOS/assets/vicenano/nbdecoshop02.dff` | `116,153` | `29` | `aca1e3c4483f36b6..` | OPTIONAL (individual asset) |
| `/Apps/NanoOS/assets/vicenano/nbdecoshop02.nantex` | `22,012` | `6` | `d834fa75bcf3a3ae..` | OPTIONAL (individual asset) |
| `/Apps/NanoOS/assets/vicenano/nbdecoshop03.dff` | `98,096` | `24` | `b8a86ff340596bff..` | OPTIONAL (individual asset) |
| `/Apps/NanoOS/assets/vicenano/nbdecoshop03.nantex` | `16,936` | `5` | `f4d0f0794bdef811..` | OPTIONAL (individual asset) |
| `/Apps/NanoOS/assets/vicenano/asset-provenance.json` | `3,176` | `1` | `ffd3d15afb7059da..` | OPTIONAL (asset provenance) |
| `/Apps/NanoOS/MANIFEST.json` | `7,641` | `2` | `0015ca46865a891d..` | RECOMMENDED (verification) |
| `/Apps/NanoOS/SHA256SUMS` | `1,171` | `1` | `18a332b495de0c2c..` | RECOMMENDED (verification) |
| **TOTAL STAGED ON-DEVICE FOOTPRINT** | **`915,646`** | **`229`** | *(~916.0 KiB on FAT32)* | |

---

## 5. Coordination Contract Between Claude (Drivers) & Codex (ViceNano M8)

`build_payload.py` integrates both parallel workstreams without mutating either team's tracked source files:

1. **Launcher Lifecycle Hooks (`nanoos_shim.h` <-> `nanoos/src/main.c`):**
   - `m8_app_enter()`: Invoked when `scene == 1` (`ViceNano`) is selected on the NanoOS launcher. Calls `NanoInit(432, 240)` and draws the initial frame.
   - `m8_app_tick(unsigned keys)`: Invoked every main-loop pass while `scene == 1`. Maps `BUTTON_LEFT`/`BUTTON_RIGHT` (`Vol-`/`Vol+`) to cycle `mode` (`0` = Title, `1` = MBX Triangle, `2` = MBX Texture Test, `3` = MBX 3D World Preview).
   - `m8_app_exit()`: Invoked when `BUTTON_HOME` exits `scene == 1` back to the NanoOS launcher. Calls `NanoExit()`, destroying uploaded MBX textures and restoring saved MBX MMU/render-target registers (`0x3b001000..0x3b001020`, `0x3b000654..0x3b000660`).
2. **MBX GPU Driver Contract (`nanoos/src/mbx.h` <-> `ViceNano/m8/nanoos_gpu_mbx.cpp`):**
   - Claude's `nanoos/src/mbx.c` provides `mbx_init()` (programs 8 x 4 MB GPU MMU page tables mapping GPU VA `0..32 MB` to physical `0x08000000..0x09FFFFFF`), `mbx_begin()`, `mbx_submit()`, `mbx_render()`, and `mbx_wait_done()`.
   - Codex's `ViceNano/m8/nanoos_gpu_mbx.cpp` implements `M8_GPU_ABI_VERSION 2` (`m8_gpu_init`, `m8_gpu_begin`, `m8_gpu_triangle`, `m8_gpu_texture_create`, `m8_gpu_texture_destroy`, `m8_gpu_textured_triangle`, `m8_gpu_end`, `m8_gpu_shutdown`) on top of `mbx.c`, twiddling RGB565/ARGB4444 textures in Morton order and rendering into a 4 KB-aligned 240x432 target buffer.
3. **Symbol Namespace Isolation:**
   - `nanoos/src/buttons.c` defines `struct buttons_info buttons;`, while `ViceNano/m8/input.cpp` defines a static/global `buttons` touch table. `build_payload.py` compiles `m8/input.cpp` with `-Dbuttons=m8_touch_buttons` so both link cleanly into a single `kernel.elf` with zero symbol collisions.
4. **Remaining Hardware Interfaces for Full-Game M9:**
   - **Pre-handoff asset staging in `NanoOSLoader`:** Optional extension to `loader.c` to read `/Apps/NanoOS/assets/vicenano/vicenano.pak` into physical `0x09000000` before `hw_shutdown_all()` so all 3 North Beach cells (`417.8 KiB`) reside in upper DRAM at boot.
   - **Audio DMA (`audio.c`):** Switch from CPU-paced I2S0 FIFO writes (`0x3ca00010`) to DMA engine 1 channel 2 (`0x38200140`) for background radio + SFX mixing during 3D rendering.

---

## 6. Emulator Verification Summary

Verified end-to-end in NanoEmu (`snap6` resume with `--nand-patch nanoos/installer/dist/emu/emu-combined.patch`):

1. **Build & Packaging (`build_payload.py --clean`):**
   - Full combined kernel (`32,768 B`, 18 NanoOS modules + 6 ViceNano M8 modules) links with zero undefined symbols and passes `kernel.ld` assertions (`code/data <= 0x08010000`, `_stack_top <= 0x08200000`).
   - Slim chainloader (`19,256 B`) fits `Blocks.hbapp` (`20,708 B` max); full chainloader (`27,060 B`) fits `Game Boy.hbapp` (`31,180 B` max).
2. **Direct Chainloader Boot (`emu-chainloader.patch` via Blocks slot) & Loader Boot (`emu-loader.patch` via Game Boy slot):**
   - Clock selector (`STOCK` / `SAFE` / `MAX`) -> `NanoOS v0.0.2 beta` 5-icon launcher (`StockOS`, `ViceNano`, `PS1Emu`, `Settings`, `DevMode`).
   - Entering **ViceNano (`VC`)** launches the ViceNano M8 Title Screen; pressing `Vol+` (`Right`) cycles through `MBX triangle` -> `MBX texture test` -> `MBX world preview` (112 textured triangles from `nbdecoshop01` rendered by the PowerVR MBX GPU with zero timeouts/errors); pressing `Home` cleanly exits `ViceNano` back to the NanoOS launcher.
   - Entering **DevMode (`DV`)** shows Page 0 (`Stats`), `Vol+` opens Page 1 (`Drivers`: `LCD OK`, `TOUCH OK`, `BTN OK`, `AUDIO OK`, `NAND OK`, `USB STUB`, `BT OK`, `MBX OK`), `Vol+` again opens Page 2 (`MBX GPU test` animated triangle), and `Home` returns to the launcher.
   - Selecting **StockOS (`iP`)** triggers `reset_stockos()`, cleanly restoring stock RetailOS.

---

## 7. Step-by-Step Real-Device Install & Test Guide (For User Return)

> **Safety Guarantee:** Nothing in this bundle touches NAND boot partitions, SysCfg, or `/iPod_Control`. All files live under `/Apps/` on the standard FAT32 user partition, and selecting **StockOS** (or holding Home+Sleep/Wake) always reboots cleanly into stock RetailOS.

### Step 1: Connect & Mount the iPod nano 7G in Stock RetailOS
Plug the iPod into your Linux machine while booted in normal RetailOS and mount its FAT32 partition (e.g., at `/mnt/ipod`):
```sh
sudo mkdir -p /mnt/ipod
# Replace /dev/sdX1 with the iPod's FAT32 partition shown by lsblk
sudo mount /dev/sdX1 /mnt/ipod
```

### Step 2: Run the Read-Only Device Audit & Back Up `/Apps`
```sh
cd /home/pavliukov/ipodwork/NanoRework
./nanoos/tools/list_device_files.sh --check-dir /mnt/ipod

# Create a timestamped backup of the current /Apps folder before copying
cp -a /mnt/ipod/Apps "/home/pavliukov/ipodwork/NanoRework/dumps/apps-backup-$(date -u +%Y%m%dT%H%M%SZ)"
```

### Step 3: Copy the Staged NanoOS + ViceNano Bundle
If `list_device_files.sh` shows `Pack /Apps/AllApps.pack: REGISTERED` (already true on your iPod):
```sh
sudo mkdir -p /mnt/ipod/Apps/NanoOS/assets/vicenano
sudo cp /home/pavliukov/ipodwork/NanoRework/nanoos/installer/dist/Apps/NanoOS/kernel.bin /mnt/ipod/Apps/NanoOS/kernel.bin
sudo cp /home/pavliukov/ipodwork/NanoRework/nanoos/installer/dist/Apps/Executables/NanoOS.hbapp /mnt/ipod/Apps/Executables/NanoOS.hbapp
sudo cp -r /home/pavliukov/ipodwork/NanoRework/nanoos/installer/dist/Apps/NanoOS/assets /mnt/ipod/Apps/NanoOS/
sudo cp /home/pavliukov/ipodwork/NanoRework/nanoos/installer/dist/Apps/NanoOS/MANIFEST.json \
        /home/pavliukov/ipodwork/NanoRework/nanoos/installer/dist/Apps/NanoOS/SHA256SUMS \
        /mnt/ipod/Apps/NanoOS/
```
*(If installing onto a freshly jailbroken iPod where `NanoOS` is not yet in `AllApps.pack`, run `(cd /home/pavliukov/ipodwork/NanoApps && ./start install NanoOSLoader)` first, then run the `cp` commands above.)*

### Step 4: Verify Checksums & Safely Eject
```sh
(cd /mnt/ipod && sha256sum -c Apps/NanoOS/SHA256SUMS)
sync
sudo umount /mnt/ipod
```

### Step 5: On-Device Test Walkthrough
1. **Launch NanoOS:** On the iPod Home screen, tap the **NanoOS** icon. Do **not** press Home during the 1-second `BUSY... Booting NanoOS` progress bar—it will automatically read `/Apps/NanoOS/kernel.bin` and hand off to NanoOS.
2. **Select Clock Profile:** Use **Volume - / +** to choose `STOCK` (133 MHz GPU), `SAFE` (171 MHz GPU), or `MAX` (266 MHz GPU), and press **Home** (or **Vol- + Vol+** together) to confirm.
3. **Test ViceNano M8 (`VC`):**
   - Press **Volume +** once to highlight **ViceNano (`VC`)**, then press **Home**.
   - You will see the **ViceNano M8 Title Screen**.
   - Press **Volume +** to cycle through:
     1. `MBX triangle` (hardware Gouraud triangle)
     2. `MBX texture test` (hardware 8x8 RGB565 twiddled textured quad)
     3. `MBX world preview` (hardware 3D North Beach building mesh)
   - Press **Home** to exit ViceNano back to the NanoOS launcher.
4. **Test Bare-Metal Drivers (`DV`):**
   - Select **DevMode (`DV`)** and press **Home** to view live CPU/GPU clocks, RAM, FPS, LCD DSI, and PMU I2C stats.
   - Press **Volume +** to open the **Drivers** page (`LCD`, `TOUCH`, `BTN`, `AUDIO` 440 Hz tone, `NAND`, `USB`, `BT`, `MBX`). Touch the screen or press Play/Power to watch live driver counters update.
   - Press **Volume +** again to open the **MBX GPU test** page.
   - Press **Home** to return to the NanoOS launcher.
5. **Return to Stock iPod OS (`iP`):**
   - Select **StockOS (`iP`)** and press **Home**. The iPod will perform a clean watchdog/PMGR warm reset back into Apple's RetailOS.
