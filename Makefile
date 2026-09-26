# SPDX-License-Identifier: MIT

CROSS_COMPILE ?= /opt/gxtools/csky-linux-tools-musl-linux-7.2-gcc16.2.1-20260823/bin/csky-linux-
CC      := $(CROSS_COMPILE)gcc
LD      := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump

SOC ?= gx6702
SUPPORTED_SOCS := gx6702 gx6706 universal

ifeq ($(filter $(SOC),$(SUPPORTED_SOCS)),)
$(error unsupported SOC '$(SOC)' (expected one of: $(SUPPORTED_SOCS)))
endif
TARGET := $(SOC)
IPL_ARTIFACT := $(SOC)-ipl

ifeq ($(SOC),gx6706)
SOC_CPPFLAGS := -DSOC_GX6706=1
SOC_IPL_SRC := ipl/gx6706_ipl.c
SOC_CHIP := GX6706
BOOT_IMAGE_SIZE := 0x20000
STAGE1_SPI := ipl/gx_spi.c
else ifeq ($(SOC),universal)
SOC_CPPFLAGS := -DSOC_UNIVERSAL=1
SOC_IPL_SRC := ipl/ipl.c ipl/gx6706_ipl.c ipl/ipl_universal.c
SOC_CHIP := universal
IPL_ARTIFACT := gx-universal-ipl
STAGE1_SPI :=
else
SOC_CPPFLAGS := -DSOC_GX6702=1
SOC_IPL_SRC := ipl/ipl.c
SOC_CHIP := GX6702
BOOT_IMAGE_SIZE := 0x10000
STAGE1_SPI := ipl/gx_spi.c
endif

IPL_CHIP ?= $(SOC_CHIP)
IPL_VERSION_BASE ?= 1.0.0-open

BUILD_DIR := build/$(TARGET)
ARCHFLAGS := -EL -mcpu=ck610
INCLUDES := -Iinclude -Ibootcode -Ifatfs -I$(BUILD_DIR)
OPT ?= -Os
LTO ?= 1

OPT_CFLAGS ?= 

ifeq ($(LTO),1)
LTO_CFLAGS  := -flto
LTO_LDFLAGS := -flto -fuse-linker-plugin
endif

ifeq ($(VERBOSE_MINIFY),1)
MINIFY_CFLAGS  := -DVERBOSE_MINIFY=1
endif

# IPL_MIN=1 (default) compiles the size-golfed stage-1: the GXUB bundle
# receive path and the legacy bring-up uploader checksum fallback are
# dropped.  IPL_MIN=0 restores the full feature set.  The SPI GXBC path,
# plain RUNGET bootcode/raw U-Boot receive, and all protocol strings are
# identical in both modes.
IPL_MIN ?= 1
ifeq ($(IPL_MIN),0)
IPL_MIN_CPPFLAGS :=
else
IPL_MIN_CPPFLAGS := -DIPL_MINIMAL=1
endif


GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null)
ifeq ($(GIT_HASH),)
IPL_VERSION_STR := $(IPL_VERSION_BASE)
else
IPL_VERSION_STR := $(IPL_VERSION_BASE)-$(GIT_HASH)
endif

CPPFLAGS := $(SOC_CPPFLAGS) $(IPL_MIN_CPPFLAGS) \
	-DIPL_CHIP=\"$(IPL_CHIP)\" -DIPL_VERSION_STR=\"$(IPL_VERSION_STR)\" \
	-DIPL_CONFIG_VA=0x00101e00u
# Size-tuning knobs shared by compile and LTO link steps.  -mbranch-cost=2
# was dropped: it biases GCC toward if-conversion, which grows code.
SIZE_CFLAGS := -falign-functions=1 -falign-loops=1 -falign-jumps=1 \
	-falign-labels=1 -fno-unwind-tables -fno-asynchronous-unwind-tables
CFLAGS := $(ARCHFLAGS) $(OPT) -std=c99 -ffreestanding -fno-builtin \
	-fno-stack-protector -fomit-frame-pointer -nostdlib -Wall -Wextra \
	-ffunction-sections -fdata-sections $(SIZE_CFLAGS) $(INCLUDES) $(CPPFLAGS) $(LTO_CFLAGS) $(MINIFY_CFLAGS) $(OPT_CFLAGS)

LINKER_IPL := ld/linker-8k.ld
LDFLAGS_IPL := -EL -nostdlib --gc-sections -T $(LINKER_IPL)
LDFLAGS_BC  := -EL -nostdlib --gc-sections -T ld/linker-bootcode.ld

# With -flto the real code generation happens at link time, so the link
# command must carry the same optimization/architecture flags as the
# compile step; otherwise GCC falls back to its defaults.
LTO_LINK_CFLAGS := $(ARCHFLAGS) $(OPT) -ffreestanding -fno-builtin \
	-fno-stack-protector -fomit-frame-pointer $(SIZE_CFLAGS) \
	-ffunction-sections -fdata-sections $(LTO_CFLAGS) $(OPT_CFLAGS)

