# Compositor 1.5.3 sync

Source reviewed: `../Compositor`, `11d8d7a` (1.4.5) through `365b574` (1.5.3).
The source repository was clean. All changes here target CompositorLX; the Swift repository is unchanged.

## Review and implementation

| Upstream changes | Linux implementation | Verification |
| --- | --- | --- |
| `94e90bd`, `7e14acc`: effects survive edits; Image Size scales styles | Preserve existing layer metadata; scale effect sizes, distances, and blur during image resampling | `upstream15_tests`: effect scaling, undo; existing canvas/text/layer suites |
| `e7e295e`, `9cb57db`: translucent Color Overlay, Inner Glow, Inner Shadow | Recolor on the layer's alpha before compositing over outside effects; retain floating point pixels on deep layers | Translucent overlay and shadow golden values from `ColorOverlayTests.swift` |
| `cb139a1` through `720c121`: command palette, fullscreen, closed hand cursor | Search menu commands and tools with word ranking, Return/arrow navigation, checked/disabled states; Ctrl+F palette, F fullscreen, Esc exits fullscreen | GUI command execution and screenshots; existing canvas and UI suites |
| `6490bb3`, `3e09948`: navigator | Canvas minimap from 300% zoom, viewport outline, click/drag to center | Navigator geometry and middle zoom tests |
| `f745d65`, `b8ff682`: Camera Raw section reset | Per-section reset alongside each section eye, preserving other groups | Section reset and dialog rollback tests |
| `a2718d9`, `42a559b`, `0b4c56c`, `57ab3bb` through `a8630e4`, `735c2cc`: Camera Raw rendering | Updated shared kernels; measured Light/Color/Mixer/Calibration/Grading tables; adaptive Shadows pass; composed color lookup; measured white balance search | Updated upstream Camera Raw golden values, existing complete Camera Raw suite |
| `0415aa4`, `6b81d4a`: Last Filter | Remember committed filter settings, repeat against the current layer, preserve the previous choice after canceled previews | Progressive Blur repeat and cancel/undo regression |
| `9287cf1`, `450af5f`, `f5ae5ba`, `dac3bc6`: export | Export As with size/aspect controls, preview, RGB default, JPEG quality; PNG/JPEG/PDF/EXR/TIFF encoders; atomic output | PDF page size and ICC data, CMYK JPEG decode, TIFF output, GUI dialog checks; existing IO/export suite |
| `b0b4f01` through `a00d8b6`: New Canvas | Pixels/inches/cm/mm, 72/300 DPI, transparent/white/black, defaults reset on New | Conversion numbers, resolution/background undo, GUI creation |
| `754f188`: Rotate Canvas | Nondestructive quarter turns of layers, masks, guides, and selection | Exact upstream 3×2 pixel goldens and undo/redo |
| `06c3a22` through `cdde327`: separate Scanlines | Shared current scanline kernel; thickness, black level, glow, dots by tone, wobble, displacement, smoothness, threshold, color split, color modes | Upstream line/black-level/bead goldens and normalization |
| `5724099`, `70b3e14`: WebP and camera HEIF extensions | WebP accepted by Qt importer; `.hif` follows HEIF decoding; file chooser and desktop associations updated | WebP import; existing image/IO suites |
| `e03923f`, `92eba05`, `fc4c273`: middle mouse and zoom cursors | Ctrl/Meta+middle drag zoom; middle drag layer-list scrolling; zoom cursor follows the latest drag direction | Mouse-driven middle zoom; existing UI/canvas suites |
| `d2b8aaf`, `d0ec2f3`: layer reveal and tabs | Reveal a newly selected layer; blank start tab stays first; lowest available Untitled number | Existing layer/tab/IO suites |
| `b2a0c45`, `2522e3b`, `d3d0ce7`, `e580107`: import/paste/drop | Folder image import; paste creates a canvas; tab-bar empty space accepts new projects; new projects fit dragged layer bounds including rotation | Existing clipboard/IO suites; GUI canvas creation |
| `9a0ad3e`: Gaussian Blur | 5 px default; Preserve Transparency; extend edge colors at canvas boundaries | Alpha preservation, opaque canvas edges, updated numeric scrub test |
| `248d4d4`, `6345432`, `196342c`, `a3a0e29`: RAW | Develop at 16 bits before deterministic dither; LibRaw noise reduction and highlight recovery; Lensfun profile correction before quantization; Importing state; Camera Raw import-and-open button | RAW fixture/develop/cancel tests; existing Camera Raw suite |
| `23913d3`, `39d1257`: healing and Content-Aware Fill | Shared multiscale PatchMatch kernel used by both operations | Existing paint/heal/content-fill suites; no-op healing outside the source remains unchanged |
| `bdcedf2` through `905efc5`: deep pixels and HDR | Native 16-bit PNG layers, half-float HDR/EXR and Radiance import, 8/16 conversion, native deep compositing, dithered display, shared deep color kernels, retained precision while painting/warping/resampling | Adjacent 16-bit values on canvas, Levels precision, untouched painted pixels, HDR highlights/exposure, project round trips and version gates |
| `59025aa`, `896d5c8`: CMYK | LittleCMS profiles, saved profile choice, profile loading, proof colors/gamut warning, CMYK PSD layers, ICC-tagged JPEG/TIFF/PDF export | Profile validation, proof alpha, ink conversion, print encoders, merged CMYK PSD alpha for raw and RLE; existing PSD suite |
| `3ae11cd`: Progressive Blur | Linear/radial adjustable gradient, draggable canvas line, inversion, From/To radius, preview/cancel/repeat | Sharp-to-blurred checkerboard, undo and Last Filter |
| `87e2283`: shortcuts | Empty override disables a shortcut and survives persistence; Delete/Backspace clears a recorded key; new command/proof shortcuts registered | Shortcut regression and existing shortcut/UI suite |
| `9f1d158`, `9b3f48e`: smooth corners | Shared preview/redraw geometry with 60% corner smoothing; disabled at zero radius; format 14 field validation | Geometry fallback, save/read, rejection before v14, version undo |
| `f4b7009`, `cf0ffa1`, `9329ab8`: Camera Raw preview | Existing LX native-resolution preview retained; new sampled C histogram/vectorscope and composed CPU lookup | Existing preview rollback/revision tests; rendering tests |

