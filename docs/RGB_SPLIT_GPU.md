# Vulkan RGB Split

RGB Split is the first Vulkan compute filter in CompositorLX. Both the destructive filter and editable RGB Split filter layers call the same backend. The UI still waits for each render; this change accelerates the computation and does not introduce asynchronous previews. Other filters retain their existing rendering paths. This development change follows the published 0.5.5 release.

## Choosing a backend

Automatic selection uses Vulkan for images with at least 262,144 pixels and at least 16 combined spectral/blur samples per pixel. Smaller images and cheaper effects use CPU rendering to avoid conversion, allocation, and submission overhead. These are conservative thresholds based on the measurements below, not per-device auto-tuning.

A hardware Vulkan 1.0 compute device is required. The backend loads the system Vulkan library at runtime, chooses a discrete or integrated GPU, and rejects software Vulkan devices. It has no window-system or Qt private-API dependency. Missing loaders/drivers, initialization errors, unsupported image sizes, excessive memory requirements, and rendering failures return to the CPU implementation. A device-lost error disables further GPU attempts for that process.

For troubleshooting or comparison:

```sh
COMPOSITOR_RGB_SPLIT_BACKEND=cpu ./build/compositor-lx
COMPOSITOR_RGB_SPLIT_BACKEND=vulkan ./build/compositor-lx
```

`vulkan` requests the GPU even below the automatic thresholds, while retaining CPU fallback. Unset the variable, or use `auto`, to restore automatic selection. The C++ API also accepts `RgbSplitBackend` and an optional `RgbSplitExecution` report containing the actual backend, device name, and fallback reason.

## Image quality and resource handling

The shader uses the CPU algorithm's spectral weights, bilinear sampling, bounded sample counts, radial coordinates, edge-extension flags, and premultiplied alpha. Jitter is calculated on the CPU using the existing double-precision expression, preventing GPU sine approximations from changing the sampling pattern. Images return in their original QImage format, including 16-bit and floating-point HDR formats.

Vulkan computes with 32-bit floats while the CPU uses double-precision accumulation, so output is not bit-identical. Regression comparisons allow at most one 8-bit channel step, or 0.0001 in premultiplied floating-point channels for the 16-bit/HDR fixtures. Tests include transparent pixels, HDR values above one, all 16 edge combinations, linear/radial settings, maximum sample counts, odd dimensions, one-pixel dimensions, large coordinates, and concurrent calls. These tolerances describe tested fixtures, not a universal error bound for every image or extreme parameter value.

A shared queue and pipeline are protected by a mutex. Integrated GPUs can use host-visible device memory directly; GPUs without suitable shared memory use staging copies. Explicit barriers and a fence protect uploads and readback. The per-render Vulkan working buffer is capped at 1 GiB and allocations at one quarter of the selected memory heap; device dispatch/storage limits are also checked. The source/output image conversions consume additional host memory. Oversized images use CPU rendering. The discrete-GPU staging path is implemented but has not been exercised on physical discrete hardware here.

## Measurements

Measured on 2026-10-23: Intel Core i7-1265U, Intel Iris Xe (ADL GT2), Mesa 26.0.8, Ubuntu 26.04, Qt 6.10.2, GCC 15.2, Release build. CPU rendering used all 12 logical CPUs (`OMP_NUM_THREADS=12`). Each result is the median of three warm end-to-end renders of a deterministic translucent 8-bit fixture, after one warm-up. Conversion, jitter generation, allocations, transfers, synchronization, and output conversion are included. Shader/pipeline initialization is excluded from warm medians; the first small GPU render took 26.2 ms.

| Image | Settings | CPU | Vulkan | Speedup |
| --- | --- | ---: | ---: | ---: |
| 128 × 128 | Distance 10, no blur | 6.49 ms | 1.16 ms | 5.6× |
| 128 × 128 | Radial, distance 30, blur 12 | 51.90 ms | 3.74 ms | 13.9× |
| 512 × 512 | Distance 10, no blur | 20.77 ms | 12.72 ms | 1.6× |
| 512 × 512 | Radial, distance 30, blur 12 | 163.03 ms | 20.82 ms | 7.8× |
| 1920 × 1080 | Distance 10, no blur | 174.39 ms | 69.95 ms | 2.5× |
| 1920 × 1080 | Radial, distance 30, blur 12 | 1316.98 ms | 155.42 ms | 8.5× |

The CPU loop is single-threaded at 128 × 128, so its unusually large speedup should not be extrapolated to larger images. Automatic mode keeps that image size on the CPU to avoid first-use costs. These are measurements on one integrated GPU, not a guarantee of performance on every Vulkan device.

Reproduce:

```sh
cmake --build build --target rgb_split_gpu_tests
OMP_NUM_THREADS=12 ./build/tests/rgb_split_gpu_tests --benchmark
COMPOSITOR_REQUIRE_VULKAN_TESTS=1 ./build/tests/rgb_split_gpu_tests
ctest --test-dir build --output-on-failure
```

Tests skip GPU-specific parity cases when no GPU is available. `COMPOSITOR_REQUIRE_VULKAN_TESTS=1` turns that condition into a failure so a hardware run cannot silently pass on CPU fallback. The separate `rgb_split_no_driver_tests` suite starts a fresh process with a nonexistent driver manifest and checks exact CPU fallback output. Software-only Vulkan was also checked with Mesa llvmpipe.

## Build and shader maintenance

Vulkan headers are detected automatically; install `libvulkan-dev` to compile the backend. Use `-DCOMPOSITOR_ENABLE_VULKAN=OFF` to compile only the fallback. The executable does not link against Vulkan, and distribution packages should use the host's Vulkan loader and graphics drivers.

The Vulkan 1.0 SPIR-V shader is embedded in a checked-in header, so normal builds do not require development shader tools. After changing `src/rendering/shaders/rgb_split.comp`, install `glslang-tools` and `spirv-tools`, then run:

```sh
python3 scripts/compile-rgb-split-shader.py
python3 scripts/compile-rgb-split-shader.py --check
```

Generation runs `spirv-val` when available. The source-hash check is registered with CTest when Python is available. GPU tests were also run with Khronos Vulkan validation enabled; no validation errors were reported.

## Verification results

The final Release application build succeeded on the machine above. All **22/22 CTest suites passed** in 92.29 seconds (`OMP_NUM_THREADS=4`), including the original 19 suites. The GPU test executable reported **151 passed, 0 failed, 0 skipped** with hardware required, and again with Khronos core and synchronization validation enabled. No validation errors were reported.

A separate `COMPOSITOR_ENABLE_VULKAN=OFF` build compiled successfully; its executable passed all seven non-GPU checks and correctly skipped the 144 GPU parity cases. Missing-driver and software-only Vulkan runs returned exact CPU output. The application version and bundled subject-model smoke checks also succeeded. The final executable has no mandatory Vulkan shared-library dependency.
