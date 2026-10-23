# GPU rendering in CompositorLX 0.5.5

The 0.5.5 release and its assets were refreshed (again) with the follow-up to the GPU audit: exact CPU-matching exports, compact GPU storage, tiled large documents, an incremental canvas, asynchronous filter previews and a reworked Rendering panel. The application version stays **0.5.5**. CompositorLX is a mixed CPU/GPU editor: the CPU renderer is the reference and the fallback for everything, and the GPU is used where it measurably helps.

## What runs where

| Area | GPU (Vulkan compute) | CPU |
| --- | --- | --- |
| Canvas display | The whole document as one GPU graph (or overlapping tiles of one) in Automatic and GPU modes: all 24 blend modes, layer and folder masks, opacity, aligned and transformed layers, layer effects, and the filter layers Invert, Exposure, Levels, Curves, Gaussian Blur, Bloom / Glow, Progressive Blur and RGB Split. Re-renders after an edit reuse the composite of the unchanged layers below it. | Clipping stacks, other adjustment layers, adjustment layers on 16-bit/HDR documents, and effects rasterization. |
| Exports and committed results | Only when **GPU** is chosen explicitly. | **Automatic and CPU modes**: export, GIF frames, project previews, Copy Merged, Merge/Flatten, channel packing and sampling always use the CPU reference, so a file renders byte-identically on every machine. |
| Filters and adjustments | Exposure, Hue/Saturation, Levels, Curves, Gradient Map, Black & White, Color Balance, Gaussian/Motion/Progressive Blur, Bloom, RGB Split, mask feathering, layer-effect blur, small-radius morphology. | Invert (memory-bound; CPU is faster), the Camera RAW color cube (CPU is faster standalone), large-radius stroke morphology (the CPU deque is faster on the tested GPU), noise, grain, dither, scanlines, lens correction, tonal contrast, vignette, healing, cloning, warping, selections, codecs. |
| Filter dialogs | Previews of layers of 512 × 512 pixels and larger render on a worker thread from a snapshot of the document; only the newest result reaches the layer, the dialog never waits, and OK reuses the finished preview. | Smaller layers preview synchronously (no visible latency). |
| Camera RAW | Previews, the histogram/scope and the clipping overlay are computed on a worker; OK reuses the shown full-resolution preview or renders on a worker. | The develop pipeline itself. |
| Canvas presentation | Qt OpenGL presentation of the rendered image (Automatic on Mesa and Intel drivers; OpenGL can be chosen on others). | Raster presentation in CPU mode, on untested drivers in Automatic, offscreen, or with software GL. |
| Remove Background | — | **Released packages run the model on the CPU.** The optional OpenVINO/CUDA provider code is dormant unless the app is built against a provider-enabled ONNX Runtime; it is not tested or shipped. |

## How the renderer works

```
caller (filter, adjustment, document, layer effect)
  └─ GpuGraph: source nodes (QImage or one float channel) + passes; one output
       └─ Vulkan context (runtime-loaded libvulkan.so.1)
            ├─ before the lock: graph analysis, source format conversion
            ├─ priority lock: interactive > canvas > export; GUI-thread calls never wait (CPU fallback)
            ├─ upload (skipped for sources already resident), one command buffer, one pipeline per operation
            └─ one readback, straight into the requested image format
```

- **One contract.** Operation IDs, blend-mode IDs, storage formats and named parameter slots are generated for C++ (`src/rendering/GpuContract.h`) and GLSL (`src/rendering/shaders/contract.glsl`) from the tables in `scripts/compile-rendering-shader.py`; `GpuDocument.cpp` static-asserts the blend IDs against `BlendMode`. The shader is readable source; `--check` (a CTest) fails if any generated file is stale.
- **Compact storage.** Each node is stored in the smallest exact format: 8-bit sources and 8-bit intermediates as RGBA8 (4 bytes per pixel), 16-bit as RGBA16, half-float as RGBA16F, masks as R8, coverage and noise as one float channel; only float work uses RGBA32F. The output is written in the requested QImage format, so 8-bit results are read back at 4 bytes per pixel with no CPU conversion. A document starts from a GPU clear instead of an uploaded transparent canvas.
- **Exact 8-bit paths.** Byte blurs sum and divide in integers, the gradient map uses the integer Rec. 709 luma, Hard Mix compares exact integer products, and Normal plus the eight QPainter blend modes mirror QPainter's own integer formulas, so they match the CPU bit for bit. The other blend modes agree within the fixture tolerances below.
- **Pipelines.** One pipeline per operation through a specialization constant, compiled lazily or at startup on a worker, and persisted in a validated pipeline cache under the user cache directory.
- **Memory.** Every graph is checked against the working-memory budget (default 512 MiB) before any CPU preparation; a document that does not fit is split into canvas tiles with enough overlap for its filters' reach, or left to the CPU. Pooled buffers are released after 30 seconds idle and when the budget changes; choosing CPU closes the Vulkan device. On discrete GPUs, uploads and readback go through cached system memory and a device-local working buffer. A lost device is recreated once, then the GPU stays off for the session.
- **Device choice.** Hardware devices only (software Vulkan is allowed only with `COMPOSITOR_GPU_ALLOW_SOFTWARE=1`, for tests). Automatic prefers the integrated GPU on hybrid laptops so a discrete GPU is not woken; the Rendering panel lists the devices when there is more than one.

