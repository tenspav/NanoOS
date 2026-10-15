#!/usr/bin/env bash
# Package the NanoOS Loader app + kernel for installation on a jailbroken iPod nano 7.
# Produces /tmp/hbapps, a tree that mirrors the device's /Apps directory.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
LOADER_DIR=$(realpath "$SCRIPT_DIR")
NANOOS=$(realpath "$LOADER_DIR/../nanoos")
NANOAPPS=$(realpath "$LOADER_DIR/../../NanoApps")

echo "==> build NanoOS kernel"
make -s -C "$NANOOS" all

echo "==> build NanoOS Loader"
make -s -C "$LOADER_DIR" all

echo "==> register loader with NanoApps packaging"
mkdir -p "$NANOAPPS/apps"
ln -srf "$LOADER_DIR" "$NANOAPPS/apps/NanoOSLoader"

echo "==> pack app"
python3 "$NANOAPPS/tools/build_apps.py" NanoOSLoader

echo "==> stage kernel for /Apps/NanoOS/kernel.bin"
mkdir -p /tmp/hbapps/Apps/NanoOS
cp -f "$NANOOS/build/kernel.bin" /tmp/hbapps/Apps/NanoOS/kernel.bin

echo
echo "Staging tree ready: /tmp/hbapps"
echo "  app pack  : /tmp/hbapps/Apps/AllApps.pack"
echo "  executable: /tmp/hbapps/Apps/Executables/NanoOS.hbapp"
echo "  icon      : /tmp/hbapps/Apps/Icons/NanoOS.bin"
echo "  kernel    : /tmp/hbapps/Apps/NanoOS/kernel.bin"
echo
echo "Deploy to the iPod by mounting its FAT partition and running:"
echo "  sudo cp -r /tmp/hbapps/Apps /mnt/ipod/"
echo "or use the NanoApps start tool:"
echo "  (cd $NANOAPPS && ./start install NanoOSLoader)"
echo "and then copy the kernel to /Apps/NanoOS/kernel.bin on the device."
