# Open IPL for the NationalChip SoC's

This directory contains a vendor-blob-free first-stage loader and a
DDR-resident bootcode (Raspberry Pi `bootcode.bin`-style) for NationalChip chipsets
such as the GX6702 (Gemini 6702H5, C-SKY CK610 ABIv1).

**Note:** the bootcode after IPL is in a WIP stage, so there will be bugs when using it and is not recommended for production usage.

## Boot flow

1. SoC BootROM loads the SRAM stage-1 IPL. **Both UART and flash BootROM map
   a fixed 8 KiB window** (UART: 8188-byte payload, header field `0x08`;
   flash: `BOOT.bin[4:0x2000]`). Larger stage-1 images are not supported.
2. Stage-1: PLL, DDR, MMU, then loads DDR bootcode from SPI (`0x4000`) or
   falls back to UART (`GET`).
3. Bootcode: banner + **USB (FAT32) → SPI → UART** with verbose logs gated by
   a 512-byte writable IPL config at the end of the stage-1 image.

USB path reads `config.txt` (`start_file=`) and loads a **static** C-SKY
CK610 ABIv1 ELF32 (`start6702.elf`) whose `PT_LOAD` segments and entry lie in
mapped DDR (`0x90000000`–`0x94000000`). PIE/dynamic images and other arches
are rejected. SPI/UART load raw U-Boot at `0x93ce8420`.

## Supported platforms

Currently only GX6702 is supported and tested, chances are it will work on other NationalChip SoC's as they reuse IP blocks across generations but may need modifications to run it on other SoC's.

## Build

