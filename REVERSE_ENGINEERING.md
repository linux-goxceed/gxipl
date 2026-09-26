# GX6702/GX6706 IPL reverse-engineering notes

## Boot chain

The UART and flash BootROMs both load a fixed **8 KiB** stage-1 window into
SRAM at `0x00100000`. UART command `0x59` plus word count `0x0800` plus
`boot[0x20:0x201C]` (**8188 bytes**) plus the `boot` marker is the same on
GX6702 and GX6706. The `toob` chip ID at offset 6 is host metadata (vendor
gxdl / libre-gxdl pick 8 KiB vs 4 KiB vs 16 KiB layouts). Neither 16 KiB
BootROM image compares that field, `toob`, or `0x6705` during UART Stage 1.
UART does not run the flash CRC-32 (`0x04c11db7`) over SRAM `0x00101FF8`.
GX6706 may emit extra status bytes after `0x59`; hosts ignore junk until
`GXID`. Flash BootROM stays family-split: Cygnus verifies that CRC, Gemini
keeps the legacy trailer. There is no single flash `BOOT.bin` for both
families.

The reset-vector branch then enters the IPL implementation at
`0x00100040`. The legacy vector used a `jsri` whose target came from a
PC-relative literal pool; its two opcode bytes cannot safely be copied into a
newly linked image. The open vector instead uses a direct relative `br`.

Stage-1 ends by loading a `GXBC`-headed bootcode image from SPI offset
`0x4000` (or UART `GET`). Bootcode then tries USB FAT → SPI U-Boot → UART.

```text
u16  sum(payload) & 0xffff
u16  type = 0x00c2
u32  payload size
u8   payload[size]
```

## Recovered initialization boundary

The legacy entry sequence called three logical phases:

- clock setup at legacy address `0x001002f0`;
- DDR setup at legacy address `0x00100534`;
- vendor stage-2 receive at legacy address `0x001007f8`.

Only the first two are hardware initialization. This is why replacing the call
at `0x001000d8` produced a working loader but was not an open IPL.

## Important blocks

| Block | Physical address | Purpose |
| --- | ---: | --- |
| Early always-on controls | `0x04d00204`, `0x04d00300` | Reset-time masks/control retained from the proven entry path |
| System/clock control | `0x0030a000` | PLLs, gates, resets, DDR PHY control |
| Clock routes | `0x00601000` | Per-domain clock divider/route words |
| UART0 | `0x00402000` | Boot-time 16550-compatible UART |
| DDR controller | `0x00c00000` | Controller registers and training status |
| eFuse command/status | `0x00f80080` / `0x00f80088` | Optional DDR trim source |
| DDR physical window | `0x10000000` | Pre-MMU DDR access |
| UART0 uncached alias | `0xa0402000` | UART after MMU enable |
| U-Boot virtual entry | `0x93ce8420` | Raw stage-2 destination and entry |

The DDR controller profile consists of registers `0x000..0x268`, registers
`0x400..0x464`, two source-level field-patch lists, and 29 optional eFuse trim
descriptors. The canonical readable form of every one of these lives in the IPL
C sources (`ipl/ipl.c`, `ipl/gx6706_ipl.c`) so individual GPIO/clock/DDR
assumptions stay inspectable and changeable without copying vendor instructions.

To fit the 8 KiB stage-1 window with room for later bootcode features, the
compiled image uses size-golfed encodings of that same data, all derived from
the readable source and verified by `tests/`:

- Field-patch and eFuse-trim entries are packed into one `u32` each by the
  `FP()`/`ET()` macros in `include/ipl_internal.h` instead of 6/8-byte structs.
  The macro arguments remain the readable `(reg, shift, width, value)` form.
- The two DDR register windows are RLE-compressed by `utils/gen_ddr_rle.py`
  into `build/<soc>/ddr_rle.h` at build time and decoded straight into the
  controller by `ddr_apply_rle()`. The generator parses the canonical literals
  in the C sources and `tests/test_ddr_rle.py` asserts the decoded stream is
  byte-identical to them, so the tables cannot silently drift.
