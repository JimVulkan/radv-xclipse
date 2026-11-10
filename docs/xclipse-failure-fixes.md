# Xclipse failure handling fixes

These changes address four groups of CPU-side failure paths. They preserve BO
ownership and recording mappings when IB growth/finalization fails, roll back
frame-pacer reservations on out-of-memory submission failures, bound fallback
gralloc FD copies, and handle DRM sync-provider allocation failures through
initialization and probing.

## Behavior

- A failed IB allocation/map does not patch a chain with `cdw == 0`, publish a
  NULL recording pointer, or change the old buffer's capacity. A failed IB-array
  allocation retains ownership. Secondary execution and DGC stop after failed
  growth/finalization.
- Frame-pacer evaluators snapshot only committed semaphore values. On
  `VK_ERROR_OUT_OF_HOST_MEMORY` or `VK_ERROR_OUT_OF_DEVICE_MEMORY`, the failed
  submission's query head/size, timeline value, and frame accounting are restored.
  Per-queue locking protects rollback; the global frame lock is released before
  driver submission, and the query lock is released before GPU waits. OOM
  submissions must preserve resource/synchronization state according to the
  [Vulkan vkQueueSubmit2 contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit2.html).
- Fallback YUV import rejects FD counts larger than the destination capacity
  before copying and validates the multi-plane count in release builds too.
- A failed provider allocation returns NULL. DRM device initialization releases
  its reference and reports `-ENOMEM`; sync-type probes reject unavailable
  providers; RADV checks provider cloning and unsuccessful synchronization probes.

The separate Xclipse profiler lock-order issue is outside this change.

## Validation

From the repository root, with Python 3, a C compiler, pthreads, ASan and UBSan:

```sh
python3 bin/xclipse-failure-regressions.py --output /tmp/radv-fixed/results.json
```

The runner is self-contained and extracts production C function bodies. Test
doubles model allocation failures, Vulkan/DRM dispatch and controlled scheduling.
It covers 17 cases: 13 affected scenarios and four success controls. The new
version should report PASS for every case.

To reproduce the original failures on the reviewed upstream base:

```sh
python3 bin/xclipse-failure-regressions.py \
  --ref fe9b8943aaeec11ed9fccbfcc382da07379afbe5 --expect-bugs \
  --output /tmp/radv-before/results.json
```

The baseline should report 13 REPRODUCED and four PASS. The process exits
successfully when the expected failure signatures and controls are observed.
Per-case sanitizer logs are written next to the selected JSON output. LeakSanitizer
is disabled; this does not claim leak-sanitizer coverage.

### Recorded host results

The following results were obtained against upstream
`fe9b8943aaeec11ed9fccbfcc382da07379afbe5`, then against that base with this
patch applied. REPRODUCED means the runner observed the expected defect; it is
not a passing driver behavior. PASS on the patched tree means the corrected
behavior or success control was observed.

| Scenario | Original base | Patched tree |
|---|---|---|
| IB growth: allocation failure preserves the old BO, mapping and capacity | REPRODUCED | PASS |
| IB growth: mapping failure preserves the old recording pointer | REPRODUCED | PASS |
| IB growth: successful allocation and mapping | PASS | PASS |
| IB growth: failed finalization stops before a new allocation | REPRODUCED | PASS |
| IB finalization: failed array realloc retains BO ownership | REPRODUCED | PASS |
| Gralloc: four FDs are rejected before writing a three-element array | REPRODUCED | PASS |
| Gralloc: three FDs are copied successfully | PASS | PASS |
| DRM provider: failed allocation returns NULL | REPRODUCED | PASS |
| Frame pacer: allocation failure before driver entry rolls back reservations | REPRODUCED | PASS |
| Frame pacer: successful submission signals and drains queries | PASS | PASS |
| Frame pacer: driver host-memory failure preserves earlier successful work | REPRODUCED | PASS |
| Frame pacer: driver device-memory failure preserves earlier successful work | REPRODUCED | PASS |
| Frame pacer: concurrent disable after failed submit, externally synchronized queue | REPRODUCED | PASS |
| Frame pacer: concurrent disable after failed submit, internally synchronized queue | REPRODUCED | PASS |
| DRM device: provider failure releases the DRM reference and clears output | REPRODUCED | PASS |
| DRM device: successful initialization and destruction balance ownership | PASS | PASS |
| Sync-type probing: unavailable providers are handled without NULL access | REPRODUCED | PASS |

The independent patch is also checked in combination with PR #9
(`ea76b270654f6c673feea2d570e2b918ef96a889`) and PR #17
(`944d127c3ca1ff5b0aa949ce34d6861ea1b3d69e`). Their existing regression tests and
`git diff --check` pass in that combination.

These are host regression checks. A complete Android build, Vulkan CTS, Android
and Wayland presentation, and execution on Xclipse hardware remain necessary
before treating this as a validated driver release. No FPS or latency improvement
has been measured.

## Review risks and pending device validation

The host results do not establish absence of regressions in a complete driver.
In particular, frame-pacer locking changes need review and runtime concurrency
and pacing checks; fallback gralloc rejection needs coverage with actual
multi-plane and metadata-bearing handles. The tests compile extracted functions
with reduced surrounding types and mocked dispatch, not the complete Android
driver or its integration with the Samsung kernel and gralloc.

Pending checks on real hardware (for example, the Galaxy S24 FE / Xclipse 940):

- Build both Android and Wayland variants with the project's supported NDK.
- Run with Vulkan validation where available and check logcat for validation
  errors, device loss, hangs and abnormal memory growth.
- Exercise ordinary rendering and command-buffer reset/reuse, secondary command
  buffers and DGC where supported, including repeated initialization/destruction.
- Compare Anti-Lag OFF/ON, frame caps and toggling under load, including closing
  an app while work is in flight. Check both native Android and Wayland paths
  and monitor frame pacing/performance.
- Import supported RGB and multi-plane YUV buffers and verify plane contents,
  camera/video interoperability where applicable, and clean rejection of
  unsupported handle layouts.
- Exercise memory-pressure/failure paths using a controlled development setup,
  and run the relevant Vulkan CTS coverage before release.

All device checks above remain pending. These changes are proposals for
maintainer evaluation and on-device testing, not a claim of a regression-free
or release-validated driver.
