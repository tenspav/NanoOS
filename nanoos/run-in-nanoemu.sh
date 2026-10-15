#!/bin/bash
# Emulator only: original disk is read-only; replacements live in a patch file.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
make -C "$HERE" all chainloader
cd "$ROOT"
python3 emulator/tools/disk_patch.py dumps/ipod-disk.dd \
    "/Apps/Executables/Blocks.hbapp=$HERE/build/chainloader.hbapp" \
    -o "$HERE/build/chainloader.patch"
exec python3 "$HERE/test_nanoemu.py" "$@"
