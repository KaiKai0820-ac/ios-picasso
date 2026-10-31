# Touch on the attached Redmi K30 5G

The 2026-10-31 phone has displayed the stock iOS Siri onboarding page.
Power and volume-down events work. Touch remains disabled: the existing
`sweet_touch.c` probes Goodix/FocalTech over I2C and does not drive this board's
Novatek SPI controller. `sweettouch=1` is not a fix.

## Observed hardware

Recovery root ADB and the phone's live FDT agree on:

| Item | Value |
| --- | --- |
| Linux input name | NVTCapacitiveTouchScreen |
| Driver family | nt36672c / NVT-ts-spi |
| SoC serial engine | GENI SPI, `0x00984000`, size `0x4000` |
| Touch transfer configuration | mode 0, 8 bits, 5 MHz maximum |
| Reset / IRQ | TLMM GPIO4 / GPIO5 |
| Serial pins | GPIO6, GPIO7, GPIO8, function `qup11` |
| TLMM region | `0x0f000000`, size `0x01000000` |
| Trim ID | `0c 00 00 72 66 03` |
| Firmware / PID | version 7 / 5931 |
| Sensor matrix | 18 x 36 |
| Reset / fast-read register | `0x3f0fe` / `0x3f310` |
| Selected firmware | novatek_nt36672c_g7a_fw02.bin |

The FDT's SPI controller default maximum is not the touch device's frequency;
use the latter's 5 MHz setting. The generic pinctrl initially includes GPIO9,
but the picasso override uses only GPIO6/7/8. Do not copy the generic pin list
or the sweet driver's north-TLMM base.

## Verified reference and firmware

Reference repository: [LineageOS/android_kernel_xiaomi_sm7250][source], commit
`8e49d06af7ff5757c03ab181874cf1e90a1e1192`.
This is a source reference, not a claim that it is Recovery's exact commit.

- `drivers/input/touchscreen/nt36672c/nt36xxx.c`: SPI framing, chip identification,
  input packets, GPIO and panel selection.
- `nt36xxx_mem_map.h`: the observed trim ID matches **NT36675_memory_map**,
  despite the driver directory and firmware filenames saying nt36672c.
  Its event buffer is `0x22d00`; do not select NT36672A's `0x21c00` by filename.
- `nt36xxx_fw_update.c`: host-download partition parsing and CRC validation.
- `drivers/spi/spi-geni-qcom.c`: serial-engine transport.
- `arch/arm64/boot/dts/vendor/qcom/picasso-sm7250.dtsi`: board overrides.
- `firmware/Makefile`: includes the selected Novatek firmware in the kernel
  when `CONFIG_TOUCHSCREEN_NT36xxx_HOSTDL_SPI=y`, even with empty
  `CONFIG_EXTRA_FIRMWARE`.

The Intel HEX firmware was decoded with record checksums and address continuity
verified. **All 139,264 bytes match the original Recovery kernel exactly** at
kernel-image offset 29,752,368. The original Recovery image's SHA256 was checked
against the pre-flash backup before comparison. Firmware SHA256:
`9daa042550e1c8bd703c18b4ad6d0218a46bc24e6a4a7667417c1c4edcf495b3`.

The complete firmware file is 139,264 bytes; the vendor driver's final `NVT`
sector marker selects **131,072 bytes as the download range**. These are
different sizes. Firmware version 7 and its complement also validate.
Binary, source snapshot and comparison manifest remain outside Git in
`k30-preparation/diagnostics/novatek-reference`.

## Work required in XNU

1. Establish a picasso-specific GENI SPI transport, clocks and pinmux from the
   Linux reference and live board values. Use bounded command timeouts and
   stop on transport errors. No controller MMIO probe has been attempted yet.
2. Read and match the trim ID before selecting the memory map. Read the
   firmware state; if host download is required, use the matching firmware
   and port the vendor partition/CRC sequence. A raw linear firmware write
   does not implement the vendor download format.
3. Read event packets with the correct dummy-byte handling: SPI reads add one
   dummy byte; the Linux helper copies received bytes from offset 2 into the
   caller's offset 1. It clears bit 7 for reads and sets it for writes.
   Page selection sends `ff, addr >> 15, addr >> 7`.
4. Decode contact ID/status and 12-bit X/Y coordinates, handle release events
   and transport failures, then connect to `sweet_hid_touch`. Verify the
   existing framebuffer/HID coordinate transform before claiming touch works.

The first acceptance milestone is a matching chip ID in the XNU log; the
second is actual down/move/up input reaching iOS.

## Identification probe prepared

`xnu/patches/0210-picasso-novatek-spi-probe.patch` adds an opt-in
`picassonvtprobe=1` path ahead of the legacy touch driver. It waits 150 seconds
so the persistent logger (default delay 120 seconds) is running, then checks
the serial-engine protocol/FIFO state, reads the trim ID and, on a match,
reads firmware information through the NT36675 memory map. It emits no HID
events and performs no controller reset or firmware download. It returns
without touching hardware when the boot argument is absent.

The probe uses Lito-specific values verified from the Linux reference:

| Item | Lito/picasso value |
| --- | --- |
| GCC wrapper1 vote register | `0x52008`, bits 18-21 and 23 |
| SE7 root clock | command `0x18140`, branch `0x1813c` |
| Wrapper core clock branches | `0x23138`, `0x23140` |
| Wrapper AHB branches | `0x18004`, `0x18008` |
| SPI pin register bank | TLMM WEST `0x0f500000` |
| Reset and IRQ pin register bank | TLMM EAST `0x0f900000` |

FIFO transactions are limited to 32 bytes and bounded by hardware FIFO
depth. Clock, transfer and abort polls have finite timeouts; a transfer error
stops the probe. Unsupported protocol, occupied engine, disabled FIFO, or a
DFS level that is not XO/1 cause the probe to stop before transactions.
Probe clock is at most 480 kHz. The reset and IRQ GPIOs are not reconfigured.

Both the new source and its legacy-driver entry point compile locally as
ARM64 Mach-O objects against the headers and compile flags from successful
CI run `41087536020`. The patch reverse-applies cleanly to the resulting
source tree. These checks verify compilation and patch placement, **not
hardware communication**. No probe kernel has been linked or flashed yet;
the phone continues running the confirmed local31d GUI boot.

## Separate display and stability work

`tools/picasso-quiet-boot.py` repacks the existing verified boot with
`sweetfb_noconsole=1 sweetufshb=0`, retaining `sweettouch=0`. Both embedded
payload command lines and the Android image ID are updated; all other bytes
remain identical. Source SHA256, both payload copies, RAM range, image layout
and output readback are checked. It never flashes a phone.

The console is muted after the first GUI swap, while persistent logging and
panic rendering remain available. The user confirmed that local31d again
shows lock/Siri UI and that the console overlay has disappeared. A SpringBoard
shared-cache slide SIGBUS observed on the previous boot is a separate
unresolved stability issue.

[source]: https://github.com/LineageOS/android_kernel_xiaomi_sm7250/tree/8e49d06af7ff5757c03ab181874cf1e90a1e1192
