# Lens Effects on the GPU

In the refreshed 0.5.8 assets, all five Lens Effects run as Vulkan compute: Anamorphic Flare, Starburst, Mist, Prism and Bokeh. On the Mac, Core Image runs the same filter chains on the GPU. Before this, only Bokeh's disc gather (`GpuOp::Bokeh`, the MetalBokeh kernel) used the GPU, and everything else ran on the CPU with OpenMP. The CPU path stays as the reference and the fallback, and it got faster too. The application version is still **0.5.8**.

## What runs where

| Step | Where |
| --- | --- |
| Brightness masks (luma through CIToneCurve), the Threshold > 95 source mask | CPU (OpenMP); they need all three channels at once. The Threshold > 95 mask's wide blur runs on the GPU. |
| Linear light, gamma, tone curves (softClip), the flare's lift | GPU (`LensPoint`) |
| Gaussian blurs: direct kernel up to σ 8, three boxes up to σ 16, a reduced copy beyond (see below) | GPU (`Gaussian`, `BoxBlur`, `LensReduce`, `LensResample`) |
| Streaks: the row maximum (`spread`), the squeeze, the blur and the stretch back | GPU (`MorphologyStep`, `LensSqueeze`, `LensStretch`) |
| Starburst's turn into each spoke's frame and back | GPU (`LensResample`, `LensStreakBack`) |
| Prism's copies, their blur and the lighten | GPU (`LensPrism`, `LensCombine`) |
| Bokeh's smaller copy, the discs (all three channels in one MetalBokeh kernel run) and the way back | GPU (`LensResample`, `Bokeh`) |

Each effect runs one color channel at a time, on single-channel float planes (`Red32`), as one graph per channel: the planes go up once and the result comes back once. Working one channel at a time keeps a 24-megapixel photo within the default 512 MiB GPU working memory. If anything fails (no Vulkan device, a graph over the memory budget, or the GPU busy while the GUI thread is waiting), the whole effect falls back to the CPU, which draws the same picture. **Automatic** uses the GPU for layers of 512 × 512 pixels and larger, as for the other filters. **View › Rendering…** lists the Lens graphs (`LensPoint`, `LensSqueeze`, …) among the recent GPU graphs.

## What changed in the shared renderer

- **New operations**: `LensPoint`, `LensCombine`, `LensSqueeze`, `LensStretch`, `LensStreakBack`, `LensResample`, `LensPrism` and `LensReduce`, plus a `clear` (outside is empty) mode for `Gaussian`. They come from the generated contract in `scripts/compile-rendering-shader.py`, like every other operation.
- **In-place region passes** (`GpuGraph::update`). A Lens pass can write into its input over a rectangle, leaving the rest as it was. Sums are accumulated in place, and each Starburst band touches only its own rows of the packed ray plane. The executor refuses an update whose input is read again later, so that case falls back to the CPU instead of drawing something wrong.
- **Allocator**. Free neighboring slots merge, a larger free slot is split, and the free slot at the end grows, so small passes no longer strand large slots. Sources marked **transient** (`GpuGraph::transient`) give their memory to later passes once these have read them, and are uploaded again next time. All other sources stay resident between graphs, as the canvas relies on.
- **Limits**: up to 2048 passes and 4096 nodes per graph (was 512 and 2048).

## Wide blurs on a smaller copy

A Gaussian wider than σ 16 now runs on a smaller copy and is scaled back up, the same idea as MetalBokeh's discs. Each pixel of the copy averages a block of `ceil(σ / 7)` pixels on a side, the copy is blurred by the rest of σ (the block average and the bilinear scaling account for the rest of the spread), and bilinear scaling brings it back. Its cost then stays the same however wide the blur. The CPU and the GPU both do it, with the same arithmetic.

On a 6000 × 4000 night photo, this changes the CPU result of the shipped 0.5.8 as follows:
- Anamorphic Flare: at most 3 levels, on 0.05% of channel values.
- Mist: at most 2 levels.
- Starburst, Prism and Bokeh: unchanged.

## Measurements