- The `SOC=universal` build keeps the uncompressed arrays because it shares
  `ddr_regs_0` between the Gemini and Cygnus paths.

With `LTO=1` (default) and `IPL_MIN=1` (default) the stage-1 body is 3690 bytes
on GX6702 and 4378 bytes on GX6706, inside the 7680-byte window enforced by
`ld/linker-8k.ld`. `IPL_MIN=0` restores the GXUB bundle receive path and the
legacy bring-up-uploader checksum fallback (3914 / 4610 bytes).

The CK610 coprocessor/MMU sequence in `start.S` is intentionally kept in
assembly. It matches the working mapping exactly: IPL SRAM remains executable,
DDR appears at `0x90000000`, and uncached MMIO appears at `0xa0000000`.

The open IPL also installs clock routes 1--16 and the recovered final
`0x0030a170..0x0030a17c` state before entering U-Boot. The vendor flow left
SRAM helper routines for a later loader to do this; calling those fixed SRAM
addresses is invalid once the whole stage has been replaced.

## HD2015 front-panel (8051 / low-power domain)

The HD2015 front panel is **not** driven by the main CK610 CPU. It is bit-banged
by the SoC's standby **8051 (LPC / low-power) core** using a TM1650/FD650 2-wire
protocol on 8051 pins **P1.5 = CLK, P1.6 = DAT**; the pads are permanently muxed
to the LPC domain. The stock system works because eCos loads `gxlowpower.fw` into
LPC code-SRAM (`0xA0100000`) and starts the 8051. The open IPL omits this,
expecting U-Boot/Linux to start the 8051 core so both panel lines idle stuck-high. 
Full RE notes, the exact loading procedure, the CRG suspects for the 8051 start 
bit (prime: `0xA030A178`, never write 0), and the next-session experiment plan 
live in `../HD2015_8051_PANEL_NOTES.md`.

## Container trailer / flash stage-1 CRC

UART downloads tolerate the legacy vendor word `33 de a1 89` at body offset
`0x1ff8` (the UART path does not verify it).

**Flash BootROM does verify the stage-1 trailer.** After copying
`BOOT[4:0x2000]` → `0x00100000` (8188 bytes), BootROM `@0x650` computes an
MSB-first CRC-32 (poly `0x04C11DB7`, init `0xFFFFFFFF`, no final XOR) over
`SRAM[0x00100000:0x00101FF8]` and compares it to the word at `0x00101FF8`.
Mismatch → UART download fallback (`B0`/`B8` … `X`). Flash images must write
that CRC into the trailer (see `mk_flash_image.py`).

**GxLoader `CRC32 Enable` / TABLE per-partition CRCs are separate.** They are
checked by GxLoader when it trusts the partition map (LOGO/KERNEL, …), not by
BootROM when it maps the first 8 KiB of BOOT. Open IPL cold-boots after a
BOOT-only `serialdown` even if TABLE still lists a stale BOOT CRC.

## GX6706 clean-room initializer

The Cygnus H5 and S5 stage-1 bodies are byte-identical for all 8188 BootROM
payload bytes. Their completed-initialization entry is `0x001007ec`; X5 has a
longer, board-identifying implementation and uses `0x001008e4`. The open port
uses H5/S5 as the generic baseline and does not link or copy vendor code.

All raw code was wrapped as little-endian ELF32 with C-SKY flags
`0x11000002`, then disassembled explicitly with `-m csky:ck610 -EL`. Leaving
`e_flags` at zero changes how current binutils decodes ABI-v1 instructions.
`gxtest/tools/dump/disassemble_bootrom.py` performs this wrapping for BootROM
captures. The BootROM itself was used to recover the transport, fixed window,
chip-family ID and CRC behavior; the H5/S5 loader was the initialization
source.

