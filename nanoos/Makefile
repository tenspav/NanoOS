CROSS_COMPILE ?= arm-none-eabi-
CC := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
# -mno-unaligned-access: with the MMU off all memory is Strongly-ordered,
# where an unaligned load faults (GCC merges byte loads into ldrh/ldr).
CFLAGS := -mcpu=cortex-a5 -marm -O2 -mno-unaligned-access -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-pic -fno-common -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -MMD -MP
OBJECTS := $(addprefix build/,startup.o main.o lcd.o font.o input.o reset.o devmode.o mem.o timer.o fmt.o spi.o touch.o buttons.o audio.o nand.o usb.o bt.o mbx.o mbx_test.o drivers.o)
# Size: the chainloader package must fit the Blocks.hbapp slot it replaces
# in the emulator test (20708 bytes, emulator/tools/disk_patch.py). Code
# off the pixel/bus hot paths is Thumb-2 (-O2: -Os calls libgcc for divisions by constants); lcd, input, timer, font
# stay ARM -O2 (tuned on the device).
THUMB_OBJS := $(addprefix build/,main.o devmode.o fmt.o spi.o touch.o buttons.o audio.o nand.o usb.o bt.o mbx.o mbx_test.o drivers.o)
$(THUMB_OBJS): CFLAGS += -mthumb
# MBX: register pokes and a test page, no divisions: -Os
build/mbx.o build/mbx_test.o: CFLAGS += -Os
.PHONY: all clean
all: build/kernel.bin
build:
	mkdir -p $@
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
-include $(OBJECTS:.o=.d)

# Embed this kernel as the existing chainloader's fallback. Build in isolation
# so probe/chainloader's hello payload and regression artifacts stay intact.
NANOAPPS ?= $(abspath ../../NanoApps)
CHAIN := ../probe/chainloader
APPFLAGS := -mcpu=cortex-a8 -mthumb -mfpu=neon -Os -ffreestanding -fno-builtin -fno-pic -fno-common -ffunction-sections -fdata-sections -I$(NANOAPPS)/sdk -I$(NANOAPPS)/sdk/freestanding -I$(CHAIN)
.PHONY: chainloader
chainloader: build/chainloader.hbapp
build/chainloader-app.o: $(CHAIN)/chainloader.c | build
	$(CC) $(APPFLAGS) -c $< -o $@
build/shutdown-app.o: $(CHAIN)/shutdown.c | build
	$(CC) $(APPFLAGS) -c $< -o $@
build/hb_fs.o: $(NANOAPPS)/sdk/hb_fs.c | build
	$(CC) $(APPFLAGS) -c $< -o $@
build/handoff.o: $(CHAIN)/handoff.S build/kernel.bin
	cp build/kernel.bin build/payload.bin
	cd build && $(CC) $(APPFLAGS) -c ../../probe/chainloader/handoff.S -o handoff.o
build/chainloader.elf: build/chainloader-app.o build/shutdown-app.o build/hb_fs.o build/handoff.o
	$(CC) $(APPFLAGS) -nostdlib -Wl,--gc-sections,--build-id=none,--no-undefined,-q,-e,payload_entry,-Ttext,0 $^ -lgcc -o $@
build/chainloader.hbapp: build/chainloader.elf
	python3 $(NANOAPPS)/tools/mkrelocapp.py $< $@ payload_entry
