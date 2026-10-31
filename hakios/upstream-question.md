# Câu hỏi cho upstream HakiOS (picasso / SM7250)

**Bối cảnh**: port mới `picasso` (Redmi K30 5G, SM7250). Mu-Silicium picasso (YinCQL fork) + `hakios-uefi`
(platform_picasso thêm trong `xnuloader/loader.c`), identity thật từ `DeviceTree.d22ap.im4p`, kernel tự build
`make picasso_defconfig && make kernel` (chỉ driver chung + socinfo/cpufreq/thermal/ufs).

**Đã chạy được**: loader `platform picasso`, display 1080x2400 @ a0000000, USB, UFS survey (109 partitions),
kernel in log tới `vm_page_bootstrap`.

**Điểm dừng** (đo bằng marker ghi vào ring log loader, một lần chạy):

```
CP92 CP100 CP101 CP109 CP112 CP113 CP114 CP115 CP116
```

tức `kernel_bootstrap()` → `vm_mem_bootstrap()` → `vm_map_init()` xong (CP109) → `kmem_init()` →
`vm_map_create_options()` → **`zalloc_id(ZONE_ID_VM_MAP, Z_WAITOK|Z_NOFAIL|Z_ZERO)` không trả về** (CP117 không tới).

**Số liệu ngay trước đó** (dump `loader_cp_hex`):

```
printf: "VM boostrap: 0 maps, 0 entries and 0 holes available"
H1=0x4000 map_data_size   H2=0x4000 kentry_data_size   H3=0x4000 map_holes_data_size
H4=fffffff140974000 map_data   H5=fffffff14097c000 map_holes_data
H6/H7/H8 = 0   zone_count_free(vm_map_zone / entry / holes) SAU zone_cram_early
```

**Câu hỏi**:
1. Trên redwood, dòng `VM boostrap: ...` in ra số nào? Nếu khác 0 thì zone early của chúng tôi không được
   cram đúng; nếu cũng 0 thì điểm dừng thật sự nằm trong `zalloc_id`.
2. Điều gì khiến `zalloc_id(Z_WAITOK)` chặn ở giai đoạn này trên một SoC mới (grow zone cần `kernel_map`
   đang được tạo)? Có cần thêm bước/SoC hook nào cho SM7250 (ví dụ khác biệt PMAP/`pmap_virtual_region`)?
3. Nếu có thể, cho biết log kernel đầy đủ của một lần boot redwood ngay sau `VM boostrap` để so sánh thứ tự.

Chi tiết tái lập: `hakios/README.md` mục "2026-10-31 10:05"; nhánh `hakios` của repo này; fork
`minhtritt1996/hakios-xnu-kernel` nhánh `picasso` (platform + build fix).