STAGE1_SRCS := ipl/start.S $(SOC_IPL_SRC) ipl/ipl_common.c \
	ipl/ipl_config.c ipl/gx_chip.c ipl/efuse.c $(STAGE1_SPI)
# Per-SoC builds consume the generated RLE DDR stream; the universal build
# keeps the canonical readable tables compiled in.
ifneq ($(SOC),universal)
STAGE1_GEN := $(BUILD_DIR)/ddr_rle.h
endif
BC_SRCS := bootcode/main.c bootcode/print.c bootcode/spi_boot.c \
	bootcode/uart_boot.c bootcode/usb_boot.c bootcode/elf.c \
	bootcode/lib.c bootcode/gx_table.c bootcode/usb/usb_msc.c \
	bootcode/fat/diskio_usb.c fatfs/ff.c fatfs/ffunicode.c ipl/ipl_config.c ipl/gx_spi.c ipl/efuse.c
STAGE1_OBJS := $(addprefix $(BUILD_DIR)/,$(STAGE1_SRCS:.c=.o))
STAGE1_OBJS := $(STAGE1_OBJS:.S=.o)
BC_OBJS := $(addprefix $(BUILD_DIR)/,$(BC_SRCS:.c=.o))

.PHONY: all clean bootcode ipl flash-image test flash-probe64 flash-verbose flash-uboot allinone usb-hello

# Keep the bare `make` default on the IPL/bootcode build.  This must stay
# explicit: the first target in the file is the default goal, and
# `usb-hello` is defined above `all`, so without this a plain `make` would
# only compile the sample.
.DEFAULT_GOAL := all

usb-hello: samples/usb-hello/start6706.elf

samples/usb-hello/start6706.elf: samples/usb-hello/hello.c samples/usb-hello/hello.ld
	$(CC) $(ARCHFLAGS) -Os -ffreestanding -fno-builtin -nostdlib -nostartfiles \
		-T samples/usb-hello/hello.ld -o $@ samples/usb-hello/hello.c

ifeq ($(SOC),universal)
all: $(IPL_ARTIFACT).boot $(IPL_ARTIFACT).dis
else
all: $(IPL_ARTIFACT).boot $(TARGET)-bootcode.bin $(IPL_ARTIFACT).dis
endif

ipl: $(IPL_ARTIFACT).boot

bootcode: $(TARGET)-bootcode.bin

ifeq ($(SOC),universal)
flash-image:
	$(error SOC=universal is UART-only; flash images remain family-split)
else
flash-image: TABLE-$(SOC).bin
endif

allinone: gx-universal-ipl-allinone.boot

gx-universal-ipl-allinone.boot: gx-universal-ipl.boot utils/mkallinone.py
	python3 utils/mkallinone.py $< $@

test:
	python3 -m unittest discover -s tests -v

flash-probe64: BOOT-flash-probe64.bin
flash-verbose: BOOT-flash-verbose64.bin
flash-uboot: BOOT-flash-uboot.bin

BOOT-flash-probe64.bin: utils/mk_flash_image.py bootcode/tiny_flash_test.c
	python3 utils/mk_flash_image.py --soc gx6702 --probe \
		--cross-compile "$(CROSS_COMPILE)" -o $@

BOOT-flash-verbose64.bin: utils/mk_flash_image.py bootcode/main.c
	python3 utils/mk_flash_image.py --soc gx6702 --full-bootcode \
		--cross-compile "$(CROSS_COMPILE)" -o $@ \
		--table-out TABLE-flash-verbose64.bin

BOOT-flash-uboot.bin: utils/mk_flash_image.py bootcode/main.c
	python3 utils/mk_flash_image.py --soc gx6702 --uboot ../u-boot/u-boot.bin \
		--cross-compile "$(CROSS_COMPILE)" -o $@ \
		--table-out TABLE-flash-uboot.bin

$(IPL_ARTIFACT).elf: $(STAGE1_OBJS) $(LINKER_IPL)
ifeq ($(LTO),1)
	$(CC) $(LTO_LINK_CFLAGS) -nostdlib -nostartfiles $(LTO_LDFLAGS) \
		-Wl,--gc-sections -Wl,-Map=$@.map -T $(LINKER_IPL) -o $@ $(STAGE1_OBJS)
else
	$(LD) $(LDFLAGS_IPL) -Map $@.map -o $@ $(STAGE1_OBJS)
endif

$(IPL_ARTIFACT).bin: $(IPL_ARTIFACT).elf
	$(OBJCOPY) -O binary $< $@

$(IPL_ARTIFACT).boot: $(IPL_ARTIFACT).bin utils/mkboot.py
	python3 utils/mkboot.py --soc $(SOC) $< $@

