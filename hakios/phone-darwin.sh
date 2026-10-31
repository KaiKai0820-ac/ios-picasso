#!/bin/bash
# Dua HakiOS + Darwin userland 16.5 len picasso.
#
#   hakios/phone-darwin.sh [/tmp/boot-loader.img] [SECS]
#
# 1. flash boot image (chi co loader.efi + usb: loader o lai, khong tu chay kernel)
#    -> can ABL fastboot: giu Volume Down + Power
# 2. loader len; nap userland/system.apfs (2,3 GB) vao RAM bang `oem ramdisk` + `stage`
# 3. chay kernel (ban da fix redzone) + root.apfs (md1) + cmdline cua .config
# 4. theo doi, fetch log, in cac moc va launchd
set -u
IMG=${1:-/tmp/boot-loader.img}
SECS=${2:-120}
H=${HAKIOS:-/home/tritm/hakios-u}
cd "$H" || exit 1

fastboot devices 2>/dev/null | grep -q fastboot || { echo "phone-darwin: khong thay fastboot (Vol-Down + Power)"; exit 1; }
prod=$(timeout 8 fastboot getvar product 2>/dev/null | sed -n 's/^product: //p')
[ "$prod" = "picasso" ] || { echo "phone-darwin: dang o '$prod', can ABL fastboot (Vol-Down + Power)"; exit 1; }

echo "== 1. flash $(basename "$IMG")"
timeout 180 fastboot flash boot "$IMG" 2>&1 | tail -2 || exit 1
timeout 20 fastboot reboot >/dev/null 2>&1

echo "== 2. cho loader"
for i in $(seq 1 20); do fastboot devices 2>/dev/null | grep -q xnuloader && { echo "loader up"; break; }; sleep 3; done
fastboot devices 2>/dev/null | grep -q xnuloader || { echo "phone-darwin: loader khong len"; exit 1; }

echo "== 3. nap system.apfs ($(du -m userland/system.apfs | cut -f1) MB) vao RAM"
t0=$(date +%s)
timeout 60 fastboot oem ramdisk 2>&1 | grep -q OKAY || { echo "phone-darwin: oem ramdisk tu choi"; exit 1; }
timeout 1800 fastboot stage userland/system.apfs 2>&1 | tail -1 | grep -q OKAY || { echo "phone-darwin: nap system.apfs that bai"; exit 1; }
echo "   xong sau $(( $(date +%s) - t0 ))s"

echo "== 4. chay kernel"
SOC=$(grep -v '^#' devices/picasso/soc.args | grep . | tr '\n' ' ')
./xnuloader/mkpayload.py -o run-payload.bin kernel=hakios-kernel ramdisk=userland/root.apfs \
	--cmdline "loader_return_after=$SECS epoch=$(date +%s) -v debug=0x8 hv_wdt_pet=1 cpus=1 $SOC" >/dev/null || exit 1
timeout 120 fastboot stage run-payload.bin 2>&1 | tail -1 | grep -q OKAY || { echo "phone-darwin: stage kernel that bai"; exit 1; }
timeout 30 fastboot oem run 2>&1 | tail -1
end=$(( $(date +%s) + SECS + 90 )); OUT=/tmp/phone-darwin-$(date +%H%M%S).log
while [ "$(date +%s)" -lt "$end" ]; do
	r=$(FB_TIMEOUT=15 timeout 20 fastboot getvar result 2>/dev/null | sed -n 's/^result: //p')
	[ -n "$r" ] && case $r in *running*) ;; *) echo "phone-darwin: result: $r"; break;; esac
	sleep 2
done
timeout 40 fastboot get_staged "$OUT" >/dev/null 2>&1
echo "phone-darwin: log $OUT ($(stat -c %s "$OUT" 2>/dev/null || echo 0) B)"
tr -d '\0' < "$OUT" 2>/dev/null | grep -aE "VM boostrap|launchd|SpringBoard|resident image|panic|Process 1" | head -20
