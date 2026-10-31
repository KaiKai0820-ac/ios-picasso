# Picasso GPU and presentation adaptation plan

Source review: 2026-10-31. This is a plan, not an implemented or phone-tested GPU driver.

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
| Presentation improvements: dedicated worker, double/triple buffering, hardware-vblank page flip | ios-apollo has an independent presentation path and patch0156 for page flips. Its README marks0156 built but not booted at the inspected revision. | Useful alongside either renderer. Requires SM7250 DPU and display-SMMU validation; does not by itself accelerate rendering. |
| Metal -> metal2vulkan -> Turnip -> native XNU GPU service | UniXNU AdrenoMetal reports GPU composition verified on sheng/A740. Its documentation requires Adreno740, Turnip, metal2vulkan, IOGPUFamily and IOSurface interfaces. | Longer-term reference. An A620 XNU backend and compatibility with our OS ABI remain work; this is not a K30 drop-in. |
| HakiOS native Metal command model | hakios-metal reports A642L buffer/copy/fence tests; its README explicitly says shaders are not yet connected. | Not a replacement for the currently usable OpenGL ES UI route. |

Upstream reports are attributed to their authors, not independently reproduced on our K30.
The inspected sources did not provide a ready-made, validated K30 A620 GPU image.

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

- [ios-apollo README](https://github.com/aTechGeekDev/ios-apollo/blob/f791faa1f37897db640727c266ea470a5c1f895e/README.md): GPU UI path; author-reported19-54 FPS; page-flip patch not booted.
- [ios-apollo userland](https://github.com/aTechGeekDev/ios-apollo/blob/f791faa1f37897db640727c266ea470a5c1f895e/userland/README.md): GPU memory test, triangle readback test, Mesa and CA bridge.
- [ios-apollo CA kernel integration](https://github.com/aTechGeekDev/ios-apollo/blob/f791faa1f37897db640727c266ea470a5c1f895e/xnu/patches/0114-apollo-ca-bridge-kernel.patch): IOSurface import and completion handling.
- [ios-apollo page flip](https://github.com/aTechGeekDev/ios-apollo/blob/f791faa1f37897db640727c266ea470a5c1f895e/xnu/patches/0156-apollo-page-flip-no-tearing.patch): SM8250 implementation, requires board adaptation.
- [HakiOS GPU driver](https://github.com/oosexgang/hakios-xnu-kernel/blob/a3384e031eb1e605cf37adaefc19991f04f6274b/drivers/gpu/hakios.adreno.c): staged A642L bring-up and persistent GPU API.
- [Mesa Freedreno documentation](https://docs.mesa3d.org/drivers/freedreno.html) and [Mesa25.0.7 A620 device description](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-25.0.7/src/freedreno/common/freedreno_devices.py).
- [SM7250 GPU device tree](https://github.com/LineageOS/android_kernel_xiaomi_sm7250/blob/8e49d06af7ff5757c03ab181874cf1e90a1e1192/arch/arm64/boot/dts/vendor/qcom/lito-gpu.dtsi) and [A620 vendor core configuration](https://github.com/LineageOS/android_kernel_xiaomi_sm7250/blob/8e49d06af7ff5757c03ab181874cf1e90a1e1192/drivers/gpu/msm/adreno-gpulist.h).
- [UniXNU AdrenoMetal](https://github.com/UniXNU/AdrenoMetal/blob/ce5e19e33e05e1873d38d818e23cabfe0ac708c3/README.md) and [integration documentation](https://unixnu.com/drivers/adreno-metal): A740 verified according to upstream.
- [HakiOS Metal status](https://github.com/oosexgang/hakios-metal/blob/54a46fadec5bd9684ebd77f3a0834c84b5eb01a6/README.md): shader integration remains unfinished.

Existing local reuse points: `tools/userland-bridge/mesa/`,
`tools/userland-bridge/ca-bridge/`, and `xnu/patches/0191-sweet-fb-clear-after-mute.patch`
(the placeholder service to eventually replace). No code was cherry-picked during research.
