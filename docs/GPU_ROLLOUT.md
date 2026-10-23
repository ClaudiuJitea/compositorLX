# GPU rendering in the refreshed 0.5.5 release

This update keeps the application version, GitHub release, and asset names at **0.5.5**. It replaces the original CPU-oriented packages with the shared-renderer build. It is a mixed CPU/GPU implementation, not a claim that every function executes on the GPU.

## Coverage and CPU boundaries

| Area | Implementation in this update | Boundary or fallback |
| --- | --- | --- |
| Shared backend | Vulkan 1.0 compute, one persistent pipeline/queue, reusable memory, source-upload cache, intermediate lifetime reuse, one readback per graph | Hardware integrated/discrete GPUs only; missing loader/driver, limits, allocation/dispatch failures fall back to CPU. Device loss disables subsequent attempts. |
| Color adjustments | GPU exposure, invert, levels/curves lookup, 8-bit Hue/Saturation, gradient map, black-and-white and color balance | Automatic accelerates expensive exposure/HSL; cheap standalone adjustments remain CPU. Some deep-color adapters still use CPU reconstruction. Deep HSL retains QColor's exact lookup generation; HDR exposure retains its existing CPU implementation. |
| Blur/effects | Gaussian, Motion, Progressive Blur, RGB Split and Bloom; mask feathering and layer-effect coverage blur/morphology | Premultiplied float computation supports deep images; byte algorithms preserve their intermediate quantization. Local Gaussian/Bloom/lookup operations can tile with overlap. Oversized non-local graphs fall back. |
| Document rendering | All 24 blend modes, layer/group masks, opacity, pixel-aligned placement; resident 8-bit chains for Invert, Exposure, Levels, Curves, Gaussian, Bloom, Progressive Blur and RGB Split | Clipping stacks, unsupported adjustments and deep adjustment layers use the established renderer with individually accelerated filters. Rotated/fractional or partially opaque placement is rasterized through QPainter to preserve its coverage/rounding. |
| Canvas | Optional public Qt OpenGLWidgets presentation; stable display-image cache avoids repeated deep-color dithering; existing background flattening retained and queued-completion handling fixed | One CPU readback remains between Vulkan rendering and Qt presentation. This is not Vulkan/OpenGL zero-copy sharing. Software GL, unsupported/offscreen platforms, CPU mode, or context failure use raster presentation. |
| Camera RAW | Coalesced asynchronous previews for images ≥262,144 pixels; only the latest result reaches the document; cancellation cannot commit stale pixels. GPU cube kernel available in explicit GPU mode | The standalone color cube was slower on this machine, so Automatic uses CPU. Histograms, denoise, optics and many RAW stages remain CPU. Final OK commits still render synchronously. |
| Interactive editing | Large blur-brush regions reuse the shared blur backend; small patches avoid GPU overhead. Canvas pan/zoom uses hardware presentation where available | Brush coverage, healing, cloning, smudge/warp, flood fill and selection topology retain CPU implementations. |
| Remove Background | Provider discovery and optional OpenVINO GPU/CUDA session selection, with CPU retry on provider initialization/inference failure | **Released packages bundle CPU ONNX Runtime.** GPU AI requires a compatible provider-enabled custom runtime and its drivers. It is not accelerated merely because Vulkan works. Optional provider execution was not validated on physical hardware here. |
| Other work | Existing behavior preserved | Noise/grain, scanlines/dither, some vignette/tonal operations, text/vector rasterization, ICC proofing, import/export codecs, GIF encoding, project storage and history remain wholly or partly CPU. |

The seven rollout areas are covered by implementation, measurement, UI integration, AI-provider evaluation, and validation. Remaining CPU boundaries above are explicit scope limits; a fully GPU-resident editor, additional kernels, provider-enabled distribution packages and per-device autotuning are follow-up work.

## Controls and diagnostics

Use **View › Rendering…** to select Automatic, CPU, or GPU (with CPU fallback), choose a 64–2048 MiB working-memory budget (default 512 MiB), and inspect the device, last graph's dispatch/upload/readback counts, and available AI providers. Preferences persist across launches. GPU mode requests supported kernels even when transfer overhead may make them slower.

Environment overrides:

```sh
COMPOSITOR_RENDER_BACKEND=cpu ./build/compositor-lx
COMPOSITOR_RENDER_BACKEND=vulkan ./build/compositor-lx
COMPOSITOR_GPU_MEMORY_MIB=256 ./build/compositor-lx
COMPOSITOR_CANVAS_BACKEND=raster ./build/compositor-lx
COMPOSITOR_AI_BACKEND=cpu ./build/compositor-lx
QT_QPA_PLATFORM=offscreen ./build/compositor-lx --check-renderer
```

`auto` restores automatic backend selection. `COMPOSITOR_AI_BACKEND=openvino|cuda|auto` selects an available custom-runtime provider. Legacy `COMPOSITOR_RGB_SPLIT_BACKEND` remains supported for that filter; an explicit legacy override takes precedence over its automatic selection.

Automatic generally requires ≥262,144 pixels and an operation classified as expensive; RGB Split also requires ≥16 combined samples. These are conservative fixed thresholds, not live device benchmarking. A supported resident chain can include cheap operations without separate transfers. The memory limit counts pooled Vulkan host/device buffers (including staging), not the document's QImages and other application memory. Allocations also respect device storage/dispatch limits and one quarter of the selected memory heap.

