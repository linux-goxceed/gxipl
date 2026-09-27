# SPDX-License-Identifier: MIT

CROSS_COMPILE ?= /opt/gxtools/csky-linux-tools-musl-linux-7.2-gcc16.2.1-20260823/bin/csky-linux-
CC      := $(CROSS_COMPILE)gcc
LD      := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump

SOC ?= gx6702
SUPPORTED_SOCS := gx6702 gx6706 universal

# USB_DEBUG gates the USB register dumps / BOT+EHCI error traces.  It must be
# resolved before IPL_SUFFIX below, which reads it.
USB_DEBUG ?= 0

ifeq ($(filter $(SOC),$(SUPPORTED_SOCS)),)
$(error unsupported SOC '$(SOC)' (expected one of: $(SUPPORTED_SOCS)))
endif
TARGET := $(SOC)

# Debug builds get their own artifact name so a later plain `make` cannot
# silently overwrite the image you are about to flash.  A debug build must
# never overwrite a release build (or vice versa): they share the artifact
# name, and a later plain `make` silently replaces the one you were about to
# flash.  Verified the hard way twice.  With this on, the debug image is
# <soc>-ipl-dbg.boot and cannot collide.
#
# Test against "1" explicitly: `ifdef USB_DEBUG` is TRUE for USB_DEBUG=0,
# which would name every release build -dbg.
ifeq ($(USB_DEBUG),1)
IPL_SUFFIX := -dbg
else
IPL_SUFFIX :=
endif

IPL_ARTIFACT := $(SOC)-ipl$(IPL_SUFFIX)

ifeq ($(SOC),gx6706)
SOC_CPPFLAGS := -DSOC_GX6706=1
SOC_IPL_SRC := ipl/gx6706_ipl.c
SOC_CHIP := GX6706
BOOT_IMAGE_SIZE := 0x20000
STAGE1_SPI := ipl/gx_spi.c
# The SoC's own choice of transport.  Recorded so the non-SPI warning below
# can stay silent for it while still firing when a user picks one.
DEFAULT_GX6706_TRANSPORT := stick
# ---------------------------------------------------------------------------
# The flash-safe `usb` variant fits at 8118 B with FAT16 and FAT32 enabled.
# It retains SPI+USB; the `stick` default remains a UART-recoverable bring-up
# image (USB -> UART, no SPI) at 7478 B. `usb-only` is 7010 B and has no
# recovery path at all.
#
# For a flash-resident GX6706 image, select IPL_TRANSPORT=usb.
#
# WARNING -- `stick` IS NOT FLASH-SAFE, and this is a user-visible trap.
# A `stick` image dropped into SPI flash can NEVER read a bootcode from
# that flash: the SPI driver is not linked in at all.  With no stick and no
# UART host, the board goes dead and the failure looks like "the loader is
# broken" rather than "this build has no SPI path".  The UART receiver is
# what makes such an image recoverable, so a `stick` image only makes sense
# when a UART host is available to re-flash.
#
# For a unit that must boot unattended from flash, use the flash-safe
# `usb` variant. It is 8118 B (10 B free) with FAT16/FAT32 IPL USB support.
# The compact eFuse descriptors retain per-unit DDR calibration and geometry.
#
# Override on the command line to change: make SOC=gx6706 IPL_TRANSPORT=...
IPL_TRANSPORT ?= $(DEFAULT_GX6706_TRANSPORT)
else ifeq ($(SOC),universal)
SOC_CPPFLAGS := -DSOC_UNIVERSAL=1
SOC_IPL_SRC := ipl/ipl.c ipl/gx6706_ipl.c ipl/ipl_universal.c
SOC_CHIP := universal
IPL_ARTIFACT := gx-universal-ipl$(IPL_SUFFIX)
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

# The per-SoC default is applied before the generic default below, and both
# must be resolved before BUILD_DIR.  IPL_TRANSPORT is therefore given its
# generic default here rather than further down, so that the object directory
# is consistent with the transport the rest of the makefile acts on.
#
#   gx6706 selects its bring-up default in its SoC block above.
#   USB_DEBUG=1 USB_DEBUG_SPI=0 sets `usb-only` (a debug image must still
#     link; the default set does not).
#   Everything else, and any explicit override, gets `both`.
ifeq ($(IPL_TRANSPORT),)
IPL_TRANSPORT := both
endif

