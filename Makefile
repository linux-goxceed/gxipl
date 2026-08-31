# SPDX-License-Identifier: MIT

CROSS_COMPILE ?= /opt/gxtools/csky-linux-tools-musl-linux-7.2-gcc16.2.1-20260823/bin/csky-linux-
CC      := $(CROSS_COMPILE)gcc
LD      := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump

SOC ?= gx6702
SUPPORTED_SOCS := gx6702 gx6706

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
else
SOC_CPPFLAGS := -DSOC_GX6702=1
SOC_IPL_SRC := ipl/ipl.c
SOC_CHIP := GX6702
BOOT_IMAGE_SIZE := 0x10000
endif

IPL_CHIP ?= $(SOC_CHIP)
IPL_VERSION_BASE ?= 1.0.0-open

ARCHFLAGS := -EL -mcpu=ck610
INCLUDES := -Iinclude -Ibootcode -Ifatfs

GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null)
ifeq ($(GIT_HASH),)
IPL_VERSION_STR := $(IPL_VERSION_BASE)
else
IPL_VERSION_STR := $(IPL_VERSION_BASE)-$(GIT_HASH)
endif

CPPFLAGS := $(SOC_CPPFLAGS) \
	-DIPL_CHIP=\"$(IPL_CHIP)\" -DIPL_VERSION_STR=\"$(IPL_VERSION_STR)\" \
	-DIPL_CONFIG_VA=0x00101e00u
CFLAGS := $(ARCHFLAGS) -Os -std=c99 -ffreestanding -fno-builtin \
	-fno-stack-protector -fomit-frame-pointer -nostdlib -Wall -Wextra \
	-ffunction-sections -fdata-sections $(INCLUDES) $(CPPFLAGS)

LINKER_IPL := ld/linker-8k.ld
LDFLAGS_IPL := -EL -nostdlib --gc-sections -T $(LINKER_IPL)
LDFLAGS_BC  := -EL -nostdlib --gc-sections -T ld/linker-bootcode.ld

BUILD_DIR := build/$(TARGET)
STAGE1_SRCS := ipl/start.S $(SOC_IPL_SRC) ipl/ipl_common.c \
	ipl/ipl_config.c ipl/gx_spi.c
BC_SRCS := bootcode/main.c bootcode/print.c bootcode/spi_boot.c \
	bootcode/uart_boot.c bootcode/usb_boot.c bootcode/elf.c \
	bootcode/lib.c bootcode/gx_table.c bootcode/usb/usb_msc.c \
	bootcode/fat/diskio_usb.c fatfs/ff.c ipl/ipl_config.c ipl/gx_spi.c
STAGE1_OBJS := $(addprefix $(BUILD_DIR)/,$(STAGE1_SRCS:.c=.o))
STAGE1_OBJS := $(STAGE1_OBJS:.S=.o)
BC_OBJS := $(addprefix $(BUILD_DIR)/,$(BC_SRCS:.c=.o))

.PHONY: all clean bootcode ipl flash-image test flash-probe64 flash-verbose flash-uboot

all: $(IPL_ARTIFACT).boot $(TARGET)-bootcode.bin $(IPL_ARTIFACT).dis

ipl: $(IPL_ARTIFACT).boot

bootcode: $(TARGET)-bootcode.bin

flash-image: TABLE-$(SOC).bin

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
	$(LD) $(LDFLAGS_IPL) -o $@ $(STAGE1_OBJS)

$(IPL_ARTIFACT).bin: $(IPL_ARTIFACT).elf
	$(OBJCOPY) -O binary $< $@

$(IPL_ARTIFACT).boot: $(IPL_ARTIFACT).bin utils/mkboot.py
	python3 utils/mkboot.py --soc $(SOC) $< $@

$(IPL_ARTIFACT).dis: $(IPL_ARTIFACT).elf
	$(OBJDUMP) -d $< > $@

$(TARGET)-bootcode.elf: $(BC_OBJS) ld/linker-bootcode.ld
	$(LD) $(LDFLAGS_BC) -o $@ $(BC_OBJS)

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

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ARCHFLAGS) $(SOC_CPPFLAGS) -c $< -o $@

clean:
	rm -rf build
	# Remove objects left by the pre-build-directory Makefile.
	rm -f ipl/*.o bootcode/*.o bootcode/usb/*.o bootcode/fat/*.o fatfs/*.o
	rm -f gx6702-ipl.elf gx6702-ipl.bin gx6702-ipl.boot gx6702-ipl.dis \
		gx6702-bootcode.elf gx6702-bootcode.bin gx6702-bootcode.dis \
		gx6706-ipl.elf gx6706-ipl.bin gx6706-ipl.boot gx6706-ipl.dis \
		gx6706-bootcode.elf gx6706-bootcode.bin gx6706-bootcode.dis \
		BOOT.bin BOOT-gx6702.bin BOOT-gx6706.bin \
		TABLE-gx6702.bin TABLE-gx6706.bin \
		BOOT-flash-probe64.bin TABLE-flash-probe64.bin \
		BOOT-flash-verbose64.bin TABLE-flash-verbose64.bin \
		BOOT-flash-uboot.bin TABLE-flash-uboot.bin
