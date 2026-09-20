# Mac → Linux feature parity audit

Date: 2026-09-20. Reference: the local `../../Compositor` source snapshot.

## Verdict

**CompositorLX now implements the main Mac editing feature families and every concrete gap found in this audit.** Full behavioral parity still requires completion of the Mac test migration and cross-platform golden-image runs.

## Implementation update

The gaps originally recorded below were implemented after the audit:

- live-mask source deletion now offers Bake and Delete, Remove Links and Delete, or Cancel;
- selection overlays trace actual mask edges with animated marching ants;
- HEIC/HEIF has a libheif decoder path, with the Qt decoder retained as a fallback (`libheif-dev` is a documented build dependency);
- the layer panel represents arbitrary nesting, collapses folders, renames inline, and supports visibility swipes as one undo step;
- URL-less image MIME drops are accepted by the canvas and project tabs;
- editable zoom covers 0.1–3200%;
- the eyedropper has a configurable before/after sample ring;
- Help → Check for Updates queries the project’s latest GitHub release.

Focused tests were added for live-mask baking, nested/collapsed layer rows, inline rename signaling, the full zoom range, and URL-less image drops. The historical table is retained as the record of what the audit found before implementation.

This is a source-level feature/workflow audit plus execution of the current Linux build and tests. It is not a certification that every Swift function has an equivalent, nor a side-by-side Mac/Linux visual comparison. Mac frameworks and private helpers need functional equivalents, not necessarily one-to-one ports. The Mac application/tests could not be executed in this Linux environment.

## Original missing or partial behavior (resolved)

Paths below are relative to this directory; line numbers describe the audited snapshot.

| Priority | Feature | Mac behavior and reference | LX finding and reference |
|---|---|---|---|
| High | Deleting a live/clipping-mask source | Offers **Bake and Delete**, **Cancel**, or **Remove Links and Delete**, preserving appearance when baking. `../../Compositor/Compositor/Document/LiveLayerMask.swift:101` | `../src/core/EditorSession.cpp:2026` deletes the source and resets dependent `maskSourceId` values. There is no equivalent bake/delete choice in the window command. Hidden dependent pixels can become visible. |
| High | Accurate selection outline | Draws the actual selection path, including nonrectangular boundaries, with marching ants. `../../Compositor/Compositor/Rendering/TransformOverlay.swift:130` | `../src/ui/CanvasWidget.cpp:445` shades the selection mask but draws only `region.boundingRect()`. Ellipse/lasso/wand selections work as masks, but their displayed dashed outline is a rectangle. |
| High | HEIC/HEIF import on this installation | Image import supports HEIC through the Mac image stack; see `../../Compositor/Compositor/IO/ImageImporter.swift`. | The file chooser advertises HEIC/HEIF, but import delegates to `QImageReader` (`../src/ui/MainWindow.cpp:2677`). A runtime query of installed Qt image readers returned neither format. No dedicated HEIF decoder is wired into `../CMakeLists.txt`. TIFF, PNG, and JPEG readers are present. This is a deployment-dependent capability gap, not proof that Qt can never support HEIF. |
| Medium | Collapsible folders and full visual nesting | Tracks collapsed folders and computes hierarchical rows/depth. `../../Compositor/Compositor/Document/LayerGroups.swift:10`, `../../Compositor/Compositor/UI/NativeLayerList.swift:530` | Group relationships and operations exist, but `../src/ui/LayerListModel.cpp:76` exposes every layer as a flat row; its nesting role is only a boolean (`:107`). No collapse state or disclosure action exists. Deep nesting is not represented with corresponding depth. |
| Medium | Non-file image drag/drop | Resolves image data/file promises for screenshot thumbnails and images dragged from other apps. `../../Compositor/Compositor/IO/ImageFileDrop.swift:20` | `../src/ui/MainWindow.cpp:3250` accepts URL drops and imports local files. The tab drop handler at `:3321` similarly accepts layer payloads or URLs. There is no image-MIME payload import path for drops without local file URLs. Clipboard paste is implemented separately. |
| Medium | Numeric zoom entry and minimum zoom | Editable percentage and arrow stepping; range **0.1–3200%**. `../../Compositor/Compositor/UI/NavigationToolHeader.swift:14`, `../../Compositor/Compositor/Rendering/CanvasViewport.swift:11` | Zoom display is a `QLabel` (`../src/ui/MainWindow.cpp:1090`), with no numeric entry. `../src/ui/CanvasWidget.cpp:293` clamps zoom to **1–3200%**. Fit, actual pixels, zoom tool, and zoom-in/out controls exist. |
| Medium | Visibility swipe | Press an eye and drag through rows to apply one visibility state in a single undo transaction. `../../Compositor/Compositor/UI/NativeLayerList.swift:862` | LX handles individual visibility toggles (`../src/ui/MainWindow.cpp:1097`). Its layer-list mouse-move filter (`:3306`) changes cursors only; it does not implement the swipe interaction. |
| Low | Inline layer renaming | Edits the layer name inside its row. `../../Compositor/Compositor/UI/NativeLayerList.swift:609` | Rename works through a `QInputDialog` (`../src/ui/MainWindow.cpp:1721`). The model has no editable-name flag/setData path; inline renaming is absent. |
| Low | Eyedropper sample comparison ring | Optional ring compares the pre-drag color with the current sample. `../../Compositor/Compositor/Rendering/SampleRingOverlay.swift`, `../../Compositor/Compositor/ContentView.swift:47` | Eyedropper and color picking exist, but no corresponding ring overlay or toggle was found in LX's canvas/options implementation. |
| Platform | In-app update checking | Sparkle-backed **Check for Updates…** command. `../../Compositor/Compositor/CompositorApp.swift:93` | No in-app updater or update-check command. LX has Linux package installation instead; whether package management should replace this is a product decision. |

