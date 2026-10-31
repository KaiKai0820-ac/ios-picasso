# Picasso UFS read staging

## local31f evidence (2026-10-31)

The fresh console from local31f reports HFS catalog node8085 with an invalid
forward link `0x44617277` (ASCII `Darw`). Recovery's raw read of the same 8192-byte
catalog node has normal links8086/8084. The initial HFS and SpringBoard errors
precede the identification-only Novatek probe at150s. SpringBoard repeatedly
faults, including SIGBUS at0x2061061d0 with slide0x30484000; waiting at the logo
does not establish useful progress.

External evidence is in `k30-preparation/boot-log-20261031-133218` and
`diagnostics/hfs-node8085-local31f.bin`. The full ring-area SHA256 independently
matches the Recovery source:
`1c18115b8680e67fbe49d6f7c4514ed68581bbe80b9d196e887fe14dfd6d898f`.
This does not establish whole-volume health, and no filesystem repair was run.

## Race and patch0211

`sweet_ufs_strategy()` previously copied a read into the upper256KiB of the shared
1MiB DMA bounce allocation, released the driver lock, and then copied that
snapshot into the caller. Another read can replace the same stage, and a full
bounce-buffer log dump can replace it with console text. Holding the lock during
the caller copy is unsafe because a caller page fault can recursively issue
disk I/O.

Patch0211 gives each read request a private wired `kalloc_data()` snapshot of
at most256KiB, allocated before taking the driver lock. The locked bounce copy
and unlocked caller copy remain in their original order. The request frees its
snapshot on both successful completion and error exits. Caller-copy progress is
local until the lock is reacquired. Writes retain their existing path.

## Validation and limits

- The actual modified source compiles to ARM64 Mach-O using Clang21, the matching
  CI compile command and headers. Patch reverse-application check passes.
- `tools/tests/ufs-read-stage-regression.py BEFORE.c AFTER.c` extracts the actual
  strategy and copy functions into a host harness. ASan/UBSan runs deterministically
  reproduce corruption in the original under an overlapping reader, log-buffer
  overwrite and recursive caller-page read. All three retain correct bytes with
  patch0211, including unlocked recursive I/O.
- Multi-chunk unaligned reads, partial writes and early-error allocation cleanup
  pass for both versions. This models software interleavings, not UFS hardware.
- Device stability validation is pending. The stability boot disables the Novatek
  probe (`picassonvtprobe=0`, `sweettouch=0`) to isolate this change. Touch input
  remains unimplemented; a successful SPI transfer with an all-FF ID is not chip
  identification.

## local31g stability deployment

CI [41173608116](https://github.com/KaiKai0820-ac/ios-picasso/actions/runs/41173608116)
successfully built source376ada776abdcd62678bd0de898000bba0aaa77a in6m30s.
The small kernel artifact passes GitHub's ZIP digest, CRC and bundled kernel
SHA256 checks. Kernel SHA256:
`eadf99691a2a11655a1186605f37a71d671607462171827165b6512ceb892f3c`.

Named local31g image is37130240 bytes, SHA256:
`d444162d4c9f934d56a6ae21408221b95603f6347d7b39ebc509745f0a334d25`.
Both payloads and the actual command, original ADT/full DTB, proven RAM range
and OS word validate; official mkbootimg reconstruction is byte-identical.
The command explicitly disables Novatek probing and legacy touch.

At13:52:41 the phone rebooted after verified writes to boot and the dedicated
log area. Full128MiB boot SHA256:
`db222c198146ca3d1f78e9dd772d6a7d862d866bb2450dd9eca329fc5225c730`.
Recovery remains byte-identical. Full previous boot and verified ring are backed
up externally in `probe-deploy-20261031-135232`. Windows enumerates Sweet iOS
USB; GUI observation and fresh stability logs are still pending. Do not rerun
the recorded deployment (`stability_local31g` in the external flash journal).
