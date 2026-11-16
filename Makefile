CROSS_COMPILE ?= arm-none-eabi-
CC := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
# -mno-unaligned-access: with the MMU off (NANOOS_MMU=0) all memory is
# Strongly-ordered, where an unaligned load faults (GCC merges byte loads
# into ldrh/ldr); with it on, the uncached DMA sections still are.
CFLAGS := -mcpu=cortex-a5 -marm -O2 -mno-unaligned-access -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-pic -fno-common -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -Iruntime/gl2 -Iruntime/storage -MMD -MP
OBJECTS := $(addprefix build/,startup.o main.o lcd.o font.o input.o reset.o devmode.o mem.o timer.o fmt.o spi.o touch.o buttons.o audio.o nand.o usb.o bt.o mbx.o mbx_test.o drivers.o clk.o mmu.o game.o asset.o btui.o libc_shim.o gl2rt.o gl2vsc.o gl2api.o gl2tex.o sftl.o disk.o mount.o partition.o hfs.o)
# Size: the chainloader package must fit the Blocks.hbapp slot it replaces
# in the emulator test (20708 bytes, emulator/tools/disk_patch.py). Code
# off the pixel/bus hot paths is Thumb-2 (-O2: -Os calls libgcc for divisions by constants); lcd, input, timer, font
# stay ARM -O2 (tuned on the device).
THUMB_OBJS := $(addprefix build/,main.o devmode.o fmt.o spi.o touch.o buttons.o audio.o nand.o usb.o bt.o mbx.o mbx_test.o drivers.o clk.o mmu.o game.o asset.o gl2vsc.o gl2api.o gl2tex.o sftl.o disk.o mount.o partition.o hfs.o)
$(THUMB_OBJS): CFLAGS += -mthumb
# MBX: register pokes and a test page, no divisions: -Os
build/mbx.o build/mbx_test.o build/mmu.o build/game.o build/asset.o: CFLAGS += -Os
.PHONY: all clean
all: build/kernel.bin
build:
	mkdir -p $@
# Build runtime/compiler from canonical NanoOS/runtime sources.
build/gl2rt.o: runtime/gl2/gl2rt.c runtime/gl2/gl2rt.h | build
	$(CC) $(CFLAGS) -c $< -o $@
build/gl2vsc.o: runtime/gl2/gl2vsc.c runtime/gl2/gl2vsc.h | build
	$(CC) $(CFLAGS) -c $< -o $@
build/gl2api.o: runtime/gl2/gl2api.c runtime/gl2/gl2api.h | build
	$(CC) $(CFLAGS) -c $< -o $@
build/gl2tex.o: runtime/gl2/gl2tex.c runtime/gl2/gl2api.h | build
	$(CC) $(CFLAGS) -c $< -o $@
build/sftl.o: runtime/storage/sftl.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/disk.o: runtime/storage/disk.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/mount.o: runtime/storage/mount.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/partition.o: runtime/storage/partition.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/hfs.o: runtime/storage/hfs.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/startup.o: src/startup.s | build
	$(CC) $(CFLAGS) -c $< -o $@
build/kernel.elf: $(OBJECTS) kernel.ld
	$(CC) $(CFLAGS) -nostdlib -Wl,-T,kernel.ld,--gc-sections,--build-id=none,--no-undefined,-Map,build/kernel.map $(OBJECTS) -lgcc -o $@
	$(CROSS_COMPILE)size $@
build/kernel.bin: build/kernel.elf
	$(OBJCOPY) -O binary $< $@
clean:
	rm -rf build

# Build any NanoOS SDK application (examples/hello_gles2 or external APP_DIR)
APP_DIR ?= examples/hello_gles2
.PHONY: app
app:
	$(MAKE) -C $(APP_DIR) NANOOS_DIR=$(abspath .) native
-include $(OBJECTS:.o=.d)

# Embed this kernel as the existing chainloader's fallback. Build in isolation
# so probe/chainloader's hello payload and regression artifacts stay intact.
NANOAPPS ?= $(abspath ../../NanoApps)
KERNEL_PAYLOAD ?= build/kernel.bin
CHAIN := ../probe/chainloader
APPFLAGS := -mcpu=cortex-a8 -mthumb -mfpu=neon -Os -ffreestanding -fno-builtin -fno-pic -fno-common -ffunction-sections -fdata-sections -I$(NANOAPPS)/sdk -I$(NANOAPPS)/sdk/freestanding -I$(CHAIN)
.PHONY: chainloader
chainloader: build/chainloader.hbapp
build/chainloader-app.o: loader/chainloader.c src/report.h src/checkpoint_report.h src/btoc_report.h | build
	$(CC) $(APPFLAGS) -Isrc -c $< -o $@
build/shutdown-app.o: $(CHAIN)/shutdown.c | build
	$(CC) $(APPFLAGS) -c $< -o $@
build/hb_fs.o: $(NANOAPPS)/sdk/hb_fs.c | build
	$(CC) $(APPFLAGS) -c $< -o $@
build/handoff.o: $(CHAIN)/handoff.S $(KERNEL_PAYLOAD)
	cp $(KERNEL_PAYLOAD) build/payload.bin
	cd build && $(CC) $(APPFLAGS) -c $(abspath $(CHAIN))/handoff.S -o handoff.o
build/chainloader.elf: build/chainloader-app.o build/shutdown-app.o build/hb_fs.o build/handoff.o
	$(CC) $(APPFLAGS) -nostdlib -Wl,--gc-sections,--build-id=none,--no-undefined,-q,-e,payload_entry,-Ttext,0 $^ -lgcc -o $@
build/chainloader.hbapp: build/chainloader.elf
	python3 $(NANOAPPS)/tools/mkrelocapp.py $< $@ payload_entry

# Startup enables scalar VFP; no NEON/hard-float ABI.
build/gl2api.o: CFLAGS += -mfpu=vfpv3-d16 -mfloat-abi=softfp -ffp-contract=off

# Local RAM-only graphics experiments; explicit object rebuild required on change.
build/mbx.o: CFLAGS += $(MBX_EXPERIMENT_FLAGS)

# RAM-only FIFO memory-attribute experiment, kept consistent in both units.
build/mbx.o build/mmu.o: CFLAGS += $(FIFO_EXPERIMENT_FLAGS)