## Main feature coverage found in source

“Present” means an implementation and relevant UI wiring were found. It does not assert pixel-identical results or exhaustive edge-case coverage.

| Feature family | LX coverage | Qualifications |
|---|---|---|
| Projects and workspace | New canvas, import, save/save-as, tabs, cross-project layer copying, dirty-close handling, recovery, v1–v7 project reader/writer | Cross-platform round trips still need Mac-produced fixtures and execution on both platforms. |
| Layers | Create/delete/duplicate, rename, reorder, grouping, visibility, opacity, blend modes, merge down/selection/group, flatten | Folder display, rename interaction, visibility swipe, and source-mask deletion gaps above. |
| Masks and clipping | Raster/folder masks, add/from-selection, toggle/delete/invert, linking/unlinking, independent transforms, load pixels/mask as selection, clipping relationships | Source-mask deletion lacks the bake option. |
| Transforms | Move/scale/rotate/flip, multi-layer/folder transforms, free-corner distortion, numeric editing, snapping, staged commit/cancel | Interaction parity has tests but is not exhaustively demonstrated. |
| Selections and clipboard | Rectangle/ellipse, freehand/polygonal lasso, wand, add/subtract, invert/expand/contract, outline/pixel movement, fill/clear, floating selection, copy/cut/paste/copy merged | Actual selection boundary display is incomplete. |
| Painting | Brush/eraser, size/hardness/opacity, stroke smoothing, straight lines, mask/selection-aware painting, undo/cancel | Full Mac tiled-raster architecture is not ported; see implementation differences below. |
| Retouching | Clone aligned/unaligned and layer/composite sampling, healing modes, blur, smudge, liquify, content-aware fill | Shared C kernels improve consistency, but full behavioral test migration remains incomplete. |
| Gradient and shape | Linear/radial gradients, color/transparency variants, reverse/opacity, editable preview; rectangle/rounded rectangle/ellipse | LX additionally has editable text functionality, which is not in the supplied Mac tool enumeration. |
| Adjustments | Hue/Saturation, Levels/Auto, Curves, Exposure, Gradient Map, Grain; destructive edits and editable adjustment layers | Controls and live preview paths exist; exact cross-platform render matching is not established. |
| Filters | Invert, Gaussian/motion blur, noise, lens correction, remove background | Remove Background uses a different model/backend. |
| Canvas | Crop, canvas/image resize, resolution controls, canvas/layer flips, pan/zoom, pixel grid | Numeric zoom entry and 0.1–1% range are missing. |
| Export | PNG/JPEG, JPEG quality/encoded preview, metadata, atomic writes | Existing Linux export tests pass; no Mac/Linux image comparison was performed. |
| Color and shortcuts | Foreground/background picker, eyedropper, swap/reset, tool/edit/image/layer/view shortcuts | Sample ring is missing; shortcut presence is not a guarantee of identical platform-specific key routing. |

