# K30 A620 implementation work packages

Updated 2026-10-31. The opt-in A620 backend, real GPU service, optional firmware
packaging, presentation worker and GPU-userland build integration are now implemented
in `xnu/picasso-gpu/`, `tools/` and `.github/workflows/gpu-userland.yml`.
Four changed kernel translation units compile locally for ARM64. Actual-source
allocator/ring/fence and firmware parser checks pass ASan/UBSan; default ADTs
remain byte-identical with a fixed seed. Full linking and device execution remain
to be verified. GPU userland CI41186390350 succeeded at7d9556e; both tests and
all three libraries were downloaded and verified for SHA256, iOS ARM64 target,
load dependencies and embedded signatures. Kernel CI41186398866 is in progress
at the same exact source. Hardware page flipping is deferred pending K30 DPU measurements.
The detailed work packages below retain the design and acceptance criteria;
see `xnu/picasso-gpu/SOURCE.md` for implemented arguments and limitations.
Research and pinned upstream links are in
[PICASSO_GPU_PLAN.md](PICASSO_GPU_PLAN.md).

## Decision and baseline

Use Mesa Freedreno/GLES and the existing CA/EAGL bridge, backed by a native A620
XNU driver. Reuse Apollo's client interface and generic buffer/fence machinery;
adapt hardware initialization from the SM7250 vendor driver. Rendering and display
presentation are separate milestones. Metal and 120-Hz output are outside the
first implementation.

Keep local31j as the rollback baseline: user-confirmed touch, still software
framebuffer presentation, with a 150-second touch initialization delay. Its kernel
source is `c13e874eabb8de224d6f6738fe8235228cf47edf`, CI run `41179410783`.
The image `shim/out/shim-boot-local31j.img` has SHA256
`d00e97e3e59b8fba8eed8d566d2fd091af5d634246dc1c06fd4ad30c5c39b808`.
The last verified complete 128-MiB boot partition hash is
`e5f01335ad791ad86bc79862d985d44958ecdb6b9a0882a292da881ad87f27ab`.
Existing HFS allocation errors remain an independent unresolved issue.

`tools/picasso-gpu-boot.py` builds a named offline test image from the exact
local31j baseline. Without `--kernel` it validates the baseline and prints the
proposed configuration. It appends only the GPU ADT node (with a fresh phandle),
preserves touch/stock DTB/RAM/OS settings, rejects truncated or duplicate boot
arguments, and independently reconstructs the final Android image with mkbootimg.
It never connects to a phone. Start with `--level 6 --marker gpu31a`; firmware
is mandatory from level11, and keeping the service requires level21+ with
authenticated ZAP mode3. Never infer hardware success from a packaged image.

## Completed: extract the board configuration

`tools/picasso-gpu-inventory.py` reads saved DTBs using the existing pure-Python
FDT parser. It resolves clock and IOMMU phandles, reports MMIO/reservations,
power domains and secure-loader configuration, and records input/tree hashes.
It has no ADB, network, MMIO or device-write path. Example commands from the repo:

```sh
python3 tools/picasso-gpu-inventory.py dtb/picasso-live.dtb --output ../gpu-picasso-live-inventory.json
python3 tools/picasso-gpu-inventory.py dtb/boot_unpacked/dtb --list-trees
python3 tools/picasso-gpu-inventory.py dtb/boot_unpacked/dtb --tree-index 9 --output ../gpu-picasso-stock-v2-inventory.json
```

The saved final live tree is 669693 bytes, SHA256
`57576eb51c207af433413b781c2d2226c64338dd587488c79d042f8b61ec7291`.
Its model is `Qualcomm Technologies, Inc. 7250 picasso`.
These are configuration facts, not a fresh hardware-register measurement:

| Item | K30 configuration / reference |
| --- | --- |
| Final live-tree chip ID | `0x06020001`; do not substitute the generic base DTS's `0x06020000` |
| Derived GMU chip word | `0x06020100`, using the vendor packing formula |
| GMEM | 512 KiB, from the pinned A620 vendor core definition |
| GPU registers | `0x03d00000 + 0x40000` |
| CX debug / RSCC / isense | `0x03d61000 + 0x800` / `0x03de0000 + 0x1000` / `0x03d8b000 + 0x2000` |
| GMU registers | `0x03d6a000 + 0x30000` |
| GMU PDC configuration / sequencer | `0x0b290000 + 0x10000` / `0x0b490000 + 0x10000` |
| GPUCC / GPU SMMU | `0x03d90000` / `0x03da0000 + 0x10000` |
| SMMU TCU | `0x03dc2000 + 0x20` |
| CX GDSC / hardware control | `0x03d9106c` / `0x03d91540` |
| GX GDSC / domain / reset | `0x03d9100c` / `0x03d91508` / `0x03d91008` |
| Secure loader | `a620_zap`, PAS 13; reservation `0x8bc15400 + 0x2000` |
| Display / DFPS reservations | `0xa0000000 + 0x2300000` / `0xa2300000 + 0x100000` |
| Firmware names in vendor core | `a650_gmu.bin`, `a650_sqe.fw`, `a620_zap`; bytes still to acquire and hash |

