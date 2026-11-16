# NanoOS Architecture, Runtime & Application SDK

## 1. System Architecture & Ownership

NanoOS is a bare-metal single-address-space kernel for the iPod Nano 7 (`S5L8740` ARM Cortex-A5 + PowerVR MBX Lite GPU) that boots via a RetailOS chainloader (`NanoOS.hbapp`) and executes standalone `NGM1` (`game.bin`) applications.

```
+-------------------------------------------------------------------+
| Application Repo (e.g. shmup-nano7, examples/hello_gles2)         |
|   Uses ONLY: nano_app.h, nano_fs.h, nano_heap.h, EGL/egl.h,       |
|              GLES2/gl2.h (no raw physical addresses or drivers)   |
+-------------------------------------------------------------------+
| NanoOS Application SDK (NanoOS/sdk/)                              |
|   - nano_app.c  : entry, input/touch mapping, NGAME_HOME exit     |
|   - nano_fs.c   : HFS+ volume mount & POSIX-like file I/O         |
|   - nano_heap.c : O(1) coalescing multi-region heap + libc shims  |
|   - app.mk / app.ld / start.S / mkgame.py                         |
+-------------------------------------------------------------------+
| Canonical Runtime Libraries (NanoOS/runtime/)                     |
|   - runtime/gl2/     : GLES2/EGL bridge, VGP compiler, gl2tex     |
|   - runtime/storage/ : SFTL, disk cache, partition, HFS+, mount   |
+-------------------------------------------------------------------+
| NanoOS Kernel (NanoOS/src/) — Hardware Ownership                  |
|   - nand.c (FMC/PPN), mbx.c (TA/ISP/TSP), lcd.c (scanout),        |
|     touch.c, buttons.c, mmu.c (memmap.h), game.c (NGAME ABI v23)  |
+-------------------------------------------------------------------+
```

### Hardware Ownership Boundaries
- **Kernel (`NanoOS/src/`):** Exclusively owns MMU/cache configuration (`memmap.h`), NAND FMC/PPN transport (`nand.c`), MBX GPU command submission/IRQs (`mbx.c`), LCD framebuffer DMA & vsync (`lcd.c`), touchscreen (`touch.c`), and physical buttons (`buttons.c`).
- **System Services (`gameabi.h`):** `NGAME_ABI = 1`, services table `v23`. Applications receive `const struct nanoos_api *api` at entry and never program hardware registers directly.
- **App-linked Runtime (`NanoOS/runtime/`, `NanoOS/sdk/`):** Statically linked into each `game.bin` (`MM_GAME = 0x0b000000`) so `gl2` command-buffer building, VGP shader compilation, and HFS+/SFTL read caching run in the application's CPU context without kernel bloat or driver duplication.

## 2. 64 MiB DRAM Memory Map (`src/memmap.h`)

| Region | Address Range | Attributes | Owner |
|---|---|---|---|
| `MM_KERNEL` | `0x08000000..0x08200000` | Cached | NanoOS kernel `.text/.data/.bss`, MMU tables, stack |
| `MM_LCD` | `0x08200000..0x08300000` | Uncached | Double-buffered `240x432` ARGB8888 LCD scanout |
| `MM_DMA` | `0x08500000..0x08800000` | Uncached | Kernel `ppn_dma`, MBX parameter/PB tables |
| `MM_GAME_GPU` | `0x08a00000..0x0a000000` | Uncached | SDK-managed GPU texture arena (`egl_nano.c` / `gl2nano.c`) |
| `MM_GAME_HEAP` | `0x0a000000..0x0b000000` | Cached | `ns_game` (`0x0a400000`, ~1.5 MiB) + SDK CPU heap (`nano_heap.c`) |
| `MM_GAME` | `0x0b000000..0x0ba00000` | Cached | `game.bin` image (`text/data/bss`) + tail heap |
| `MM_GAME_HEAP2` | `0x0ba20000..0x0be40000` | Cached | Secondary SDK CPU heap region (clear of report mirrors) |

