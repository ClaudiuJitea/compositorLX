# GPU and multithreading in the refreshed 0.5.8

After [Lens Effects](LENS_EFFECTS_GPU.md), the second refresh of the 0.5.8 assets moves more work onto the GPU where the macOS app uses its GPU, and onto every CPU core everywhere else. The version is still **0.5.8**.

All measurements below: Intel Core i7-1265U (12 threads), Intel Iris Xe (ADL GT2), Mesa 26.0, Release build, 6000 × 4000 8-bit picture, median of three runs, `feature_benchmark` (`tools/FeatureBenchmark.cpp`). "Before" is the same benchmark built from the commit before these changes (`384dabf`), run back to back on a quiet machine. On this integrated GPU, the CPU and the GPU share memory bandwidth; a discrete GPU should gain more on the GPU rows, but that has not been measured.

## The canvas

- **Large documents stay on the GPU.** A document graph used to start from a transparent float canvas (16 bytes a pixel), so a single 6000 × 4000 layer needed more than the 512 MiB default working memory and the whole canvas fell back to the CPU. Now the bottom Normal layer starts the canvas, a transparent start is stored at the document's precision, and a graph the executor finds too large is retried in tiles before the CPU takes over.
- **More live adjustment layers on the GPU.** Hue/Saturation, Black & White, Color Balance, Gradient Map, Motion Blur, Add Noise and Grain layers used to send the whole canvas to the CPU. They now run as passes of the document graph. Add Noise and Grain are new GPU operations ported from macOS `GPUNoise.swift`, with the same hashes as the shared C kernels. They are keyed to document pixels, so tiles line up with the whole canvas, as Motion Blur's shear does.

| Canvas, 6000 × 4000, two layers and… | Before (GPU mode) | Now (GPU mode) | CPU |
| --- | ---: | ---: | ---: |
| a live Add Noise layer | 861 ms (fell back to the CPU) | 187 ms | 402 ms |
| a live Hue/Saturation layer | 288 ms (fell back to the CPU, filter on the GPU) | 160 ms | 1004 ms |
| a live Levels layer | 192 ms | 165 ms | 852 ms |
| (a single layer) | fell back to the CPU | 47 ms | 43 ms |

With only plain layers, the CPU is as fast as or faster than the GPU on this integrated GPU (QPainter composites 8-bit layers very quickly). Automatic's existing choice is unchanged.

## Camera RAW

The composed color table (Light, Color, Curve, Mixer and Grading folded into one 33³ lookup) runs on Vulkan as macOS `MetalCameraRawLookup` does. Cells are picked with integer arithmetic, and pixels whose result lies within 0.01 of a rounding boundary are redone by the CPU's double-precision lookup. The result is byte for byte what `adjust_camera_raw` makes. The test checks all 16,777,216 opaque colors and every alpha value, as the Mac's `CameraRawGPUTests` do. It replaces the earlier float cube, which could be a level off. 6000 × 4000, Light and Color: CPU 362 ms, GPU 225 ms. The rest of the develop pipeline (Shadows, Effects, Detail, Optics) is the shared macOS kernels, which already run on all cores, as on the Mac.

## Shared kernels on all cores

The macOS app runs some of its shared C kernels on one thread: noise, grain, vignette, tonal contrast, lens distortion, gradient map, black & white, and color balance. Their rows are independent, so `scripts/port-kernel.py` now runs each one's outer row loop on every core when the kernels are synced (`ROW_PARALLEL`). Every row is computed exactly as before; the outputs are identical (checksums equal with 1 and 12 threads, and before and after).

| Filter, 6000 × 4000, CPU | Before | Now |
| --- | ---: | ---: |
| Add Noise | 1551 ms | 383 ms |
| Grain | 1048 ms | 285 ms |
| Lens Correction | 473 ms | 153 ms |
| Vignette | 736 ms | 250 ms |
| Tonal Contrast | 2845 ms | 1027 ms |
| Feather mask (CPU fallback) | 866 ms | 279 ms |
| Layer effects (stroke, shadows, glows, overlay) | 2558 ms | 965 ms |

