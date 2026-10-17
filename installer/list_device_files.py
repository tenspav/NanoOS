#!/usr/bin/env python3
"""list_device_files.py -- List all files required on the real iPod nano 7G
FAT32 partition (/Apps/...) to install NanoOS + ViceNano M8, with sizes,
4 KB cluster counts, SHA-256 checksums, and optional comparison against a
mounted /Apps directory or backup snapshot (read-only).
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

INSTALLER_DIR = Path(__file__).resolve().parent
NANOOS_DIR = INSTALLER_DIR.parent
REWORK_DIR = NANOOS_DIR.parent
DIST_DIR = INSTALLER_DIR / "dist"
MANIFEST_PATH = DIST_DIR / "Apps/NanoOS/MANIFEST.json"
DEFAULT_BACKUP = REWORK_DIR / "dumps/apps-backup-20261015T115834Z-pre-nanoos"

sys.path.insert(0, str(REWORK_DIR / "probe"))
try:
    from packtool import parse_pack
except ImportError:
    parse_pack = None


ROLE_DESCRIPTIONS = {
    "/Apps/NanoOS/kernel.bin": (
        "REQUIRED (update + fresh)",
        "Combined NanoOS bare-metal kernel + 8 hardware drivers + ViceNano M8",
    ),
    "/Apps/Executables/NanoOS.hbapp": (
        "REQUIRED (if loader updated)",
        "NanoOSLoader v0.0.2 beta (HBAppKind 1 raw-surface open-boot loader)",
    ),
    "/Apps/Icons/NanoOS.bin": (
        "FRESH INSTALL ONLY",
        "112x112 SilverDB Home screen icon for NanoOS",
    ),
    "/Apps/NanoOS/assets/vicenano/vicenano.pak": (
        "RECOMMENDED (assets)",
        "4 KB-aligned NPAK v1 contiguous ViceNano M8 world asset archive",
    ),
    "/Apps/NanoOS/assets/vicenano/vcworld.pak": (
        "RECOMMENDED (ViceNano mode 4)",
        "NPAK v1 runtime pack the loader appends to kernel.bin: Vice City street corner",
    ),
}


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_pack_for_nanoos(pack_path: Path) -> str:
    if not pack_path.exists():
        return "MISSING"
    if parse_pack is None:
        return "PRESENT (unparsed)"
    try:
        _, apps = parse_pack(pack_path.read_bytes())
        for idx, a in enumerate(apps):
            if a["name"] == "NanoOS" or a["sbid"].lower() == "hb.nanoosloader":
                return (
                    f"REGISTERED at index {idx}/{len(apps)} "
                    f"(name={a['name']!r}, sbid={a['sbid']!r}, "
                    f"app_id=0x{a['app_id']:08x}, kind={a['kind']})"
                )
        return f"NOT REGISTERED ({len(apps)} other apps in pack)"
    except Exception as exc:
        return f"ERROR parsing pack: {exc}"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--check-dir",
        type=Path,
        default=None,
        help="Optional read-only path to compare against (e.g. /mnt/ipod/Apps or dumps/apps-backup-...)",
    )
    ap.add_argument(
        "--check-backup",
        action="store_true",
        help="Compare against dumps/apps-backup-20261015T115834Z-pre-nanoos (read-only)",
    )
    ap.add_argument(
        "--json",
        action="store_true",
        help="Emit machine-readable JSON output",
    )
    args = ap.parse_args()

    if not MANIFEST_PATH.exists():
        print("==> Staged bundle not found; running build_payload.py first...", file=sys.stderr)
        subprocess.run([sys.executable, str(INSTALLER_DIR / "build_payload.py")], check=True)

    manifest = json.loads(MANIFEST_PATH.read_text())
    compare_root = args.check_dir or (DEFAULT_BACKUP if args.check_backup else None)

    if args.json:
        print(json.dumps(manifest, indent=2))
        return

    print("=" * 96)
    print(f"NanoOS + ViceNano M8 Device File Manifest (Bundle: {manifest['bundle_version']})")
    print(f"Staging Root: {DIST_DIR / 'Apps'}")
    print("=" * 96)
    print(f"{'Device Target Path (/mnt/ipod/...)':<46} {'Bytes':>9} {'4K Blk':>6}  {'SHA-256 (first 16)':<18}  {'Install Scope'}")
    print("-" * 96)

    total_bytes = 0
    total_blocks = 0
    for f in manifest["device_files"]:
        dpath = f["device_path"]
        size = f["size_bytes"]
        blks = f["clusters_4k"]
        sha = f["sha256"]
        scope, _ = ROLE_DESCRIPTIONS.get(
            dpath,
            ("OPTIONAL (asset file)", "Individual extracted ViceNano M8 asset"),
        )
        total_bytes += size
        total_blocks += blks
        print(f"{dpath:<46} {size:9d} {blks:6d}  {sha[:16]}..  {scope}")

    # Include MANIFEST.json and SHA256SUMS
    for meta_name in ("MANIFEST.json", "SHA256SUMS"):
        mpath = DIST_DIR / "Apps/NanoOS" / meta_name
        if mpath.exists():
            msize = mpath.stat().st_size
            mblks = (msize + 4095) // 4096
            msha = sha256_file(mpath)
            total_bytes += msize
            total_blocks += mblks
            print(
                f"{'/Apps/NanoOS/' + meta_name:<46} {msize:9d} {mblks:6d}  {msha[:16]}..  "
                f"RECOMMENDED (verification)"
            )

    print("-" * 96)
    print(
        f"{'TOTAL STAGED ON-DEVICE FOOTPRINT':<46} {total_bytes:9d} {total_blocks:6d}  "
        f"(~{total_blocks * 4:.1f} KiB on FAT32)"
    )
    print("=" * 96)

    if compare_root is not None:
        # Normalize compare_root: allow either /mnt/ipod or /mnt/ipod/Apps
        apps_root = compare_root / "Apps" if (compare_root / "Apps").is_dir() else compare_root
        print(f"\nRead-Only Comparison Against: {apps_root}")
        print("-" * 96)
        for pack_name in ("AllApps-B.pack", "AllApps.pack"):
            status = check_pack_for_nanoos(apps_root / pack_name)
            print(f"  Pack /Apps/{pack_name:<16}: {status}")
        print()
        for f in manifest["device_files"]:
            rel_under_apps = f["device_path"].removeprefix("/Apps/")
            existing = apps_root / rel_under_apps
            if not existing.exists():
                state = "NEW (not present on target)"
            else:
                ex_sha = sha256_file(existing)
                if ex_sha == f["sha256"]:
                    state = "UP-TO-DATE (SHA-256 matches)"
                else:
                    state = f"CHANGED (target={ existing.stat().st_size } B, sha={ex_sha[:12]}..)"
            print(f"  {f['device_path']:<46} -> {state}")
        print("-" * 96)

    print("\nQuick Real-Device Install Commands (run ONLY when iPod is mounted at /mnt/ipod):")
    print("  # 1. Fast in-place update (when NanoOS icon is already on the iPod Home screen):")
    print(f"  sudo mkdir -p /mnt/ipod/Apps/NanoOS/assets/vicenano")
    print(f"  sudo cp {DIST_DIR}/Apps/NanoOS/kernel.bin /mnt/ipod/Apps/NanoOS/kernel.bin")
    print(f"  sudo cp {DIST_DIR}/Apps/Executables/NanoOS.hbapp /mnt/ipod/Apps/Executables/NanoOS.hbapp")
    print(f"  sudo cp -r {DIST_DIR}/Apps/NanoOS/assets /mnt/ipod/Apps/NanoOS/")
    print(f"  sudo cp {DIST_DIR}/Apps/NanoOS/MANIFEST.json {DIST_DIR}/Apps/NanoOS/SHA256SUMS /mnt/ipod/Apps/NanoOS/")
    print("  (cd /mnt/ipod && sha256sum -c Apps/NanoOS/SHA256SUMS)")
    print("  sync && sudo umount /mnt/ipod")


if __name__ == "__main__":
    main()
