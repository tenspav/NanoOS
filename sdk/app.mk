# Shared NanoOS Application SDK Build Fragment
# Apps set: NANOOS_DIR, APP_NAME, APP_SRCS (and optional APP_CPPFLAGS, APP_CFLAGS, APP_USE_LIBC)
# and include $(NANOOS_DIR)/sdk/app.mk

.DEFAULT_GOAL := native
CROSS ?= arm-none-eabi-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE := $(CROSS)size

BUILD ?= build
ARCH := -mcpu=cortex-a5 -mthumb -mfloat-abi=soft -mno-unaligned-access
NANO_GL2 := $(NANOOS_DIR)/runtime/gl2
NANO_STOR := $(NANOOS_DIR)/runtime/storage
NANO_SDK := $(NANOOS_DIR)/sdk
NANO_SRC := $(NANOOS_DIR)/src

SDK_LIBC_INCLUDES :=
ifeq ($(APP_USE_LIBC),1)
SDK_LIBC_INCLUDES := -I$(NANO_SDK)/include/libc
endif
SDK_INCLUDES := -I$(NANO_SDK)/include -I$(NANO_GL2)/std -I$(NANO_GL2) -I$(NANO_STOR) -I$(NANO_SRC) $(SDK_LIBC_INCLUDES)

SDK_BASE_FLAGS := $(CPPFLAGS) $(APP_CPPFLAGS) $(ARCH) -std=c99 -Os -ffreestanding -fno-builtin -fno-common -fno-strict-aliasing -fno-pic -mword-relocations -ffunction-sections -fdata-sections -MMD -MP
SDK_CFLAGS := $(SDK_BASE_FLAGS) -Wall -Wextra -Werror $(SDK_INCLUDES)
APP_ALL_CFLAGS := $(SDK_BASE_FLAGS) $(SDK_LIBC_INCLUDES) $(APP_CFLAGS) $(SDK_INCLUDES)

GL2_PREFIX := -include $(NANO_GL2)/std/gl2prefix.h

SDK_STOR_FILES := sftl.c disk.c mount.c hfs.c partition.c write.c fmc_program.c fmc_erase.c reclaim.c append.c collect.c btoc.c checkpoint_plan.c
SDK_LIB_FILES := game.c nano_game.c save.c

SDK_OBJS := \
	$(BUILD)/sdk/start.o \
	$(BUILD)/sdk/nano_app.o \
	$(BUILD)/sdk/nano_fs.o \
	$(BUILD)/sdk/nano_heap.o \
	$(BUILD)/sdk/memory.o \
	$(BUILD)/sdk/gl2std.o \
	$(BUILD)/sdk/egl_nano.o \
	$(BUILD)/sdk/gl2api.o \
	$(BUILD)/sdk/gl2rt.o \
	$(BUILD)/sdk/gl2vsc.o \
	$(BUILD)/sdk/gl2tex.o \
	$(BUILD)/sdk/gl2nano.o \
	$(addprefix $(BUILD)/sdk/stor_,$(SDK_STOR_FILES:.c=.o)) \
	$(addprefix $(BUILD)/sdk/sdkstor_,$(SDK_LIB_FILES:.c=.o))

APP_OBJS := $(patsubst %.c,$(BUILD)/app/%.o,$(APP_SRCS))

.PHONY: native clean install

$(BUILD)/sdk $(BUILD)/app:
	mkdir -p $@

$(BUILD)/sdk/start.o: $(NANO_SDK)/src/start.S | $(BUILD)/sdk
	$(CC) $(ARCH) -c $< -o $@

$(BUILD)/sdk/nano_app.o: $(NANO_SDK)/src/nano_app.c | $(BUILD)/sdk
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(BUILD)/sdk/nano_fs.o: $(NANO_SDK)/src/nano_fs.c | $(BUILD)/sdk
	$(CC) $(SDK_CFLAGS) -I$(NANO_SDK)/src -I$(NANO_SDK)/include/libc -c $< -o $@

