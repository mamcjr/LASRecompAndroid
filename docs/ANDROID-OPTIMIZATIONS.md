# Android optimization work pending device validation

These changes were initially built and checked on the PC. A subsequent user-driven device comparison is recorded below; it does not validate every optimization or GPU path.

## Changes

- ADPF session initialization, replacement, reporting and diagnostic snapshots share a mutex. In unified mode only the presenting thread manages the session.
- ADPF retains the established render wall-time signal by default. `LARECOMP_ADPF_CPU_TIME=1` opts into CPU-only measurement; this is experimental because excluding waits/descheduling can change the frequency governor response. Frame pacing retains wall time in either mode.
- Consecutive Vulkan draws reuse the pipeline and its baked configuration until a relevant command changes state, a pass restarts, the program changes or the topology class changes. Rebinding identical state does not invalidate it. Pipeline readiness is checked on every draw.
- Texture conversion shares a growing scratch buffer across mip levels and layers. Stable dynamic textures can avoid another full hash through exact comparisons against a bounded cache (eight snapshots, each at most 1 MB). Periodic full verification and forced full hashing remain available. This does not implement OS page-write tracking.
- Load-only global-memory copies can reuse an immutable arena allocation within the same submission and guest-write generation, after an exact byte comparison. Changed bytes always receive a new allocation; resident GPU-written regions keep their existing synchronization. Disjoint resident upload runs share a barrier in the same upload command buffer.
- Android shader compilation defaults to half the logical CPU count, capped at four workers and at least one. Compute shader translation runs on the translation queue. Its code and local workgroup metadata are captured before queuing. Graphics and compute configure and use the non-reentrant translator under the same lock.
- The APK bundles a deterministic SHA-256 fingerprint of the game-builder assets. The activity unpacks them only when that fingerprint changes, with an install-time fallback for older APK layouts. The first update from the old stamp scheme still unpacks once.

## Diagnostic switches

| Environment variable | Behavior |
| --- | --- |
| `LARECOMP_ADPF_CPU_TIME=1` | Experiment with CPU-only render timing instead of the default wall-time ADPF signal. |
| `LARECOMP_VK_PIPE_REUSE=0` | Rebuild and look up the pipeline configuration on every draw. |
| `LARECOMP_VK_PIPE_VERIFY=1` | Rebuild keys on cache hits and abort with a diagnostic if an invalidation was missed. |
| `LARECOMP_GCOPY_REUSE=0` | Allocate and copy load-only arena ranges as before. |
| `LARECOMP_TEX_SNAPSHOTS=0` | Disable exact texture snapshots. |
| `LARECOMP_TEX_FULLHASH=1` | Force the existing full-hash path. |
| `LARECOMP_SHADER_WORKERS=N` | Override compilation worker count (1–16). |
| `LARECOMP_SYNC_SHADERS=1` | Retain synchronous translation/compilation for deterministic diagnosis. |

## PC validation

Build with `android/lasrecomp/gradlew.bat :app:assembleRelease --offline` from its Gradle directory.

Run the upload cache checks from the repository root:

```text
python tools/test_android_upload_caches.py <Android-NDK>/toolchains/llvm/prebuilt/windows-x86_64/bin
```

The test extracts the actual C cache helpers, compiles them to WebAssembly with the NDK compiler, and runs them in Node.js. Allocation and submission tracking are test doubles; Vulkan is not exercised. The checks cover unchanged and changed content, hash-tag invalidation, texture destruction, cache size limits, allocation failure, scratch reuse, submission/generation changes, ring exhaustion and preservation of older copies.

Device validation is still needed for lighting/compute results, texture changes, pass/seed transitions, frame-time percentiles, memory consumption and thermal behavior. No FPS improvement is claimed from compilation or these isolated tests.

## Device comparison: ADPF timing (2026-10-20)

The user reported more FPS oscillation at 50% internal resolution with CPU-only render timing. The captured Field samples showed about 52–58 FPS and maximum frame times of 35–40 ms, without shader compilation during those intervals.

A comparison APK restored the established wall-time ADPF signal while retaining the session synchronization, new settings menu and other optimizations. The same connected POCO device loaded Turnip Gen8 V37. In a later roughly 26-second Field log excerpt, interval averages were 58.9–59.9 FPS. Most interval maxima were 17–29 ms, with isolated 33–34 ms peaks. The user reported stable frametime.

This supports keeping wall-time reporting as the default and leaving `LARECOMP_ADPF_CPU_TIME=1` experimental. It is an informal comparison, not a controlled benchmark: scene movement and thermal conditions were not held constant, and occasional long frames remain. Qualcomm/Mali copy-path correctness and the remaining performance changes still require separate validation.