### H5/S5 initialization boundary

| Phase | Vendor address | Open implementation |
| --- | ---: | --- |
| PLL/clock/UART | `0x001002f8` | `gx6706_clocks_init()` |
| DDR/PHY/eFuse | `0x00100418` | `gx6706_ddr_init()` |
| stage-2 loader | `0x001007ec` | shared MMU/boot selection |

The recovered PLL output words are `0x00211063` at `SYS+0xc0` and
`0x00112038` at `SYS+0xc4/+0xc8`. UART is initialized first from 24 MHz and
again from 29.4912 MHz after the route changes; its reset controls are bits
0/1 at `SYS+0x480`. The resulting vendor loader reports 672 MHz for CPU and
DDR in all three `loader-test` captures.

The four early route descriptors are:

| Route | Divider value | Gate |
| ---: | ---: | ---: |
| 10 | `0x01745d17` | `SYS+0x174`, bit 17 |
| 11 | `0x01999999` | `SYS+0x174`, bit 18 |
| 15 | `0x08000000` | `SYS+0x170`, bit 2 |
| 19 | `0x10000000` | `SYS+0x174`, bit 15 |

DDR is represented as 155 controller words at offsets `0x000..0x268`, 31
words at `0x400..0x478`, 52 bit-field patches and 50 eFuse-driven trims.
Calibration bytes are read from eFuse `0x159..0x15f`; the low nibble at
`0x138` selects the silicon variant. Variant 3 additionally writes
`DDR+0x144=0x2828`, `DDR+0x410=0x43039e03` and `DDR+0x460=0x4305`. Training is
complete when bit 5 at `DDR+0xb8` asserts. The `SYS+0x124` bit-29 control is
set only for variant 1; the available board reports variant 3 (`kgd:3`) and
the known-good H5/S5 initializer leaves that bit clear.

The MMU startup stays shared with GX6702. The vendor initializer does not
access the DDR aperture before that transition. During bring-up, disposable
open and vendor-comparison RAM diagnostics entered immediately after their
matching MMU setup, retained SRAM code/stack, and used the CK610 uncached
direct-map alias `0xb0000000` for physical DDR `0x10000000`. Those payloads
were removed after the 64 MiB alias and destructive sweeps passed.

### GX6706 late peripherals

The USB setup is a clean-room transcription of the H5/S5 late-loader path: USB
PLL output `0x0011102d`, 15 clock-route descriptors, 10 system-register field
descriptors, pad controls at `SYS+0x1b0/+0x200`, PHY trim writes at
`0xa0702018/0xa0702418`, configuration at `0xa090a000`, and EHCI at
`0xa0904000`.

#### Where the USB tables actually come from

The transcription source is **`libre-gxdl/loaders/cygnus-6706S5-sflash-24M.boot`**,
which is a complete working GX6706 GxLoader (stage-1 + stage-2) — not
`re-boot/extracted_partitions/BOOT-128k-gx6706.bin`. That file is
`0x0000..0x1fff` stage-1 followed by stage-2 from `0x2000`; the stage-2 image
runs at `0x93ce6400`, so `file_offset = runtime - 0x93ce6400`.

The USB tables are stage-2 data, not code, and the loop helpers that consume
them are the real specification:

| table | runtime address | file offset | count × stride |
| --- | --- | --- | --- |
| clock route | `0x93d00aac` | `0x1a6ac` | 15 × 12 B |
| field patch | `0x93d00b60` | `0x1a760` | 10 × 24 B |

| record | size | layout |
| --- | --- | --- |
| clock route | 12 B | `[u8 index][u8 gate][u16 pad][u32 value][u32 gate_mask]` |
| field patch | 24 B | `[u8 target][u8 gate][u16 pad][u32 clear][u32 first][u32 second][u32 value][u32 gate_mask]` |