# The variant selectors below must be part of BUILD_DIR, not just the artifact
# name.  Otherwise the objects compiled for one variant are reused for the
# next: make sees them as up to date, recompiles nothing, and silently links
# the previous variant's image.  This has bitten twice.
#
#   USB_DEBUG   a USB_DEBUG=1 build links against USB_DEBUG=0 objects and emits
#               a -dbg artifact with no USB_DEBUG code in it (silently
#               identical to release).
#   TRANSPORT   IPL_TRANSPORT=flash reuses `both` objects and links a 7786 B
#               GX6702 image while claiming to be the 3274 B flash-only build.
#               On GX6706, `both` still exceeds 8128 B while `usb` now fits;
#               a stale cache can therefore mask a GX6706 `both` link failure.
#
# Both selectors are therefore part of the object directory below.
BUILD_DIR := build/$(TARGET)$(IPL_SUFFIX)-$(IPL_TRANSPORT)

# FORCE_RELINK: the artifact name is variant-independent (see the .elf rule),
# so a variant switch must invalidate the previous link.  This has to be
# decided at parse time -- a recipe line runs only once make has already
# decided the target is out of date, which is too late.
#
# The stamp must NOT live inside $(BUILD_DIR): that directory is per-variant,
# so each variant would always see an empty (or absent) stamp and could never
# detect a switch.  It is keyed on the artifact name instead, which is shared
# across variants -- that is the whole point, since two variants contend for
# the same gx6702-ipl.elf.
#
# The stamp is written from the elf recipe, i.e. only when the link actually
# runs.  Writing it at parse time instead looks simpler but is wrong: make
# expands $(shell) for every goal it considers, so a stamp written at parse
# time is overwritten before the relink it was supposed to trigger, and the
# next parse sees the old value again.  Writing it in the recipe keeps the
# stamp and the elf in step by construction.
VARIANT_STAMP := build/.$(IPL_ARTIFACT).variant
ifneq ($(wildcard $(VARIANT_STAMP)),)
ifneq ($(shell cat $(VARIANT_STAMP) 2>/dev/null),$(IPL_TRANSPORT))
$(info forcing relink: $(cat $(VARIANT_STAMP)) -> $(IPL_TRANSPORT))
FORCE_RELINK := 1
endif
endif

# Phony target used as an extra prerequisite to force the relink above.
.PHONY: FORCE_RELINK_STAMP
FORCE_RELINK_STAMP:
ARCHFLAGS := -EL -mcpu=ck610
INCLUDES := -Iinclude -Ibootcode -Ifatfs -Iipl/fat -Iipl/usb -I$(BUILD_DIR)
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

# USB_DEBUG=1 restores the USB register dumps / BOT+EHCI error traces.  The
# default 0 keeps them out of the image entirely (not just unreached), so the
# strings and their call sites cost nothing.  Shared by the stage-2 USB core
# and the minimal IPL USB core -- see include/usb_debug.h.
#
# The traces cost ~1.0 KB. GX6702 `both` is 7786 B and GX6706 `stick` is
# 7478 B, so a USB_DEBUG build does NOT fit with either default transport.
# Set USB_DEBUG_SPI=0 to also drop the SPI driver from the debug image
# (gx_spi.c is ~250 B even when never called) -- that is safe for a debug
# image, which is flashed over UART and never needs the SPI recovery path.
ifeq ($(USB_DEBUG),1)
USB_DEBUG_CFLAGS := -DUSB_DEBUG=1
ifeq ($(USB_DEBUG_SPI),0)
IPL_TRANSPORT := usb-only
endif
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