## Quality and resource safety

The graph records compute passes with explicit barriers and waits for completion before readback. Source images are immutable snapshots; cached uploads are invalidated when their identity or layout changes. Concurrent preview/export callers share a serialized queue. Worker completion is drained before device teardown, ahead of driver/validation-layer static destruction.

GPU float arithmetic is not universally bit-identical to CPU double arithmetic. Fixtures allow one 8-bit step for filters, two steps for composed documents, 0.00015 for the tested deep filters/blends, and 0.001 for Progressive Blur's half-float canonical output. RGB Split retains its stricter existing comparisons. These are fixture tolerances, not universal bounds for arbitrary extreme inputs.

Testing uncovered a Qt 6.10 direct 8-bit-premultiplied-to-float conversion that could replace alpha with 1. A shared conversion helper now uses a 16-bit bridge, preserving transparency in CPU and GPU paths. Tests check the known alpha independently rather than comparing two equally incorrect conversions.

## Measurements

Measured 2026-10-23 on Intel Core i7-1265U / Iris Xe ADL GT2, Mesa 26.0.8, Ubuntu 26.04, Qt 6.10.2, GCC 15.2, Release build. `OMP_NUM_THREADS=12`; deterministic translucent 8-bit input; median of three warm renders after warm-up; no concurrent build/test run. Conversion, transfers, synchronization and output conversion are included. Persistent pipeline/source caches are warm, so cold first use can be slower.

| 1920 × 1080 workload | CPU ms | GPU ms | CPU/GPU |
| --- | ---: | ---: | ---: |
| Exposure +0.75, offset .05, gamma 1.2 | 329.57 | 9.05 | 36.4× |
| Gaussian, byte algorithm, radius 2.5 | 124.00 | 59.84 | 2.1× |
| Gaussian, byte algorithm, radius 12.5 | 125.10 | 64.62 | 1.9× |
| Gaussian, float algorithm, radius 3.5 | 332.88 | 63.76 | 5.2× |
| Bloom amount 35, radius 4 | 161.73 | 66.51 | 2.4× |
| Progressive Blur, 12 levels, radius 2–15 | 5049.56 | 394.91 | 12.8× |
| Camera RAW exposure .7, temperature 15 | 19.52 | 26.49 | 0.74×; Automatic keeps CPU |
| Hue/Saturation +25/+30/−12 | 296.21 | 11.30 | 26.2× |
| Motion Blur −31°, distance 13 | 48.43 | 19.09 | 2.5× |

At 512 × 512, the corresponding exposure/HSL measurements were 41.57/1.80 ms and 34.07/1.45 ms; Progressive Blur was 633.60/25.81 ms. Performance varies with image content, settings, cache state, thermal state and GPU. These numbers do not establish AMD/NVIDIA performance. Historical standalone RGB Split measurements are in [RGB_SPLIT_GPU.md](RGB_SPLIT_GPU.md).

Reproduce with `OMP_NUM_THREADS=12 ./build/tests/gpu_renderer_tests --benchmark`; operation indices are defined in that test's `operation()` function.

## Validation and building

The full suite now contains **26 CTest suites**. Added checks cover translucent conversions, 8/16/float filters, all 24 blend modes, resident adjustment/filter chains with one readback, source-cache invalidation, large overlapping tiles, allocation limits, morphology/coverage, group masks, concurrency, missing drivers, queued preview completion, cancellation/destruction, preferences, undo/redo, PNG output and Camera RAW rollback.

The hardware renderer suite reports **120 passed, 0 failed, 0 skipped** with `COMPOSITOR_REQUIRE_VULKAN_TESTS=1`. The separate RGB Split suite has 151 checks. Khronos core/synchronization validation also passes with forced staging (`COMPOSITOR_GPU_FORCE_STAGING=1`) on Iris Xe; that exercises the copy/barrier path without claiming a physical discrete-GPU test. The OpenGL canvas presentation test passes on the actual Iris Xe/XCB session and the offscreen raster path. Physical AMD/NVIDIA GPUs and optional GPU AI providers remain untested.

The embedded shader is Vulkan 1.0 SPIR-V. Normal builds need only Vulkan headers to enable compute; runtime loading avoids a mandatory libvulkan link. `-DCOMPOSITOR_ENABLE_VULKAN=OFF` builds the compute fallback. Qt OpenGLWidgets is detected separately. After shader changes:

```sh
python3 scripts/compile-rendering-shader.py   # glslangValidator and optional spirv-val
python3 scripts/compile-rendering-shader.py --check
OMP_NUM_THREADS=4 ctest --test-dir build --output-on-failure
COMPOSITOR_REQUIRE_VULKAN_TESTS=1 ./build/tests/gpu_renderer_tests
```

`packaging/build-packages.sh` builds and runs the full suite, checks the subject model, verifies the version, and runs the packaged renderer self-check both normally and with a missing Vulkan ICD before generating SHA256SUMS. Drivers are supplied by the host, not bundled in the AppImage. The current Ubuntu 26.04 packages retain the existing glibc 2.43 requirement.
