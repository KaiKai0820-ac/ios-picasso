# Source and integration

Canonical implementation files live here. `tools/picasso-gpu-patch.py` combines
them with `integration.patch` into `0216-picasso-adreno620.patch` during CI.
Edit these files, not the generated patch. The integration diff targets the
repository's kernel after patches through 0215. No firmware bytes are included.

`picasso_gpu.c` and `SweetGPU.cpp` adapt ios-apollo revision
`892b91d31ed6064924117120a7155d077a621af8`, effective sources reconstructed
from its ordered patches (0096 onwards, including its buffer/fence extensions).
Upstream's MIT copyright/permission notice is retained as `LICENSE.Apollo`.
Its files credit the earlier HackiOS native GPU implementation; those credits
are retained in the source. Apollo's boot results do not validate this board.

Lito register configuration, PDC/RSC data and protection ranges are derived from
LineageOS/android_kernel_xiaomi_sm7250 at
`8e49d06af7ff5757c03ab181874cf1e90a1e1192`: `adreno_a6xx_gmu.c`,
`adreno_a6xx.c`, `adreno-gpulist.h`, `a6xx_reg.h`, `gpucc-lito.c`,
`gcc-lito.c`, and the final saved device tree. These vendor sources identify
GPL-2.0-only; retain `COPYING.GPL2` with the adapted implementation and table
data. Vendor copyright: The Linux Foundation 2018-2020 and Qualcomm Innovation
Center 2023. Mesa A620 parameters come from Mesa 25.0.7.

This is an experimental, opt-in board port. Its service shares one GPU address
space and polls fences; it does not implement per-client isolation, interrupt
driven completion, hang recovery, thermal/DVFS policy, suspend/resume, or native
Metal. Firmware authentication remains the vendor TrustZone operation.

Kernel arguments:

- `picassogpu=0` (default): no GPU access and no GPU service.
- `picassogpu=1..22`: execute the staged ladder documented in the source header.
- `picassogpudelay=180` (default, capped 600): seconds before the ladder starts.
- `picassogpuzap=0` (default): no TrustZone call. At stage 20 use 1 for queries,
  2 for metadata authentication, 3 for the full authenticated load.
- `picassogpukeep=1`: retain hardware only after stage 21 and the independent
  indirect-buffer/fence self-test pass, then publish the real `SweetGPU` service.
- `picassopresent=1`: use the framebuffer copy worker; default uses the existing
  path. Both paths defer unfinished GPU surfaces and use an absolute timer cadence.
- `picassofbstats=1`: log copy-worker frame counters every 120 presented frames.

GPU-enabled ADTs require `mkadt.py --picasso --gpu-probe`, or all four validated
firmware options. For a late-starting GPU and the optional CA bridge,
`sweetgl_wait=240` covers the default GPU delay. Installing the bridge and
`sweetinject=/usr/local/lib/libsweet-ca.dylib:backboardd` are later steps after
the standalone GPU and GLES tests pass.

The framebuffer worker still CPU-copies into the existing scanout allocation.
Hardware page flips and direct GPU scanout are not enabled: the K30's active DPU
pipe, SMMU mapping and hardware latch/vblank behavior require device measurements.
