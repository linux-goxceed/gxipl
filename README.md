# Open IPL for NationalChip GX6702 and GX6706

`gxipl` is a clean-room, vendor-blob-free first-stage loader and DDR-resident
bootcode for NationalChip C-SKY CK610 ABIv1 SoCs. Plain `make` retains the
GX6702 build. `SOC=gx6706` selects the generic Cygnus H5/S5 initialization
path recovered from the byte-identical vendor H5 and S5 stage-1 bodies.
`SOC=universal` builds a UART-only chip-probe stub that trains DDR from the
silicon name register, prints `GXID`, then waits for `RUNGET`.

U-Boot itself is not part of this repository or port.

## Current status

The GX6706 implementation includes:

- the 672 MHz PLL, clock-gate, DDR timing/training, eFuse calibration and UART
  initialization recovered from H5/S5;
- the shared CK610 SRAM startup and MMU map, with 64 MiB DDR at
  `0x90000000`–`0x94000000` and uncached MMIO at `0xa0000000`;
- UART recovery, `GXBC` loading, a Cygnus DesignWare-style SPI NOR reader,
  TABLE/BOOT discovery, and USB EHCI/FAT32 loading;
- the GX6702/GX6706 chip name and eight-byte public ID in the verbose bootcode
  banner;
- GX6706 BootROM containers, and this tree's default 128 KiB BOOT/TABLE
  packaging for that SoC (BOOT size is an SDK/image choice, not silicon).

The checked-in `../loader-test` console captures prove that the H5, S5 and X5
vendor loaders all reach DDR-resident GxLoader on the available board, read its
SPI flash and report 672 MHz CPU/DDR. On 2026-08-30, the open generic H5/S5
initializer additionally passed the 64 MiB alias test and the complete
destructive sweep on that board: `RAM SWEEP PASS size=64MiB passes=70`. Thus
the capacity and open DDR initialization are hardware-verified on the available
unit. SPI JEDEC and BOOT reads are also hardware-verified. USB mass-storage
loading and repeated cold-boot testing remain hardware-validation gates.

## Boot flow

1. BootROM loads an 8 KiB SRAM stage-1 body at `0x00100000`. UART Stage 1 is
   shared across Gemini and Cygnus; the `.boot` chip ID is host-only.
2. Stage-1 initializes UART, clocks and DDR, enables the shared MMU layout,
   prints `GXID`, then loads a `GXBC` bootcode record from SPI offset `0x4000`
   or UART (`RUNGET`). The universal UART stub skips SPI.
3. Bootcode tries USB FAT32, SPI BOOT and UART in that order. `force_uart` and
   the existing skip flags can change this selection.

USB accepts a static little-endian C-SKY ELF32 ABIv1 executable whose entry and
all `PT_LOAD` ranges fit in DDR. The default is `start6702.elf` on GX6702 and
`start6706.elf` on GX6706. `config.txt` can override it with:

```ini
[gx]
start_file=my-loader.elf
```

## Build

A C-SKY CK610 ABI-v1 cross-toolchain is required. The repository's existing
toolchain default remains in the Makefile and can be overridden normally.

```sh
# Unchanged default
make

# Generic GX6706 IPL + bootcode
make SOC=gx6706 all

# UART chip-probe stub (no flash image, no bootcode)
make SOC=universal
# optional vendor-gxdl second envelope (do not send both members on UART)
make SOC=universal allinone

# Host format tests
make test
```

### Stage-1 size knobs

The IPL must fit the 7680-byte code window (8 KiB body minus the 512-byte
config at `0x1e00`), enforced by an `ASSERT` in `ld/linker-8k.ld`. Two knobs
control how much of the optional functionality is compiled in:

```sh
# Default: LTO on, minimal stage-1 (3690 B on GX6702, 4378 B on GX6706)
make SOC=gx6702 ipl

# Full feature set: adds the GXUB bundle receive path and the legacy
# bring-up-uploader checksum fallback (3914 B / 4610 B)
make SOC=gx6702 IPL_MIN=0 ipl

# Disable link-time optimization (larger, easier to correlate with .dis)
make SOC=gx6702 LTO=0 ipl
```

`IPL_MIN=1` (the default) drops only two things from the UART loader: the
`GXUB` U-Boot bundle record appended to an IPL container, and the old
`expected >> 16 == 0x00c2` checksum fallback used by the bring-up uploader.
The SPI `GXBC` boot path, plain `RUNGET` bootcode/raw U-Boot receive, and every
protocol string (`RUNGET`, `EMETA`, `ECHK`, `OK`, `EBC`, `EBCCHK`, `EPLL`,
`EDDR`) are byte-identical in both modes. `EBUNDLE`/`EBUNDLECHK` are emitted
only by `IPL_MIN=0` builds.

