# GX6702 IPL reverse-engineering notes

## Boot chain

The UART and flash BootROMs both load a fixed **8 KiB** stage-1 window into
SRAM at `0x00100000`. UART: header byte `0x08`, host payload
`boot[0x20:0x201C]` = **8188 bytes**. Flash: `BOOT.bin[4:0x2000]` with a
CRC32 trailer at body `0x1FF8`.

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
descriptors. Keeping these as typed data makes individual GPIO/clock/DDR
assumptions inspectable and changeable without copying vendor instructions.

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
that CRC into the trailer (see `mk_flash_probe64.py`).

**GxLoader `CRC32 Enable` / TABLE per-partition CRCs are separate.** They are
checked by GxLoader when it trusts the partition map (LOGO/KERNEL, …), not by
BootROM when it maps the first 8 KiB of BOOT. Open IPL cold-boots after a
BOOT-only `serialdown` even if TABLE still lists a stale BOOT CRC.