GPU SMMU stream/mask pairs are `gfx3d_user=0/0x401`,
`gfx3d_secure=2/0x400`, `gmu_user=4/0x400`, `gmu_kernel=5/0x400`,
and test device `7/0x400`. These are device-tree routing inputs, not observed
context-bank programming; do not use stream IDs as context-bank numbers.

The original boot DTB bundle has 12 trees. Index 9 describes Lito v2 with
`0x06020001`; index 10 describes Lito v1 with `0x06020000`. The saved final live
tree has MSM revision 65536 and chip ID `0x06020001` after board configuration.
Matching GPU IDs does not establish which base tree ABL selected. Preserve the
whole original bundle and use the final live configuration for this inventory.

All-bin candidate frequencies are 0/275/400/500/540/625/670/750 MHz; the active
speed bin is unresolved. Neither 750 MHz nor Apollo's 305-MHz setting is an
appropriate unverified default.

Validation completed: saved live tree and stock indices 9/10 parse; live GPU/SMMU
regions match stock index 9; all referenced clocks/IOMMUs resolve. A concatenated
bundle without an explicit index and index 8's A619 tree are rejected with exit 2.
The generated reports and device dumps remain outside git.

## 1. Kernel patch: staged hardware bring-up

Implemented changes in the generated XNU source:

| File | Work |
| --- | --- |
| `bsd/dev/picasso_gpu.c` (new) | A620 board descriptor, stage runner, bounded polling and stage logs |
| `bsd/conf/files` | Register the new source under the existing board build condition |
| `bsd/dev/sweet_ufs.c` | One-time startup hook after the log/storage path is available; create a dedicated probe thread |
| `xnu/patches/` in this repository | Store the resulting ordered patch after local compile and clean apply checks |

Boot argument `picassogpu=N` defaults to 0. The source uses Apollo's finer
22-stage ladder: level 1 reads GCC; level 2 enables the configuration bus;
level 3 reads GPUCC; levels 4/5 establish CX and clocks; level 6 validates the
GMU counter. Later levels gate SMMU, firmware, HFI, GX, CP and authenticated ZAP
work. Runtime free-context and stream-overlap checks precede SMMU programming.
The original grouping below describes the dependency order:

1. Read only the relevant GCC clock/vote state; no GPUCC/GMU accesses.
2. Establish the documented configuration-bus prerequisites, then read GPUCC
   clock/GDSC state. Stop before subsequent stages on an unmet prerequisite.
3. Apply the required CX/clock sequence and verify that the GMU's 19.2-MHz
   always-on counter progresses across timed samples. No firmware or submissions.

A higher level includes earlier levels. Proposed log records carry stage, register,
before/after values, elapsed time and result, followed by one final pass/fail record.
Polling loops have finite deadlines. A deadline cannot rescue an MMIO read on an
unpowered bus, so stage prerequisites must precede every new block access.
Track which state this driver changed and unwind only those changes where valid;
do not blanket-disable clock or power state inherited from the loader.

Use Lito definitions, verified in the pinned vendor clock sources: GCC base
`0x00100000`, CFG_AHB `+0x71004`, MEMNOC_GFX `+0x7100c`, SNOC_DVM_GFX
`+0x71018`, GPU_IREF `+0x8c014`, DDR_GPU_AXI `+0x71154`, and vote
`+0x52000` bits 15/16. GPUCC GMU RCG is `+0x1120`, AHB `+0x1078`,
CX_GMU `+0x1098`, GX_GMU `+0x1064`. Register definitions are not evidence that
these clocks are already enabled on this boot.

**Acceptance:** default-off boot behavior is unchanged; each requested stage
produces a terminal result; counter progression is observed for level 3; software
GUI/touch remain usable. This first patch neither publishes a real GPU service
nor programs TrustZone, firmware, SMMU translation or GPU command buffers.
Its result is hardware evidence, not an FPS improvement.

## 2. Firmware, GMU, SMMU and one command