# IPL_TRANSPORT selects which stage-1 transports are linked in.
#   both (default)  SPI -> USB -> UART.  Safe to flash: the image always
#                   has a recovery path, because the SPI branch needs no
#                   external media at all.
#   flash           SPI only, no USB.  Mirrors the vendor "straight to
#                   flash" IPL, and is what ships on flash GxLoader units.
#   uart            UART only, no SPI.  Mirrors the vendor UART-enabled IPL
#                   that libre-gxdl feeds.  Transient by nature: it exists to
#                   pull GxLoader down the wire, not to live in flash.
#   usb             SPI -> USB, no UART.  For a flash-resident image that
#                   must not keep the UART loader resident.
#   stick           USB -> UART, no SPI.  Smallest, but NOT flash-safe: with
#                   no valid bootcode in flash and no stick plugged in there
#                   is nothing left to boot from.  Build on demand only.
#
# gx6706 does not fit all four paths in 8128 B; see REVERSE_ENGINEERING.md.
# The SoC block above may set a different default before this line.

# Membership tests, so adding a variant is a one-line change above.  A
# transport is present unless the selected variant's name omits it.
ifeq (,$(filter both flash usb,$(IPL_TRANSPORT)))
IPL_TRANSPORT_CPPFLAGS += -DIPL_NO_SPI=1
endif
ifeq (,$(filter both uart stick,$(IPL_TRANSPORT)))
IPL_TRANSPORT_CPPFLAGS += -DIPL_NO_UART=1
endif
# `usb-only` is the USB_DEBUG trim: USB stage-1.5, nothing else.
ifeq (,$(filter both usb stick usb-only,$(IPL_TRANSPORT)))
IPL_TRANSPORT_CPPFLAGS += -DIPL_STAGE15_USB_DISABLED=1
endif

# Refuse to quietly emit a non-flash-safe flash image.  A `stick` (or
# `usb-only`) build has no SPI driver linked, so once it is in SPI flash it
# can never read a bootcode from there; the board then looks bricked with no
# obvious reason.  Catch it at build time rather than in a support call.
#
# Only warns for an explicitly chosen non-SPI transport: the gx6706 default
# is `stick`, and warning on every ordinary build would be pure noise.  The
# Makefile comment above the gx6706 default carries the real warning.
#
# Escape hatch: set IPL_ALLOW_NO_SPI=1 to silence it, e.g. when a non-SPI
# image is deliberate for a bring-up board.
ifneq ($(filter both flash usb,$(IPL_TRANSPORT)),)
# has SPI: nothing to check
else
ifeq ($(origin IPL_TRANSPORT),command line)
ifneq ($(IPL_ALLOW_NO_SPI),1)
# NOTE: no "|" characters below.  A pipe terminates the make function call
# and yields a bare "unterminated call to function 'warning'" parse error,
# which stops the build before anything is compiled.  Same for "(" and ")".
$(warning IPL_TRANSPORT=$(IPL_TRANSPORT) was requested explicitly and links NO SPI driver. This image cannot boot from SPI flash, and cannot recover from flash if flashed there: with no SPI path and no valid bootcode in flash, the board has nothing left to boot from. For a unit that must boot unattended from flash, use IPL_TRANSPORT=both, flash or usb. Set IPL_ALLOW_NO_SPI=1 if this non-SPI image is deliberate.)
endif
endif
endif


GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null)
ifeq ($(GIT_HASH),)
IPL_VERSION_STR := $(IPL_VERSION_BASE)
else
IPL_VERSION_STR := $(IPL_VERSION_BASE)-$(GIT_HASH)
endif

# Deferred (=, not :=) on purpose: STAGE15_CPPFLAGS is defined further down.
CPPFLAGS = $(SOC_CPPFLAGS) $(IPL_MIN_CPPFLAGS) \
	-DIPL_CHIP=\"$(IPL_CHIP)\" -DIPL_VERSION_STR=\"$(IPL_VERSION_STR)\" \
	-DIPL_CONFIG_VA=0x00101fc0u $(STAGE15_CPPFLAGS) $(IPL_TRANSPORT_CPPFLAGS)
# Size-tuning knobs shared by compile and LTO link steps.  -mbranch-cost=2
# was dropped: it biases GCC toward if-conversion, which grows code.
SIZE_CFLAGS := -falign-functions=1 -falign-loops=1 -falign-jumps=1 \
	-falign-labels=1 -fno-unwind-tables -fno-asynchronous-unwind-tables
