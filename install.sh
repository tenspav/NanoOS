#!/bin/bash
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"

echo "==> NanoOS installer"
echo "==> This assumes a jailbroken iPod nano 7G is connected and mounted."

# Build and package
echo "==> Building NanoOS kernel + loader..."
cd "$ROOT/NanoOSLoader"
./package.sh

# Detect iPod mount
MOUNT=""
for candidate in /mnt/ipod /media/ipod /tmp/ipodmnt; do
    if [ -d "$candidate/Apps" ]; then
        MOUNT="$candidate"
        break
    fi
done

if [ -z "$MOUNT" ]; then
    echo "!! Could not find mounted iPod at /mnt/ipod, /media/ipod or /tmp/ipodmnt"
    echo "!! Please mount the iPod FAT partition and re-run."
    exit 1
fi

echo "==> Installing to $MOUNT..."
sudo cp -r /tmp/hbapps/Apps "$MOUNT/"

echo "==> Done. Reboot the iPod and look for the NanoOS icon on the home screen."
