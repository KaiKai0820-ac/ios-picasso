# Picasso Novatek active touch

Patch0213 extends the independently identified SPI controller with active
initialization and input reporting. Select `picassonvt=1 sweettouch=0` to use it.
`picassonvtprobe=1` alone retains the previous read-only identification mode.
Both modes start after150seconds, allowing the GUI and persistent log to start.

The active path selects reset4 and IRQ5 states from the live Picasso FDT,
engineering-resets the controller, releases reset, and verifies the known
`0c 00 00 72 66 03` trim ID. It follows the pinned vendor NT36675 hardware-CRC
RAM-download flow: ILM/DLM/info partitions, destination/length/golden CRC banks,
BLD CRC enable, firmware CRC enable and BOOT_RDY. It requires matching CRCs,
CRC_DONE with no ILM/DLM errors, reset state REK, firmware7/inverse248,
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
These tests do not establish physical touch operation; that requires the new
kernel's CRC/firmware/frame logs and observed UI interaction on the phone.

Protocol/init reference:
`LineageOS/android_kernel_xiaomi_sm7250@8e49d06af7ff5757c03ab181874cf1e90a1e1192`,
`drivers/input/touchscreen/nt36672c/nt36xxx{,_fw_update}.c` and
`nt36xxx_mem_map.h`. Board and prior probe evidence: [PICASSO_TOUCH.md](PICASSO_TOUCH.md).
