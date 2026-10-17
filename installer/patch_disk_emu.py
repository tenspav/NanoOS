#!/usr/bin/env python3
"""patch_disk_emu.py -- Create non-destructive N7PATCH1 NAND overlay files
from the staged NanoOS + ViceNano bundle for testing in NanoEmu.

Never modifies dumps/ipod-disk.dd. Generates three patch files in
nanoos/installer/dist/emu/:
  1. emu-chainloader.patch : Overlays /Apps/Executables/Blocks.hbapp with
                             dist/emu/chainloader.hbapp (direct boot on Blocks tap)
  2. emu-loader.patch      : Overlays /Apps/Executables/Game Boy.hbapp with
                             dist/emu/NanoOSLoader-emu.hbapp and
                             /Apps/Data/Blocks/world.bin with dist/Apps/NanoOS/kernel.bin
                             (full NanoOSLoader BUSY -> disk read -> handoff flow)
  3. emu-combined.patch    : Overlays BOTH slots at once so either Blocks or
                             Game Boy can be tapped in the same NanoEmu session.
"""
import argparse
import struct
import subprocess
import sys
from pathlib import Path

INSTALLER_DIR = Path(__file__).resolve().parent
NANOOS_DIR = INSTALLER_DIR.parent
REWORK_DIR = NANOOS_DIR.parent
DIST_DIR = INSTALLER_DIR / "dist"
EMU_DIR = DIST_DIR / "emu"

DISK_IMG_DEFAULT = REWORK_DIR / "dumps/ipod-disk.dd"
DISK_PATCH_TOOL = REWORK_DIR / "emulator/tools/disk_patch.py"
KERNEL_SLOT_EMU_FULL = "/Apps/Data/Probe/vrom0.bin"
KERNEL_SLOT_EMU_SLIM = "/Apps/Data/Blocks/world.bin"


def verify_patch_file(patch_path: Path) -> tuple[int, list[int]]:
    """Verify N7PATCH1 header and return (block_count, list_of_lbas)."""
    data = patch_path.read_bytes()
    if len(data) < 8 or data[:8] != b"N7PATCH1":
        raise ValueError(f"{patch_path}: invalid N7PATCH1 header")
    body = data[8:]
    rec_size = 8 + 4096
    if len(body) % rec_size != 0:
        raise ValueError(f"{patch_path}: trailing bytes in N7PATCH1 payload")
    count = len(body) // rec_size
    lbas = [
        struct.unpack_from("<Q", body, i * rec_size)[0]
        for i in range(count)
    ]
    return count, lbas


def run_disk_patch(disk_img: Path, out_patch: Path, replacements: list[str]) -> None:
    cmd = [
        sys.executable,
        str(DISK_PATCH_TOOL),
        str(disk_img),
        *replacements,
        "-o",
        str(out_patch),
    ]
    subprocess.run(cmd, cwd=REWORK_DIR, check=True)
    count, lbas = verify_patch_file(out_patch)
    lba_range = f"0x{min(lbas):x}..0x{max(lbas):x}" if lbas else "empty"
    print(f"    Verified {out_patch.name}: {count} x 4 KB blocks (FTL LBA {lba_range})")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--disk",
        type=Path,
        default=DISK_IMG_DEFAULT,
        help="Read-only FAT32 disk dump (default: dumps/ipod-disk.dd)",
    )
    ap.add_argument(
        "--mode",
        choices=["chainloader", "loader", "combined", "all"],
        default="all",
        help="Which emulator patch file(s) to generate (default: all)",
    )
    ap.add_argument(
        "--build-if-missing",
        action="store_true",
        default=True,
        help="Run build_payload.py automatically if dist/ artifacts are missing",
    )
    args = ap.parse_args()

    kernel_bin = DIST_DIR / "Apps/NanoOS/kernel.bin"
    kernel_slim_bin = EMU_DIR / "kernel-slim.bin"
    chain_hbapp = EMU_DIR / "chainloader.hbapp"
    loader_emu = EMU_DIR / "NanoOSLoader-emu.hbapp"

    if args.build_if_missing and not (kernel_bin.exists() and kernel_slim_bin.exists() and chain_hbapp.exists() and loader_emu.exists()):
        print("==> Staged artifacts missing; building combined payload first...")
        subprocess.run([sys.executable, str(INSTALLER_DIR / "build_payload.py")], check=True)

    if not args.disk.exists():
        sys.exit(f"Disk image not found: {args.disk}")

    EMU_DIR.mkdir(parents=True, exist_ok=True)
    orig_stat = (args.disk.stat().st_size, args.disk.stat().st_mtime_ns)

    if args.mode in ("chainloader", "all"):
        print("\n==> Generating emu-chainloader.patch (Blocks slot -> embedded kernel+ViceNano)")
        run_disk_patch(
            args.disk,
            EMU_DIR / "emu-chainloader.patch",
            [f"/Apps/Executables/Blocks.hbapp={chain_hbapp}"],
        )

    if args.mode in ("loader", "all"):
        print("\n==> Generating emu-loader.patch (Game Boy slot -> NanoOSLoader + vrom0.bin -> full kernel.bin)")
        run_disk_patch(
            args.disk,
            EMU_DIR / "emu-loader.patch",
            [
                f"/Apps/Executables/Game Boy.hbapp={loader_emu}",
                f"{KERNEL_SLOT_EMU_FULL}={kernel_bin}",
                f"{KERNEL_SLOT_EMU_SLIM}={kernel_slim_bin}",
            ],
        )

    if args.mode in ("combined", "all"):
        print("\n==> Generating emu-combined.patch (both Blocks and Game Boy slots patched)")
        run_disk_patch(
            args.disk,
            EMU_DIR / "emu-combined.patch",
            [
                f"/Apps/Executables/Blocks.hbapp={chain_hbapp}",
                f"/Apps/Executables/Game Boy.hbapp={loader_emu}",
                f"{KERNEL_SLOT_EMU_FULL}={kernel_bin}",
                f"{KERNEL_SLOT_EMU_SLIM}={kernel_slim_bin}",
            ],
        )

    after_stat = (args.disk.stat().st_size, args.disk.stat().st_mtime_ns)
    assert orig_stat == after_stat, "dumps/ipod-disk.dd was unexpectedly modified!"

    print("\n==> Source disk image untouched (verified size & mtime).")
    print("==> Example NanoEmu launch command (from NanoRework/):")
    print(
        "    ./emulator/run.sh \\\n"
        "      --ramfull dumps/snap6/ram-full.bin --iram dumps/snap6/iram.bin \\\n"
        "      --ctx emulator/fixups/snap6-ctx.bin --overlay dumps/snap6/overlay.bin \\\n"
        "      --os-poke-file emulator/fixups/snap6-resume.poke --torn-map dumps/snap6/torn-map.bin \\\n"
        "      --nand-miss oracle --nand-disk dumps/ipod-disk.dd --nand-disk-offset 49216 \\\n"
        "      --nand-rsrc emulator/rsrc/rsrc-1.1.2.img1 \\\n"
        f"      --nand-patch { (EMU_DIR / 'emu-combined.patch').relative_to(REWORK_DIR) } \\\n"
        "      -M nano7,mmu=0x08b28000,torn-ignore-pc=0x093d2a80:0x093d4100"
    )


if __name__ == "__main__":
    main()
