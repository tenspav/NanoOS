# NanoOS Loader: RetailOS raw-surface app -> build/NanoOSLoader.hbapp
#   make                          device build, loads /Apps/NanoOS/kernel.bin
#   make KERNEL_PATH=/some/file   other kernel path (the emulator test uses one,
#                                 since the dumped disk has no /Apps/NanoOS)
# Reuses probe/chainloader's shutdown.c + handoff.S and nanoos's font.
# Paths are relative to this Makefile so the directory can be symlinked into
# NanoApps/apps/ for packaging without breaking the build.
LOADER_DIR := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))
NANOAPPS   ?= $(realpath $(LOADER_DIR)/../../../NanoApps)
CHAIN      := $(LOADER_DIR)/../../probe/chainloader
NANOOS     := $(LOADER_DIR)/../../nanoos
BUILD      ?= $(LOADER_DIR)/build

APP_NAME    := NanoOSLoader
SRCS        := loader.c font_full.c $(NANOOS)/src/font.c \
               $(CHAIN)/shutdown.c $(CHAIN)/handoff.S
RAW_SURFACE := 1
# make BUILD=dir keeps a variant apart from build/.
# handoff.S .incbin's "payload.bin" as the chainloader's built-in fallback;
# the loader never uses it, so an empty one is generated in $(BUILD)/.
EXTRA_CFLAGS += -I$(CHAIN) -I$(NANOOS)/src -Wa,-I$(BUILD)
EXTRA_CFLAGS += $(LOADER_DEFS)   # e.g. LOADER_DEFS=-DLOADER_DEBUG
ifdef KERNEL_PATH
EXTRA_CFLAGS += -DNANOOS_KERNEL_PATH='"$(KERNEL_PATH)"'
endif

include $(NANOAPPS)/sdk/hb_app.mk

$(BUILD)/$(APP_NAME).reloc.elf: $(BUILD)/payload.bin
$(BUILD)/payload.bin: | $(BUILD)
	: > $@