Each build also writes `<soc>-ipl.elf.map` so section and symbol sizes can be
inspected directly.

The GX6706 build produces:

```text
gx6706-ipl.boot       8224-byte UART container / 8 KiB body
gx6706-ipl.dis        CK610 ABI-v1 disassembly
gx6706-bootcode.bin   DDR-resident stage-2
```

Create a flash-layout pair with:

```sh
make SOC=gx6706 flash-image
# BOOT-gx6706.bin  = 128 KiB in this tree's default packaging
# TABLE-gx6706.bin = matching 512-byte partition table
```

The GX6706 container uses family chip ID `0x6705`. Its trailer at body offset
`0x1ff8` is the MSB-first CRC-32 (`0x04C11DB7`, init `0xffffffff`, no final
XOR) over body bytes `0x0000..0x1ff7`.

## UART chip-probe stub

Flashers that do not know the board can send one SRAM image. Stage-1 reads the
chip-name register, trains Gemini or Cygnus DDR, then identifies itself:

```text
GXID family=gemini name=6702S5-NNNB\r\n
RUNGET
```

That `GXID` line is the ident API. Hosts must ignore BootROM handshake noise
(`B0`/`B8`/`X` and GX6706 extra status bytes) until it appears. Map
`family=gemini` to `gx6702-bootcode.bin` and `family=cygnus` to
`gx6706-bootcode.bin` (DDR init family, not a BOOT partition size). This
tree's flash helpers default to 64 KiB BOOT for `SOC=gx6702` and 128 KiB for
`SOC=gx6706`, but that is SDK/firmware layout: a GX6702 image can be built
with 128 KiB BOOT, and a GX6706 image can use 64 KiB. Read the on-flash TABLE
for the real size.

`family=gx6616` and `family=gx3211` share the 8 KiB Stage 1 window with
Gemini/Cygnus (`PROTOCOL.md`). `family=gx6612` is a 10 KiB IPL in the 16 KiB
`0x6612` BootROM transfer. None of those three have open DDR init or hardware
coverage: the stub prints `GXID` then `ENODDR` and does not issue `RUNGET`.

```sh
make SOC=universal
make bootcode
make SOC=gx6706 bootcode
python3 libre_gxdl.py -b gx-universal-ipl.boot -d /dev/ttyUSB0 \
  --bootcode-dir .
```

After `GXID`, the host sends `gx6702-bootcode.bin` or `gx6706-bootcode.bin` as
`GXBC`. If that file is missing it stops; it does not resend the 8 KiB stub
as vendor Stage 2 (`EBUNDLE`).

`gx-universal-ipl.boot` is an 8 KiB UART container with chip ID `0x6701` (so
vendor gxdl still sees Gemini) and a Cygnus-style CRC-32 trailer.
`utils/mkboot.py --soc universal` writes extra 8 KiB targets into `GXMT`
(`0x6705` / `0x6616` / `0x3211`). Other wraps stay zeros unless you pass
`--extra-chip-id` (repeatable) or `--no-extra-chip-ids`. `libre_gxdl.py`
prints the catalog when present; Stage 2 still follows `GXID`. The earlier
Gemini UART confirmation used a zeros-reserved image and still selected
`gx6702-bootcode.bin` from `GXID family=gemini name=6702S5-NNNB`. Stock
`boot.elf` ignores the reserved field (it still prints `chip: 6701` and
uploads). It then sends the 8224-byte file as Stage 2; open IPL answers
`EBUNDLE` because there is no `GXUB`/`GXBC` — that is IPL-only, not a header
reject. Use `libre_gxdl.py --bootcode-dir .` for Stage 2. Flash CRC remains
Cygnus-only; `SOC=universal` does not produce a flash `BOOT.bin`.

Hardware: that one `.boot` reached GoXceed on GX6702S5-NNNB. Cygnus still
needs the same stub plus `gx6706-bootcode.bin`. A `0x6705`-headered copy of
the same body is still untested on UART.

Do not concatenate an optional `GXAI` second envelope into open-IPL Stage 2
(`EBUNDLE`). Open flashers send the shared Stage 1 once.

## RAM-only functional bring-up

After the initializer runs prove 64 MiB, upload `gx6706-ipl.boot` and its
DDR-resident bootcode entirely over UART. This exercises the normal open-IPL
MMU transition and `GXBC` handoff without writing flash:

```sh
python3 ../gxtest/tools/flash/gxupload_smoke.py \
  -b gx6706-ipl.boot --bootcode gx6706-bootcode.bin \
  --full -d /dev/ttyUSB0 --stream
```

