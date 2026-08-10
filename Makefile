# SPDX-License-Identifier: MIT

CROSS_COMPILE ?= /opt/gxtools/csky-linux-tools-uclibc-linux-4.9.56-20260805/bin/csky-linux-
CC      := $(CROSS_COMPILE)gcc
LD      := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump

# Stage-1 is fixed at 8 KiB (UART and flash BootROM both map that window).
IPL_CHIP ?= GX6702
IPL_VERSION_BASE ?= 1.0.0-open

ARCHFLAGS := -EL -mcpu=ck610
INCLUDES := -Iinclude -Ibootcode -Ifatfs

GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null)
ifeq ($(GIT_HASH),)
IPL_VERSION_STR := $(IPL_VERSION_BASE)
else
IPL_VERSION_STR := $(IPL_VERSION_BASE)-$(GIT_HASH)
endif

CFLAGS := $(ARCHFLAGS) -Os -std=c99 -ffreestanding -fno-builtin \
	-fno-stack-protector -fomit-frame-pointer -nostdlib -Wall -Wextra \
	-Werror -ffunction-sections -fdata-sections $(INCLUDES) \
	-DIPL_CHIP=\"$(IPL_CHIP)\" -DIPL_VERSION_STR=\"$(IPL_VERSION_STR)\" \
	-DIPL_CONFIG_VA=0x00101e00u

LINKER_IPL := ld/linker-8k.ld
LDFLAGS_IPL := -EL -nostdlib --gc-sections -T $(LINKER_IPL)
LDFLAGS_BC  := -EL -nostdlib --gc-sections -T ld/linker-bootcode.ld

STAGE1_OBJS := ipl/start.o ipl/ipl.o ipl/ipl_config.o ipl/gx_spi.o

BC_OBJS := bootcode/main.o bootcode/print.o bootcode/spi_boot.o \
	bootcode/uart_boot.o bootcode/usb_boot.o bootcode/elf.o \
	bootcode/lib.o bootcode/gx_table.o bootcode/usb/usb_msc.o \
	bootcode/fat/diskio_usb.o fatfs/ff.o ipl/ipl_config.o ipl/gx_spi.o

.PHONY: all clean bootcode ipl flash-probe64 flash-verbose flash-uboot

all: gx6702-ipl.boot gx6702-bootcode.bin gx6702-ipl.dis

flash-probe64: BOOT-flash-probe64.bin

flash-verbose: BOOT-flash-verbose64.bin

flash-uboot: BOOT-flash-uboot.bin

BOOT-flash-probe64.bin: utils/mk_flash_probe64.py bootcode/tiny_flash_test.c
	python3 utils/mk_flash_probe64.py -o $@

BOOT-flash-verbose64.bin: utils/mk_flash_probe64.py bootcode/main.c
	python3 utils/mk_flash_probe64.py --full-bootcode -o $@ \
		--table-out TABLE-flash-verbose64.bin

BOOT-flash-uboot.bin: utils/mk_flash_probe64.py bootcode/main.c
	python3 utils/mk_flash_probe64.py --uboot ../u-boot/u-boot.bin -o $@ \
		--table-out TABLE-flash-uboot.bin

ipl: gx6702-ipl.boot

bootcode: gx6702-bootcode.bin

include/ipl_version.h: include/ipl_version.h.in
	@echo '/* auto */' > $@
	@echo '#ifndef IPL_VERSION_H' >> $@
	@echo '#define IPL_VERSION_H' >> $@
	@echo '#define IPL_VERSION_STR "$(IPL_VERSION_STR)"' >> $@
	@echo '#define IPL_CHIP "$(IPL_CHIP)"' >> $@
	@echo '#endif' >> $@

gx6702-ipl.elf: $(STAGE1_OBJS) $(LINKER_IPL)
	$(LD) $(LDFLAGS_IPL) -o $@ $(STAGE1_OBJS)

gx6702-ipl.bin: gx6702-ipl.elf
	$(OBJCOPY) -O binary $< $@

gx6702-ipl.boot: gx6702-ipl.bin utils/mkboot.py
	python3 utils/mkboot.py $< $@

gx6702-ipl.dis: gx6702-ipl.elf
	$(OBJDUMP) -d $< > $@

gx6702-bootcode.elf: $(BC_OBJS) ld/linker-bootcode.ld include/ipl_version.h
	$(LD) $(LDFLAGS_BC) -o $@ $(BC_OBJS)

gx6702-bootcode.bin: gx6702-bootcode.elf
	$(OBJCOPY) -O binary $< $@

gx6702-bootcode.dis: gx6702-bootcode.elf
	$(OBJDUMP) -d $< > $@

BOOT.bin: gx6702-ipl.boot gx6702-bootcode.bin utils/splice_boot.py
	python3 utils/splice_boot.py --ipl-boot gx6702-ipl.boot \
		--bootcode gx6702-bootcode.bin -o $@

%.o: %.c include/ipl_version.h
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(ARCHFLAGS) -c $< -o $@

bootcode/%.o: bootcode/%.c include/ipl_version.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

bootcode/usb/%.o: bootcode/usb/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

bootcode/fat/%.o: bootcode/fat/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

fatfs/%.o: fatfs/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(STAGE1_OBJS) $(BC_OBJS) \
		gx6702-ipl.elf gx6702-ipl.bin gx6702-ipl.boot gx6702-ipl.dis \
		gx6702-bootcode.elf gx6702-bootcode.bin gx6702-bootcode.dis \
		gx6702-tiny-flash.elf gx6702-tiny-flash.bin \
		BOOT.bin BOOT-flash-probe64.bin TABLE-flash-probe64.bin \
		BOOT-flash-verbose64.bin TABLE-flash-verbose64.bin \
		BOOT-flash-uboot.bin TABLE-flash-uboot.bin \
		include/ipl_version.h
	rm -f bootcode/*.o bootcode/usb/*.o bootcode/fat/*.o fatfs/ff.o