## Controls and diagnostics

**View › Rendering…** sets the renderer (Automatic, CPU, GPU with CPU fallback), the GPU working memory (64–2048 MiB), the canvas presentation (Automatic, Raster, OpenGL) and, with several GPUs, the device. It shows the device, driver, Vulkan version and memory heaps, the last 20 GPU graphs (operations, time, transfers, priority, fallback reason), **Run self-check** (as `--check-renderer`) and **Copy diagnostics**. Diagnostics load in the background.

Environment overrides take precedence; the panel shows them and never saves them as preferences:

```sh
COMPOSITOR_RENDER_BACKEND=auto|cpu|vulkan   # renderer
COMPOSITOR_GPU_MEMORY_MIB=256               # working-memory budget
COMPOSITOR_CANVAS_BACKEND=auto|raster|opengl
COMPOSITOR_GPU_DEVICE="Device name"         # as listed in the panel
COMPOSITOR_AI_BACKEND=cpu|openvino|cuda|auto
QT_QPA_PLATFORM=offscreen compositor-lx --check-renderer
```

Testing aids: `COMPOSITOR_GPU_ALLOW_SOFTWARE=1` (lavapipe), `COMPOSITOR_GPU_FORCE_STAGING=1` (discrete-GPU copy path), `COMPOSITOR_GPU_NO_PIPELINE_CACHE=1`, `COMPOSITOR_GPU_IDLE_TRIM_MS`, `COMPOSITOR_SYNC_PREVIEWS=1`.

**What Automatic sends to the GPU.** Images of at least 262,144 pixels (512 × 512) for operations measured faster there: the document graph (canvas only), Exposure, Hue/Saturation, Levels, Curves, Gradient Map, Black & White, Color Balance, all blurs, Bloom, RGB Split, mask feathering, layer-effect blur and morphology up to radius 6. The thresholds come from the benchmark below; Invert, the standalone Camera RAW cube and larger morphology radii stay on the CPU because they were slower on the GPU.

## Accuracy

The CPU renderer is the reference. Fixture tolerances (all exercised by the tests): 8-bit filters within 1 level, documents within 2 levels (a 20-layer stack of mixed modes: 3 levels), 16-bit and float filters and blends within 0.00015, Progressive Blur's half-float output within 0.001, HDR component blends within 0.0005. The 8-bit Gaussian, the morphology and the Normal/QPainter-mode composites match exactly.

Fixed CPU-side issues found by the audit:

- HDR documents using Hue, Saturation, Color or Luminosity layers produced NaN pixels (gray colors made the W3C clip divide 0 by 0). The degenerate clip is now skipped on both backends, which keeps the result continuous.
- Hard Mix decided exact ties (e.g. 168/174 + 4/116 = 1) by floating-point accident; it now compares exact integers.
- Blend-mode layers that are scaled or rotated now composite as a placed plane, like the custom modes already did; QPainter's scaled Nearest drawing sampled differently under non-SourceOver modes.
- 8-bit exposure uses an exact lookup table of every (value, alpha) pair, and Hue/Saturation and the custom blend modes run on all cores, with results proven identical to the per-pixel code.

## Measurements

Measured 2026-10-23 with `gpu_benchmark` on an Intel Core i7-1265U (12 threads) with Iris Xe (ADL GT2), Mesa 26.0.8, Ubuntu 26.04, Qt 6.10.2, Release build, default 512 MiB budget; medians of 3–5 runs. Results vary with content, settings, clocks and thermal state, and say nothing about other GPUs. Peak resident memory of the whole benchmark: 1867 MiB.

#### First GPU use in a fresh process

| Pipeline cache | Device + first graph (ms) |
|---|---:|
| disabled | 34.2 |
| warm (second launch) | 29.6 |

#### Filters at 1920×1080, 8-bit (cold = new image each run; warm = repeated image)

| Operation | CPU (ms) | GPU cold (ms) | GPU warm (ms) | Speedup cold |
|---|---:|---:|---:|---:|
| Exposure | 9.3 | 8.6 | 2.9 | 1.08× |
| Hue/Saturation | 52.4 | 9.3 | 3.6 | 5.64× |
| Gaussian 2.5 | 133 | 20.8 | 16.5 | 6.37× |
| Gaussian 12.5 | 135 | 29.1 | 23.3 | 4.66× |
| Gaussian 60 | 134 | 40.0 | 30.3 | 3.34× |
| Bloom 35/4 | 164 | 33.7 | 25.3 | 4.87× |
| Progressive Blur 0→15 | 4922 | 296 | 299 | 16.63× |
| Motion Blur 13 px | 53.5 | 5.7 | 4.0 | 9.31× |
| Invert | 3.3 | 3.6 | 1.6 | 0.92× |
| Levels | 56.6 | 5.8 | 3.8 | 9.84× |
| Gradient Map | 12.2 | 3.7 | 2.3 | 3.32× |
| Black & White | 120 | 4.6 | 3.3 | 26.27× |
| Color Balance | 202 | 4.4 | 3.0 | 46.24× |
| Morphology r=3, 2000×2000 coverage | 37.9 | 23.2 | — | 1.64× |
| Morphology r=25, 2000×2000 coverage | 35.8 | 31.9 | — | 1.12× |
| Morphology r=250, 2000×2000 coverage | 35.1 | 28.5 | — | 1.23× |