Dither, Scanlines and the Camera RAW pipeline were already split across cores; Color Range (110 ms), Magic Wand (13 ms), Content-Aware Fill and spot healing (110–160 ms for a 120 px area) were left as they are.

## Brushes, Liquify and Smudge

Within one dab every pixel is independent, so the dab's rows run on all cores. The same goes for the stroke's region composite and the end-of-stroke blend. Pixels are identical, checked with checksums. A pointer move at 6000 × 4000:

| Brush | 400 px before → now | 1000 px before → now |
| --- | ---: | ---: |
| Liquify | 20.2 → 4.2 ms | 138 → 29.5 ms |
| Smudge | 12.4 → 3.4 ms | 75.7 → 22.7 ms |
| Soft brush | 4.1 → 1.0 ms | 26.8 → 8.0 ms |
| Hard brush | 3.4 → 0.8 ms | 21.5 → 6.4 ms |

Ending a Liquify or Smudge stroke: 130 → 70–90 ms. The Mac moves these dabs onto Metal (`MetalWarp`, `MetalBrushCoverage`) mostly so that its GPU canvas never re-uploads the document on each pointer move. LX's dabs already touch only the brush's area, and a GPU round trip per pointer move would cost about what the dabs now do on the CPU, so they stay on the CPU.

## Layer effects

The layer's sharp coverage is worked out once per render instead of up to five times, and every remaining loop runs on all cores; the blur and the stroke's morphology were already on the GPU. 3000 × 2000 cut-out with a stroke, drop shadow and both glows: CPU 1956 → 699 ms, GPU mode 578 → 408 ms.

## Remove Background

- **The preview refines a smaller copy**, as the Mac's does (`GuidedMatte.refine(…, limit: 1400)`): the guided filter works on a copy at most 1400 pixels long, with its radius scaled to suit, and the refined mask is scaled back up. OK still refines at full size. 6000 × 4000, Refine 12: 1725 ms before, now 182 ms for each slider move (973 ms at full size, on all cores).
- **The model stays on the CPU.** U²-Net-p takes about 0.25 s on a 320 × 320 copy, about 0.3 s for the whole detection at 24 MP. ONNX Runtime's OpenVINO provider (Intel GPUs) would save at most about 0.2 s once per detection. It would also add very roughly 150–250 MB of OpenVINO runtime to each package. The code to use it stays in place for a build against a provider-enabled ONNX Runtime.

## Verification

- `feature_gpu_tests`:
  - Camera RAW's GPU table against the CPU on every opaque color and every alpha.
  - Add Noise and Grain against the C kernels within a level.
  - The Remove Background preview limit.
- `gpu_document_tests`:
  - Each new live adjustment kind against the CPU canvas, whole and in tiles: noise, grain and motion blur line up across tile edges.
  - A single-layer document that fits where it used to fall back.
- All 31 test suites pass. `scripts/ci-gpu-tests.sh` (lavapipe, forced staging, Khronos validation layer with synchronization checks) passes with 0 validation errors.
- Identical outputs for every multithreaded path: the benchmark's checksums match between 1 and 12 threads, and between the builds before and after.
- In the real app on the Wayland desktop, a 6000 × 4000 document with live Hue/Saturation and Add Noise layers renders on the canvas as one GPU graph (14–37 ms), and a 600 px Liquify stroke shows on the canvas and undoes as "Liquify".

## Next candidates

1. **Smaller previews for the other filters**, as macOS does: it previews every filter except Camera RAW, Noise, Grain, Dither, Scanlines, Content-Aware Fill and Remove Background on a copy at most 2048 pixels long. LX still previews them at full size, which on a 24 MP layer is the main cost of dragging a slider.
2. Camera RAW's Shadows, Effects and Detail kernels on the GPU (the Mac runs them on the CPU too).
3. Remove Background's model on a GPU provider, if the package size is acceptable.