$(BUILD)/sdk/nano_heap.o: $(NANO_SDK)/src/nano_heap.c | $(BUILD)/sdk
	$(CC) $(SDK_CFLAGS) -I$(NANO_SDK)/include/libc -c $< -o $@

$(BUILD)/sdk/memory.o: $(NANO_SDK)/src/memory.c | $(BUILD)/sdk
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(BUILD)/sdk/gl2std.o: $(NANO_GL2)/std/gl2std.c | $(BUILD)/sdk
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(BUILD)/sdk/egl_nano.o: $(NANO_GL2)/std/egl_nano.c | $(BUILD)/sdk
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(BUILD)/sdk/gl2api.o: $(NANO_GL2)/gl2api.c | $(BUILD)/sdk
	$(CC) $(SDK_CFLAGS) $(GL2_PREFIX) -O2 -mfpu=vfpv3-d16 -mfloat-abi=softfp -ffp-contract=off -c $< -o $@

$(BUILD)/sdk/gl2rt.o: $(NANO_GL2)/gl2rt.c | $(BUILD)/sdk
	$(CC) $(SDK_CFLAGS) $(GL2_PREFIX) -c $< -o $@

$(BUILD)/sdk/gl2vsc.o: $(NANO_GL2)/gl2vsc.c | $(BUILD)/sdk
	$(CC) $(SDK_CFLAGS) $(GL2_PREFIX) -c $< -o $@

$(BUILD)/sdk/gl2tex.o: $(NANO_GL2)/gl2tex.c | $(BUILD)/sdk
	$(CC) $(SDK_CFLAGS) $(GL2_PREFIX) -O2 -c $< -o $@

$(BUILD)/sdk/gl2nano.o: $(NANO_GL2)/gl2nano.c | $(BUILD)/sdk
	$(CC) $(SDK_CFLAGS) $(GL2_PREFIX) -O2 -mfpu=vfpv3-d16 -mfloat-abi=softfp -ffp-contract=off -c $< -o $@

$(BUILD)/sdk/stor_%.o: $(NANO_STOR)/%.c | $(BUILD)/sdk
	$(CC) $(SDK_BASE_FLAGS) -O2 -Wall -Wextra -Werror -fno-tree-loop-distribute-patterns $(SDK_INCLUDES) -c $< -o $@

$(BUILD)/sdk/sdkstor_%.o: $(NANO_SDK)/src/storage/%.c | $(BUILD)/sdk
	$(CC) $(SDK_BASE_FLAGS) -O2 -Wall -Wextra -Werror -fno-tree-loop-distribute-patterns -I$(NANO_SDK)/src/storage $(SDK_INCLUDES) -c $< -o $@

$(BUILD)/app/%.o: %.c | $(BUILD)/app
	@mkdir -p $(dir $@)
	$(CC) $(APP_ALL_CFLAGS) -c $< -o $@

$(BUILD)/$(APP_NAME).elf: $(SDK_OBJS) $(APP_OBJS) $(NANO_SDK)/src/app.ld
	$(CC) $(ARCH) -nostdlib -Wl,--gc-sections,-T,$(NANO_SDK)/src/app.ld,-Map,$(BUILD)/$(APP_NAME).map $(SDK_OBJS) $(APP_OBJS) -lgcc -o $@
	$(SIZE) $@

$(BUILD)/$(APP_NAME).bin: $(BUILD)/$(APP_NAME).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/game.bin: $(BUILD)/$(APP_NAME).bin $(NANO_SDK)/tools/mkgame.py
	python3 $(NANO_SDK)/tools/mkgame.py $< $@

native: $(BUILD)/game.bin

install: $(BUILD)/game.bin
	cp $< /Volumes/iPod/Apps/Data/Game/game.bin

clean:
	rm -rf $(BUILD)

-include $(SDK_OBJS:.o=.d) $(APP_OBJS:.o=.d)
