# Android optimization work pending device validation

These changes were initially built and checked on the PC. A subsequent user-driven device comparison is recorded below; it does not validate every optimization or GPU path.

## Changes

### Shader local memory and the jump-table fallback (2026-10-26)

Two engine fixes:

- **Shader local memory sized to its use.** The translator declared `uint lmem[256]` and zeroed all of it in every invocation, even in shaders that only spill one register there (`STL` / `LDL` at `[0x0]`): 1 KB a thread lives in scratch memory, and the loop writes 256 times a pixel. One draw with such a shader took 95-102 ms of GPU on the Adreno 830 for that. Now the array is as large as the accesses need (constant addresses: the words they reach; a register address: the SPH's local memory size, or 256 without an SPH), a power of two (the index is masked with `lmask_`). GLSL fuzz (`tools/test_translate.c`, 4,000 programs, same seed): 2,726 compiled and validated, 0 failures, as with the translator before; `tools/test_translate_ops.sh`: 29 cases, 0 failures (GLSL and 32-bit GLSL).
- **`gen.py`: the jump-table fallback only at block starts.** Every function with a jump table got a fallback switch over all of its instructions (in case the table's emulation missed a target): an edge into each instruction. Now only block starts - branch targets, the tables' own and each instruction after a jump, return or trap; a function with an unresolved table keeps every instruction, as before. 1.0.0, the 545 files rebuilt: 101 -> 60 CPU minutes, the slowest 5.5 -> 1.3 min, the build 4 min on the PC; 1.0.1 from scratch in 5.5 min. Also: the cleanup of an earlier run's extra chunks matched only 3-digit names.

The game code's id changes, so each release's game code builds again. On the phone (POCO F7 Ultra, Turnip, 2026-10-26) the 0.5.5 release build with both rebuilt 1.1.0's game code from the start screen's setup in **4 min 15 s** (hot/cold: 108,790 of 121,651 functions cold), and a copy of the benchmark's save played 106 s in Mabe Village at a steady 60 fps, the picture right, no errors.

### Shader cache and Vulkan startup robustness

The SPIR-V cache now includes the compilation mode in its identity: `LARECOMP_VK_OPT` experiments cannot populate the same entries as normal compilation. Cache generation 4 deliberately bypasses earlier keys, because old entries do not identify their optimization mode. The first encounter with a shader will compile again; this is not a startup-speed improvement. Old cache files are not deleted by this change.

Cached reads check the file size (maximum 16 MiB), five-word header, supported SPIR-V version framing, reserved schema, nonzero ID bound, and each instruction's word-count bounds. A malformed entry is treated as a cache miss and recompiled. This is not complete semantic validation, a checksum, or a guarantee against every kind of corruption: a semantically invalid or instruction-aligned truncated module may pass framing. Cache directory initialization uses `pthread_once` across compiler workers, and a failed file close prevents committing that write.

Failed `vkCreateShaderModule` calls now log the VkResult and module size. Graphics/compute builds stop before attempting pipeline creation with a null module. This does not turn all shader failures into a user-visible dialog or guarantee that a driver's compiler cannot crash.

Physical-device ranking now happens after checking for a nonempty queue with graphics, compute and successful presentation support for the actual surface. An unusable higher-ranked GPU is skipped instead of preventing use of another compatible GPU. The selection still requires Vulkan 1.3 and does not implement separate graphics/presentation queues. See [presentation support queries](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPhysicalDeviceSurfaceSupportKHR.html) and the [SPIR-V binary layout](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#PhysicalLayout).

PC checks: `tools/test_shader_cache.py <NDK llvm bin>` covers 17 cache-identity/framing cases; `tools/test_vulkan_compat.py <NDK llvm bin>` covers 21 required/optional-feature and queue-selection cases. They execute production C helpers as WebAssembly and do not exercise a real driver. No change to ADPF, pacing, shader optimization defaults or Vulkan feature requirements. No fix for any community device has been confirmed from these checks.

### Bounded shader constant tables (2026-10-21)

The first village benchmark control crashed before the route with SIGSEGV in `maxwell_translate+0x34c`; symbolization mapped it to `brx_table` reading `cb1[j]`, called by `translate_compute`. The compute translator owned a copy of only the shader code, while BRX analysis inspected up to 1024 words immediately after that allocation. Whether those reads faulted depended on adjacent memory, so earlier successful runs did not establish safety.

The bounded translation entry point now receives the actual constant byte count. BRX inspection stops at complete available words, capped at 1024. The asynchronous Vulkan compute snapshot includes up to 4096 constant bytes after the code and frees the combined snapshot after translation. Graphics and Metal callers provide their stage's recorded constant size. The legacy code-only API treats constants as unavailable and uses the existing conservative control-flow fallback; it no longer assumes readable memory after a supplied code buffer. This does not fix graphics source lifetime or address-based program-cache identity, which are separate concerns.

`tools/test_shader_constants.py <NDK-bin>` executes the production table helper at the end of WebAssembly linear memory: 12 cases cover missing/partial constants, a final readable word, valid targets, duplicates and the 1024-word cap. Reinstating the old 1024-word scan makes the guard test trap beyond memory. These tests verify the table bounds, not the full translator or rendered equivalence. Android release assembly passed. The correction is local and does not change ADPF; performance comparisons use it in both control and experimental APK variants.

### Texture decode/upload diagnostics for community reports

`LARECOMP_TEXTURE_TRACE=1` enables `[texture-trace]` summaries in the texture upload path. It is off by default and adds no GPU query, flush or wait. Each summary separates native uploads from BC and ASTC CPU-decoding paths, reporting mip/layer levels uploaded, source block bytes, allocated staging bytes, total CPU decode time and the slowest level's decode time.

The window is reported on the next upload after at least two seconds; idle periods do not generate zero-activity lines, and a trailing partial window may not be reported. `staging` includes padding and allocations for cropped uploads, so it is not measured GPU transfer bandwidth. CPU decode timing excludes hashing, deswizzling, allocation and driver/GPU work. The counters cover `upload_texture`, not every copy/readback or the separate pending-RGBA path. Use this with scene, resolution, driver and independent frametime measurements before deciding whether parallel decode, NEON or fewer conversions would help a MediaTek device. No gain has been measured on those devices yet.

### Android import preflight and actionable setup errors

The SAF importer now estimates additional free space before removing a previous game preparation: ExeFS bytes + twice RomFS bytes + 2 GiB of build workspace + 256 MiB of metadata margin. Existing imported files are not credited as reclaimable space, making reimport conservative. This is an estimate, not a guarantee against later exhaustion or provider changes. Insufficient-space messages show the estimated requirement and available GiB; the earlier preparation has not been removed at that point.

Unknown file sizes cause an instruction to select a local game folder instead of silently counting those files as zero. Copies compare bytes read with the provider-reported size; null directory listings and unavailable app storage produce explicit errors. A primary-storage source inside the app's import target is rejected before that target can be cleared. Copy errors still do not provide transactional rollback after copying has begun. The launcher now preserves specific errors from copying, image construction and code compilation, separating setup failures from Vulkan failures.

`tools/test_import_storage.py` exercises the production Java sizing/path policy on a PC JDK: copy/image accounting, workspace reserve, negative/overflow inputs, locale-independent details, and same/nested/sibling/normalized paths. It does not simulate SAF or validate dialogs on a phone. Android documents [unknown document sizes](https://developer.android.com/reference/android/provider/DocumentsContract.Document#COLUMN_SIZE) and the advisory nature of [usable-space estimates](https://developer.android.com/reference/java/io/File#getUsableSpace()).

- ADPF session initialization, replacement, reporting and diagnostic snapshots share a mutex. In unified mode only the presenting thread manages the session.
- ADPF retains the established render wall-time signal by default. `LARECOMP_ADPF_CPU_TIME=1` opts into CPU-only measurement; this is experimental because excluding waits/descheduling can change the frequency governor response. Frame pacing retains wall time in either mode.
- Consecutive Vulkan draws reuse the pipeline and its baked configuration until a relevant command changes state, a pass restarts, the program changes or the topology class changes. Rebinding identical state does not invalidate it. Pipeline readiness is checked on every draw.
- Texture conversion shares a growing scratch buffer across mip levels and layers. Stable dynamic textures can avoid another full hash through exact comparisons against a bounded cache (eight snapshots, each at most 1 MB). Periodic full verification and forced full hashing remain available. This does not implement OS page-write tracking.
- Load-only global-memory copies can reuse an immutable arena allocation within the same submission and guest-write generation, after an exact byte comparison. Changed bytes always receive a new allocation; resident GPU-written regions keep their existing synchronization. Disjoint resident upload runs share a barrier in the same upload command buffer.
- Android shader compilation defaults to half the logical CPU count, capped at four workers and at least one. Compute shader translation runs on the translation queue. Its code and local workgroup metadata are captured before queuing. Graphics and compute configure and use the non-reentrant translator under the same lock.
- The APK bundles a deterministic SHA-256 fingerprint of the game-builder assets. The activity unpacks them only when that fingerprint changes, with an install-time fallback for older APK layouts. The first update from the old stamp scheme still unpacks once.

## Diagnostic switches

### GPU counters and presentation diagnostics

`LARECOMP_TIMING_TRACE=1` reuses the existing timestamp-query results and emits summaries about every two seconds. It is disabled by default and adds no GPU query, queue flush or wait. Counter records retain their execution frame, submission and host timestamp so a result resolved later is not attributed to the current frame. An independent 128-record diagnostic history checks the runtime's existing four-record pairing history without changing the values supplied to the game.

- `gpu-pairs`: corrected GPU counter spans before the pacer override, values delivered to the game, raw spans, removed inter-submission idle, host-recording spans, address-pair/frame mismatches, lost results and clamp counts. Buckets are `<8`, `8–12`, `12–16`, `16–20` and `>=20` ms. `starts` counts unpaired records, normally a pair's start; an unmatched record is not inherently proof of a lost timestamp.
- `gpu-submissions`: main-command-buffer timestamp intervals, separated between submissions carrying presentation semaphores and other submissions. The non-overlapping interval calculation is the same one already used for `gpu_busy_total`; upload command buffers are not independently timed. These spans can include dependencies and are not hardware utilization measurements.
- `gpu-fences`: calls to `renderer_wait_seq` and how often a requested future submission was clamped to the last submitted sequence. This observes the existing behavior; it does not change waits or submit work.
- `present-waits`: CPU wall time in timeline waits, slot reuse, `vkAcquireNextImageKHR`, `vkQueuePresentKHR` and `vkQueueSubmit2`, with call counts, total, per-present mean and maximum. Slot reuse includes its timeline wait and retirement work, so those categories must not be added together. Atomic wait aggregates may straddle a reporting boundary. CPU return time from acquisition/presentation is not the time the image is displayed; SurfaceFlinger remains the source of measured frametime.

On the test POCO at 50%, a repeated FSR1 capture observed approximately 7.88 ms in the game's corrected counter pairs and 4.08 ms in presentation submissions during fast windows. Slow windows averaged 12.17 and 7.70 ms respectively (about 19.87 ms combined). In those same observations, CPU acquisition averaged 0.032–0.048 ms per frame. No missing four-entry history matches, cross-frame pairs, invalid intervals or query failures were observed, and none of the observed fence waits were clamped to a future/unsubmitted request. This excludes those failure modes only for the measured scene and execution paths.

A bilinear isolation capture did not fix the oscillation: 30.77 FPS over 89.24 seconds, p99 41.71 ms. It reduced presentation timestamp spans in slow windows, but other spans increased; no improvement with equal visual quality was established. Upscaling defaults, pacing and ADPF remain unchanged. GPU frequency was not available in the attempted Perfetto capture, so a frequency-governor explanation remains unconfirmed.

`tools/test_gpu_timing_trace.py <NDK llvm bin>` executes the production diagnostic history helper as WebAssembly, checking address reuse, intervening records, cross-frame visibility, wraparound and expiration. It does not validate a Vulkan driver's timestamp behavior. Vulkan semantics used to interpret these measurements are documented by [timestamp writes](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdWriteTimestamp2.html) and [image acquisition](https://docs.vulkan.org/spec/latest/chapters/VK_KHR_surface/wsi.html#_acquiring_presentable_images).

### Follow-up: Auto frame-rate recovery after settings changes

`LARECOMP_PACE_TRACE=1` emits a summary about every two seconds: one-step frames, late-only frames (period above 20 ms with measured work at or below 16.2 ms), work-over-budget frames, frames with the pacer's 30 FPS override, mean period/blocked/game/render times, maximum work, smoothed per-step work and current retry delay. It is disabled by default, leaves ADPF and pacing decisions unchanged, and avoids per-frame logcat output. Late-only is a measurement classification, not proof that a late frame is harmless. `LARECOMP_PACE=0` is an existing diagnostic control that leaves the game's own GPU-based FPS choice in place; it is not a forced-60 mode.

Follow-up device isolation with the same diagnostic APK found 30/60 oscillation even with `PACE=0`; disabling the pacer is therefore not sufficient. A one-frame control, natural game choice and a two-frame async queue each received an initial and warm 60-second observation. Warm observations averaged 52.03, 59.26 and 59.11 FPS respectively, but initial observations were 38.46, 47.84 and 31.18 FPS, and a preliminary control averaged 55.63. No history gaps were detected. These single-pass results and the variable control do not establish a repeatable benefit; the queue remains one frame and pacing remains enabled. No release was published. Further investigation should correlate the game's GPU counter pairs with frame identity and presentation waits, keeping ADPF unchanged.

The shared `pace_slow` and `pace_on` control flags are now atomic because the game thread writes them while the render thread reads them during counter resolution. This is a concurrency correctness fix, not a demonstrated performance improvement. It was built separately from the APK used for the comparison. Production-function replay tests also verify that enabling the summary diagnostics preserves decisions and does not emit per-frame logs.

Android pacing now discards its sample window, average and failed-trial backoff when the frame-rate mode or internal-resolution setting changes. Previously, a workload that failed several 60 FPS trials could carry an up-to-80-second retry delay into a lower-resolution configuration or a return from fixed 30 FPS to Auto. The transition frame is excluded because it may contain menu time and render-target allocation. Ordinary loading hitches do not reset the backoff, so they cannot continually restart unsuccessful trials. Existing overload thresholds, trial intervals, ADPF signals/targets and CPU affinity are unchanged. This responds to explicit resolution-setting changes, not every dynamic-resolution step.

`tools/test_android_pacing.py <NDK llvm bin>` executes the production pacing function as WebAssembly with a deterministic clock. Behavioral replays cover sustained 60 FPS, overload fallback, repeated failed trials, resolution changes, fixed/Auto transitions, isolated hitches and the unchanged ADPF target. These checks establish state-transition behavior; they do not establish a GPU performance gain.

Device follow-up (POCO F7 Ultra, 50%, Auto, isolated copy of the save): an initial 59.52-second observation averaged 38.56 FPS (p99 41.68 ms), recovering near 60 late in the window. A subsequent fixed-30-to-Auto transition correctly logged fresh history, but overload detection returned to 30 within 0.74 seconds; a 29.38-second observation averaged 31.11 FPS (p99 41.68 ms). Neither capture had missing SurfaceFlinger history. This is not a controlled baseline comparison and does not establish regression or improvement. It shows that clearing old history alone does not fix the observed oscillation. The change remains local and unpublished; the public 0.3.8 APK and original settings were restored after testing.

### Follow-up: upload synchronization and descriptor metadata

An experimental `appCategory="game"` manifest enabled Game Mode recognition (`standard` and `custom` on the test POCO). In this session, several 720p captures were substantially slower, even with the Vulkan changes disabled. Explicitly opting out of Game Mode FPS/scaling interventions did not restore the earlier behavior. However, the original APK and the Vulkan-only variant also showed transient 30/60-mode changes, so the category is not established as the sole cause. The category change was removed pending a repeatable comparison of Android/OEM policy and runtime pacing. No mode is forced, and ADPF timing and CPU affinity are unchanged.

Each compiled shader program now partitions reflected bindings into an immutable per-set plan. With the experimental descriptor-plan switch enabled, draws/dispatches visit only the relevant bindings, with special UBOs deferred until texture scales are known, in the same order as the original two scans. Live buffer contents, texture views, descriptor-set caching and submission lifetimes still follow the existing paths; no mutable descriptor data is skipped or reused based only on metadata.

With the experimental upload-barrier switch enabled, only the initial dependency of the upload command buffer narrows its destination to ALL_TRANSFER with TRANSFER_READ/TRANSFER_WRITE. The source remains ALL_COMMANDS, preserving ordering against prior reads and writes. Initial image transitions retain their own image barriers. Publication at the end of uploads and synchronization between render passes/compute remain conservative. The upload buffer must continue to contain only transfer operations and initial layout transitions.

Both experiments are **off by default**. A 59.26-second warmed-up capture of the Vulkan-only variant at 1600x720 reached 59.27 FPS, p99 33.36 ms, versus the earlier published-build sample of 59.76 FPS, p99 16.68 ms. These sequential captures do not establish causation, but do not justify enabling the changes as a performance improvement. The published APK was retained. Runtime 30/60 pacing transitions were also observed on the unmodified APK; a reproducible comparison must control warm-up and mode history. A SurfaceFlinger history gap in one control capture was treated as missing observations, not a 12-second game frame.

`tools/test_descriptor_plan.py <NDK-bin>` compares the actual C plan helper against the legacy selection/order across 31,040 generated cases, including empty sets, deferred UBOs, unsupported bindings and maximum binding count. This checks metadata equivalence, not Vulkan execution or rendered image correctness. The original upload-cache checks remain separate.

References: [Game Mode integration](https://developer.android.com/games/optimize/adpf/gamemode/gamemode-api), [Khronos barrier guidance](https://docs.vulkan.org/samples/latest/samples/performance/pipeline_barriers/README.html). These changes are not a claim of a measured FPS gain.

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
| `LARECOMP_VK_DESCRIPTOR_PLAN=1` | Opt into the experimental precomputed binding plan (off by default). |
| `LARECOMP_VK_UPLOAD_BARRIER=1` | Opt into the experimental transfer-only destination dependency at upload start (off by default). |

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