A route record with `index == 0` is skipped. Otherwise the register is
`0xa0600ffc + index*4`, written as `value|BIT(30)` and then
`value|BIT(30)|BIT(31)`, and `gate_mask` is OR-ed into the gate register
(`gate` 1 → `0xa030a170`, 2 → `0xa030a174`). A field record patches target
`0xa030a024` / `0xa030a178` / `0xa030a17c` for `target` 1 / 2 / 3 as
`(reg & ~clear) | value | second`, then re-writes with `| first`.

#### Route table bug found on hardware (fixed)

Route index 8 had been transcribed as `0x09245fd9`. The real value is
**`0x09249249`** — the plain 1/7-stride pattern its neighbours also use
(`0x05555555`, `0x0ccccccc`, `0x05d1745d`). The stray bits left that clock
route mis-muxed, so EHCI never completed a transfer: the async qTD stayed
`Active` with the error counter saturated and `USBSTS` reported AAE. Removing
the value also removed the `if (r->index == 8)` special case that had been
OR-ing bits 6/7 into `0xa030a174` to compensate.

The other 14 route records and all 10 field records were re-checked against
the vendor loader and match byte-exact. `cygnus_usb_clocks()` is now a
faithful transcription, including the trailing gate OR-lists
(`0xa030a170`: `0x07000000|BIT(30)|BIT(23)|0x300001e0|0x00070000`;
`0xa030a174`: `BIT(1)|BIT(2)|BIT(14)|0x1f800000|BIT(4)`).

**Verified on real GX6706 hardware:** with only this correction the loader
enumerates the stick, mounts FAT, reads `config.txt` and `start.elf`, and
jumps to the loaded ELF.

#### EHCI notes: do not derive endpoint speed from PORTSC

`PORTSC` bit 2 (PED) being set does **not** prove the high-speed chirp
succeeded — a full-speed device also gets PED. And `PORTSC` bits 27:26 (PSPD)
is not usable on this part: it reads `0` (full speed) even when the link is
genuinely high-speed. A high-speed stick is identifiable from its descriptor
instead, since a 512-byte bulk `wMaxPacketSize` is legal only at high speed.

Forcing the QH `Endpt1` EPS field from PSPD makes the device reject the very
first 8-byte SETUP with `XACT_ERR` and breaks `SET_ADDRESS`. The QH speed field
stays hardcoded high-speed, as U-Boot's `ehci_encode_speed()` would report for
a high-speed device.

The `USBERR BOT CSW status=1` line in verbose logs is expected: it is the soft
`TEST UNIT READY` probe, a zero-length SCSI command with no data stage, whose
return value `usb_msc_init()` deliberately ignores.

#### Disassembling CK610 binaries

`objdump -m csky` defaults to cskyv2 and emits garbage for these images, and
`-m csky:ck610` on a raw binary aborts. Wrap the image in an ELF and set the
architecture flags first:

```sh
csky-linux-objcopy -I binary -O elf32-csky-little \
  --set-section-flags .data=alloc,load,readonly,code in.bin out.elf
# patch e_flags at 0x24 to 0x11000002
csky-linux-objdump -d out.elf
```

The vendor stage-1 jumps to the stage-2 image at `0x93ce6400`. Basing that
image at `0x93ce0d30` (the first address seen in an earlier bad import) shifts
every routine by `0x56d0` and turns literal-pool data into plausible-looking
code. With the correct base, GxLoader's SPI registration routine at
`0x93cf1410` identifies the real controller as a conventional 32-bit
DesignWare SSI at uncached `0xa0e00000`; `0xa0700000` is the wrapper aperture,
not the SSI register file.

The controller uses the standard offsets: CTRLR0 `+0x00`, CTRLR1 `+0x04`,
SSIENR `+0x08`, SER `+0x10`, BAUDR `+0x14`, FIFO levels `+0x20/+0x24`, status
`+0x28`, IMR `+0x2c`, version `+0x5c`, and data `+0x60`. GxLoader performs
32-bit loads and stores. Its neighboring wrapper initialization at
`0x93ced730` writes `0` then `0xc0` to `0xa0701000`, applies the pad mux at
`SYS+0x1f0`, and sets bits 0 and 1 at `0xa0701280` before SSI registration.

