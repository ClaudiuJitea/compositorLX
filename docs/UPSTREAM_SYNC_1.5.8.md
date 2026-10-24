# CompositorLX 0.5.8 port audit

Reference: local Compositor `c9c7531` ("Publish update feed for Compositor 1.5.8", 2026-10-23). Previous Linux baseline: `47cb96b` (1.5.6). The range covers two macOS releases, 1.5.7 (build 53) and 1.5.8 (build 54). CompositorLX skips 0.5.7 and goes straight to 0.5.8.

## What changed upstream

| Mac commit | Change | Classification |
| --- | --- | --- |
| `a971eb6`, `64097c9` (merge of #311 in part) | GIF timeline playback runs on the `GIFPreviewPlayback` deadline scheduler, so decoding time never lengthens a frame's saved delay, and frames whose time has passed are skipped | In scope |
| `6078d7e` | GIF frames are read with one decoder kept open (`GIFSource.withReader`), carrying on from the last frame read instead of rebuilding from frame 0 | In scope |
| `a5c900d` | The delay button shows the GIF's own milliseconds (`40–240ms`) instead of *Original*; the menu item reads `40–240 ms · Original` | In scope |
| `6edbaba`, `393bcd7`, `c7ff16d`, `97d28a3` | Filter › Lens Effects: Anamorphic Flare, Starburst, Mist, Prism, Bokeh (Round, Hexagon, Oval, Cat's Eye); Prism's Blur and One Way / Both Ways; Threshold to 100 with source-size weighting past 95 | In scope (Core Image + Metal → C++/OpenMP, Vulkan bokeh) |
| `c51a2f2` | RGB Split's blur smears both ways, centered on the pixel (`u − 0.5`) | In scope (CPU and Vulkan kernel) |
| `b35ac38`, `9bf47cb`, `140b422` | Reader accepts every version 1…17 plus 27; new error wording | In scope: LX already accepted 15 and 16, but never accepted 27. The gate and message now match |
| `9537736`, `8adf15e`, `574ea8d`, `c9c7531` | Version bumps, appcast | Mac-only (Sparkle) |
| README | Lens Effects listed among filters | In scope (README) |

`scripts/sync-kernels.sh ../Compositor` reports **kernels in sync**, because none of the shared `*Pixels.c` kernels changed in the range.

## Plan per change

### 1. GIF playback scheduler (`a971eb6`, `64097c9`)

- Mac: `Document/GIFPreviewPlayback.swift`, `UI/GIFTimeline.swift` (`play(clock:)`), `CompositorTests/GIFPreviewPlaybackTests.swift`.
- LX: new `src/core/GifPlayback.h` (pure scheduler: given the frame durations, the start position and the elapsed time, it returns the frame to show and the next deadline), `src/ui/GifTimeline.cpp` (a `QElapsedTimer` + single-shot `QTimer` drive the scheduler instead of chaining one delay per frame).
- Technique: the Mac loop samples the clock *before* decoding, shows the frame, and sleeps until `elapsed + end − time`. Qt has no injectable `Clock`, so the scheduler is a value type with an explicit `now`. The widget feeds it `QElapsedTimer` milliseconds, and tests feed it a fake clock that advances during "decoding", as the Mac `TestClock` does. Delays come from overrides or the source (or the uniform delay), never from wall time.
- Test: golden sequences from `GIFPreviewPlaybackTests` (`timelineKeepsSavedTimingAndStableFrameIdentities` for both modes, `timelineSkipsExpiredFramesAfterSlowDecode`, `decodingDoesNotAccumulateDelayAtAnyPlaybackSpeed`, `slowDecodingSkipsExpiredIntervalsAndOnceEndsOnTheLastFrame`), plus a paused timeline never decoding.

### 2. One GIF reader kept open (`6078d7e`)

- Mac: `Document/GIFAnimation.swift` (`withReader`), `IO/GIFDecoder.swift`.
- LX: `src/core/GifAnimation.h` / `src/io/GifIO.cpp` (`GifAnimation::frame`). `GifSource` keeps a mutex-guarded `QBuffer` + `QImageReader` and the index and image it last read. A later frame carries on from there, an earlier one restarts, and the same one is returned from the cache.
- Test: frames whose later GIF frames are only the changed patch, read out of order and in order, match a fresh decode pixel for pixel, and copies of the animation share the one reader. Decode speed was not benchmarked separately.

### 3. Delay label shows the GIF's milliseconds (`a5c900d`)

- Mac: `UI/GIFTimeline.swift` (`originalDelays(spaced:)`).
- LX: `src/ui/GifTimeline.cpp`. The button reads `100ms` in uniform mode, otherwise `40–120ms` (or `100ms` when every frame is the same). The Original menu item reads `40–120 ms · Original`. The values are each frame's override or its source delay, ignoring the uniform delay, exactly as `frameDelays[index] ?? duration`.
- Test: `upstream156_tests` expectations updated, and a new test covers mixed and equal delays and the menu text.

### 4. Filter › Lens Effects (`6edbaba`, `393bcd7`, `c7ff16d`, `97d28a3`)

- Mac: `Document/LensEffects.swift`, `Rendering/MetalBokeh.swift`, `Document/Filters.swift`, `UI/FilterSheet.swift`, `CompositorTests/LensEffectsTests.swift`.
- LX: new `src/rendering/LensEffects.{h,cpp}` (settings, `normalized()`, `isIdentity()`, `apply()`), `EditorSession::applyLensEffects` (undo name "Lens Effects", Last Filter), `MainWindow::lensEffectsDialog` (menu item **Lens Effects…** right after RGB Split, as in Mac `FilterKind` order). Bokeh gets a new `Bokeh` GPU op in `shaders/rendering.comp` and `GpuOperations::bokeh`, with the identical CPU loop as the fallback.
- Technique: every Core Image step is reproduced on float RGBA in linear light, with Core Image's own semantics:
  - `CISRGBToneCurveToLinear` / `CILinearToSRGBToneCurve` as the extended sRGB curves.
  - `CIToneCurve` as a natural cubic spline through its five points, applied in gamma 2 as Core Image documents, with `CIColorMatrix` and the Rec. 709 luminance vector.
  - `applyingGaussianBlur` as a separable Gaussian (σ, ±3σ). On an unclamped image the outside reads as clear; with `clampedToExtent()` the edges are repeated.
  - `CIMorphologyRectangleMinimum/Maximum` as van Herk running min/max.
  - The streak trick, with the same cost profile: squeeze along the line, blur evenly, stretch back (box-filtered squeeze for `CILanczosScaleTransform`, bilinear stretch).
  - Rotations as bilinear resampling about the origin.
  - `CILinearDodgeBlendMode` over black as addition, `CILightenBlendMode` as per-channel max, `CISubtractBlendMode` clamped at 0, `CIAreaMaximum` as a max reduction.
  - The final `CIBlendWithAlphaMask` puts the layer's own alpha back.

  Core Image's y axis points up, so angles are mirrored into Qt's y-down grid: Prism at 0° steps right and at 90° steps up, and Starburst's angle turns counterclockwise on screen, as on the Mac. The bokeh kernel is the Metal kernel line for line (shrink to a ≤ 20 px radius, margin `1.7r + 2`, edge-clamped samples, `aperture()` per shape, ring weighting). 16-bit and HDR layers run once on their float pixels, as `runsAtSixteenBits` asks. The Mac Filter sheet's controls, ranges, units, defaults, tooltips and segmented pickers are reproduced in a Qt dialog with a live preview and Preview checkbox. OK with nothing to do (`isIdentity`) leaves no undo step.
- Test: every `LensEffectsTests.swift` case ported with the Mac's pixel coordinates and thresholds (400×240 night scene, 20 px light). Also covered: normalization ranges, menu placement, the dialog's per-effect rows, undo name, Last Filter, a 16-bit layer, and CPU vs Vulkan bokeh agreement on lavapipe (`rgb_split_gpu_tests` gains the bokeh comparison so `ci-gpu-tests.sh` runs it).

### 5. RGB Split blur both ways (`c51a2f2`)

- Mac: `Rendering/MetalDispersion.swift`.
- LX: `src/rendering/RgbSplit.cpp` (CPU) and `shaders/rendering.comp` (Vulkan), `u = (j + jitter)/smear − 0.5`. The shader is regenerated with `scripts/compile-rendering-shader.py` using `glslangValidator` from `.deps`.
- Test: a horizontal blur of a centered bar spreads symmetrically: the left and right energy match, and the centroid stays put. The existing CPU-vs-Vulkan RGB Split comparison keeps both backends equal.

### 6. Project versions (`b35ac38`)

- Mac: `IO/ProjectStore.swift` (`supported = 1...current ∪ {27}`).
- LX: `src/io/ProjectReader.cpp`. Format 27 (the macOS GIF branch's builds) is now accepted, with every `version ≥ n` gate passing, as on the Mac. The message reads "This project uses format version N. This app supports versions 1–17 and 27." The writer still writes 17.
- Test: the Mac `projectsFromEarlierReleasesStillOpen` (rewrite the manifest to 15 and 16 and load), plus 27 accepted and 18 / 26 / 28 rejected with the new message.

## Implementation notes

- **CIToneCurve's gamma 2.** Core Image documents that CIToneCurve "is applied in a perceptual (gamma 2) version of the working space". The LX `ToneCurve` therefore evaluates the spline on √x and squares the result. Every Lens Effects curve goes through it: the brightness mask, the Threshold > 95 cut, and `softClip`. Applying the spline to plain values instead leaves `anamorphicThresholdDecidesWhichLightsFlare` short: 7 levels against the Mac's > 8. With gamma 2, every ported Mac expectation holds.
- **Edges.** An unclamped Core Image blur reads clear outside the image; `clampedToExtent()` repeats the edge. Morphology reads clear outside. Prism's blurred copies repeat the edge of all the copies together. Each of these is kept exactly.
- **Cost.** Streaks fuse the morphology, squeeze and stretch per row and add straight into their target. Morphology is van Herk / Gil-Werman. Empty rows are skipped while turning Starburst's frames and back. Blurs wider than σ 8 use three Kovesi box passes.
- **Format 27** is accepted and then treated as 17 in memory, so saving writes the current format, as the macOS app does.

## Linux differences

- **Lens Effects on the GPU.** In the first 0.5.8 assets, Lens Effects ran on the CPU with OpenMP, except Bokeh's disc gather (`GpuOp::Bokeh`). In the refreshed assets, all five effects run as Vulkan compute, with the CPU as reference and fallback; see [LENS_EFFECTS_GPU.md](LENS_EFFECTS_GPU.md). Core Image runs the whole chain on the GPU in half floats, while LX works in 32-bit floats, so results can differ by a level or so. `CILanczosScaleTransform`'s squeeze is an area average here.
- **Previews.** Like every LX filter panel, Lens Effects previews at the layer's full size; large layers render off the GUI thread. On a 3840 × 2160 or 4000 × 3000 layer (12 threads), a result takes about 1.2–3.5 s, or about 5.4 s for Starburst at 14 points or Bokeh at its smallest size with no GPU. macOS works out its preview from a smaller copy on the GPU.
- **Mac-only.** The appcast and Sparkle build numbers are macOS-only.
- **Pre-existing gap.** Export As › GIF has no animated preview in LX, so the macOS `GIFPreviewPlayback` use there has no counterpart. The scheduler is in `core/GifPlayback.h`, ready for it.

## Verification

On Linux, 2026-10-24, with Ubuntu 26.04, Qt 6.10.2 and GCC 15.2.0:

- **Clean Release build:** 28/28 CTest suites pass. The new `upstream158_tests` has 37 cases (each Mac commit above):
  - every `LensEffectsTests.swift` case, with its coordinates and limits
  - Threshold past 95, the Starburst and Prism angle directions, Prism blur, large bokeh of every shape, normalization, alpha and 16-bit, undo name and Last Filter, the panel and its menu placement
  - RGB Split's centered smear
  - every `GIFPreviewPlaybackTests.swift` scheduling case, with its exact times
  - partial GIF frames read in any order against a fresh decode
  - the delay labels and menu in the window, with playback
  - `projectsFromEarlierReleasesStillOpen`, plus 27 and the refusal message
- **GPU tests.** `rgb_split_gpu_tests` gains 24 CPU-vs-Vulkan Bokeh comparisons. They pass on this machine's Vulkan device and, through `scripts/ci-gpu-tests.sh`, on lavapipe, with and without forced staging. The Khronos validation layer is not installed, so that pass was skipped.
- **Real app on the Wayland desktop.** Opening a 3840 × 2160 night-sky photo, each effect previewed in the panel, and Prism was applied. The Edit menu read "Undo Lens Effects", and "Last Filter: Lens Effects" was enabled. `ui_snapshot` renders the panel and the GIF timeline's `40–120ms` label.
- **Packages.** `packaging/build-packages.sh` builds and tests again (28/28). `CompositorLX-0.5.8-x86_64.AppImage` reports 0.5.8 and passes the renderer self-check. With `LD_LIBRARY_PATH` and `QT_PLUGIN_PATH` removed, it:
  - opened a `.comp` holding that photo
  - applied Lens Effects › Bokeh
  - exported a 3840 × 2160 PNG through Export PNG

  `compositorlx_0.5.8_amd64.deb` declares 0.5.8, and `SHA256SUMS` verifies both.

Upstream code was compared with the local Swift checkout only. No macOS runtime or pixel comparison was made.