## Platform differences and verification limits

- Metal, Core Image GPU acceleration, Sparkle updates, Quick Look, and AppKit chrome remain macOS-specific. Linux uses Qt, shared CPU kernels, LittleCMS, OpenEXR, and LibRaw.
- Progressive Blur interpolates a twelve-level Gaussian pyramid. It reproduces the radius gradient and controls; it is not a pixel-exact replacement for Core Image's `CIMaskedVariableBlur`.
- LibRaw supplies noise reduction and highlight recovery; Lensfun supplies distortion/vignetting correction in linear RGB for recognized camera/lens metadata. The RAW checkbox is enabled only when a usable, unambiguous profile is installed. Lensfun profiles and Apple's embedded camera profiles have different coverage; proprietary embedded profiles are not interpreted.
- Export previews provide Fit, 100%, zoom buttons, hand panning, and matte color. Preview rasters are capped at 8192 pixels per side; exports use the requested full dimensions.
- CMYK conversion uses installed Linux ICC profiles and LittleCMS. Output depends on the selected profile; macOS profile availability and ColorSync rounding differ. Profiles are chosen explicitly in Export As, which starts in RGB.
- The CPU Camera Raw panel already rendered the full native layer; Metal acceleration and its platform benchmarks cannot be exercised here.
- Automated verification runs offscreen on Linux. Real macOS ↔ Linux project opening, physical printer output, and desktop mouse/Wayland behavior have not been tested on macOS or a physical printer.
- The follow-up below supersedes the previous rotated-text and small blank-stroke limitations. A long stroke whose bounding rectangle spans a large canvas can still allocate large images; sparse allocation across separate distant dabs would need a tiled document raster.
- Typography depends on installed fonts and Qt shaping. Core Image filter kernels, ColorSync rounding, and the macOS GPU brush integration cannot be reproduced pixel-for-pixel from Linux tests. Feature coverage and numerical/persistence regressions are tested here; full macOS runtime equivalence remains unverified.

## Follow-up toward complete feature coverage

The follow-up audits the outstanding paths against `DeepLayerTests.swift`, `ProgressiveBlurTests.swift`, `SelectionClipboard.swift`, Camera Raw grading controls, `RawImporter.swift`, `ImageExporter.swift`, and the prior LX audit.