Measured on 2026-10-24 on an Intel Core i7-1265U (12 threads) with Intel Iris Xe (ADL GT2), Mesa 26.0, Release build, using `lens_benchmark` (`tools/LensBenchmark.cpp`). The input is a 6000 × 4000 8-bit night scene with nine small lights, and each figure is the median of three end-to-end `apply()` calls, conversions included. On this integrated GPU, the CPU and the GPU share the same memory bandwidth. A discrete GPU should gain more, but that has not been measured.

| Effect | Before (0.5.8, CPU) | Now, CPU | Now, Vulkan | Vulkan vs before |
| --- | ---: | ---: | ---: | ---: |
| Anamorphic Flare | 4131 ms | 4225 ms | 1594 ms | 2.6× |
| Anamorphic Flare, Threshold 98 | 4814 ms | 4714 ms | 2139 ms | 2.3× |
| Starburst | 3517 ms | 4012 ms | 1630 ms | 2.2× |
| Starburst, 14 points, length 30 | 5767 ms | 6729 ms | 2675 ms | 2.2× |
| Mist | 3864 ms | 2343 ms | 1109 ms | 3.5× |
| Mist, size 100 | 3936 ms | 2351 ms | 1054 ms | 3.7× |
| Prism | 2512 ms | 2362 ms | 922 ms | 2.7× |
| Prism, blur 50 | 7254 ms | 4212 ms | 1264 ms | 5.7× |
| Bokeh | 3180 ms (2159 ms with the Vulkan kernel) | 2994 ms | 1206 ms | 2.6× (1.8×) |

The two timing columns were measured at different points in one long session on a laptop, so differences of about 15% between them are thermal noise; the CPU code for Starburst did not change. The "Now, CPU" column includes the parallel conversions and the reduced-copy wide blur.

How the GPU time was brought down, in order: one chain per channel instead of a plane at a time; wide blurs on a smaller copy; edge-padded box blurs that match the CPU's; in-place sums; Starburst computing only the rows of the turned frame that can carry a ray, which the CPU also skips; one shared table of row maxima for all four rays; and all four rays turned back in one pass from a packed plane.

## Verification

- `lens_effects_gpu_tests`: CPU against Vulkan for 11 settings × 8-bit, 16-bit and HDR layers on a 900 × 600 scene, large enough for the reduced-copy blurs and banded Starburst. Tolerances: one level for 8-bit, 2·10⁻³ for 16-bit, and 4·10⁻³ relative for HDR (stored as half floats). Also covered: region updates (and the refusal of an unsafe one), a transient source uploaded again after its memory was reused, and Automatic leaving small pictures to the CPU. `lens_effects_no_driver_tests` checks that every effect falls back to the exact CPU result without a Vulkan driver.
- `scripts/ci-gpu-tests.sh` now runs the Lens suite too: on lavapipe, with forced staging, and under the Khronos validation layer with synchronization checks (0 errors).
- The full `ctest` suite passes (30 tests).
- In the real app on the Wayland desktop, each effect was previewed and applied through **Filter › Lens Effects…** on a 6000 × 4000 document. The previews and results came from GPU graphs.

## Next GPU candidates

CPU-only paths found while doing this, by expected gain:

1. **Full-resolution canvas compositing of large documents.** A 6000 × 4000 document's composite graph (Clear, Composite, Transform at RGBA32F) needs more than 512 MiB, so the canvas falls back to the CPU at that size. Compact formats or tiling for that path would keep it on the GPU.
2. **Tonal Contrast, Vignette, Add Noise, Scanlines, Dither**: per-pixel or small-neighborhood filters that map directly onto the existing operations.
3. **Lens Correction** (the Lensfun remap): a `LensResample`-style pass driven by a displacement table.
4. **Healing and cloning**: interactive brushes that would benefit from GPU blending of dabs.
5. **Remove Background**: ONNX Runtime with a GPU execution provider (OpenVINO for Intel, CUDA for NVIDIA). The code exists but is dormant, because the released packages bundle a CPU-only runtime.
6. **The Lens masks**: luma and tone curve on the GPU from the three uploaded channels would save about 80 ms per effect, but would need a three-input operation.
