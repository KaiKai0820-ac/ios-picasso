# Picasso GPU and presentation adaptation plan

Source review: 2026-10-31; Apollo refreshed to `892b91d31ed6064924117120a7155d077a621af8`.
This is a plan, not an implemented or phone-tested K30 GPU driver.

## Current phone

The user-confirmed local31j image still uses the framebuffer software path. Its manifest
sets `sweetfb_hz=30`; its early boot log publishes an explicitly stub `SweetGPU` and an
empty `IOAcceleratorES`. Stock xpcproxy/backboardd are installed, and the optional Mesa/CA
bridge libraries were not shipped for this GUI milestone. The stub's message mentioning
softpipe does not prove that Mesa is loaded on the phone.

`AppleCLCD::swap()` records the newest surface submission. A software timer, nominally
60 Hz, copies dirty surfaces every second tick at the current setting. CPU row copies
and data-cache cleaning make the framebuffer visible to the display controller. This
neither measures actual FPS nor establishes the panel's physical refresh rate. It does
not implement hardware-vblank page flipping.

No phone reboot, flash, GPU register access, or image change was performed for this review.

## Available routes

| Route | Evidence reviewed | Suitability for this port |
| --- | --- | --- |
| Mesa Freedreno + OpenGL ES/CoreAnimation bridge + native XNU GPU service | ios-apollo reports whole-UI GPU rendering on Adreno 650; HakiOS reports rendering on Adreno 642L. Mesa 25.0.7 explicitly describes A620. | Preferred first hardware-rendering route: this repository already carries the matching user-space bridge and Mesa build patches. |
| Presentation improvements: dedicated worker, double/triple buffering, hardware-vblank page flip | At the refreshed Apollo revision, patch0156 has been booted: boot88 reports38-45 presented FPS and working page flips, with one unexplained reset. | Useful alongside either renderer. Requires SM7250 DPU and display-SMMU validation; does not by itself accelerate rendering. |
| Metal -> metal2vulkan -> Turnip -> native XNU GPU service | UniXNU AdrenoMetal reports GPU composition verified on sheng/A740. Its documentation requires Adreno740, Turnip, metal2vulkan, IOGPUFamily and IOSurface interfaces. | Longer-term reference. An A620 XNU backend and compatibility with our OS ABI remain work; this is not a K30 drop-in. |
| HakiOS native Metal command model | hakios-metal reports A642L buffer/copy/fence tests; its README explicitly says shaders are not yet connected. | Not a replacement for the currently usable OpenGL ES UI route. |

Upstream reports are attributed to their authors, not independently reproduced on our K30.
The inspected sources did not provide a ready-made, validated K30 A620 GPU image.

## Apollo source audit at892b91d

The repository was cloned locally at the revision above. Selected kernel files were
reconstructed from its ordered patch data using GNU patch with zero context fuzz;
no upstream build scripts or device utilities were executed. External inspection output
is in `../gpu-apollo-extracted/`, with hashes and patch lists in `review-manifest.json`.

| Effective source | Lines | Role |
| --- | --- | --- |
| `bsd/dev/sweet_gpu.c` | 2518 | SM8250/A650 clocks, power, firmware, SMMU, command processor, submissions/fences;17 patch sections |
| `iokit/Kernel/arm/SweetGPU.cpp` | 822 | msm-style IOKit client and IOSurface import;3 patch sections |
| `iokit/Kernel/arm/SweetFramebuffer.cpp` | 1237 | dedicated presentation thread, GPU completion checks, frame statistics and flip-buffer selection;20 patch sections |
| `bsd/dev/sweet_display.c` | 251 | SM8250 DPU source-address flips;patch0156 |

Byte comparisons found all8 inspected user-space files identical between Apollo's
`userland/` and our `tools/userland-bridge/`: Mesa build, DRM implementation/header,
both Mesa patches, `eagl.c`, `sweet_ca.c`, and `sweet_gl_osmesa.c`. Installing these
libraries alone cannot supply the missing hardware driver.

Concrete A650 assumptions requiring adaptation include reported chip ID `0x06050002`,
GMU chip word `0x06050200`, GMEM `0x120000` (1152KiB), a650 ZAP firmware and its
reserved loading address. Our A620 reference instead has chip ID `0x06020000`,
512KiB GMEM and `a620_zap`. Clocks, power/HFI tables and address mappings also need
board validation. The display code assumes framebuffer reservation
`0x9c000000 + 0x2300000`, SM8250 DPU pipe registers and display-SMMU routing;
our current K30 framebuffer is `0xa0000000`.

Additional missing integration: our `tools/mkadt.py` lacks Apollo's GPU firmware
packaging options (`--gmu-fw`, `--sqe-fw`, `--zap-mdt`, `--zap-seg`), and the actual
GPU service must replace the existing stub rather than add a duplicate class.

Refreshed runtime evidence is **author-reported**, not reproduced here. Boot88 received
44-53 frames/s from iOS and presented38-45, with3.9-5.7ms CPU copies. Page flipping
worked, but CPU copies remain; direct GPU rendering/scaling into the scanout buffers is
listed as the next task. The run ended in a sudden reset without a panic. Patch0158
restores the memory-bus vote's high default as a suspected workaround; its cause is not
proven and that patch is marked built, not booted. This supersedes the previous
f791faa snapshot's "page flips not yet booted" status.

## A620-specific reference facts

Mesa25.0.7 `src/freedreno/common/freedreno_devices.py` includes `GPUId(620)` with
A6xx/gen1 parameters. This establishes user-space command-generation support, not
working XNU power management or memory mappings.

