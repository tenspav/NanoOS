CROSS_COMPILE ?= arm-none-eabi-
CC := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
CFLAGS := -mcpu=cortex-a5 -marm -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-pic -fno-common -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -MMD -MP
OBJECTS := $(addprefix build/,startup.o main.o lcd.o font.o input.o reset.o devmode.o mem.o)
.PHONY: all clean
all: build/kernel.bin
build:
	mkdir -p $@
build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/startup.o: src/startup.s | build
	$(CC) $(CFLAGS) -c $< -o $@
build/kernel.elf: $(OBJECTS) kernel.ld
	$(CC) $(CFLAGS) -nostdlib -Wl,-T,kernel.ld,--gc-sections,--build-id=none,--no-undefined,-Map,build/kernel.map $(OBJECTS) -o $@
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
