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
| Touch chip select | GPIO9, function `qup11`, 6mA, no pull |
| TLMM region | `0x0f000000`, size `0x01000000` |
| Trim ID | `0c 00 00 72 66 03` |
| Firmware / PID | version 7 / 5931 |
| Sensor matrix | 18 x 36 |
| Reset / fast-read register | `0x3f0fe` / `0x3f310` |
| Selected firmware | novatek_nt36672c_g7a_fw02.bin |

The FDT's SPI controller default maximum is not the touch device's frequency;
use the latter's 5 MHz setting. The picasso SPI controller override lists only
GPIO6/7/8, but **the touch device separately selects `ts_cs_active` on GPIO9**
through its own `pinctrl-0`. Both groups are necessary. Earlier notes and the
initial probe incorrectly treated the controller's group as the complete pin
configuration. Do not use the sweet driver's north-TLMM base.

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
   stop on transport errors. local31f reaches SPI but returns an all-FF trim ID;
   the initial probe's missing GPIO9 chip-select configuration is corrected in0212.
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
hardware communication**. Full-kernel CI run
[`41169768099`](https://github.com/KaiKai0820-ac/ios-picasso/actions/runs/41169768099)
then built and linked the probe successfully at commit `4f36537`, with the build
job taking 7m14s. The artifact archive matches GitHub's SHA256, and the kernel
matches its bundled `SHA256SUMS`:
`7f6829d1944f81e8e24b30d035dc486400cae784c52a7b60dd2eb9e0388ce43c`.

The named `shim/out/shim-boot-local31e.img` is prepared: 37,130,240 bytes,
SHA256 `94ba2e2fa6503a64ef7c6c5ce6a47a76751e0a728166aada977c37385136d7f5`.
Both payload copies, actual boot command, original ADT/full DTB, stock OS
word and proven RAM range validate. Independent official mkbootimg
reconstruction is byte-identical. The known-good local31d image was preserved.
The test was deployed and rebooted at13:01:09, with every written range and
the full128MiB boot partition verified. Full boot SHA256:
`6112a4d76e4c13f191bf637cb3ed2b1873be29a85bfe791806894e7c47fc126b`.
Recovery remains byte-identical. Windows enumerates Sweet iOS USB Ethernet;
the initial screen observation is the logo. Neither identification nor GUI
on this test kernel is established yet; the next step is collecting the
persistent ring through Recovery after the delayed probe has run.

### First hardware result and clock-check correction

The user confirms local31e shows lock/Siri UI with no touch response.
Fresh ring `boot-log-20261031-130553` contains only marker local31e;
338,201 console bytes, flush47. Its full4MiB+4KiB read matches an independent
phone SHA256. The delayed probe executes, then exits before serial-engine
access or any SPI transaction:

```
PICASSO NVT: clock vote=0xbc1000 rcg=0 cfg=0x100000 dfs=0x21
PICASSO NVT: clock branch 0x18008 halted (0x80000002)
PICASSO NVT: initialization stopped; GUI input is unchanged
```

This exposed a probe check error. At the pinned Linux revision, gcc-lito's
wrapper1 slave AHB branch declares HWCG bit1 at0x18008. `clk-branch.c` skips
the halt poll when HWCG is enabled; an idle voted branch can have CLK_OFF set.
The revised probe follows that rule only for this declared HWCG branch,
checks that all requested vote bits latched, and uses Linux's branch2 ON/FSM
condition for the other branches. It preserves HWCG and does not force clocks
on by changing branch configuration. Both modified source files compile
locally and the regenerated patch reverse-checks successfully. This correction
still needs a new full-kernel build and hardware result.

The same ring separately records a SpringBoard exception, shared-cache slide
errors and an HFS catalog-node forward-link error. Recovery raw reads of the
reported node19212 have valid links19218/19210; this does not establish that
the full filesystem is healthy. No filesystem repair or root rewrite was
performed. The runtime failures need separate investigation; at least the
first SpringBoard/text-page errors precede the delayed touch hardware probe.

The HWCG correction then built successfully in run
[`41171448815`](https://github.com/KaiKai0820-ac/ios-picasso/actions/runs/41171448815),
7m4s, at commit `18eabeb`. The downloaded archive and kernel both pass their
SHA256 checks. Named local31f boot SHA256:
`17ab31eaab549d9baa7c46ad9cdafbaae1030b7650de71cca6479889c2a7800b`.
It independently reconstructs byte-identically with official mkbootimg and
retains the original ADT/full DTB, RAM range and OS word. It was deployed and
rebooted at13:21:50; full128MiB boot readback SHA256:
`94f27f01638b45b56b901ba722d1f36901774e3a84e35b9c1c998c7cc86324cc`.
Original Recovery is unchanged. Its delayed hardware result is recorded below.
Recovery also confirms raw touch bounds0..1079/0..2399, matching the existing
SweetHID input normalization before its display offset is applied.

### local31f transport result

The independently verified ring in `boot-log-20261031-133218` reaches the SPI
engine after the corrected HWCG check:

```
PICASSO NVT: slave AHB hardware-gated (0x80000002); halt poll skipped
PICASSO NVT: SE=0x984000 fw=0x101 status=0 tx=0x20102864 rx=0x20104800 fifo-off=0
PICASSO NVT: trim err=0 id=ff ff ff ff ff ff
PICASSO NVT: probe complete match=0; no touch events enabled
```

Transfer completion does not establish a responding controller: no valid ID
matched. Power/reset/chip-select/state remain to be checked. At the pinned Linux
reference, `nt36672c/nt36xxx.c:nvt_ts_check_chip_ver_trim()` resets the bootloader
before each trim read. The reference Picasso dtsi declares reset register
0x03f0fe and SPI-fast register0x03f310; `nvt_bootloader_reset()` writes0x69,
waits5ms, then disables SPI-fast mode. Probe initialization also performs an
engineering reset before raising reset GPIO4 and waiting10ms. The dtsi includes
a separate TDDI reset on PM8150L GPIO3. These are reference sequences to investigate,
not steps executed by the current identification-only probe.

The user reports local31f remains at the logo. Initial HFS/SpringBoard failures
precede the150s probe. A definite shared UFS staging race was separately reproduced
and patched; see [PICASSO_UFS.md](PICASSO_UFS.md). The next stability boot disables
this probe explicitly, retaining `sweettouch=0`.

### Live upstream audit and chip-select correction (2026-10-31)

The user's original repository was checked live at
[`dipa-studio/ios-sweet@45263bee`](https://github.com/dipa-studio/ios-sweet/tree/45263bee51df4a0f55cdf9c649af886399488ddb).
Its README specifies Redmi Note10 Pro/SM7150 and Goodix GT9896 over I2C, not
Novatek SPI on SM7250. Patches0058/0060/0063/0071 are byte-identical to our local
versions after line-ending normalization. Patch0062 differs only in the existing
Picasso RPMh/cmd-db/USB-rail additions. No upstream touch implementation was
missing from this checkout.

The original Picasso fork was also checked live: default `hakios` atb946d2ad
and `agy` atd3d9c2e each contain203 XNU patches already byte-identical to ours.
The `main` branch has115 matching patches, and `omp` has the same touch patches
(its only differing XNU patch is USB reset handling0171). These checked branches
do not add a Novatek XNU driver. External snapshots and verified Git blob hashes
are saved in `diagnostics/upstream-touch-20261031` outside Git.

The audit exposed a specific omission in our own probe. Saved live FDT SHA256
`57576eb51c207af433413b781c2d2226c64338dd587488c79d042f8b61ec7291`
has `/soc/ts_novatek/pinctrl-0 = <0x446 0x447 0x448>`. The final reference resolves
to `ts_cs_active`, with GPIO9/qup11/6mA/no pull. The pinned Linux Picasso dtsi
independently agrees. Linux `pinctrl-lito.c` places GPIO9 in WEST with mux1.
The old probe configured only GPIO6/7/8 and left CS dependent on bootloader state.
This can explain an all-FF response, but is not yet proven to be the cause.

Patch0212 includes GPIO9, extends the WEST map to0xa000, verifies pinmux readback,
and logs reset4/IRQ5 registers read-only through a separate EAST map. It does not
toggle reset, upload firmware, or emit touch events. The actual modified source
compiles as ARM64 Mach-O against matching CI headers/flags, and the patch
reverse-application check passes. Full-kernel/device testing remains pending.

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