Primary implementation locations: `../src/core/EditorSession.{h,cpp}`, `../src/ui/MainWindow.cpp`, `../src/ui/CanvasWidget.cpp`, `../src/rendering/RasterOperations.cpp`, `../src/rendering/LayerRenderer.cpp`, and `../src/io/`.

## Implemented differently or not yet verified

- **Background removal:** Mac uses `VNGenerateForegroundInstanceMaskRequest` (`../../Compositor/Compositor/Document/SubjectRemoval.swift:31`); LX uses local ONNX Runtime/U²-Net (`../src/rendering/SubjectRemoval.cpp:24`). The feature is present, but equal subject masks/quality cannot be assumed.
- **Tiling and rendering:** Mac has `RasterSnapshot`, `TiledLayerRenderer`, and a Metal brush-coverage path. LX has full `QImage` layer buffers, dirty-region painting, background flattened recomposition, and a downsample cache. `../src/rendering/LayerRenderer.cpp:204` allocates a full-canvas output. The tracker’s “tiling” checkbox should not be read as an equivalent port of the Mac tiled-raster implementation. Large-document memory/performance parity is unverified.
- **Golden rendering and compatibility fixtures:** `PORTING_PLAN.md` still leaves Mac-generated golden rendering fixtures unchecked. Passing Linux-only tests does not establish identical blend/filter/downsample behavior or bidirectional compatibility for every editable feature.
- **Tests and desktop integration:** `PORTING.md` explicitly leaves migration of all applicable Mac tests and broader end-to-end validation unfinished. There are now full-window offscreen tests, so portions of the planning documentation are also stale. Actual X11/Wayland/HiDPI drag/drop and external-application workflows were not exercised in this audit.

## Validation performed

1. `cmake --build CompositorLX/build -j 2` — succeeded; current build was up to date.
2. `ctest --test-dir CompositorLX/build --output-on-failure` — **5/5 targets passed**, 1.41 seconds:
   - `compositor_tests`: 11 QtTest passes.
   - `editor_session_tests`: 61 QtTest passes.
   - `canvas_widget_tests`: 19 QtTest passes.
   - `main_window_tests`: 39 QtTest passes.
   - `subject_model_cli_check`: passed.
3. Qt image-reader capability probe, compiled against the installed Qt: `bmp cur gif icns ico jfif jpeg jpg mng pbm pgm png ppm svg svgz tga tif tiff wbmp webp xbm xpm`. **HEIC/HEIF absent.**

QtTest totals include initialization/cleanup entries and are not counts of matched Mac behaviors. The optional `fourKBrushPerformanceWhenRequested` test returns without benchmarking unless `BRUSH_BENCHMARK=1`; it was not enabled here. Existing release archives/packages were not rebuilt or certified by this audit.

## Suggested completion order

1. Preserve clipping-mask appearance on source deletion; draw accurate selection outlines; provide and verify HEIC/HEIF decoding.
2. Complete layer-list hierarchy/collapse, inline rename, and visibility swipe.
3. Implement non-file image drops, numeric zoom/full range, and the sample ring.
4. Add focused regression coverage for these gaps, then finish applicable Mac behavioral-test mapping and cross-platform golden fixtures.
5. Validate installed builds on real Linux desktops and measure large-document memory/latency before marking rendering and application parity complete.

No application code was changed during this audit.