The repeated-byte values captured at `0xa0700000` were wrapper-aperture bus
behavior, not packed SPI registers. The open driver now keeps the recovered
wrapper sequence but accesses `0xa0e00000` with 32-bit operations. A failed
JEDEC probe dumps every aligned controller slot through `+0x60` and `+0xf4`,
along with the clock-gate, route, byte-pad and wrapper registers. The matching
vendor capture hooks the GxLoader main path after its successful NOR probe and
before partition parsing; see `gxtest/GX6706_BOOTROM.md`. Both outputs use
machine-readable `SPIREG` and `SPIABS` records so the same capture/compare
approach can later be applied to GX6702 flash compatibility.

The expanded snapshot also includes the `0xa480xxxx` IO-domain block touched
by the wrapper routine before SSI registration. The first expanded open dump
proved this block remained at reset state. ABI-correct disassembly shows the
complete sequence, including two selector globals at `0x93d00e38/+0x3c` whose
initialized values are both `6` for the H5/S5 image. The clean-room path now
reproduces its masked clock-selector writes, reset sequence, low-byte clears,
and fixed routing words before applying the SPI route and calibrated pads.

The first post-probe comparison exposed state that was not obvious from the
registration routine alone. On the available variant-3 part, GxLoader reads
eFuse bytes `0x126/0x12a` and leaves wrapper pads `0xa0701284=0xd9` and
`0xa0701280=0xcb`; the earlier open path retained the reset/default `0x7b`
value. It also writes byte `5` at `SYS+0x794`, uses CTRLR0 `0x807`, divider
`6`, keeps SER at `1`, and disables SSI between messages. ABI-correct stage-2
disassembly then exposed the active transfer protocol: on the 1.02a core,
GxLoader sends the opcode/address with CTRLR0 `0x407` (TX-only), retains SER
at `1` while disabling SSI, then programs CTRLR0 `0x807` (RX-only), CTRLR1 to
the receive count minus one, and writes one zero to DR to start the receive
clocks. Crucially, the message layer writes `2` to the version-specific
external-CS latch (`0xa030a904` on the 1.02a core) before those phases and
restores `3` after both finish. This keeps the NOR selected across the SSIENR
toggle; leaving the latch at its captured idle value explains completed reads
that returned only `0xff`. A follow-up capture also established
`RX_SAMPLE_DLY (+0xf0)=4`; leaving it at reset produced `10 20 0b`, precisely
the expected XMC JEDEC bytes `20 40 16` shifted right by one bit. The open
driver follows the complete message sequence rather than assuming normal
full-duplex operation. Register `+0x34` is the raw interrupt-status register:
its earlier bit 0 came from leaving SSI enabled with an empty TX FIFO, not
from RX overflow.

The X5 GxLoader's `chip name` routine at `0x93cecd74` copies 12 bytes from
`0xa030a190`, reverses the byte array, and returns it to the banner printer.
The available board yields `6706S5-NNNBE`. The H5/S5 GxLoader does not print
this early line, but the field is a read-only silicon identification register,
not a string embedded in the X5 loader.

GxLoader's `public id` is separate from that chip name and from the BootROM
transport family ID. The S5 routine at `0x93cecd50` (shifted to `0x93cecda8`
in the X5 build) copies eight bytes from `0xa030a560` and applies the same
reversal. Equivalently on the little-endian CK610, the open bootcode prints
the word at `0xa030a564` followed by the word at `0xa030a560`. The available
board's expected display is `8909b32d026c1518`. Neither field is the GX6706
UART-container family value `0x6705` or the SPI JEDEC ID `0x204016`.