#### Documents (canvas render = `RenderPurpose::Display`, export = `Commit`)

| Document | CPU (ms) | Automatic canvas (ms) | Speedup | Automatic export (ms) | GPU working memory |
|---|---:|---:|---:|---:|---:|
| 1080p, 3 layers | 17.3 | 9.0 | 1.92× | 12.4 | 134 MiB |
| 4000×3000, 5 layers + rotated/masked/filters | 1254 | 238 | 5.27× | 1250 | 157 MiB |
| 6000×4000, 10 layers + effects | 3120 | 838 | 3.72× | 3088 | 232 MiB |
| 6000×4000, 10 layers + clipping | 3431 | 1779 | 1.93× | 3397 | 274 MiB |
| 4000×3000, 16-bit, 3 layers | 478 | 289 | 1.66× | 484 | 274 MiB |
| 4000×3000, HDR float, 3 layers | 468 | 409 | 1.15× | 468 | 313 MiB |
| 4000×3000, 10 layers, edit layer 9 (cpu, prefix cache) | 379 full | 89.0 | 4.26× | — | — |
| 4000×3000, 10 layers, edit layer 9 (auto, prefix cache) | 117 full | 39.4 | 2.96× | — | — |

#### Camera RAW develop and blur-brush latency

| Scenario | CPU | Automatic | Speedup |
|---|---:|---:|---:|
| Camera RAW develop, 4000×3000 | 324 ms | 334 ms | 0.97× |
| Blur-brush dab 640×640 r=6 (cpu): p50 / p95 | 25.6 / 34.4 ms | | |
| Blur-brush dab 640×640 r=6 (auto): p50 / p95 | 4.4 / 5.4 ms | | |

Notes: Exposure is only ~1.1× faster on the GPU cold because its CPU path became ~37× faster (exact lookup table); warm previews are 3× faster. The prefix cache re-renders a 12 MP, 10-layer canvas after an edit to layer 9 in 89 ms instead of 379 ms on the CPU path and in 39 ms instead of 117 ms on the GPU path. Clipping stacks and HDR documents gain less because they use the CPU renderer (clipping) or float storage (HDR). The Camera RAW develop stays on the CPU in Automatic, so its two columns are the same work. Pipeline compilation adds little on Mesa, which has its own shader cache.

Reproduce with `./build/tests/gpu_benchmark` (or `--quick`). "Cold" gives every run a new image, so uploads are included; "warm" repeats the same image, as slider previews do.

## Validation

- **CTest:** 26 suites, including `gpu_renderer_tests` (120), `gpu_document_tests` (40: placement × sampling × blend-mode matrix, HDR, audit probes, Automatic decisions, determinism of exports, tiling, prefix cache, scheduling, memory lifecycle, exact paths) and `gpu_ui_tests` (11: asynchronous previews, Rendering panel overrides, canvas presentation).
- **Iris Xe (Mesa 26.0.8):** all GPU suites with `COMPOSITOR_REQUIRE_VULKAN_TESTS=1`; under the Khronos validation layer with synchronization checks, normally and with forced staging: 0 validation errors; at 64 and 128 MiB budgets; OpenGL canvas on X11 (XWayland) and native Wayland.
- **lavapipe (software Vulkan, a second, independent implementation):** all GPU suites pass, also with forced staging and under validation. `scripts/ci-gpu-tests.sh` runs these passes on machines without a GPU; `.github/workflows/ci.yml` runs the build, the suite and that script (manually triggered).
- **Not tested on hardware:** AMD (RADV), NVIDIA, Optimus laptops, ReBAR, and GPU execution providers for Remove Background. The discrete-GPU memory path is exercised only through forced staging.

## Building

Vulkan headers are optional (`-DCOMPOSITOR_ENABLE_VULKAN=OFF` builds the CPU-only renderer); the loader and driver are found at runtime. After editing the shader or the contract tables:

```sh
python3 scripts/compile-rendering-shader.py          # needs glslang-tools; spirv-val if installed
python3 scripts/compile-rendering-shader.py --check
COMPOSITOR_REQUIRE_VULKAN_TESTS=1 ./build/tests/gpu_renderer_tests
COMPOSITOR_REQUIRE_VULKAN_TESTS=1 ./build/tests/gpu_document_tests
```

New GPU code follows `src/rendering/.clang-format`. `packaging/build-packages.sh` builds, runs the full suite, and runs the packaged renderer self-check with and without a Vulkan driver.
