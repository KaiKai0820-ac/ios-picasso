# HakiOS trên picasso (SM7250, Redmi K30 5G)

Nhánh `hakios` của repo này. **Không đụng nhánh `agy`** (đường ios-sweet/shim cũ vẫn nguyên).

## Vì sao

Repo `oosexgang/hakios-xnu-kernel` chạy **Darwin/iOS 16.5 userland có GUI thật** trên
Qualcomm (POCO X5 Pro, SM7325): lock screen, home, Control Center, app hệ thống, touch,
**GPU Adreno qua Mesa (OpenGL ES)**, Wi-Fi, camera, loa/mic, rung, `/private/var` bền,
Sileo, SSH. Xem `docs/` của họ; tóm tắt:

| Hạng mục | HakiOS (redwood/SM7325) |
|---|---|
| Firmware khởi động | Mu-Silicium UEFI **hoặc** U-Boot (open source) — `fastboot boot`, **RAM only** |
| Loader | `xnuloader` (loader.efi) — đọc payload, dựng memory map + DT, chạy XNU |
| Kernel | XNU (xnu-8796.121.2) + driver riêng (`drivers/`) bật bằng boot-arg |
| Userland | `system.apfs` (~2,4 GiB) **đẩy vào RAM** qua `xl ramdisk` (~80 s/lần vào loader) |
| Ghi UFS | chỉ khi `ufs_write=1 var_store=1` → partition `darwin` |

Vòng lặp phát triển kernel: `xl run` (upload payload 16 MiB = 0,55 s) — **không flash**,
không recovery, không power-cycle.

## Ta có sẵn gì

- **Mu-Silicium UEFI cho picasso**: `YinCQL/uefi_picasso` (đã boot thật trên máy này qua
  UniXNU-picasso) ⇒ đường **Silicium** dùng được ngay, không cần port U-Boot trước.
- **ADT/DT picasso** trong `shim/` + `tools/mkadt.py` (địa chỉ SoC đã kiểm chứng trên máy).
- Partition `darwin` + backup image (`ios-work/work9.img`, `work11.img`).

## Địa chỉ SoC picasso đã biết (từ `tools/mkadt.py`, đã chạy thật)

| Khối | Địa chỉ |
|---|---|
| UFS host | `0x01d84000` (size `0x3000`) |
| GICD / GICR | `0x17a00000` / `0x17a60000` |
| Framebuffer (cont_splash) | `0xa0000000` (35 MiB) |
| smem | `0x80900000` |
| modem_wlan carve-out | `0x8c000000` + `0xF800000` |
| Kernel window ta dùng | `0xac000000` + `0x50000000` |
| ECID (synthetic) | `0x5357454554000001` |

Cần bổ sung (lấy từ device tree thật của máy): IPCC, AOSS, RSC, apps-SMMU, cmd_db,
IPA/GSI, mpss/smem/imem, PAS ids, tên firmware (`*_ipa_fws`), panel, touch, PMIC.

## Kế hoạch

1. **Thu dữ liệu máy** (khi nối điện thoại, Android/recovery):
   `hakios/pull-picasso-fdt.sh` → `hakios/picasso/fdt.dtb`, cmdline, soc_id, partitions.
2. **Dựng skeleton device repo** `hakios-picasso` theo mẫu `hakios-sm7325`
   (`device.conf`, `soc.args`, `configs/`, `uefi/` trỏ tới picasso UEFI).
   SoC layer mới: `--soc sm7250` (HakiOS chưa có SM7250).
3. **Mốc 1**: loader + kernel của họ boot trên picasso tới `userland/hello`
   (chỉ `fastboot boot`, không flash).
4. **Mốc 2**: UFS read → `system.apfs` từ RAM.
5. **Mốc 3**: display + touch → thấy màn hình.
6. **Mốc 4**: GPU (Adreno 620) qua Mesa → có GUI.

## Ghi chú

- Nhánh này **không** sửa `xnu/patches` của agy; mọi thay đổi HakiOS nằm dưới `hakios/`.
- Repo tham chiếu đã clone: `/home/tritm/hakios-u` (core, nhánh `u-boot`),
  `/home/tritm/hakios-sm7325` (device + SoC layer mẫu).