# Deferred (=, not :=) on purpose: it references $(CPPFLAGS), which in turn
# references $(STAGE15_CPPFLAGS).  Both are set further down, so an immediate
# assignment here would freeze them while still empty and silently drop
# -DIPL_STAGE15_USB, which empties ipl/fat/stage15_usb.o and lets
# --gc-sections drop the whole USB path.
CFLAGS = $(ARCHFLAGS) $(OPT) -std=c99 -ffreestanding -fno-builtin \
	-fno-stack-protector -fomit-frame-pointer -nostdlib -Wall -Wextra \
	-ffunction-sections -fdata-sections $(SIZE_CFLAGS) $(INCLUDES) $(CPPFLAGS) $(LTO_CFLAGS) $(MINIFY_CFLAGS) $(USB_DEBUG_CFLAGS) $(OPT_CFLAGS)

LINKER_IPL := ld/linker-8k.ld
LDFLAGS_IPL := -EL -nostdlib --gc-sections -T $(LINKER_IPL)
LDFLAGS_BC  := -EL -nostdlib --gc-sections -T ld/linker-bootcode.ld

# With -flto the real code generation happens at link time, so the link
# command must carry the same optimization/architecture flags as the
# compile step; otherwise GCC falls back to its defaults.
LTO_LINK_CFLAGS := $(ARCHFLAGS) $(OPT) -ffreestanding -fno-builtin \
	-fno-stack-protector -fomit-frame-pointer $(SIZE_CFLAGS) \
	-ffunction-sections -fdata-sections $(LTO_CFLAGS) $(USB_DEBUG_CFLAGS) $(OPT_CFLAGS)

# IPL_STAGE15=1 builds the stage-1 USB/FAT stage-1.5 loader into the IPL.  It
# is forced OFF for SOC=universal: that build is a stub loader whose whole
# job is to report the chip family so a host tool can push the right image,
# so it stays UART-only and dependency-free by design (see gxipl-universal-stub
# in repo notes).  Deliberately not a knob there, so a stray override cannot
# pull USB into the stub.
#
# NOTE: this block must be defined before any *deferred* user of it is
# expanded (i.e. before a recipe runs), not necessarily before the
# assignments themselves.  See the CFLAGS comment above.
ifneq ($(SOC),universal)
IPL_STAGE15 ?= 1
else
IPL_STAGE15 := 0
endif
ifeq ($(IPL_STAGE15),1)
STAGE15_CPPFLAGS := -DIPL_STAGE15_USB=1
STAGE15_SRCS := ipl/usb/usb_msc_min.c ipl/fat/pff.c \
	ipl/fat/diskio_usb_pff.c ipl/fat/stage15_usb.c
endif
# IPL_TRANSPORT also decides which source files are compiled in, not just
# which code paths are guarded.  Dropping gx_spi.c entirely is what actually
# reclaims the ~1.1 KB; guarding the call site alone leaves it in the link
# and --gc-sections cannot remove it because the driver is still reachable
# from ipl_pre_mmu's shared helpers.
ifneq ($(SOC),universal)
ifeq (,$(filter both flash usb,$(IPL_TRANSPORT)))
STAGE1_SPI :=
endif
ifeq (,$(filter both usb stick usb-only,$(IPL_TRANSPORT)))
STAGE15_SRCS :=
endif
endif
STAGE1_SRCS := ipl/start.S $(SOC_IPL_SRC) ipl/ipl_common.c \
	ipl/ipl_config.c ipl/gx_chip.c ipl/efuse.c $(STAGE1_SPI) $(STAGE15_SRCS)
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

.PHONY: all clean bootcode ipl flash-image test flash-probe64 flash-verbose flash-uboot allinone usb-hello size-report

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
all: $(IPL_ARTIFACT).boot $(TARGET)-bootcode.bin $(IPL_ARTIFACT).dis stage15
endif

ipl: $(IPL_ARTIFACT).boot

bootcode: $(TARGET)-bootcode.bin

# Stage-1.5 USB image: the same bootcode, GXBC-wrapped, to copy onto a
# FAT16/FAT32 MBR-partitioned stick as BOOT6702.BIN / BOOT6706.BIN.  The
# filename is fixed by the IPL's uppercase 8.3 probe list; lowercase spelling
# on the host works too because FAT short-name bytes are uppercase.  PFF has no
# long-filename support.  The universal stub has no USB stage, so no
# stage-1.5 image is meaningful for it.
STAGE15_NAME = BOOT6702.BIN
ifneq ($(SOC),gx6702)
STAGE15_NAME = BOOT6706.BIN
endif
STAGE15_IMG = $(STAGE15_NAME)