$(IPL_ARTIFACT).dis: $(IPL_ARTIFACT).elf
	$(OBJDUMP) -d $< > $@

$(TARGET)-bootcode.elf: $(BC_OBJS) ld/linker-bootcode.ld
ifeq ($(LTO),1)
	$(CC) $(LTO_LINK_CFLAGS) -nostdlib -nostartfiles $(LTO_LDFLAGS) \
		-Wl,--gc-sections -T ld/linker-bootcode.ld -o $@ $(BC_OBJS)
else
	$(LD) $(LDFLAGS_BC) -o $@ $(BC_OBJS)
endif

$(TARGET)-bootcode.bin: $(TARGET)-bootcode.elf
	$(OBJCOPY) -O binary $< $@

$(TARGET)-bootcode.dis: $(TARGET)-bootcode.elf
	$(OBJDUMP) -d $< > $@

BOOT-$(SOC).bin: $(SOC)-ipl.boot $(SOC)-bootcode.bin utils/splice_boot.py
	python3 utils/splice_boot.py --ipl-boot $(SOC)-ipl.boot \
		--bootcode $(SOC)-bootcode.bin --size $(BOOT_IMAGE_SIZE) -o $@

TABLE-$(SOC).bin: $(SOC)-ipl.boot $(SOC)-bootcode.bin utils/mk_flash_image.py
	python3 utils/mk_flash_image.py --soc $(SOC) --full-bootcode \
		--cross-compile "$(CROSS_COMPILE)" \
		-o BOOT-$(SOC).bin --table-out $@

# Historical target/name: plain make BOOT.bin still creates a GX6702 image.
BOOT.bin: gx6702-ipl.boot gx6702-bootcode.bin utils/splice_boot.py
	python3 utils/splice_boot.py --ipl-boot gx6702-ipl.boot \
		--bootcode gx6702-bootcode.bin --size 0x10000 -o $@

$(BUILD_DIR)/%.o: %.c $(STAGE1_GEN)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ARCHFLAGS) $(SOC_CPPFLAGS) -c $< -o $@

# Regenerated from the canonical readable tables in the IPL sources.
$(BUILD_DIR)/ddr_rle.h: ipl/ipl.c ipl/gx6706_ipl.c utils/gen_ddr_rle.py
	@mkdir -p $(dir $@)
	python3 utils/gen_ddr_rle.py --soc $(SOC) -o $@

clean:
	rm -rf build
	# Remove objects left by the pre-build-directory Makefile.
	rm -f ipl/*.o bootcode/*.o bootcode/usb/*.o bootcode/fat/*.o fatfs/*.o
	rm -f gx6702-ipl.elf gx6702-ipl.bin gx6702-ipl.boot gx6702-ipl.dis \
		gx6702-ipl.elf.map \
		gx6702-bootcode.elf gx6702-bootcode.bin gx6702-bootcode.dis \
		gx6706-ipl.elf gx6706-ipl.bin gx6706-ipl.boot gx6706-ipl.dis \
		gx6706-ipl.elf.map \
		gx6706-bootcode.elf gx6706-bootcode.bin gx6706-bootcode.dis \
		gx-universal-ipl.elf gx-universal-ipl.bin gx-universal-ipl.boot \
		gx-universal-ipl.dis gx-universal-ipl.elf.map \
		gx-universal-ipl-allinone.boot \
		BOOT.bin BOOT-gx6702.bin BOOT-gx6706.bin \
		TABLE-gx6702.bin TABLE-gx6706.bin \
		BOOT-flash-probe64.bin TABLE-flash-probe64.bin \
		BOOT-flash-verbose64.bin TABLE-flash-verbose64.bin \
		BOOT-flash-uboot.bin TABLE-flash-uboot.bin \
		samples/usb-hello/start6706.elf
		
help:
	@echo "NationalChip IPL and bootcode config options:"
	@echo "Build targets:"
	@echo ""
	@echo "bootcode - build GX bootcode target"
	@echo "ipl - build GX IPL target"
	@echo "flash-image - build GX IPL and bootcode into a flashable BOOT.bin and TABLE.bin for GX6702"
	@echo "flash-probe64 - build GX IPL and SPI flash probe test into a flashable BOOT.bin and TABLE.bin for GX6702"
	@echo "flash-uboot - build GX IPL and U-Boot into a flashable BOOT.bin and TABLE.bin for GX6702"
	@echo "allinone - build GX IPL into a universal all in one image (meant for flashing utilities to detect device first)"
	@echo "test - run GX unit tests"
	@echo ""
	@echo "Config options:"
	@echo "SOC - configure type of SoC for IPL and bootcode build"
	@echo "accepted options: gx6702, gx6706, universal (defaults to gx6702)"
	@echo "LTO - enable link time optimization on IPL and bootcode build (defaults to 0)"
	@echo "VERBOSE_MINIFY - minify verbose logging (saves ~2 KB, defaults to 0)"
	
