#!/usr/bin/env python3
"""build_payload.py -- Build the combined NanoOS kernel (with all bare-metal
drivers) + ViceNano M8 app + NanoOSLoader + packed ViceNano assets into a
staging bundle ready for NanoOSLoader installation or NanoEmu testing.

Produces in nanoos/installer/dist/:
  - Apps/NanoOS/kernel.bin            : Full combined kernel (all 8 bare-metal
                                        drivers + DevMode Drivers page + ViceNano
                                        M8 Title/MBX Triangle/MBX World Preview)
  - Apps/Executables/NanoOS.hbapp     : NanoOSLoader v0.0.2 beta (device build)
  - Apps/Icons/NanoOS.bin             : 112x112 SilverDB Home screen icon
  - Apps/NanoOS/assets/vicenano/      : Individual .nantex/.dff files + 4 KB-aligned
                                        contiguous vicenano.pak (NPAK v1)
  - Apps/NanoOS/MANIFEST.json         : Full package metadata & LBA/cluster table
  - Apps/NanoOS/SHA256SUMS            : Standard sha256sum verification file
  - emu/chainloader-full.hbapp        : Chainloader embedding full kernel.bin
                                        (fits in Game Boy.hbapp 31,180 B slot)
  - emu/chainloader.hbapp             : Slim chainloader (omits DevMode Drivers
                                        probe page to fit in Blocks.hbapp 20,708 B)
  - emu/kernel-slim.bin               : Slim combined kernel (fits in world.bin
                                        24,608 B slot)
  - emu/NanoOSLoader-emu.hbapp        : Emulator NanoOSLoader reading full
                                        kernel.bin from /Apps/Data/Probe/vrom0.bin
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from npak import write_npak

INSTALLER_DIR = Path(__file__).resolve().parent
NANOOS_DIR = INSTALLER_DIR.parent
REWORK_DIR = NANOOS_DIR.parent
IPODWORK_DIR = REWORK_DIR.parent

VICENANO_M8_DIR = Path(os.environ.get("VICENANO_M8", IPODWORK_DIR / "ViceNano/m8"))
NANOAPPS_DIR = Path(os.environ.get("NANOAPPS", IPODWORK_DIR / "NanoApps"))
CHAIN_DIR = REWORK_DIR / "probe/chainloader"
LOADER_DIR = REWORK_DIR / "apps/NanoOSLoader"
BACKUP_DIR = REWORK_DIR / "dumps/apps-backup-20261015T115834Z-pre-nanoos"

VC_CONVERTED_DIR = Path(os.environ.get("VC_CONVERTED", REWORK_DIR / "vicenano/assets-src/converted"))

BUILD_DIR = INSTALLER_DIR / "build"
DIST_DIR = INSTALLER_DIR / "dist"

# Existing file slots on dumps/ipod-disk.dd for non-destructive N7PATCH1 overlays
KERNEL_SLOT_EMU_FULL = "/Apps/Data/Probe/vrom0.bin"
VROM0_BIN_CAPACITY = 65536
KERNEL_SLOT_EMU_SLIM = "/Apps/Data/Blocks/world.bin"
WORLD_BIN_CAPACITY = 24608
BLOCKS_HBAPP_CAPACITY = 20708
GAMEBOY_HBAPP_CAPACITY = 31180


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def patch_launcher_source(main_c_path: Path, out_c_path: Path, include_drivers_page: bool = True) -> None:
    """Inject ViceNano M8 enter/tick/exit hooks into NanoOS's main.c.
    When include_drivers_page=True (the default full kernel), both ViceNano (scene 1)
    and DevMode's Stats + Drivers pages (scene 4, Vol+/Vol-) are preserved.
    When include_drivers_page=False (slim build for the 20,708 B Blocks.hbapp slot),
    the Drivers probe page is omitted so the embedded .hbapp stays under 20,708 B.
    """
    src = main_c_path.read_text()

    if not include_drivers_page:
        src = src.replace('#include "drivers.h"\n', "")
        # Settings > Bluetooth Central (btui.c, bt.c) is not in the slim set
        src = src.replace('#include "btui.h"\n', "")
        src = re.sub(r"^.*btui_tick\(keys, tap\);\n", "", src, flags=re.M)
        src = src.replace("else if (scene == SETTINGS) btui_enter();", "")
        src = re.sub(r"^.*unsigned drivers_page = 0;[^\n]*\n", "", src, flags=re.M)
        src = re.sub(r"^.*mbx_test_leave\(\);\n", "", src, flags=re.M)
        src = src.replace("scene = 0; drivers_page = 0;", "scene = 0;")
        start = src.find("else if (scene == DEVMODE) {")
        if start >= 0:
            brace = src.index("{", start)
            depth = 1
            end = brace + 1
            while depth:
                if src[end] == "{":
                    depth += 1
                elif src[end] == "}":
                    depth -= 1
                end += 1
            src = src[:start] + "else if (scene == DEVMODE && lcd_ready()) devmode_draw();" + src[end:]

    if '#include "nanoos_shim.h"' not in src:
        assert '#include "lcd.h"' in src, 'expected #include "lcd.h" in main.c'
        src = src.replace('#include "lcd.h"', '#include "lcd.h"\n#include "nanoos_shim.h"', 1)

    # 1. Hook BUTTON_HOME exit from active scene. m8_app_exit() returns 1 when
    #    R80 holds a failure: StockOS is then preselected (one Home = warm reset
    #    into RetailOS, which keeps the report RAM) and a hint is drawn.
    #    m8_app_home() returning 1 keeps the app (Home = walk/turn/scene in
    #    the walkable Vice City mode; the Vol-+Vol+ chord always leaves).
    dp = " drivers_page = 0;" if "scene = 0; drivers_page = 0;" in src else ""
    leave = f"scene = 0;{dp} launcher(selected);"
    home_old = f"if (keys & BUTTON_HOME) {{ {leave} }}"
    if home_old not in src:
        raise RuntimeError("Could not find BUTTON_HOME scene exit in main.c")
    src = src.replace(
        home_old,
        "if ((keys & BUTTON_HOME) && !(scene == 1 && m8_app_home(keys))) { "
        "unsigned m8_saved = scene == 1 && m8_app_exit(); "
        f"if (m8_saved) selected = 0; {leave} if (m8_saved) m8_launcher_hint(); }}",
        1,
    )

    # 2. Hook scene == 1 per-frame tick before scene == DEVMODE; a nonzero
    #    m8_app_tick() is the auto exit after a saved failure (since round 11
    #    it warm-resets into RetailOS itself and returns 0; the branch stays
    #    for a backend that returns 1)
    tick = ("else if (scene == 1) { if (m8_app_tick(keys)) { m8_app_exit(); selected = 0; "
            f"{leave} m8_launcher_hint(); }} }}\n            ")
    if "else if (scene == DEVMODE) {" in src:
        src = src.replace("else if (scene == DEVMODE) {", tick + "else if (scene == DEVMODE) {", 1)
    elif "else if (scene == DEVMODE && lcd_ready()) devmode_draw();" in src:
        src = src.replace(
            "else if (scene == DEVMODE && lcd_ready()) devmode_draw();",
            tick + "else if (scene == DEVMODE && lcd_ready()) devmode_draw();",
            1,
        )
    else:
        raise RuntimeError("Could not find scene == DEVMODE tick branch in main.c")

    # 3. Hook scene == 1 enter on launch
    if "else placeholder(scene);" in src:
        src = src.replace(
            "else placeholder(scene);",
            "else if (scene == 1) m8_app_enter();\n                else placeholder(scene);",
            1,
        )
    else:
        raise RuntimeError("Could not find placeholder(scene) launch branch in main.c")

    out_c_path.write_text(src)


def build_kernel_variant(cross: str, variant: str, include_drivers_page: bool) -> tuple[Path, Path]:
    """Compile NanoOS drivers + patched launcher + ViceNano M8 into kernel.bin
    and a self-contained chainloader.hbapp.
    """
    cc = f"{cross}gcc"
    cxx = f"{cross}g++"
    objcopy = f"{cross}objcopy"
    size_bin = f"{cross}size"
    nm_bin = f"{cross}nm"

    kbuild = BUILD_DIR / f"kernel-{variant}"
    kbuild.mkdir(parents=True, exist_ok=True)

    arch = ["-mcpu=cortex-a5", "-marm", "-mfloat-abi=soft", "-mno-unaligned-access"]
    cflags = arch + [
        "-Os", "-Wall", "-Wextra", "-Werror",
        "-ffreestanding", "-fno-builtin", "-fno-pic", "-fno-common",
        "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
        "-ffunction-sections", "-fdata-sections",
        f"-I{NANOOS_DIR / 'src'}",
        f"-I{VICENANO_M8_DIR}",
        f"-I{kbuild}",
        # touch.h lever: R84 and the Home search out (~760 B) to fit
        # Settings > Bluetooth Central (btui.c, 2026-10-17)
        "-DTOUCH_DIAG=0",
        # round 8: font8x8 glyphs 32..127 only (lcd.h), 256 B
        "-DNANOOS_FONT32",
    ]
    # CPU MMU + D-cache (src/mmu.c): NANOOS_MMU=0 builds the MMU-off fallback
    if os.environ.get("NANOOS_MMU") == "0":
        cflags.append("-DNANOOS_MMU=0")
        print("==> NANOOS_MMU=0: MMU and D-cache stay off (fallback kernel)")
    # Raw NAND write skeleton (nanoos/src/nand.c): off unless asked for.
    if os.environ.get("NANOOS_NAND_WRITE") == "1":
        cflags.append("-DNANOOS_NAND_WRITE=1")
        print("==> WARNING: NANOOS_NAND_WRITE=1: guarded NAND write path compiled in")
    # round 11 size: the DevMode GPU page's diagnostic pages, 32 MB scans and
    # R50..R53 report (mbx_test.c) are out of the shipping build; the frame
    # render of each case stays
    if os.environ.get("NANOOS_MBX_DIAG") == "1":
        cflags.append("-DMBX_DIAG_PAGES=1")
        print("==> NANOOS_MBX_DIAG=1: MBX diagnostic pages compiled in (kernel.bin may not fit)")
    cxxflags = cflags + [
        "-mthumb",
        # ViceNano float math on the VFPv4-D16 (startup.s enables it);
        # softfp keeps the soft-float calling convention of the C objects
        "-mfpu=vfpv4-d16", "-mfloat-abi=softfp",
        "-DM8_NANOOS",
        "-std=c++11", "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics",
    ]

    objs = []

    # 1. Assemble startup.s
    startup_o = kbuild / "startup.o"
    subprocess.run([cc, *cflags, "-c", str(NANOOS_DIR / "src/startup.s"), "-o", str(startup_o)], check=True)
    objs.append(startup_o)

    # 2. Discover and compile driver .c files in nanoos/src/
    arm_hot_stems = {"lcd", "font", "input", "timer"}
    # round 6 size diet: cold modules without their own "Os" pragma (text
    # formatting, asset lookup, heap, reset) and the launcher build -Os in
    # the full kernel too (-O2 cost ~700 B of the 64 KiB for no speed need)
    # mmu/game: once per boot or per launch
    cold_os_stems = {"mbx", "mbx_test", "fmt", "asset", "mem", "reset", "mmu", "game"}
    if include_drivers_page:
        driver_sources = sorted(
            p for p in (NANOOS_DIR / "src").glob("*.c") if p.name != "main.c"
        )
    else:
        slim_stems = {"lcd", "font", "input", "reset", "devmode", "mem", "timer", "fmt", "mbx", "asset", "clk", "mmu", "game"}
        # DevMode shows the touch diag since the touch rework (touch.c + its SPI)
        if "touch." in (NANOOS_DIR / "src/devmode.c").read_text():
            slim_stems |= {"touch", "spi"}
        driver_sources = sorted(
            p for p in (NANOOS_DIR / "src").glob("*.c") if p.stem in slim_stems
        )

    print(f"==> [{variant}] Compiling {len(driver_sources)} NanoOS modules: "
          f"{', '.join(p.stem for p in driver_sources)}")
    for src_file in driver_sources:
        obj_file = kbuild / f"{src_file.stem}.o"
        if not include_drivers_page:
            extra_c = ["-mthumb", "-Os"]
        elif src_file.stem in arm_hot_stems:
            extra_c = ["-O2"]
        else:
            extra_c = ["-mthumb", "-Os" if src_file.stem in cold_os_stems else "-O2"]
        subprocess.run([cc, *cflags, *extra_c, "-c", str(src_file), "-o", str(obj_file)], check=True)
        objs.append(obj_file)

    # 3. Generate and compile integrated launcher.c
    launcher_c = kbuild / "launcher.c"
    patch_launcher_source(NANOOS_DIR / "src/main.c", launcher_c, include_drivers_page=include_drivers_page)
    launcher_o = kbuild / "launcher.o"
    launcher_opt = "-Os"
    subprocess.run([cc, *cflags, "-mthumb", launcher_opt, "-c", str(launcher_c), "-o", str(launcher_o)], check=True)
    objs.append(launcher_o)

    # 4. (mode 3's world preview is no longer compiled in: it rides in
    #    vcworld.pak as preview.bin, see build_asset_pack)

    # 5. Compile ViceNano M8 NanoOS modules (rename m8/input.cpp's `buttons`
    #    array to `m8_touch_buttons` so it does not collide with nanoos/src/buttons.c)
    m8_cpp_sources = [
        (VICENANO_M8_DIR / "nano.cpp", "m8-nano.o", []),
        (VICENANO_M8_DIR / "nanoos_shim.cpp", "m8-shim.o", []),
        (VICENANO_M8_DIR / "input.cpp", "m8-input.o", ["-Dbuttons=m8_touch_buttons"]),
    ]
    for opt_name, obj_name in [
        ("nanoos_gpu_mbx.cpp", "m8-gpu-mbx.o"),
        ("nanoos_world.cpp", "m8-world.o"),
        ("model.cpp", "m8-model.o"),
    ]:
        opt_path = VICENANO_M8_DIR / opt_name
        if opt_path.exists():
            m8_cpp_sources.append((opt_path, obj_name, []))

    for src_file, obj_name, extra in m8_cpp_sources:
        obj_file = kbuild / obj_name
        subprocess.run([cxx, *cxxflags, *extra, "-c", str(src_file), "-o", str(obj_file)], check=True)
        objs.append(obj_file)

    runtime_c = VICENANO_M8_DIR / "nanoos_runtime.c"
    runtime_o = kbuild / "nanoos_runtime.o"
    subprocess.run([cc, *cflags, "-mthumb", "-c", str(runtime_c), "-o", str(runtime_o)], check=True)
    objs.append(runtime_o)

    # 6. Link kernel.elf at 0x08000000 using nanoos/kernel.ld
    kernel_elf = kbuild / "kernel.elf"
    kernel_map = kbuild / "kernel.map"
    link_cmd = [
        cc, *arch, "-nostdlib",
        f"-Wl,-T,{NANOOS_DIR / 'kernel.ld'},--gc-sections,--build-id=none,--no-undefined,-Map,{kernel_map}",
        *(str(o) for o in objs),
        "-lgcc",
        "-o", str(kernel_elf),
    ]
    subprocess.run(link_cmd, check=True)
    subprocess.run([size_bin, str(kernel_elf)], check=True)

    undef = subprocess.check_output([nm_bin, "-u", str(kernel_elf)], text=True).strip()
    if undef:
        raise RuntimeError(f"Undefined symbols in {kernel_elf}:\n{undef}")

    kernel_bin = kbuild / "kernel.bin"
    subprocess.run([objcopy, "-O", "binary", str(kernel_elf), str(kernel_bin)], check=True)
    # startup.s looks for an appended asset pack at __data_end (asset.h)
    syms = subprocess.check_output([nm_bin, str(kernel_elf)], text=True)
    data_end = int(re.search(r"^([0-9a-f]+) \w __data_end$", syms, re.M).group(1), 16)
    if data_end != 0x08000000 + kernel_bin.stat().st_size:
        raise RuntimeError(f"{kernel_bin} does not end at __data_end ({data_end:#x})")

    # 7. Build self-contained chainloader.hbapp embedding kernel.bin.
    #    Use ViceNano/m8's LZSS compressed wrapper (pack_kernel.py + nanoos_chainloader.c)
    #    when available so even a 24+ KB textured kernel fits inside Blocks.hbapp (20,708 B).
    appflags = [
        "-mcpu=cortex-a8", "-mthumb", "-mfpu=neon", "-Os",
        "-ffreestanding", "-fno-builtin", "-fno-pic", "-fno-common",
        "-ffunction-sections", "-fdata-sections",
        f"-I{NANOAPPS_DIR / 'sdk'}",
        f"-I{NANOAPPS_DIR / 'sdk/freestanding'}",
        f"-I{CHAIN_DIR}",
    ]
    cl_o = kbuild / "chainloader-app.o"
    sd_o = kbuild / "shutdown-app.o"
    ho_o = kbuild / "handoff.o"
    pack_kernel_py = VICENANO_M8_DIR / "tools/pack_kernel.py"
    lzss_chain_c = VICENANO_M8_DIR / "nanoos_chainloader.c"

    subprocess.run([cc, *appflags, "-c", str(CHAIN_DIR / "shutdown.c"), "-o", str(sd_o)], check=True)

    if pack_kernel_py.exists() and lzss_chain_c.exists():
        subprocess.run([cc, *appflags, "-c", str(lzss_chain_c), "-o", str(cl_o)], check=True)
        subprocess.run([sys.executable, str(pack_kernel_py), str(kernel_bin), str(kbuild / "payload.bin")], check=True)
        chain_objs = [cl_o, sd_o, ho_o]
    else:
        fs_o = kbuild / "hb_fs.o"
        subprocess.run([cc, *appflags, "-c", str(CHAIN_DIR / "chainloader.c"), "-o", str(cl_o)], check=True)
        subprocess.run([cc, *appflags, "-c", str(NANOAPPS_DIR / "sdk/hb_fs.c"), "-o", str(fs_o)], check=True)
        shutil.copyfile(kernel_bin, kbuild / "payload.bin")
        chain_objs = [cl_o, sd_o, fs_o, ho_o]

    # Assemble a local copy of handoff.S inside kbuild so .incbin "payload.bin" embeds our kernel
    shutil.copyfile(CHAIN_DIR / "handoff.S", kbuild / "handoff.S")
    subprocess.run([cc, *appflags, "-c", "handoff.S", "-o", str(ho_o)], cwd=kbuild, check=True)

    chain_elf = kbuild / "chainloader.elf"
    subprocess.run([
        cc, *appflags, "-nostdlib",
        "-Wl,--gc-sections,--build-id=none,--no-undefined,-q,-e,payload_entry,-Ttext,0",
        *(str(o) for o in chain_objs),
        "-lgcc", "-o", str(chain_elf),
    ], check=True)

    chain_hbapp = kbuild / "chainloader.hbapp"
    subprocess.run([
        sys.executable, str(NANOAPPS_DIR / "tools/mkrelocapp.py"),
        str(chain_elf), str(chain_hbapp), "payload_entry",
    ], check=True)

    return kernel_bin, chain_hbapp


def build_loaders() -> tuple[Path, Path, Path]:
    """Build NanoOSLoader for:
      1. Real device (/Apps/NanoOS/kernel.bin)
      2. Emulator full kernel slot (/Apps/Data/Probe/vrom0.bin, 65,536 B capacity)
      3. Emulator world.bin slot (/Apps/Data/Blocks/world.bin, 24,608 B capacity)
    """
    dev_build = BUILD_DIR / "loader-dev"
    emu_full_build = BUILD_DIR / "loader-emu-full"
    emu_slim_build = BUILD_DIR / "loader-emu-slim"
    for d in (dev_build, emu_full_build, emu_slim_build):
        d.mkdir(parents=True, exist_ok=True)

    print("==> Building NanoOSLoader (device path: /Apps/NanoOS/kernel.bin)")
    subprocess.run([
        "make", "-s", "-C", str(LOADER_DIR),
        f"NANOAPPS={NANOAPPS_DIR}",
        f"BUILD={dev_build}",
        "-B",
    ], check=True)

    print(f"==> Building NanoOSLoader (emulator full slot: {KERNEL_SLOT_EMU_FULL})")
    subprocess.run([
        "make", "-s", "-C", str(LOADER_DIR),
        f"NANOAPPS={NANOAPPS_DIR}",
        f"BUILD={emu_full_build}",
        f"KERNEL_PATH={KERNEL_SLOT_EMU_FULL}",
        "-B",
    ], check=True)

    print(f"==> Building NanoOSLoader (emulator slim slot: {KERNEL_SLOT_EMU_SLIM})")
    subprocess.run([
        "make", "-s", "-C", str(LOADER_DIR),
        f"NANOAPPS={NANOAPPS_DIR}",
        f"BUILD={emu_slim_build}",
        f"KERNEL_PATH={KERNEL_SLOT_EMU_SLIM}",
        "-B",
    ], check=True)

    return (
        dev_build / "NanoOSLoader.hbapp",
        emu_full_build / "NanoOSLoader.hbapp",
        emu_slim_build / "NanoOSLoader.hbapp",
    )


TEST_PCM_RATE = 22050
TEST_PCM_MAX = 512 * 1024   # the runtime pack must stay <= 1 MB (loader ASSET_MAX_BYTES)


def make_test_pcm() -> bytes:
    """2 s of raw s16le 22050 Hz mono for the Drivers page audio test
    (nanoos/src/audio.c plays the pack entry test.pcm): a C-E-G-C arpeggio,
    0.5 s per note, three harmonics, 5 ms attack, exponential decay, peak
    about -9 dBFS. NANOOS_TEST_PCM=<file> ships that raw PCM instead; an
    empty file (/dev/null) leaves the clip out (the runtime pack is then the
    131,072 B one proven on the device: the loader allocates kernel + pack
    in one block and silently drops the pack when that fails)."""
    import array
    import math
    env = os.environ.get("NANOOS_TEST_PCM")
    if env:
        raw = Path(env).read_bytes()
        if len(raw) > TEST_PCM_MAX:
            raise SystemExit(f"NANOOS_TEST_PCM: {len(raw)} B > {TEST_PCM_MAX} B")
        return raw[: len(raw) & ~1]
    out = array.array("h")
    note = TEST_PCM_RATE // 2
    for f in (523.25, 659.25, 783.99, 1046.5):
        for i in range(note):
            t = i / TEST_PCM_RATE
            env_ = min(1.0, t / 0.005) * math.exp(-4.0 * t)
            w = sum(a * math.sin(2 * math.pi * f * h * t) for h, a in ((1, 1.0), (2, 0.3), (3, 0.15)))
            out.append(int(11600 * env_ * w / 1.45))
    if sys.byteorder != "little":
        out.byteswap()
    return out.tobytes()


def build_asset_pack(out_dir: Path) -> tuple[Path, list[dict], Path | None]:
    """Package ViceNano M8 converted assets (.nantex PAL8 textures, .dff models,
    and asset-provenance.json) into both individual files and a 4 KB-sector-aligned
    contiguous bundle (vicenano.pak, magic 'NPAK' v1, see npak.py).

    The Vice City street corner of world mode 4 (vcworld.bin, built from the
    converted North Beach building + road headers) also goes into a small
    runtime pack, vcworld.pak: NanoOSLoader appends it to kernel.bin and
    NanoOS's startup.s moves it to __asset_base (nanoos/src/asset.h).
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    gen_dir = VICENANO_M8_DIR / "generated"

    asset_names = [
        "nbdecoshop01.nantex",
        "nbdecoshop02.nantex",
        "nbdecoshop03.nantex",
        "nbdecoshop01.dff",
        "nbdecoshop02.dff",
        "nbdecoshop03.dff",
    ]
    prov_path = VICENANO_M8_DIR / "asset-provenance.json"

    entries = []
    for name in asset_names:
        src = gen_dir / name
        if not src.exists():
            raise FileNotFoundError(f"Missing generated ViceNano asset: {src}")
        shutil.copyfile(src, out_dir / name)
        entries.append((name, src.read_bytes(), "nantex" if name.endswith(".nantex") else "dff"))

    vc_pak = out_dir / "vcworld.pak"
    # the Drivers page audio test clip rides in the runtime pack (NanoOS has
    # no file system; the loader reads the pack from disk), and also ships
    # as a plain file next to the other assets
    pcm = make_test_pcm()
    audio_dir = out_dir.parent / "audio"
    runtime = []
    if pcm:
        audio_dir.mkdir(parents=True, exist_ok=True)
        (audio_dir / "test.pcm").write_bytes(pcm)
        runtime = [("test.pcm", pcm, "pcm s16le 22050 mono")]
    elif (audio_dir / "test.pcm").exists():
        (audio_dir / "test.pcm").unlink()
    vc_tool = VICENANO_M8_DIR / "tools/make_vc_world.py"
    vc_srcs = [VC_CONVERTED_DIR / "building-world.h", VC_CONVERTED_DIR / "road-world.h"]
    if vc_tool.exists() and all(p.exists() for p in vc_srcs):
        vc_bin = BUILD_DIR / "vcworld.bin"
        # every *-world.h of the block in the directory (make_vc_world.py MODELS)
        subprocess.run([sys.executable, str(vc_tool), str(VC_CONVERTED_DIR), str(vc_bin)], check=True)
        vc_data = vc_bin.read_bytes()
        # mode 3's 112-triangle preview (NVP1), read from the same pack
        pv_bin = BUILD_DIR / "preview.bin"
        subprocess.run([sys.executable, str(VICENANO_M8_DIR / "tools/make_world_preview.py"),
                        str(VICENANO_M8_DIR / "generated/nbdecoshop01.h"),
                        str(BUILD_DIR / "world_preview.h"), str(pv_bin)], check=True)
        pv_data = pv_bin.read_bytes()
        entries.append(("vcworld.bin", vc_data, "nvw2"))
        entries.append(("preview.bin", pv_data, "nvp1"))
        runtime = [("vcworld.bin", vc_data, "nvw2"), ("preview.bin", pv_data, "nvp1")] + runtime
    else:
        print(f"WARNING: no converted Vice City assets in {VC_CONVERTED_DIR}; "
              "world mode 4 will show the fallback preview")
    if runtime:
        write_npak(vc_pak, runtime)
    else:
        vc_pak = None

    if prov_path.exists():
        shutil.copyfile(prov_path, out_dir / "asset-provenance.json")
        entries.append(("asset-provenance.json", prov_path.read_bytes(), "manifest"))

    pak_path = out_dir / "vicenano.pak"
    return pak_path, write_npak(pak_path, entries), vc_pak


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cross", default=os.environ.get("CROSS_COMPILE", "arm-none-eabi-"))
    ap.add_argument("--clean", action="store_true", help="Clean build and dist directories first")
    ap.add_argument("--allow-slim-overflow", action="store_true",
                    help="Warn instead of failing when the emulator-only slim kernel/chainloader "
                         "exceed their world.bin/Blocks.hbapp slots (device bundle is still written)")
    args = ap.parse_args()

    if args.clean:
        shutil.rmtree(BUILD_DIR, ignore_errors=True)
        shutil.rmtree(DIST_DIR, ignore_errors=True)

    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    DIST_DIR.mkdir(parents=True, exist_ok=True)

    # 1. Build full combined NanoOS kernel (all 8 drivers + DevMode Drivers page + ViceNano M8)
    kernel_full_bin, chain_full_hbapp = build_kernel_variant(args.cross, "full", include_drivers_page=True)

    # 2. Build slim combined NanoOS kernel (fits inside 20,708 B Blocks.hbapp emulator slot)
    kernel_slim_bin, chain_slim_hbapp = build_kernel_variant(args.cross, "slim", include_drivers_page=False)

    # 3. Build NanoOSLoader variants
    loader_dev_hbapp, loader_emu_full_hbapp, loader_emu_slim_hbapp = build_loaders()

    # 4. Populate dist/Apps/ directory structure (mirrors /mnt/ipod/Apps/ 1:1)
    apps_dir = DIST_DIR / "Apps"
    exec_dir = apps_dir / "Executables"
    icons_dir = apps_dir / "Icons"
    nanoos_dir = apps_dir / "NanoOS"
    assets_dir = nanoos_dir / "assets/vicenano"

    for d in (exec_dir, icons_dir, nanoos_dir, assets_dir):
        d.mkdir(parents=True, exist_ok=True)

    dst_kernel = nanoos_dir / "kernel.bin"
    shutil.copyfile(kernel_full_bin, dst_kernel)

    dst_loader = exec_dir / "NanoOS.hbapp"
    shutil.copyfile(loader_dev_hbapp, dst_loader)

    icon_src = BACKUP_DIR / "Icons/NanoOS.bin"
    dst_icon = icons_dir / "NanoOS.bin"
    if icon_src.exists():
        shutil.copyfile(icon_src, dst_icon)

    emu_dist = DIST_DIR / "emu"
    emu_dist.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(chain_slim_hbapp, emu_dist / "chainloader.hbapp")
    shutil.copyfile(chain_full_hbapp, emu_dist / "chainloader-full.hbapp")
    shutil.copyfile(kernel_slim_bin, emu_dist / "kernel-slim.bin")
    shutil.copyfile(loader_emu_full_hbapp, emu_dist / "NanoOSLoader-emu.hbapp")
    shutil.copyfile(loader_emu_slim_hbapp, emu_dist / "NanoOSLoader-emu-world.hbapp")

    # vcperf.hbapp embeds local build paths: not shipped in dist (stays in
    # the ViceNano m8 build); drop a copy left by older builds
    (emu_dist / "vcperf-retailos.hbapp").unlink(missing_ok=True)

    # 5. Package ViceNano assets
    pak_path, pak_entries, vc_pak = build_asset_pack(assets_dir)

    # 6. Verify size budgets and write MANIFEST.json + SHA256SUMS
    ksize_full = dst_kernel.stat().st_size
    ksize_slim = kernel_slim_bin.stat().st_size
    csize_full = chain_full_hbapp.stat().st_size
    csize_slim = chain_slim_hbapp.stat().st_size
    lsize = dst_loader.stat().st_size

    assert ksize_full <= VROM0_BIN_CAPACITY, (
        f"Full kernel.bin ({ksize_full} B) exceeds 64 KiB fast region / vrom0.bin ({VROM0_BIN_CAPACITY} B)"
    )
    slim_errors = []
    # chainloader-full.hbapp is an emulator-only artifact (no patch script
    # consumes it); the device kernel is checked against vrom0/64 KiB above.
    if csize_full > GAMEBOY_HBAPP_CAPACITY:
        slim_errors.append(f"chainloader-full.hbapp ({csize_full} B) exceeds Game Boy.hbapp slot ({GAMEBOY_HBAPP_CAPACITY} B)")
    if ksize_slim > WORLD_BIN_CAPACITY:
        slim_errors.append(f"Slim kernel.bin ({ksize_slim} B) exceeds world.bin slot ({WORLD_BIN_CAPACITY} B)")
    if csize_slim > BLOCKS_HBAPP_CAPACITY:
        slim_errors.append(f"Slim chainloader.hbapp ({csize_slim} B) exceeds Blocks.hbapp slot ({BLOCKS_HBAPP_CAPACITY} B)")
    for msg in slim_errors:
        if not args.allow_slim_overflow:
            raise AssertionError(msg)
        print(f"WARNING: {msg}; emulator slim artifacts are unusable, device bundle unaffected")
    assert lsize <= GAMEBOY_HBAPP_CAPACITY, (
        f"NanoOS.hbapp ({lsize} B) exceeds Game Boy.hbapp slot ({GAMEBOY_HBAPP_CAPACITY} B)"
    )

    files_manifest = []
    sha_lines = []
    for fpath in sorted(apps_dir.rglob("*")):
        if fpath.is_file() and fpath.name not in ("MANIFEST.json", "SHA256SUMS"):
            rel_apps = "/" + fpath.relative_to(DIST_DIR).as_posix()
            digest = sha256_file(fpath)
            fsize = fpath.stat().st_size
            files_manifest.append({
                "device_path": rel_apps,
                "bundle_relpath": fpath.relative_to(DIST_DIR).as_posix(),
                "size_bytes": fsize,
                "clusters_4k": (fsize + 4095) // 4096,
                "sha256": digest,
            })
            sha_lines.append(f"{digest}  {fpath.relative_to(DIST_DIR).as_posix()}")

    manifest = {
        "bundle_version": "0.0.3-beta",
        "kernel": {
            "device_path": "/Apps/NanoOS/kernel.bin",
            "load_address": "0x08000000",
            "size_bytes": ksize_full,
            "max_fast_region_bytes": 65536,
            "loader_max_bytes": 2 * 1024 * 1024,
            "features": [
                "Clock selector (CPU/GPU MHz: STOCK 342/133, no CLKCON writes; SAFE 432/144, GPU 171 if the CPU retune is refused; MAX 532/266; R86 report)",
                "Touch carousel launcher, 5 icons (StockOS, ViceNano, PS1Emu, Settings = Bluetooth Central, DevMode); tap/swipe or Vol/Home",
                "ViceNano M8 (Title screen, MBX colored triangle, MBX 8x8 RGB565 texture quad, MBX 3D textured North Beach world preview)",
                "ViceNano M8 mode 4: walkable Vice City North Beach street block (nbdecoshop01 and roads nb_road15, nb_road02, nbroadnew1, faithful meshes in 8 m chunks; 32x32 textures; BT gamepad walk) from the vcworld.pak runtime pack",
                "DevMode Page 1 Stats (Clocks, RAM/Heap, FPS, LCD DSI stats, PMU I2C diagnostics)",
                "DevMode Page 2 Drivers (LCD, Touch, Buttons/Temp, Audio codec + test.pcm I2S/DMA playback, PPN NAND + R81, USB, BT HID, MBX)",
                "DevMode Page 3 GPU (Live animated PowerVR MBX Gouraud triangle test)",
                "Safe watchdog/PMGR warm reset return to stock RetailOS",
            ],
            "sha256": sha256_file(dst_kernel),
        },
        "loader": {
            "device_path": "/Apps/Executables/NanoOS.hbapp",
            "icon_path": "/Apps/Icons/NanoOS.bin",
            "bundle_id": "org.nanoapps.nanoosloader",
            "sbid": "hb.NanoOSLoader",
            "label": "NanoOS",
            "kind": 1,
            "size_bytes": lsize,
            "sha256": sha256_file(dst_loader),
        },
        "emulator_artifacts": {
            "chainloader_slim_hbapp": {
                "path": "emu/chainloader.hbapp",
                "target_slot": "/Apps/Executables/Blocks.hbapp",
                "slot_capacity_bytes": BLOCKS_HBAPP_CAPACITY,
                "size_bytes": csize_slim,
                "sha256": sha256_file(emu_dist / "chainloader.hbapp"),
            },
            "chainloader_full_hbapp": {
                "path": "emu/chainloader-full.hbapp",
                "target_slot": "/Apps/Executables/Game Boy.hbapp",
                "slot_capacity_bytes": GAMEBOY_HBAPP_CAPACITY,
                "size_bytes": csize_full,
                "sha256": sha256_file(emu_dist / "chainloader-full.hbapp"),
            },
            "loader_emu_hbapp": {
                "path": "emu/NanoOSLoader-emu.hbapp",
                "target_slot": "/Apps/Executables/Game Boy.hbapp",
                "kernel_slot": KERNEL_SLOT_EMU_FULL,
                "kernel_slot_capacity_bytes": VROM0_BIN_CAPACITY,
                "slot_capacity_bytes": GAMEBOY_HBAPP_CAPACITY,
                "size_bytes": (emu_dist / "NanoOSLoader-emu.hbapp").stat().st_size,
                "sha256": sha256_file(emu_dist / "NanoOSLoader-emu.hbapp"),
            },
        },
        "vicenano_asset_pack": {
            "device_path": "/Apps/NanoOS/assets/vicenano/vicenano.pak",
            "format": "NPAK v1 (4096-byte sector aligned)",
            "size_bytes": pak_path.stat().st_size,
            "sha256": sha256_file(pak_path),
            "entries": pak_entries,
        },
        "vicenano_runtime_pack": {
            "device_path": "/Apps/NanoOS/assets/vicenano/vcworld.pak",
            "format": "NPAK v1, appended to kernel.bin by NanoOSLoader, moved to 0x08400000 by startup.s",
            "size_bytes": vc_pak.stat().st_size if vc_pak else 0,
            "sha256": sha256_file(vc_pak) if vc_pak else None,
        },
        "device_files": files_manifest,
    }

    (nanoos_dir / "MANIFEST.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (nanoos_dir / "SHA256SUMS").write_text("\n".join(sha_lines) + "\n")

    print("\n==> Combined bundle built successfully in:", DIST_DIR)
    print(f"    Full Kernel (/Apps/NanoOS/kernel.bin)     : {ksize_full:7d} B <= 65536 B (SHA-256: {manifest['kernel']['sha256'][:16]}...)")
    print(f"    Slim Kernel (emu/kernel-slim.bin)         : {ksize_slim:7d} B <= 24608 B")
    print(f"    Loader (/Apps/Executables/NanoOS.hbapp)   : {lsize:7d} B <= 31180 B (SHA-256: {manifest['loader']['sha256'][:16]}...)")
    print(f"    Slim Chainloader (emu/chainloader.hbapp)  : {csize_slim:7d} B <= 20708 B (Blocks.hbapp slot)")
    print(f"    Full Chainloader (chainloader-full.hbapp) : {csize_full:7d} B <= 31180 B (Game Boy.hbapp slot)")
    print(f"    Asset pack (vicenano.pak, NPAK v1)        : {pak_path.stat().st_size:7d} B ({len(pak_entries)} entries, 4 KB aligned)")
    if vc_pak:
        print(f"    Runtime pack (vcworld.pak, NPAK v1)       : {vc_pak.stat().st_size:7d} B (VC North Beach street block + mode 3 preview)")


if __name__ == "__main__":
    main()