The pinned LineageOS SM7250 vendor kernel provides the following reference configuration:

- `lito-gpu.dtsi`: chip ID `0x06020000`, GPU base `0x03d00000`, GPU SMMU
  `0x03da0000`, GMU `0x03d6a000`, and secure firmware name `a620_zap`.
- `adreno_gpu_core_a620`: 512 KiB GMEM, `a650_sqe.fw`, `a650_gmu.bin`,
  `a620_zap`, A620 clock-gating/protection tables, and shared A650 GBIF parameters.

These are source configuration facts, not fresh register/firmware observations from this
phone. Validate against its saved/live device tree and its own firmware before execution.
The reused A650 firmware names do not make the A650 initialization sequence identical.

## Recommended implementation order

1. Inventory the K30's own device-tree GPU clocks, CX/GX power domains, RPMh votes,
   SMMU streams/context banks, secure-loader inputs and firmware. Use the SM7250 vendor
   driver as the hardware reference and ios-apollo/HakiOS as the XNU implementation reference.
2. Add an opt-in staged A620 bring-up path with finite waits and step-specific logs.
   Establish bus clocks before accessing the GPU block, then CX/GMU, SMMU mappings,
   firmware/HFI, GX and command-processor startup. Keep the existing software GUI fallback.
3. Replace the GPU stub only when real initialization succeeds. Implement the small
   msm-style IOKit interface expected by `sweet-drm`: device queries, buffer allocation
   and mapping, command submission, fences, and cleanup. First verify a GPU memory write
   with an independently checked result and fence; merely publishing a service is not success.
4. Build the existing Mesa userland and run an off-screen triangle test with pixel
   readback. Record `GL_RENDERER` and prove hardware submission so a softpipe fallback
   cannot be mistaken for successful A620 acceleration.
5. Integrate the CA/EAGL bridge, IOSurface buffer import and completion fences. Reuse
   ios-apollo's surface-sharing/cache-coherency design selectively; its patches depend
   on a real `SweetGPU.cpp` which this branch currently lacks. Verify lock screen,
   animation and applications separately. This milestone would not establish Metal support.
6. Measure rendering, GPU wait and panel-copy times plus presented/dropped frames. Then
   adapt a presentation worker and hardware page flipping to the SM7250 DPU, using
   verified reserved memory and display address mappings. Target stable60 FPS before
   considering120 Hz; neither rate is promised by the reference projects.

The present 30-Hz timer setting can be changed independently, but that alone does not
implement any of the GPU stages above.

## Source snapshots

- [ios-apollo README](https://github.com/aTechGeekDev/ios-apollo/blob/892b91d31ed6064924117120a7155d077a621af8/README.md) and [boot88 handoff](https://github.com/aTechGeekDev/ios-apollo/blob/892b91d31ed6064924117120a7155d077a621af8/NOTES.md): GPU UI path, working page flips and an unresolved reset; GPU scanout integration remains planned.
- [ios-apollo userland](https://github.com/aTechGeekDev/ios-apollo/blob/892b91d31ed6064924117120a7155d077a621af8/userland/README.md): GPU memory test, triangle readback test, Mesa and CA bridge.
- [ios-apollo CA kernel integration](https://github.com/aTechGeekDev/ios-apollo/blob/892b91d31ed6064924117120a7155d077a621af8/xnu/patches/0114-apollo-ca-bridge-kernel.patch): IOSurface import and completion handling.
- [ios-apollo page flip](https://github.com/aTechGeekDev/ios-apollo/blob/892b91d31ed6064924117120a7155d077a621af8/xnu/patches/0156-apollo-page-flip-no-tearing.patch): SM8250 implementation, requires board adaptation.
- [HakiOS GPU driver](https://github.com/oosexgang/hakios-xnu-kernel/blob/a3384e031eb1e605cf37adaefc19991f04f6274b/drivers/gpu/hakios.adreno.c): staged A642L bring-up and persistent GPU API.
- [Mesa Freedreno documentation](https://docs.mesa3d.org/drivers/freedreno.html) and [Mesa25.0.7 A620 device description](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-25.0.7/src/freedreno/common/freedreno_devices.py).
- [SM7250 GPU device tree](https://github.com/LineageOS/android_kernel_xiaomi_sm7250/blob/8e49d06af7ff5757c03ab181874cf1e90a1e1192/arch/arm64/boot/dts/vendor/qcom/lito-gpu.dtsi) and [A620 vendor core configuration](https://github.com/LineageOS/android_kernel_xiaomi_sm7250/blob/8e49d06af7ff5757c03ab181874cf1e90a1e1192/drivers/gpu/msm/adreno-gpulist.h).
- [UniXNU AdrenoMetal](https://github.com/UniXNU/AdrenoMetal/blob/ce5e19e33e05e1873d38d818e23cabfe0ac708c3/README.md) and [integration documentation](https://unixnu.com/drivers/adreno-metal): A740 verified according to upstream.
- [HakiOS Metal status](https://github.com/oosexgang/hakios-metal/blob/54a46fadec5bd9684ebd77f3a0834c84b5eb01a6/README.md): shader integration remains unfinished.

Existing local reuse points: `tools/userland-bridge/mesa/`,
`tools/userland-bridge/ca-bridge/`, and `xnu/patches/0191-sweet-fb-clear-after-mute.patch`
(the placeholder service to eventually replace). No code was cherry-picked during research.