| Gap found | Port/fix | Regression evidence in `upstream15_tests` |
| --- | --- | --- |
| Copy/paste and floating selections converted to 8 bits | Native clipboard insertion and floating rasters; native merge surface; corrected Qt transform composition for an offset source | Exact 16-bit values through copy and moved selection, undo |
| Blank depth conversion and gradient quantization | Allocate a clear 16-bit layer; preserve native gradient and composite formats; detach document history safely when converting depth | >900 distinct levels in a 1024-pixel gradient, selection protection, depth undo |
| Spatial HDR filters ran through normalized 8-bit operations | Floating point motion blur and Bloom/Glow; precise small-radius Gaussian convolution | Motion-blur highlight-energy conservation; native blur and existing HDR tests |
| Blur visible only on release | Paint preview from the original pixels through accumulated coverage, using the same native result at commit | Preview equals committed pixels at both depths; Cancel and undo |
| RAW lens control disabled | Lensfun profile lookup with ambiguity checks, distortion/autoscale/vignetting in linear RGB before final dither; tone and resize also before dither | Installed Canon profile alters geometry at native depth, cancellation, unknown-profile no-op |
| Grading always showed four wheels | Three-Way plus Shadows/Midtones/Highlights/Global views with shared settings and reset | Page visibility and control changes |
| Exposure/Grain/B&W/Color Balance/Gradient Map/Dither reopened at defaults | Remember committed numeric, toggle, color, and Dither settings; Cancel preserves prior memory | Exposure commit/cancel/reopen and Dither style/settings |
| Rotated/flipped/scaled text edited untransformed | Qt graphics proxy transforms the actual editable widget, including input mapping, caret, and resize handles | Type and commit rotated/flipped text, preserve placement, clean up proxy; screenshot review |
| Small stroke allocated whole blank canvas | Allocate and grow a local stroke rectangle; preserve coverage coordinates, mask placement, undo/cancel; fix native alpha scans during trimming | 10,000×10,000 blank canvas allocates <16 KB for initial 16-pixel dab; growth in both directions, cancel and undo |
| Cross-project drop point unavailable | Hover activates target tab; canvas drop centers copied native layers on pointer; foreign layer-panel drops use the existing copy/bake path | Drop a rotated 16-bit layer onto another canvas, position/depth/undo |
| HEIF fallback forced 8 bits and lacked real fixtures | Enable required libheif backend, decode 8/10/12/16 bits, normalize samples, honor ICC/NCLX, validate size before decode | Synthetic actual HEVC `.heic` and 10-bit `.hif` fixtures, >400 distinct tones |
| TIFF and merged PNG save could quantize deep images | RGB TIFF uses 16 bits for deep input; package PNGs canonicalized to 16 bits | Exact low bits after TIFF and merged-layer project round trips |
| Linux saves omitted the embedded preview | Optional `QuickLook/Preview.jpg`, white matte, 1024-pixel cap, 50 MP generation limit as upstream | Saved preview decodes, document content still round trips |
| Closing during a floating filter missed its pending revision | Finish the panel transaction, then retry Close and prompt to save | Previously saved document prompts after committing pending Exposure |
| UI omissions | Shared color picker for Fill/Grid/Extension; editable shape badges; HDR layer label; Last Filter name and command state; Scanlines removed from Dither menu | Existing UI suites, Dither menu regression, Last Filter regressions |

