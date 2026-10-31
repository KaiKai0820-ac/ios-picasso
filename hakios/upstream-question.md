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

## Số liệu bổ sung (zalloc / zcram) — 2026-10-31 10:40

Chuỗi mốc đầy đủ một lần chạy:

```
CP92 100 101 109 112 113 114 115 116
CP120 CP121 CP122                       (zalloc_ext -> zalloc_item)
H20=0                                   z_elems_free của ZONE_ID_VM_MAP = 0
CP124 CP126                             (zone_expand_locked, đã chiếm expander)
                                        ... CP127 KHÔNG tới: chết trong zone_allocate_va_locked
```

Và ngay trong `zcram()` (gọi từ `vm_map_init`):

```
CP130 H30(pages)=1  H31(chunk_pages)=1  H32(zone_elem_outer_size)=0
      H34(z_elem_size)=0  H35(z_elems_avail)=0  H36(z_chunk_elems)=0x66  H37(flags)=0
CP131 H33(z_elems_free sau chunk) = 0
```

Ngay sau `zone_create_ext(VM_MAP_ZONE_NAME, sizeof(struct _vm_map), ZFLAGS, ZONE_ID_VM_MAP, NULL)`:

```
H40(z_elem_size)=0  H41(z_elems_avail)=0  H42(zone_elem_outer_size)=0
```

⇒ Zone `ZONE_ID_VM_MAP` **chưa có `z_elem_size`** tại thời điểm `vm_map_init()` cram nó; `zcram_and_lock()`
tính `elem_size = zone_elem_outer_size(zone) = 0` rồi chia cho nó ⇒ nghi vấn fault im lặng (không có console).
Sau đó `zalloc_id()` thấy `z_elems_free == 0` nên đi nhánh blocking `zone_expand_locked()` — mà mở rộng cần
`kernel_map`, chính là map đang được tạo trong `vm_map_create_options()` ⇒ treo.

Câu hỏi 4: trên redwood, `zone_array[ZONE_ID_VM_MAP].z_elem_size` có khác 0 ngay sau `zone_create_ext()`
không? Có bước nào (ví dụ `zone_metadata_init()` / `startup_phase >= STARTUP_SUB_ZALLOC`) mà ở một SoC mới
cần chạy trước `vm_map_init()` không?