The stage-1 uploader should first receive `GET`. Successful DDR execution is
then identified by the `NationalChip GoXceed Bootcode` banner with `(GX6706)`.
With no usable USB or SPI payload, the bootcode should reach `waiting for
download`; stop the stream with Ctrl-C and hardware-reset.

Continue validation in this order:

1. stable banner/UART and the MMU transition;
2. UART-loading and execution of a small DDR-resident payload;
3. SPI JEDEC/read behavior, TABLE discovery at `0x20000`, and a stage-2 `GXBC`
   read from `0x4000`; on failure, save the aligned 32-bit register dump between
   `SPI REGDUMP BEGIN` and `SPI REGDUMP END`;
4. FAT32 USB loading of `start6706.elf`;
5. missing/corrupt USB and SPI payloads falling through to UART.

Typical verbose bootcode output starts with `GX6706`, then:

```text
Attempting to boot from USB...
No USB device detected, trying SPI...
SPI JEDEC ID: 0x00xxxxxx
Reading: BOOT (GxLoader partition on 0x00000000, size 128KiB)
Unable to boot from BOOT (GxLoader partition), trying UART (60s timeout)...
```

GX6702 uses the same `SPI JEDEC ID: 0x00xxxxxx` line. Its RDID operation is
one native gxflash word (`9f 00 00 00`); the response byte received during the
opcode is discarded and DATA lanes 1–3 form the displayed ID. A failed probe
also reports the gxflash CTRL, STAT and DATA words plus the SPI route register,
which can be compared between boxes before debugging U-Boot's SPI-NOR match.
Two tested GX6702 boxes report `0x001c3016` for an EON EN25Q32 and
`0x00204016` for an XMC XM25QH32B/XM25QH32C/XM25QE32C, respectively. These
successful reads verify the gxflash RDID path on both boards; U-Boot must
enable both `CONFIG_SPI_FLASH_EON` and `CONFIG_SPI_FLASH_XMC` and include the
4 MiB XMC `0x204016` part in its SPI-NOR table.

Use the existing 512-byte configuration at body address `0x00101e00` to
select verbosity and skip/direct/force behavior:

```sh
python3 utils/iplcfg.py gx6706-ipl.boot --show
python3 utils/iplcfg.py gx6706-ipl.boot --verbose 0 -o gx6706-ipl-quiet.boot
```

Its binary layout is unchanged from GX6702. The trailer bytes inside the
configuration window are excluded from the configuration checksum.

## Flash backup, test and restore

Only proceed after the RAM, UART, SPI and USB gates pass. The vendor UART
loader remains the recovery path even if the on-flash IPL is broken.

```sh
LOADER=../libre-gxdl/loaders/cygnus-6706S5-sflash-24M.boot
GXDL=../libre-gxdl/libre_gxdl.py

# Back up before writing anything.
python3 "$GXDL" -b "$LOADER" -d /dev/ttyUSB0 \
  -c "serialdump BOOT 131072 backup-BOOT-gx6706.bin"
python3 "$GXDL" -b "$LOADER" -d /dev/ttyUSB0 \
  -c "serialdump TABLE 512 backup-TABLE-gx6706.bin"
sha256sum backup-BOOT-gx6706.bin backup-TABLE-gx6706.bin

# Write the generated pair, BOOT first and TABLE second.
python3 "$GXDL" -b "$LOADER" -d /dev/ttyUSB0 \
  -c "serialdown BOOT BOOT-gx6706.bin"
python3 "$GXDL" -b "$LOADER" -d /dev/ttyUSB0 \
  -c "serialdown TABLE TABLE-gx6706.bin"
```

Test repeated cold boots, USB success, SPI success, media-missing UART
fallback and corrupt-payload rejection. Restore with the same `serialdown`
commands using the two backup files. Keep power stable while TABLE or BOOT is
being written.

## Flash artifact layout

This tree's default `make SOC=gx6706 flash-image` uses a 128 KiB BOOT. The
on-device partition can be 64 KiB or 128 KiB depending on how that firmware
was configured in the SDK, on either Gemini or Cygnus.

| Location | Default GX6706 packaging in this tree |
| --- | --- |
| BOOT `0x00000` | `AA55AA55` marker |
| BOOT `0x00004` | fixed 8 KiB stage-1 body |
| BOOT `0x04000` | `GXBC` + open DDR bootcode |
| flash `0x20000` | updated TABLE partition (after a 128 KiB BOOT) |
| BOOT `0x10000` | optional second `GXBC` payload when explicitly packaged |

`make` / `SOC=gx6702` defaults to a 64 KiB BOOT and TABLE at `0x10000`. See
`REVERSE_ENGINEERING.md` for the clean-room register recovery notes.

## License

MIT. ChaN FatFs retains the terms in `fatfs/LICENSE.txt`.