Lensfun APIs follow the [Lensfun correction API](https://lensfun.github.io/manual/v0.3.2/group__Correction.html). Install `liblensfun-dev` and `liblensfun-data-v1` alongside the existing development dependencies. LibRaw and libheif are required so a successful build includes their import features. HEVC decoding also needs the distribution's libheif decoder plugin (e.g. `libheif-plugin-libde265`). PQ/HLG NCLX transfer conversion uses Qt 6.8+; older Qt builds still decode the stored high-bit-depth samples.

The two HEIF fixtures are synthetic grayscale ramps generated by `tests/fixtures/generate_heif.cpp` with libheif and an x265 encoder plugin. The encoder is needed only to regenerate fixtures, not for tests or normal import.

## Parity re-audit (commit by commit)

Every upstream commit in the range was checked against the port, including the smaller UI commits the tables above did not list. These were missing or different and are now fixed:

| Upstream | Gap found | Fix | Regression evidence in `upstream15_tests` |
| --- | --- | --- | --- |
| `067fca4`, `f25effa` | The footer's adjustment button came after effects and was labelled "More layer actions"; effects tooltip was old | New adjustment layer (half-filled circle) before Add layer effect; adjustment list in macOS `AdjustmentKind` order | `layersFooterAdjustmentBeforeEffects` |
| `ad4bddf`, `088a5c1`, `720c121` | F only fullscreened the window; panels stayed; Esc left at once, even mid-crop or mid-transform | Toggle Fullscreen is Canvas Only: tab bar, options bar, tool rail, rulers, navigator, Layers panel and status bar hidden, canvas on black, menu shortcuts kept; checked menu item, disabled without a canvas; Esc leaves only when nothing is pending and the editor is the active window | `toggleFullscreenShowsTheCanvasAlone` |
| `07b8b1e`, `94b57f7`, `bba92dc` | Palette used substring ranking, listed tools without their modes or keys, kept disabled rows among enabled ones, and had no Add Layer Mask | Ported `CommandPaletteSearch` tiers and fuzzy scoring; tool modes (Polygonal Lasso, Object Selection, Liquify, Smudge, Ellipse, Line…) with keys; enabled first; Add Layer Mask commands; hidden items and Help skipped | `commandPaletteRanksAndRunsToolModes` |
| `b9186f7`, `905efc5`, `450af5f` | Export As always offered EXR and always wrote deep documents at 16 bits; no direct Export PNG; Export As had no shortcut | EXR only with an HDR layer; 8-bit dithered by default with an opt-in "16 bits per channel" PNG; JPEG quality and last format remembered; scale presets; summary line and JPEG size; project-named file; Export JPEG / Export PNG / Export As (Ctrl+Alt+Shift+W) as on macOS | `exportAsOffersDepthAndExrOnlyWhenTheyKeepSomething`; Mac shortcut table in `io_audit_tests` |
| `3e09948` | Navigator on by default and forgotten on restart | "Navigator (300% and above)", off until turned on, remembered | `navigatorStartsOffAndIsRemembered` |
| `c0a1cd0` | The Hand tool's cursor could reopen mid-pan | Closed for the whole pan | Covered by the existing cursor/canvas suites |

Checked and already matching, or platform-specific: one View › Snap, the Layers panel only graying for lasting states, Export As checkerboard, Don't Save button order (platform order on Linux), the options bar (wraps rather than scrolls on Linux, so it never squeezes), and the Edit menu's macOS text extras and update alert (macOS only). Export As now hands TIFF the 8-bit render, as macOS does (only PNG offers 16 bits there); the TIFF encoder still writes 16 bits when given a deep image. Remaining deliberate difference: CMYK Proof Colors and Gamut Warning default to Ctrl+Y and Ctrl+Shift+Y; they are unassigned on macOS.

## Reproducing the sync and checks

```sh
scripts/sync-kernels.sh ../Compositor --apply
python3 scripts/sync-camera-raw-resources.py ../Compositor
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
OMP_NUM_THREADS=4 ctest --test-dir build --output-on-failure -j 4
```

`port-kernel.py` preserves each upstream dispatch band's body and partition, using OpenMP instead of Apple blocks. The retained `ContentFill.c` is a compatibility adapter to `patch_fill`. Camera Raw table values are unchanged; only their compression changes from LZMA to Qt's zlib container. The helper scripts are deterministic.

## Verified in this workspace

On Linux, 2026-10-18: the final build succeeds and **18/18 CTest suites pass** (37.89 seconds; 44.84 seconds after the parity re-audit, with 5 new GUI regressions). The full-suite run includes all follow-up regressions listed above. Kernel sync reports no drift, Python sync helpers compile, `git diff --check` passes, and the Compositor source checkout remains clean.

The GUI tests execute commands, dialogs, cross-project drops, transformed text editing, and Close with a pending filter transaction. Screenshot review covers Search Commands, Export As, physical-unit canvas creation, and the rotated/flipped text editor; the latter was corrected to remain transparent over the canvas. Tests also decode actual generated 8-bit HEVC and 10-bit HIF files through libheif.

Build/test logs for this follow-up are `/tmp/compositor-parity-release-build.log` and `/tmp/compositor-parity-release-tests.log`. Development packages unavailable system-wide were extracted under `/tmp/compositor-deps/prefix` and supplied through the local CMake cache; no temporary include/library path is hardcoded in the source. Normal builds use the development packages listed in the README.

No macOS runtime comparison, physical-printer run, or real Wayland pointer session was available. The differences above therefore remain explicit; passing Linux tests does not certify complete pixel equivalence. Released as CompositorLX 0.5.3.