To build the IPL, you will need a installed C-SKY ABI v1 toolchain which you can build one [here](https://github.com/matu6968/csky-gcc-build-scripts).

```sh
make CROSS_COMPILE=/opt/gxtools/.../bin/csky-linux-
# artifacts: gx6702-ipl.boot, gx6702-bootcode.bin, optional BOOT.bin
make BOOT.bin
```

Options:

| Variable | Meaning |
|----------|---------|
| `IPL_CHIP=GX6702` | Banner chip tag |
| `IPL_VERSION_BASE=1.0.0-open` | Version; Git short hash appended when available |

```sh
python3 utils/iplcfg.py gx6702-ipl.boot --show
python3 utils/iplcfg.py gx6702-ipl.boot --verbose 0 -o gx6702-ipl-quiet.boot
```

## RAM-only test

Upload with `--full` (UART BootROM window is always 8 KiB):

```sh
cd ../gxtest
python3 gxupload_smoke.py \
  -b ../gxipl/gx6702-ipl.boot \
  --full --uboot ../u-boot/u-boot.bin \
  -d /dev/ttyUSB0
```

Two-stage (bootcode over UART, then U-Boot):

```sh
python3 gxupload_smoke.py \
  -b ../gxipl/gx6702-ipl.boot \
  --full --bootcode ../gxipl/gx6702-bootcode.bin \
  --uboot ../u-boot/u-boot.bin \
  -d /dev/ttyUSB0
```

Single-file UART loader (open IPL + U-Boot):

```sh
python3 ../u-boot/board/nationalchip/gx6702/mkgxboot.py \
  gx6702-ipl.boot ../u-boot/u-boot.bin ../u-boot/u-boot-gx6702.boot
python3 ../libre-gxdl/libre_gxdl.py \
  -b ../u-boot/u-boot-gx6702.boot -d /dev/ttyUSB0

# Or use the bring-up uploader without a separate --uboot argument:
python3 ../gxtest/tools/flash/gxupload_smoke.py \
  -b ../u-boot/u-boot-gx6702.boot --full -d /dev/ttyUSB0
```

The combined image uses a checksummed `GXUB` record understood by the open
IPL. It contains no vendor stage-1 or stage-2 code. Raw `--uboot` uploads
remain supported.

You can get `gxupload_smoke.py` from [gxutils](https://github.com/linux-goxceed/gxutils).

Early breadcrumbs remain `I` / `R` / `U` / `N` (suppressed when verbose). Use
`--full` so the uploader answers the final `RUNGET` marker (`GET`-only matching
remains compatible).

## IPL config (512 bytes)

Last 512 bytes of the stage-1 body (`0x00101e00`). Read only after DDR/MMU.
Invalid magic/CRC → defaults (verbose on). Flags include verbosity, skip
USB/SPI/UART, `uart_direct`, `force_uart`. The BootROM trailer word at
`0x1FF8` is preserved inside this window (excluded from the config CRC).

## Flash SPI-bootcode probe (64 KiB BOOT)

Flash BootROM maps only `BOOT.bin[4:0x2000]` → SRAM (same **8 KiB** window as
UART) and **CRC-checks that window** against the trailer at `0x1FF8` (unlike
UART). That is the only flash integrity check BootROM performs.

**Partition `CRC32 Enable: TRUE` is not a BootROM gate.** GxLoader uses the
TABLE’s per-partition CRCs when *it* boots LOGO/KERNEL; rewriting BOOT without
updating TABLE still cold-boots an open IPL as long as the stage-1 trailer CRC
is correct (confirmed: `serialdown BOOT` alone → open banner). Update TABLE
anyway so listings match and a return to stock GxLoader does not trip on a
stale BOOT CRC.

```sh
make flash-probe64 CROSS_COMPILE=/opt/gxtools/.../bin/csky-linux-
# -> BOOT-flash-probe64.bin + TABLE-flash-probe64.bin
```

Flash BOOT (required). TABLE is optional for open-IPL bring-up, recommended
for a consistent map:

```sh
# replace the libre-gxdl and loader path to your real path where libre-gxdl/loaders are
LOADER=../libre-gxdl/loaders/gemini-6702H5-sflash-24M.boot
python3 ../libre-gxdl/libre_gxdl.py -b "$LOADER" -d /dev/ttyUSB0 \
  -c "serialdown BOOT BOOT-flash-probe64.bin"
python3 ../libre-gxdl/libre_gxdl.py -b "$LOADER" -d /dev/ttyUSB0 \
  -c "serialdown TABLE TABLE-flash-probe64.bin"
```

Cold-boot with `tio` open first:

| UART | Meaning |
|------|---------|
| `B0`/`B8` … `X` only | BootROM UART fallback — stage-1 trailer CRC wrong or IPL rejected |
| open banner / `IRUN` | Flash mapped & ran the 8 KiB IPL (trailer CRC OK) |
| `FLASHBC` | SPI loaded the tiny payload from `0x5000` |
| `GET` / `OK` | Tiny payload ready for UART U-Boot |

Restore stock: `serialdown BOOT ../BOOT.bin` then stock `TABLE.bin`.

## Flash verbose boot (U-Boot-only upload)

```sh
make flash-verbose
# -> BOOT-flash-verbose64.bin + TABLE-flash-verbose64.bin
```

8 KiB IPL (BootROM CRC trailer) + full verbose bootcode at `0x4000`. Flash
BOOT (TABLE optional for open IPL; update it when you care about GxLoader’s
partition CRCs), then cold-boot and upload only U-Boot:

```sh
# replace the libre-gxdl and loader path to your real path where libre-gxdl/loaders are
LOADER=../libre-gxdl/loaders/gemini-6702H5-sflash-24M.boot
python3 ../libre-gxdl/libre_gxdl.py -b "$LOADER" -d /dev/ttyUSB0 \
  -c "serialdown BOOT BOOT-flash-verbose64.bin"
python3 ../libre-gxdl/libre_gxdl.py -b "$LOADER" -d /dev/ttyUSB0 \
  -c "serialdown TABLE TABLE-flash-verbose64.bin"

# After flash: reset board; host skips IPL upload
python3 ../gxtest/gxupload_smoke.py --uboot-only \
  --uboot ../u-boot/u-boot.bin -d /dev/ttyUSB0 -v
# optional: --dtr-reset to pulse reset after the waiter starts
```

Expect the GoXceed banner (no `IRUN` crumbs when verbose), USB miss, then SPI
reads the GxLoader **BOOT** partition (first TABLE entry) and prints its real
offset/size, e.g. `Reading: BOOT (GxLoader partition on 0x0, size 64KiB)`.
Like stock GxLoader, the next-stage bootloader is packed **inside BOOT** as a
second `GXBC` (after stage-2 bootcode, default `@0x10000` when BOOT is large
enough). On miss → UART `waiting for download`.

```sh
# 64 KiB BOOT (IPL + bootcode only) — SPI announces BOOT then falls back to UART
python3 utils/mk_flash_probe64.py --full-bootcode

# GxLoader-style: IPL + bootcode + U-Boot in one BOOT (grows partition + TABLE)
python3 utils/mk_flash_probe64.py --uboot ../u-boot/u-boot.bin
```

## Flash layout (`splice_boot.py` / `mk_flash_probe64.py`)

| Offset | Content |
|--------|---------|
| `0x0` | `AA55AA55` |
| `0x4` | IPL body (8 KiB) |
| `0x4000` | `GXBC` + stage-2 bootcode (`0x5000` in the flash-probe image) |
| `0x10000` | `GXBC` + U-Boot when BOOT is grown (`--uboot`); else TABLE starts here on stock 64 KiB BOOT |

See `REVERSE_ENGINEERING.md` for the recovered boot chain and register map.

## License

MIT

Additionally, this project uses [ChaN FatFS](https://elm-chan.org/fsw/ff/), which it's licensed under [these terms](fatfs/LICENSE.txt).