The identification register layout is shared with Gemini. The GX6702H5
GxLoader routine at `0x93cecbbc` reads and reverses the same eight bytes at
`0xa030a560`; the reference hardware reports `7f08ab7154378bad`. That H5
loader does not request a chip-name string. The related Gemini 6703X5 loader
does: its routine at `0x93cece20` reads and reverses the same 12 bytes at
`0xa030a190`, and its decoder contains `6702H5`, `6702S5`, and `6701`
mappings. The open GX6702 banner therefore exposes both fields, while treating
a non-printable chip-name register as unavailable. Stock GX6702 runtime dumps
show raw words `0x4e4e4200 0x53352d4e 0x36373032`; reversal, with the leading
raw NUL treated as trailing string padding, produces `6702S5-NNNB`. The vendor
board detector compares only the first six characters and consequently emits
the shorter `6702S5` before the NOR banner.

The UART chip-probe stub reads the same 12-byte field at physical
`SYS_BASE+0x190` (`0x0030a190`) before DDR, maps `6701`/`6702`/`6703` to
Gemini clocks and `6705`/`6706` to Cygnus clocks, then prints a
machine-parseable line before `RUNGET`:

```text
GXID family=gemini name=6702S5-NNNB\r\n
```

`family` is `gemini` or `cygnus` when open DDR init exists. `gx6616` and
`gx3211` (8 KiB UART window) and `gx6612` (10 KiB IPL in a 16 KiB BootROM
window, see `PROTOCOL.md`) are detection-only: untested, no DDR, `ENODDR`,
no `RUNGET`. Completely unknown silicon prints `family=unknown` and `ECHIP`.
`name=` is the reversed printable string or `unavailable`. Hardware still
needs to confirm that one CRC-sealed universal body with a `0x6701` header
reaches `GXID` on a GX6706 UART box, and a `0x6705` header does the same on
GX6702 UART; BootROM must not hang on the envelope.

GX6702 cross-board RDID results also separate flash support from controller
transport: a box identified by GxLoader as EN25Q32 returns `1c 30 16`, while a
box identified as XM25QH32B/XM25QH32C/XM25QE32C returns `20 40 16`. Both are
valid 4 MiB devices and both are read correctly by the open gxflash path. The
corresponding U-Boot fork originally supported the EON `0x1c3016` table entry
but disabled XMC and lacked its `0x204016` entry, explaining why only the
second box failed detection.

GX6702's gxflash engine transfers only complete 32-bit words. JEDEC RDID is
therefore sent as `9f 00 00 00`. The byte sampled during the opcode appears in
DATA bits 7:0 and is discarded; manufacturer, memory-type and capacity bytes
are read from DATA bits 15:8, 23:16 and 31:24 respectively. This is the same
lane handling used by the U-Boot `spi-mem` implementation, and lets the open
bootcode report an ID before TABLE/BOOT parsing on both supported SoCs.

## GX6706 BootROM and flash format

The UART header uses family chip ID `0x6705`. The container is a 32-byte
`toob` header followed by the same fixed 8192-byte body used by flash. The
BootROM consumes 8188 bytes before the trailer. GX6706 requires the MSB-first
CRC-32 (`0x04c11db7`, init `0xffffffff`, no XOR-out) at body `0x1ff8`.

The stock GX6706 image used for recovery here has a 128 KiB BOOT partition.
That size is an SDK/firmware layout choice, not a Cygnus hardware limit:
Gemini (GX6702) images are also built with 128 KiB BOOT, and a Cygnus image
can use 64 KiB. Generated images in this tree default to 128 KiB for
`SOC=gx6706` (stage-1 body at file offset `0x4`, `GXBC` stage-2 at `0x4000`,
TABLE immediately after BOOT). TABLE multi-byte partition fields remain
big-endian; its per-BOOT CRC and TABLE CRC use the existing reflected/zlib
CRC-32 convention, not the BootROM trailer algorithm.