Acquire the phone's own firmware files/segments, validate their metadata and
record hashes outside git. Extend `tools/mkadt.py` with optional Apollo-compatible
GPU properties/options (`--gmu-fw`, `--sqe-fw`, `--zap-mdt`, `--zap-seg`). Verify
that omitting those options preserves the current generated ADT byte for byte.
Validate segment placement against the K30 reservation, not Apollo's ZAP address.

Port Lito power/PDC/HFI initialization and A620 protection/clock-gating data.
For example, vendor `adreno_a6xx_gmu.c` obtains `vrm.soc` from command DB and
adds A620-specific PDC commands using `vrm_resource_addr + 4`; changing only
Apollo's chip ID would miss this. Reuse existing RPMh support only after checking
its resource lookup, acknowledgement and voltage-level contracts for these votes.

Derive SMMU context geometry from the implementation and route the documented
streams explicitly. Use fresh driver-owned allocations for ring/memstore/fence
buffers, with checked IOVAs and cache maintenance; do not borrow scanout or
secure carveouts. Bring up firmware/GMU/HFI and the command processor in the
vendor dependency order, splitting stages further when logs expose a failure.

**Acceptance:** authenticated firmware startup and HFI response; a minimal GPU
memory-write command changes a known destination to the expected value, followed
by an independently checked completion fence. Log SMMU faults and timeout states.
Service creation or a firmware filename match is insufficient.

## 3. Real GPU service and Mesa pixel test

Selectively port Apollo's `iokit/Kernel/arm/SweetGPU.cpp` to the A620 backend:
device queries, buffer allocation/mapping, submission, completion and cleanup.
Remove the placeholder `SweetGPU` class introduced by patch 0191 when linking
the real class. Publish the hardware service only after successful initialization;
do not trigger CA injection merely because the existing stub service is present.

The eight audited Mesa/DRM/CA user-space files already match Apollo byte for byte.
Build the existing GPU memory and off-screen GLES tests with that userland. Add a
dedicated GPU-userland workflow (a macOS runner is suitable for the bridge build)
instead of replacing the shell/SSH-only `.github/workflows/userland.yml`.

**Acceptance:** record `GL_RENDERER`, submit a triangle through hardware and check
pixel readback plus fences. Detect software fallback explicitly. A triangle test
must pass before installing the bridge into SpringBoard/backboardd.

## 4. CoreAnimation and surface sharing

Adapt Apollo 0114/0118 selectively into this branch's SweetSurface and
SweetFramebuffer: IOSurface import, shared allocation lifetime, cache coherency
and GPU-completion waits. Preserve the current K30 framebuffer layout. Copying
the whole Apollo framebuffer file would also import unrelated backlight/CPU
frequency dependencies.

**Acceptance:** lock screen, swipe/animation and representative applications work
with verified hardware submissions; no stale/uncompleted surface is presented.
Record rendering, GPU-wait and CPU-copy durations separately. This establishes
the GLES/CA route; it does not establish native Metal support.

## 5. Presentation and page flipping

First port the independent presentation worker and absolute timer scheduling
(Apollo 0148), with submitted/presented/dropped counts and copy/wait timing.
Then adapt the three-buffer scanout work (0156) after identifying the active K30
DPU pipe, stride, format, display-SMMU mapping and actual panel timing. Check all
buffer ranges against the live RAM/reservation map and scanout requirements.

Apollo boot88 still CPU-copies GPU-rendered surfaces before its page flips;
direct GPU rendering/scaling into scanout is separate future work upstream too.
Its author reports 38-45 presented FPS and one unexplained reset. Patch 0158's
high DDR vote is an unbooted suspected mitigation, not an established K30 setting.

**Acceptance:** measured presentation cadence, complete frame ownership/fence
transitions, no tearing or new resets in a defined run. Establish stable output
before raising the target toward 60 FPS. The panel's advertised 120 Hz does not
demonstrate that this driver can render or present at that rate.

## Build and review gates for the future driver patches

Compile changed ARM64 files locally against the saved CI buildtree, refresh
exported headers where needed, and check patch application against the pristine
tree before a full kernel CI run. Record the exact source SHA and artifact hash.
Before any later device trial, verify the payload command line, `--picasso` boot
layout, and the selected probe level; follow the repository's write/readback
rules. Each experiment should change one bring-up stage and have an explicit
log collection endpoint. This document and inventory tool do not start a trial.

Pinned hardware source: `LineageOS/android_kernel_xiaomi_sm7250` revision
`8e49d06af7ff5757c03ab181874cf1e90a1e1192`, particularly `gpucc-lito.c`,
`gcc-lito.c`, `adreno_a6xx_gmu.c`, `adreno-gpulist.h` and `lito-gpu.dtsi`.
Apollo revision: `892b91d31ed6064924117120a7155d077a621af8`.