## 3. GPU, Shader Compiler & Texture Formats

- **MBX Lite Pipeline:** `gl2rt.c` emits native TA/TSP/ISP draw headers; `gl2vsc.c` compiles GLSL vertex shaders into VGP microcode; `gl2api.c` + `gl2soft.inc` lowers supported single-pass fragment shaders (`C`, `P`, `T`, `T*C`, `T*P`) directly onto TSP combiners with zero software fallback.
- **PVRTC 4bpp Hardware Verification (`texpvrtc`, `run398`/`run399`):**
  - Physical Nano 7 silicon testing revealed that MBX TSP format `0x20000` decodes as **2bpp** (8×4 blocks), whereas `0x38000` (`fmt = 0x38`) decodes **4bpp PVRTC** (4×4 blocks) for both opaque (`RGB554`/`RGB555`) and alpha (`ARGB3443`/`ARGB3444`) blocks.
  - Block order is PowerVR Morton twiddle (`Y` in even bits `0x5555`, `X` in odd bits `0xAAAA`).
  - `glCompressedTexImage2D` (`0x8C00` RGB, `0x8C02` RGBA) supports full mip chains down to `1x1` (`max(w*h/2, 32)` bytes per level).
- **Supported Native Texture Formats:** `ARGB8888` (`0x19`), `RGB565` (`0x05`), `ARGB4444` (`0x09`), `ARGB1555` (`0x01`), `AL88` (`0x11`), and `PVRTC4` (`0x38`). `glTexImage2D` supports mip levels up to `GL2_MAX_TEXTURE_SIZE`.

## 4. Storage, NAND & Filesystem Optimisations

- **NAND PPN Driver (`src/nand.c`):** Removed redundant 4,096-word uncached `ppn_dma` pre-fill and replaced single-word copies with `burst_copy_4096()` (8-word unrolled transfers).
- **SFTL Mount (`runtime/storage/mount.c`):** Non-recovery `survey()` reads only the first usable bank of `page 0` per block (~2,084 vs ~8,336 reads), and `erased()` checks the 16-word spare record before scanning the 16 KiB page buffer.
- **Disk Cache (`runtime/storage/disk.c`):** 16-page (256 KiB) LRU sector cache + 32-bit `fast_copy()`.
- **HFS+ Catalog Lookup (`runtime/storage/hfs.c`):** Index-node binary search from `cat_tree.root` (`OPEN_COLD`: `299.26 ms -> 28.81 ms`, with cache `13.01 ms`; `OPEN_WARM`: `1.15 ms`).

## 5. Application SDK Reference (`NanoOS/sdk/`)

Include `<nano_app.h>`, `<nano_fs.h>`, `<nano_heap.h>`, `<EGL/egl.h>`, `<GLES2/gl2.h>`:

- **Lifecycle & Input (`nano_app.h`):**
  - `nano_app_init(api)` — binds `NGAME` API, initializes EGL platform & input state.
  - `nano_app_poll_input(&in)` — polls buttons (`home_pressed` via `NGAME_HOME`) and maps landscape touch `(0..431, 0..239)` to portrait display `(239 - y, x)` with `pressed`/`moved`/`released` edges.
  - `nano_app_usec()`, `nano_app_dcache_clean()`, `nano_app_gfx_stats()`.
- **Filesystem (`nano_fs.h`):**
  - `nano_fs_mount()`, `nano_fs_open(path)`, `nano_fs_read()`, `nano_fs_pread()`, `nano_fs_seek()`, `nano_fs_tell()`, `nano_fs_close()`, `nano_fs_read_all()`.
- **Memory (`nano_heap.h` + `sdk/include/libc/`):**
  - `malloc`, `calloc`, `realloc`, `free` with O(1) doubly-linked coalescing and next-fit rover (~22.4 MiB usable across `MM_GAME_HEAP`, `MM_GAME` tail, and `MM_GAME_HEAP2`), plus `nano_heap_stats()`.

