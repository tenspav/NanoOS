# Installing NanoOS Loader on a jailbroken iPod nano 7

This app is a RetailOS homebrew that boots the bare-metal NanoOS kernel.  It is packaged with the NanoApps toolchain and installed through the same homebrew resident as other apps.

## What you need

- A jailbroken iPod nano 7 with the NanoApps resident installed.
- The `NanoApps` repo checked out next to this repo (i.e. `.../NanoApps`).
- `arm-none-eabi-gcc`, Python 3 + Pillow, and the NanoApps build deps.

## Build

From this directory:

```sh
make -C ../../nanoos all   # NanoRework/nanoos/build/kernel.bin
make -C . all              # NanoRework/apps/NanoOSLoader/build/NanoOSLoader.hbapp
```

## Package

The easiest way to produce a complete on-disk `/Apps` tree is:

```sh
./package.sh
```

This script:

1. Builds the kernel and loader.
2. Symlinks this directory into `NanoApps/apps/NanoOSLoader` so `tools/build_apps.py` can find it.
3. Runs `NanoApps/tools/build_apps.py NanoOSLoader` to create `/tmp/hbapps/Apps/{AllApps.pack,Executables,Icons}`.
4. Copies `NanoRework/nanoos/build/kernel.bin` to `/tmp/hbapps/Apps/NanoOS/kernel.bin`, which is where the loader looks for it.

## Install

### Option A: manual copy (any platform)

Mount the iPod's FAT partition read-write (for example at `/mnt/ipod`) and copy the staged tree:

```sh
sudo mkdir -p /mnt/ipod/Apps
sudo cp -r /tmp/hbapps/Apps /mnt/ipod/
sudo sync
```

Make sure `/mnt/ipod/Apps/NanoOS/kernel.bin` exists after the copy.

### Option B: NanoApps `start` tool (Linux host with the iPod attached)

From the `NanoApps` repo root:

```sh
./start install NanoOSLoader
```

This builds, packs, and writes the loader to the device, then re-execs the resident so the icon appears.  `start` does **not** copy the kernel, so afterwards run:

```sh
# while the iPod disk is still mounted by start
sudo cp NanoRework/nanoos/build/kernel.bin /mnt/nanoapps-ipod/Apps/NanoOS/kernel.bin
sudo sync
```

(Replace `/mnt/nanoapps-ipod` with your actual mount point if you set `IPOD_MOUNT`.)

## After install

On the iPod:

1. Return to the RetailOS Home screen.
2. The new icon is labelled **NanoOS** (gamepad glyph, blue disc).
3. Open it, select ViceNano / PS1Emu / Settings, and press **Home** to boot NanoOS.

Nothing is written to `/iPod_Control`; a reset returns to RetailOS.

## Updating the kernel

After rebuilding `NanoRework/nanoos/build/kernel.bin`, copy the new file to `/Apps/NanoOS/kernel.bin` on the device.  You do not need to re-install the loader app unless the loader itself changed.
