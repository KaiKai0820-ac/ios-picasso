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

## Pipeline đã dựng (2026-10-31)

| Bước | Lệnh / kết quả |
|---|---|
| Firmware | `hakios-mu` = uefi_picasso + `hakios-uefi/hakios.patch` + `files/` + `patches/0002` → `Build/picassoPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd` (2 MiB) |
| Loader | `hakios-u/xnuloader/loader.efi`, build bằng clang-15 + lld-link-15, có `platform_picasso` (`patches/0001`) |
| Container | `xnuloader/mkpayload.py` → loader.efi + kernel + cmdline + usb |
| Boot image | `hakios/build-bootimg.py` → gzip(BootShim + FD + XNUP) + picasso.dtb, header v1 (**7.720.960 B**) |

Chi tiết trong `hakios/README.md` mục dưới; boot image hiện tại ở `/tmp/hakios-picasso-boot.img`.
Backup `boot` trước khi thử: `ad6ea9d0bea679c70c7c3801bbf50939` (128 MiB, trên `/sdcard` của máy).

## Kết quả trên máy (2026-10-31 07:58)

Chain HakiOS **chạy được trên picasso**:

| Mốc | Bằng chứng |
|---|---|
| UEFI (Mu-Silicium picasso + HakiOS XnuLoader) | boot image 7.720.960 B flash vào `boot`, máy lên USB `product: xnuloader` |
| Loader | log trên máy: `platform picasso`, `display 1080 x 2400 at a0000000`, `CNTFRQ=19200000`, `usb: started` |
| Kernel build riêng (sm7250) | `hakios-kernel` 14.800.464 B, `kernel.ver=...-sm7250-...` |
| Kernel chạy | `kernel returned: log at ..., 1306 bytes` — kernel chạy 20 s rồi trả về loader |
| Log kernel | `kprintf initialized`, `initialize_screen: b=A0000000 w=438 h=960`, `gic: v3 0xfffffff155064000`, `Darwin Kernel Version 22.5.0 ... sm7250`, `vm_page_bootstrap: 269057 free pages` |

Kernel `prebuilt` (redwood/sc7280) **treo cứng tại entry** — phải build kernel với config picasso
(`make picasso_defconfig && make kernel`) mới chạy.

### Sửa toolchain để build được kernel trên Ubuntu 22.04
- `patches/0003`: `xnu-deps/host/bin/xnu-clang` dùng clang-15 và bỏ `-ftrivial-auto-var-init=zero`
  (clang <16 từ chối cờ này).
- `patches/0004`: `SETUP/replacecontents` cần `strlcat` (glibc < 2.38 chưa có) — shim static.
- EDK2: Python 3.10 thiếu `tomllib` → file `tomllib.py` re-export `tomli` trong venv.
- `patches/0001`/`0002`: platform_picasso + bỏ UniXNUPkg/DarwinBootPkg + `-DPAYLOAD_BLOCK` cho BootShim.

### Vòng lặp test
`hakios/xl-run.sh [SECS] [args...]` — stage payload + `oem run` + `get_staged` (không dùng `xl` vì
`xl enter` cần ảnh U-Boot/Silicium và `getvar` dễ treo). Sau kernel treo phải **power-cycle**;
kernel trả về bình thường thì loader vẫn sống.

### Tiếp theo
`CONFIG_HAIKOS_UFS=y` + `system.apfs` trong RAM → launchd (bước 3 của guide).

## Trạng thái 2026-10-31 08:25 (sau khi test trên máy)

| Hạng mục | Kết quả |
|---|---|
| UEFI + loader trên picasso | ✅ `platform picasso`, `display 1080x2400 at a0000000`, USB `xnuloader` |
| Identity thật (DT Apple `DeviceTree.d22ap.im4p`) | ✅ loader 71.680 B (trước: 66.048 B placeholder) |
| UFS trong loader | ✅ `ufs: unit 5 ... 109 partitions and 6 units` |
| Kernel sm7250 chạy | ✅ in log, trả về loader sau `loader_return_after` |
| Kernel dừng ở | ❌ ngay sau `vm_page_bootstrap` / `VM boostrap: 0 maps...` (giống nhau với `cpus=1`, `cpus=8`, có/không `soc_*`) |
| `bootcpu=7` | ❌ PSCI: `core 700: the firmware answers 0 to the request to start it` |
| Checkpoints 10–21 (đều trong `arm_init`) | đã qua hết |

Kết luận: `arm_init` xong; dừng trong `machine_startup`/`kernel_bootstrap` trước printf kế tiếp.
Bước tiếp: thêm `loader_checkpoint()` vào các mốc sau `vm_page_bootstrap` (patch riêng) để khoanh vùng,
hoặc lấy log so sánh từ QEMU (`make qemu-open`, đã cài qemu-system-arm + AAVMF;
QEMU 6.2 không có `cortex-a76` → `QEMU_CPU=cortex-a72`).

Phụ thuộc host: `pip install lzfse` (mkidentity), `xnuloader/build.sh` dùng clang-15 (patch 0005).