## 6. Build & Deployment Workflow

```sh
# 1. Build NanoOS kernel
make -C NanoOS

# 2. Build an application (default: examples/hello_gles2)
make -C NanoOS app
make -C NanoOS app APP_DIR=/path/to/external-app

# 3. Build RetailOS chainloader (NanoOS.hbapp)
make -C NanoOS chainloader NANOAPPS=/path/to/nano7kit/nanoapps
```

## 7. Measured Hardware Benchmarks (Nano 7)

- **Mount time (`US[0]`):** `19.36 s` (`run401`) → `7.86 s` (`run402`, 1-bank `survey()`) → **`4.97 s`** (`run403`, + `nand.c` burst/DMA fix).
- **SHMUP `act1.scene` load (`US[3]`):** `5.85 s` (`run400`, PNGs) → **`0.85 s`** (`run403`, offline `.ntx` PVRTC 4bpp + native formats).
- **SHMUP `act1.scene` sustained frame rate (`run403`):** `1,505` GPU frames, `software=0`, `fault=0`, `glerr=0`, median `16.83 ms` (`~59.4 FPS`).

## 8. Key Bugs Resolved

1. **PVRTC 4bpp format descriptor (`0x20000` vs `0x38000`):** Hardware probe `run398` caught that `0x20000` is 2bpp on MBX Lite; `0x38000` is 4bpp for both RGB and RGBA.
2. **Mip level > 6 rejection:** `image_format()` in `gl2tex.c` capped `level > 6` (64×64 max), rejecting 256×256 mip levels `7..8`; updated to `(1u << level) <= GL2_MAX_TEXTURE_SIZE`.
3. **MD5 entity token overwrite (`entities.c`):** `ENT_LoadEntity()` overwrote `LE_getCurrentToken()` before `ENT_Put()`, causing `cityBlue.obj.md5mesh` to be opened 48× instead of once.
4. **Shader/program bounds (`gl2api.c`):** `GL2_MAX_PROGRAMS`/`GL2_MAX_SHADERS` had hardcoded bounds in `shp()`/`glIsShader()`/`glUseProgram()`.
5. **Touch drag (`commScale == 0`):** SHMUP required `SRC_OnResizeScreen(240, 432)` before `IO_EVENT_MOVED` coordinates scaled properly.

## 9. Verified vs Experimental / Limitations

- **Verified on physical Nano 7:** Kernel + `NGAME` ABI `v23`, EGL window surface + vsync presentation, VGP vertex compilation, single-pass TSP combiners (`C/P/T/T*C/T*P`), `ARGB8888`/`RGB565`/`ARGB4444`/`PVRTC4` (`0x38000`) + mipmaps, SFTL/HFS+ read-only mount & streaming, touch + `NGAME_HOME` exit.
- **Limitations:** Multi-pass arbitrary fragment shaders fall back to software or are rejected (`kUberVsMbx`/`kUberFsMbx` lowers per-vertex lighting); `NanoOS` app SDK filesystem is read-only by default (persistent saves require preallocated 32 KiB save files via `sdk/src/storage/save.c`); `test_gl2api.py` requires the parent `gles2shim`/`glsl2vgp` models.

## 10. Regression Testing

```sh
# Host unit & sanitizer tests (inside NanoOS)
python3 NanoOS/runtime/gl2/test_gl2pvrtc.py
python3 NanoOS/runtime/gl2/std/test_gl2stdapp.py
python3 NanoOS/runtime/storage/test_hfs.py
python3 NanoOS/runtime/storage/test_storage.py

# Physical Nano 7 automated test (with run227 NanoOS.hbapp & Disk Mode)
python3 bundle_native.py --fixture texpvrtc --out hwbridge/build/payload.bin
python3 hwbridge/run_native.py --out hw-reports/runNN.txt
```
