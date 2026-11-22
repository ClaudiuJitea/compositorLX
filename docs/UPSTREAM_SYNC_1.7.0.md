# CompositorLX 0.7.0 port audit

Reference: upstream Compositor `9e6fe04` ("Publish update feed for Compositor 1.7.0", 2026-11-21); `8391036` is the 1.7.0 version bump. Previous Linux baseline: `c9c7531` (1.5.8). The range has 173 commits and covers every macOS release from 1.5.9 to 1.7.0: 1.5.9–1.5.12, 1.6.0–1.6.15 and 1.7.0. CompositorLX goes from 0.5.8 straight to 0.7.0.

The local `Compositor/` folder is a plain copy with no `.git`, so the range was read from a fresh clone of the upstream repository at `9e6fe04`. Every commit's diff was read.

## What changed upstream

| Mac commits | Change | Classification |
| --- | --- | --- |
| `7b5ba97`, `338a609`, `500e133` | Vignette, Lens Effects and Tonal Contrast as adjustment layers (format 18); the Filter menu lists the layer-capable filters first | In scope |
| `b0f0b2b` | Sharpen (unsharp mask, Amount and Radius) as a filter and an adjustment layer (format 19) | In scope (Vulkan + CPU) |
| `6d49a23` | Prism's Fringe (format 20) | In scope |
| `c1c5356`, `2c98c1b`, `8926ea8`, `19081ff`, `bc679b0` | Reduce Noise: the filter, its non-local means on the GPU, and the adjustment layer (format 21) | In scope (shared C kernel; Vulkan NLM). `19081ff` is a macOS 26 Core Image fix |
| `3adce92`, `82a9aeb`, `8f8c721`, `503bda6`, `6dea5ac`, `8997bfe`, `e63b825`, `72b71b2`, `8a9a94e`, `a587dcc`, `a38cb04`, `3500c15` | Shape editing: fill, inside stroke and corner radius on a placed rectangle or ellipse (format 22); a selected line's color, width, dashes and caps; the stroke draw order; fill colour rules; the tool opens on the selected shape's kind; folder-top placement | In scope (see Linux differences for the previews) |
| `b9a0ed8`, `97c3b3f` | Filter › Rim Light | In scope (Vulkan + CPU; U2Net as the subject) |
| `1405545` … `851edb0`, `5c3489e` (19 commits) | Filter › Volumetric Light with DepthMini depth, Depth Contrast, Keep Subject Clear, Extract Depth | In scope (DepthMini converted to ONNX; beams on Vulkan) |
| `4ae432b`, `93a8aeb`, `002737a`, `4a73a1d` | Align and Distribute (Move bar popup, Layer menu) | In scope |
| `36ced5d`, `a304465`, `f7ad0f9`, `bbe485f` | Custom crop ratios, grouped presets, 2.39:1, Original → Canvas | In scope |
| `122e585`, `70bc6d6`, `9986d12`, `4585446` | Tool headers: icon flip buttons, Expand/Contract/Feather as icon buttons sharing one amount (5 px), Feather adds up | In scope (icons; see differences for the pill look) |
| `1ea5eab`, `fc3c81a`, `720f210`, `2872eb1` | Opacity keys with selection tools; Space moves the Marquee mid-drag; no cursor flicker with Space; F in the Layers panel toggles full screen | In scope |
| `0673a4a` | Export Layer(s) as PNG from the Layers panel | In scope |
| `d693a2a`, `de0a43d`, `5de396b` | Multi-layer Hide/Show, Clipping, Convert to 16/8-Bit; deep layers stay deep through 8-bit edits | In scope |
| `42eb9b2`, `cee4d09`, `46af4f2` | Image Size scrub limits; New Canvas remembers its size; wheel zoom direction | In scope |
| `bb5c8cc`, `9cd704d`, `63f1013`, `820b8fa`, `4f9ed78` | Show Extras (Ctrl+H outside Move); status bar selection size and feather; the undo floor at the document's start | In scope |
| `6793557`, `ead9a0e` | Core Graphics hairline pieces in selection outlines | Mac-only (Qt's outline has no such pieces) |
| `2facf90` | An opened image goes back out as it came: tab name, Export/Save names and folders, Export As remembers its format; Export JPEG removed, Export As on ⇧⌥⌘S (and ⇧⌥⌘W) | In scope |
| `a0cc980` | Export As › PSD (layers, folders, masks, clipping, flattened copy, 16 bits when a layer is) | In scope |
| `79f98de` | RGB Split with blur in two passes | In scope (CPU and Vulkan) |
| `7651033`, `71bb6f0` | Starburst shares its work; Bokeh discs share one weights table | Starburst in scope (spread formula); Bokeh table is a Metal-side optimisation, already per-dispatch in LX |
| `8e6bca6` | Stroke effect reaches as far in every direction (true distance) | In scope (CPU, OpenMP) |
| `1f1d84c` | Brush Anti-alias off (pencil); the Brush bar drops its color swatch | In scope |
| `de6837b`, `1315aae`, `90e7948` | Sizes and the angle beside the pointer; rotate from just outside a corner with a curved pointer | In scope |
| `8fb163e` | View › Show › Distances | In scope (Ctrl for ⌘) |
| `aa22633`, `01e6505`, `dc3a211`, `3990a25`, `af0dcb9`, `aef9f9b`, `8e2659d` | Paste beside a folder; new layers join a clipping group; hiding a base hides its stack, its clipped eye shows it, duplicates make a second stack; Alt-arrow duplicate; Alt-drag always copies | In scope |
| `acfc5eb` | Painting far past a scaled-down layer paints it at its displayed size; GPU-failed strokes carry on on the CPU | In scope (the GPU half has no LX counterpart: the brush is CPU) |
| `6120b8e`, `b9d92e4`, `f7ae463` | Return in transform and Type size fields hands the keys back; Up/Down step panel fields | In scope |
| `a62490d`, `3717575`, `2d44b8d`, `b611179`, `21fecb7` | Edit panels share one position, first centered on the canvas's right edge; no Preview on adjustment layer panels; Levels' Reset beside Channel; separate adjustment and filter footer menus | In scope |
| `60a4f9c`, `096dbe2`, `9943ed8`, `34ac9f0`, `3761be8` | Color Picker: Return confirms, live hex/RGB digits, swatches sample into an open picker; one picker; redesign | Behaviour in scope; visual redesign not ported (see differences) |
| `c214ee6`, `d8f020a`, `f327645`, `a376ade` | Slim slider with a glass knob; one swatch style; tool rail reset/swap | Not ported (Liquid Glass/SwiftUI look; LX keeps Qt's widgets) |
| `547ae0e`, `e8e0594` | Duplicate and Hand pointers | Not ported (LX uses the desktop's own DragCopy and open-hand cursors) |
| `70b7f7c`, `2e8283e`, `98b7ea4`, `3a62cbd`, `394fdd6`, `3c7b84d`, `0f62671` | Layers panel draws its own row drag (half-strength row, gap splitting by height) | Not ported (LX uses Qt's model/view drag; see differences) |
| `6a28e42`, `4445be9`, `9a9f8da`, `903ec9b`, `31961ba` | Canvas caching for adjustment and Lens Effects layers; PNG decode; tiled mask preview; Lens mask live | Checked: `9a9f8da`, `903ec9b`, `31961ba` have no LX counterpart (LX converts layers once at load, paints whole images, renders lens layers exactly); the two caches are listed as candidates below |
| `3940d3b`, `1f84c73` | Deselect after a no-change Magic Wand click | Not needed (LX's wand is synchronous) |
| `9fa59c3`, `9d70c0d`, `603cbb9` | Full screen after an update, Sparkle alert checkbox, no Services menu | Mac-only |
| `a29f7ee`, `55fdb70`, `c041a5a`, `40c5a8f` | CI batching and drag-test hygiene | Mac-only (CI) |
| Version bumps and appcasts (`efc06ff` … `9e6fe04`) | Sparkle | Mac-only |

`scripts/sync-kernels.sh ../Compositor` brings `vendor/compositor-rendering/AdjustPixels.c/.h` in line with Reduce Noise's non-local means (`noise_sigma`, the `adjust_nonlocal_means_gpu` hook). `scripts/port-kernel.py` now translates the kernel's `dispatch_once` into an OpenMP-safe once flag.

## Plan per change

### 1. Formats 18–22 (`7b5ba97`, `b0f0b2b`, `6d49a23`, `8926ea8`, `3adce92`)

- Mac: `ProjectStore.swift` (`ProjectManifest.current = 22`, the reader's gates), `LayerAdjustment.swift`, `ShapeTool.swift`.
- LX: `ProjectWriter::currentVersion = 22`. The reader accepts 1–22 and 27 ("…supports versions 1–22 and 27."). `adjustmentRequiredVersion` gives 18 for Vignette/Lens Effects/Tonal Contrast, 19 for Sharpen, 20 for Prism Fringe and 21 for Reduce Noise; a shape stroke needs 22. A project is written at the lowest version its content needs, as before.
- Tests: every Mac `ProjectStore` gate, every version from 1 to 22 and 27 opening, and the refusal messages.

### 2. New filters and layers

- **Sharpen** (`FilterLayers::sharpen`): CIUnsharpMask on premultiplied linear light with the edges carried on. Vulkan `OP_SHARPEN` with an exact Gaussian; the CPU twin matches to a level. Large pictures go in bands of rows (the blur's reach as margin), so the float planes fit the GPU budget.
- **Reduce Noise** (`FilterLayers::reduceNoise`): `adjust_camera_raw_detail` from the shared kernel. Its non-local means goes to Vulkan (`OP_NONLOCAL_MEANS`, an 8×8 shared-memory tile) in bands of 2²¹/width rows with a 9-row margin.
- **Vignette, Tonal Contrast, Sharpen layers** draw on the document graph (`FilterLayers::appendLayer`). Lens Effects and Reduce Noise layers render through the CPU layer renderer, whose inner work uses the GPU.
- **Prism Fringe**, the **Starburst** spread (`max(thin·2, reach·fraction)·.8`) and Lens Effects measured by the document's side (`LensEffects::measure`).
- **Rim Light** (`RimLight.cpp`): a `chain<B>` template over a CPU backend and a GpuGraph backend (`OP_RIM_MATH`, `OP_RIM_SURFACE`, `OP_RIM_STRAND`), so both draw the same chain. The subject is U2Net's raw mask refined at max(2, side·0.008), cached per image.
- **Volumetric Light** (`VolumetricLight.cpp`): DepthMini (the Mac's Core ML program, converted by `scripts/convert-depth-model.py` to `models/depthmini.onnx`, 3.4 MB, palettized weights kept as uint8 indices + table) run by ONNX Runtime. The guided filter (radius 13, ε 1e-3), the composed depth, the ramp and Depth Contrast's equalization are as macOS's. The beams gather on Vulkan (`OP_VOLUMETRIC_BEAMS`, with an integer hash so the CPU twin matches); the scene, depth and composition run on the CPU with OpenMP, cached across slider moves.
- **RGB Split** with blur in two passes (`OP_RGB_SMEAR` into a copy padded by the spectrum's reach, then `OP_RGB_SPECTRUM`), the CPU twin the same two passes.
- **Stroke effect** (`LayerEffectsRenderer::edgeDistances`): Felzenszwalb–Huttenlocher's exact distance transform, rows then columns, each on all cores.
- **Brush Anti-alias**: the hard dab is now the anti-aliased circle Core Graphics fills (one pixel of rim, as macOS's); Anti-alias off paints pixels whose centers the tip reaches, fully, and forces Hardness to 100.

### 3. Shapes (`3adce92` … `a38cb04`)

- LX: `src/core/ShapeEditing.cpp`. `EditorSession::shapeImage(style, size)` draws a rectangle or ellipse filling its box with its stroke inside. Following `e63b825`, the whole shape goes in the stroke color, then the inside (the shape less a band twice the width) in the fill, so each edge pixel is drawn once.
- The session holds what the bar shows (`displayed*`, `set Displayed*`) and edits the selected live shape as one "Shape Style" step, or one per scrub with `begin/finishShapeStyleEdit`.
- Lines are relaid around their two ends (`relayoutLine`). `drawShape` makes a new shape from the bar, using the next fill (the foreground, or the fill last picked until the foreground changes). Fill on a live shape recolors it ("Fill").
- Swatch pickers preview on the canvas and put the shape back on Cancel. Scaling a live shape redraws its radius and stroke in document pixels.
- UI: Fill and Stroke swatches and the stroke Width on the Shape bar. The canvas drags out the shape filled, with its stroke. Selecting a live shape with the Shape tool switches the tool to that kind.

### 4. Export (`2facf90`, `a0cc980`, `0673a4a`)

- `ImageExporter::startingFormat/exportName/exportFolder`. `EditorSession::sourceImagePath` is set when an image opens into an empty window. Export As remembers its format per document and between launches (`lastExportFormat`). Export JPEG is gone, and Export As takes Ctrl+Alt+Shift+S, with Ctrl+Alt+Shift+W too unless it's given to another command.
- `src/io/PSDWriter.cpp`, a port of `PSDWriter.swift`:
  - the header, resolution and sRGB profile resources
  - layer records bottom to top, with folders as `lsct` dividers and groups, masks (a folder's over the canvas), clipping and Unicode names
  - PackBits at 8 bits, raw at 16
  - a flattened composite
- Each layer is drawn alone on the canvas (with its effects, at full opacity and in Normal) and trimmed to its pixels. Notes ("Text drawn as pixels", "Effects drawn into the pixels", "Adjustment layers left out") show under Export As › PSD, and the size fields are hidden there.

### 5. Layers and editing

- `insertPixelLayer(..., besideFolder)`, `newLayerIndex`, `adoptClipping`, `hiddenClippingStack`, `relinkCopiedClipping`, `duplicateAndNudgeLayers`, and `LayerRenderer::effectivelyVisible`, under which a clipped layer whose base sits below it and is hidden is hidden too. A live mask's source above its target can still be hidden on purpose and shape the target.
- Painting past a scaled-down layer: when growing over the canvas at its own resolution would pass max(min(surface limit, max(16 MP, 4 canvases)), 2× its pixels), the press doesn't grow it. If the stroke then reaches past the layer, it starts again inside the same undo step, on the layer resampled to its displayed size, and its points so far are replayed.
- Align/Distribute (`LayerAlign.cpp`), Convert to 16/8-Bit, multi-layer Hide/Show and Clipping, and layer PNG export follow the Mac's session functions one to one.

### 6. Canvas and UI

- Readouts beside the pointer (`CanvasWidget::dragOutSize`): a shape's or Marquee's size, a line's length, a resized layer's size and a rotated layer's angle; the status bar also shows "Shape …"/"Line …".
- Rotation from just outside any corner (26 screen points), with a curved double-arrow pointer turned to the corner in 16 steps, three quarters of the Mac's first size.
- Distances (`core/LayerDistances.cpp`; View › Show › Distances, off by default): while moving, and with Ctrl held.
- Edit panels share one remembered position, kept by the corner nearer the side they were moved toward; the first opens centered on the canvas's right edge.
- Adjustment layer panels hide Preview; Levels' Reset sits across from Channel. The footer has New Adjustment Layer and New Filter Layer menus (a ring around a checkered disc).
- Color picker: live hex digits (`palette::fromTypedHex`); with the palette picker open, clicking the foreground or background swatch samples it into the picker.
- Brush bar: Anti-alias (Brush and Eraser). Up/Down step panel fields; Shift steps by ten. Return in transform and Type size fields hands the keys back.

## Linux differences

- **Subject detection.** Vision's saliency, person segmentation and body pose have no Linux counterpart. Rim Light and Volumetric Light use U2Net (as Remove Background does), and Volumetric Light's outline is U2Net's firmed mask, so it has no special handling for heads, hair strands or the gaps between limbs. U2Net is always "confident": it outlines something even in an empty room, where Vision would report no subject. The Volumetric beams test therefore stubs the provider to "no subject".
- **GPU split.** On Vulkan: Rim Light's whole chain, Volumetric Light's beams, Sharpen, Reduce Noise's non-local means and RGB Split's two passes. On the CPU, with OpenMP: Volumetric Light's scene, depth and composition (cached), the stroke effect's distance transform, DepthMini through ONNX Runtime's CPU provider, the PSD writer and the brush. Results match the GPU's within the stated tolerances.
- **Shape previews** are drawn as an overlay above the canvas, as LX's always were, so `72b71b2` and `8a9a94e` (where a preview sits within the stack) have nothing to correct here. The preview shows the fill and inside stroke as macOS's does.
- **Alt and Ctrl.** ⌘ becomes Ctrl (Show Extras, Distances' hover) and ⌥ becomes Alt (Alt-arrow duplicate, Alt-drag copy). An Alt-drag in the Layers panel copies; Ctrl, Qt's own copy key on Linux, copies too.
- **Hard brush.** LX's hardness-100 brush used to be aliased. It now carries the one-pixel anti-aliased rim that macOS's has, and the pencil is the aliased one.
- **Look.** The slim slider with its Liquid Glass knob, the redesigned Color Picker field, the one-swatch style, the tool rail's reset/swap glyphs, the pill grouping of icon buttons and the custom pointers are not reproduced. LX keeps Qt's widgets and the desktop's cursors. The behaviour that came with those commits (Return, live digits, swatch sampling, one picker) is ported.
- **Layers panel drags** stay Qt's model/view drag: no half-strength row drawn by the list, and no height-split gaps under folders or over clipping groups. The drop rules (folders, clipping, copies) are LX's existing ones.
- **Expand/Contract/Feather prompts** remain LX's modal panels, centered on the window, rather than taking the shared floating position.
- **Volumetric Light's light color** can't be sampled from the canvas while its picker is open, because the picker is modal.
- **PSD** names go into the Pascal field as ASCII, `?` for the rest; the Unicode `luni` block carries the full name, as Photoshop reads it. LX's own PSD importer reads 8-bit files only, so 16-bit exports round-trip through other apps, not LX.
- **Mac-only.** Sparkle, the update alert, Services, Core Graphics hairline fixes, macOS 26's BGRA kernel fix and CI batching.

## GPU candidates (still on the CPU)

| Priority | Path | Why | Note |
| --- | --- | --- | --- |
| High | Rim Light on pictures past the GPU budget (about 20 s at 24 MP on 12 cores) | The chain's long blurs need wide bands | Run the chain at a reduced working size for the blurs, or tile with shared blurred planes |
| High | Volumetric Light's scene, glows, air and composition | Seconds per preview on a large photo when the cache misses | All point and blur work; maps onto existing ops plus a ramp pass |
| High | Brush and eraser coverage | macOS paints with Metal coverage; the LX brush is CPU | Coverage stamping is embarrassingly parallel; needs a persistent device image |
| Medium | Stroke effect distance transform | 1.7's exact reach is a row pass plus a parabola-envelope column pass | A jump-flood pass would do it in log₂(reach) dispatches |
| Medium | Rim Light's subject refinement (guided filter) | Runs once per image, at the mask's size (2048 px at most) | Box filters already exist on the graph |
| Medium | Adjustment and Lens Effects layer caching (`6a28e42`, `4445be9`) | macOS keeps each layer's result while nothing under it changes | A cache, not a GPU move, but it removes repeated GPU and CPU work |
| Low | DepthMini inference | A 512 px input, once per image | ONNX Runtime's GPU providers are not in the bundle |
| Low | Reduce Noise's other stages | NLM is the bulk | |
| Low | PSD export's per-layer drawing | Export-time only | |

## Measurements

6000 × 4000, Release, Intel Iris Xe (ADL GT2) over Vulkan, 12 threads, median of three end-to-end runs (`feature_benchmark 6000 4000 --upstream170`):

| Case (6000 × 4000) | CPU ms | Vulkan ms | Speed-up | Max difference (8-bit levels) |
|---|---:|---:|---:|---:|
| Sharpen (100, radius 3) | 1362 | 353 | 3.9× | 1 |
| Reduce Noise (50, 50, 50) | 10443 | 5836 | 1.8× | 1 |
| RGB Split, distance 10, blur 20 (two passes) | 3704 | 658 | 5.6× | 3 |
| Rim Light (two lights, given subject) | 19663 | CPU fallback | – | – |
| Volumetric Light beams (half size) | 11158 | 634 | 17.6× | 1 |
| Stroke effect, 24 px reach, square (before) | 197 | – | – | – |
| Stroke effect, 24 px reach, round (1.7) | 187 | – | – | – |

Before and after:

- **Sharpen and RGB Split.** The first run fell back to the CPU at this size: the whole-picture float planes went past the default 512 MiB GPU budget. Both now go to Vulkan in bands of rows, with the reach as margin. Sharpen matches one dispatch to within a level. RGB Split is within the stated tolerance: at most 0.001% of channels more than a level apart, none more than 6, the same as one dispatch against the CPU (`filter_layers_gpu_tests::sharpenInBands`, `rgbSplitInBands`).
- **Rim Light at 24 MP.** It still exceeds the budget, and its blurs (up to 3.5% of the long side, chained) would need bands with over 1,500 rows of margin, so it stays on the CPU there. Smaller pictures use Vulkan (`rimLight` tests). The CPU path went from 32.4 s to 19.7 s with a cache-friendly vertical box pass, giving bit-identical results. This makes it the first GPU candidate above.
- **RGB Split's second pass.** Before the two-pass change, every pixel took spectral × smear samples (21 × 7 = 147 here); it now takes 7 + 21.
- **The exact stroke distance** costs no more than the old square reach.

## Verification

On Linux, 2026-11-22, with Ubuntu 26.04, Qt 6.10.2, GCC 15.2.0, an Intel Iris Xe (Vulkan) and 12 threads:

- **Clean Release build:** 33/33 CTest suites pass, in the development tree and in `packaging/build-packages.sh`'s fresh build.
  - `upstream170_tests` (77 cases) ports the Mac tests of the range: every format gate and version, Sharpen and Reduce Noise, Prism Fringe and Starburst, the menus, the selection amount and Feather, Show Extras and the undo floor, the Marquee's Space move, Align and Distribute, crop ratios, multi-layer commands, layer PNG export, Rim Light and Volumetric Light (with the real U2Net and DepthMini), paste and new-layer placement, clipping stacks, Alt-arrow, export defaults, the round stroke, RGB Split's reach, every shape-editing case, PSD export (layers, folders, masks, clipping, 16 bits, PackBits), the pencil, readouts beside the pointer, corner rotation, Alt-drag copies, panel Preview, the Color Picker's typed hex and swatch sampling, painting past a scaled-down layer and Distances.
  - Older suites were updated where macOS changed behavior: the "1–22" refusal, the undo floor, the crop combo box, Export As on Ctrl+Alt+Shift+S, the shared selection amount and a live mask's source above its target.
- **GPU against CPU:** `filter_layers_gpu_tests` (17 cases), with these tolerances:
  - Sharpen within 1 level, including in bands on a 2100 × 2100 picture.
  - Non-local means within 1 level on the mean and 0.012% of channels past one level, with no seams between bands.
  - Rim Light within 1 level and the Volumetric beams within 1 level.
  - RGB Split in bands: at most 0.001% of channels more than one level apart, none more than 6.
  - The document graph's Vignette, Tonal Contrast and Sharpen layers within 1 level.

  The other GPU suites pass too, 411 cases in all. On the Iris Xe they pass with `COMPOSITOR_REQUIRE_VULKAN_TESTS=1`, none skipped. Through `scripts/ci-gpu-tests.sh` they pass on lavapipe, with and without forced staging. The Khronos validation layer is not installed, so that pass was skipped.
- **The app.** `compositor-lx` started on the Wayland desktop and ran until stopped. `ui_snapshot` renders the Shape bar's Fill, Stroke, Width and Radius and the footer's new filter-layer menu.
- **Packages.** `CompositorLX-0.7.0-x86_64.AppImage` reports 0.7.0, passes the renderer self-check on Vulkan and the subject-model check, and carries `u2netp.onnx` and `depthmini.onnx`. With `LD_LIBRARY_PATH` and `QT_PLUGIN_PATH` removed, offscreen and driven from inside by a preloaded helper, it:
  - opened a `.comp` project
  - applied Filter › Sharpen through its panel
  - exported a PNG through Export PNG's save dialog, which offered the project's name

  `compositorlx_0.7.0_amd64.deb` declares 0.7.0, and `SHA256SUMS` verifies both.
- **Packaging fixes.** `microsoft/onnxruntime`'s 1.23.2 download returned 404 from this machine. `ONNXRUNTIME_ARCHIVE` lets the build use a local copy of the pinned archive, still checked against its SHA-256. The current `appimagetool` refuses a 0700 AppDir, so the script now opens it to a+rX.

Upstream code was compared with the Swift sources of a fresh upstream clone. No macOS runtime or pixel comparison was made.