stage15: $(STAGE15_IMG)

$(STAGE15_IMG): $(TARGET)-bootcode.bin utils/mk_stage15.py
	python3 utils/mk_stage15.py $< --soc $(SOC) -o $@

stage15-image: $(STAGE15_IMG)

# Budget report.  The BootROM loads 8188 B and CRCs the body up to 0x1ff8, so
# 0x1ff8 (8184 B) is the true hardware ceiling.  CODE_LIMIT below is the
# project's own limit, currently set by the linker ASSERT because the IPL
# config is reserved at the top of the body.
CODE_LIMIT := 0x101fc0
size-report: $(IPL_ARTIFACT).bin
	@size=$$(stat -c%s $<); \
	limit=$$(( $(CODE_LIMIT) - 0x100000 )); \
	printf '%-16s %5d B / %5d B  (%4d B free)\n' \
		"$(IPL_ARTIFACT)" "$$size" "$$limit" "$$(( limit - size ))"; \
	if [ "$$size" -gt "$$limit" ]; then \
		echo "ERROR: IPL exceeds the code window by $$(( size - limit )) B"; \
		exit 1; \
	fi

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

# The artifact name deliberately does NOT carry the transport: the documented
# workflow flashes gx6706-ipl.boot, and a variant-specific name would change
# it.  Two variants therefore want the same .elf, and make would otherwise see
# the previous variant's elf as up to date (its objects live in $(BUILD_DIR),
# so they are correct) and skip the link, leaving the wrong image in place.
#
# FORCE_RELINK is set by the parse-time check below, which compares the
# selected variant against the one recorded in the stamp file.  It is
# evaluated before make builds its dependency graph, which is the only place
# a decision like this can actually change whether the rule runs.
$(IPL_ARTIFACT).elf: $(STAGE1_OBJS) $(LINKER_IPL) $(if $(FORCE_RELINK),FORCE_RELINK_STAMP,)
ifeq ($(LTO),1)
	$(CC) $(LTO_LINK_CFLAGS) -nostdlib -nostartfiles $(LTO_LDFLAGS) \
		-Wl,--gc-sections -Wl,-Map=$@.map -T $(LINKER_IPL) -o $@ $(STAGE1_OBJS)
else
	$(LD) $(LDFLAGS_IPL) -Map $@.map -o $@ $(STAGE1_OBJS)
endif
	@mkdir -p $(dir $(VARIANT_STAMP))
	@echo "$(IPL_TRANSPORT)" > $(VARIANT_STAMP)

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
	@echo "IPL_TRANSPORT - which stage-1 boot transports to link in"
	@echo "             both   SPI -> USB -> UART, flash-safe (gx6702 default)"
	@echo "             flash  SPI only, mirrors the vendor flash IPL"
	@echo "             usb    SPI -> USB, the flash-safe gx6706 shape"
	@echo "             uart   UART only, for a host-pushed GxLoader"
	@echo "             stick  USB -> UART, no SPI (gx6706 default)"
	@echo ""
	@echo "             WARNING: stick and usb-only link NO SPI driver, so such an"
	@echo "             image cannot boot from SPI flash and cannot recover if it is"
	@echo "             flashed there. Suitable for bring-up with a UART host only."
	@echo "IPL_ALLOW_NO_SPI - set 1 to silence the above warning deliberately"
	@echo "LTO - enable link time optimization on IPL and bootcode build (defaults to 0)"
	@echo "VERBOSE_MINIFY - minify verbose logging (saves ~2 KB, defaults to 0)"
	@echo "USB_DEBUG - restore USB register dumps and BOT/EHCI error traces (defaults to 0)"
	@echo "             build-time: default 0 removes the strings and call sites entirely"
	@echo "USB_DEBUG_SPI - set 0 with USB_DEBUG=1 to also drop the SPI driver, which"
	@echo "             is required for the trace build to fit 8128 B (default 1)"
	@echo "             use: make SOC=gx6702 USB_DEBUG=1 USB_DEBUG_SPI=0 ipl"
	
