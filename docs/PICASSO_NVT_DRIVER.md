# Picasso Novatek active touch

Patch0213 extends the independently identified SPI controller with active
initialization and input reporting. Select `picassonvt=1 sweettouch=0` to use it.
`picassonvtprobe=1` alone retains the previous read-only identification mode.
Both modes start after150seconds, allowing the GUI and persistent log to start.

The active path selects reset4 and IRQ5 states from the live Picasso FDT,
engineering-resets the controller, releases reset, and verifies the known
`0c 00 00 72 66 03` trim ID. It follows the pinned vendor NT36675 hardware-CRC
RAM-download flow: ILM/DLM/info partitions, destination/length/golden CRC banks,
BLD CRC enable, firmware CRC enable and BOOT_RDY. Patch0215 adds exact readback
of all97,584 downloaded bytes while the MCU remains in the bootloader.
It requires CRC_DONE with no ILM/DLM errors, reset state REK, firmware7/inverse248,
matrix18x36 and panel1080x2400 before reporting input. It programs no controller
flash, PMIC, or display power state.

The public firmware is byte-identical to the built-in firmware in this
device's saved Recovery. `tools/picasso-nvt-firmware.py` pins both the public
Intel HEX SHA256 and decoded binary SHA256, validates records, version,
download-end marker and partition bounds, and creates a temporary build patch.
Firmware bytes and generated headers are excluded from the source repository.
CI generates patch0214 before applying the normal patch series.

The SPI FIFO now streams complete67-byte transactions rather than imposing
the64-byte FIFO capacity as a transaction limit. The65-byte point packet
decodes up to10slots. The worker forwards down/move/up through existing
`sweet_hid_touch`, polls every8ms, ignores unchanged contacts and releases
held contacts on failure. Empty FF slots are normal release frames; watchdog
FD/FE and recovery77 patterns are ignored and a persistent bad stream stops
the worker. Automatic watchdog reload and suspend/resume are not implemented.

Validation: actual XNU source compiles as ARM64 Mach-O using matching CI flags
and generated headers. Run `python3 tools/tests/nvt-touch-regression.py` on
Linux to reconstruct the patched driver and test its actual FIFO and HID loop
with ASan/UBSan. Tests include full-duplex partial words and64-byte boundaries,
transfer error/timeout, down/move/up, slot10, empty releases, malformed IDs,
duplicate slots, coordinate bounds and releasing held fingers after bus errors.
With `--source /path/to/patched/bsd/dev/picasso_nvt.c` and its generated firmware
header present, tests also exercise the actual init/download functions against
a mock controller: reset ordering, all real partition bytes, CRC bank values,
CRC failure flags, reset timeouts and rejecting a different chip. The mock
supplies CRC results; hardware CRC execution is still a device-only check.
These tests do not establish physical touch operation; that requires the new
kernel's CRC/firmware/frame logs and observed UI interaction on the phone.

Protocol/init reference:
`LineageOS/android_kernel_xiaomi_sm7250@8e49d06af7ff5757c03ab181874cf1e90a1e1192`,
`drivers/input/touchscreen/nt36672c/nt36xxx{,_fw_update}.c` and
`nt36xxx_mem_map.h`. Board and prior probe evidence: [PICASSO_TOUCH.md](PICASSO_TOUCH.md).

The first active image, local31i, built successfully in userfork CI41177965312
at efc0d615 and was flashed with readback verification on2026-10-31 at14:57:48.
Full128MiB boot SHA256:
`280593774c5dc94d2e6b7c5489eb1b1b5f4f0e9ee9ab6688083e10a3dbb92389`.
The original Recovery and preceding full boot/ring are preserved. USB
enumeration is observed; controller CRC/firmware gates, physical input and
UI response still need on-device verification.

## First active hardware result and patch0215

Fresh local31i ring (387524console bytes, flush91, independently verified
SHA25670e84444cebe33ea6001fcd2338c0620d9b65ee4a62cd4d1cdf55de47c2bc624)
shows reset4 high, IRQ5 input, all three partition writes completed, firmware
INIT0xa0 and CRC_DONE0x4 with no error flags. ILM raw CRC matches, but the
DLM raw snapshot is0x1377dab7 rather than0x41e3fa54. Our additional comparison
returns EIO before fw-info or HID polling. This establishes the immediate
reason input was never enabled; it does not establish the cause of the raw
DLM difference or prove the touch pipeline works.

The pinned vendor driver uses reset readiness for successful download and
reads raw CRC result registers separately, four bytes each, on failure.
Patch0215 matches those read widths, verifies every transferred RAM byte
before BOOT_RDY and requires hardware CRC status, REK readiness and fw-info.
A differing post-boot result snapshot is logged, while genuine CRC error
flags, missing CRC_DONE, RAM mismatches, read errors or reset timeouts stop
initialization. Tests cover these outcomes using actual extracted functions.
Hardware validation of this change is pending.

Recovery comparison on the same device: existing Novatek Linux driver reports
firmware7, matrix18x36, valid1080x2400 info and NORMAL_RUN0xa3; CRC status is04.
Read-only raw CRC diagnostics nevertheless show golden ILM288823b4,
golden DLM28cb9da3, result ILMacc6fe83 and result DLM4d845bdc. These values
do not match the pinned startup constants or one another. Separate4byte and
combined8byte reads agree. This supports treating post-boot snapshots as
diagnostic, and does not establish why the registers differ after startup.
