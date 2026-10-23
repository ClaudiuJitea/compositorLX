# CompositorLX 0.5.5 port audit

Reference: local Compositor `bd76fc9` (2026-10-22), compared with the previous Linux baseline `365b574`. The Swift checkout remains unchanged.

| Upstream feature | Linux implementation |
| --- | --- |
| GIF layers, format 17 | Immutable source bytes, lazy full-frame decode, small thumbnails, editable frame overrides, selected frame, frame identities, delete/duplicate, timing, geometry, project read/write, undo, history memory accounting |
| GIF timeline and export | Selected layer playback; frame strip and delays; composited multi-layer export clock, final-frame holds, GIF delay limits, looping and transparency; giflib encoding through atomic output |
| PDF import | Poppler Qt6, all pages, 300 DPI for new canvases or aspect-fit to an existing canvas, aggregate pixel-budget validation before rendering |
| RGB Split | CPU spectral sampling based on the upstream Metal kernel, linear/radial direction and blur streaks, selection-aware destructive edits, Last Filter, Split to Layers |
| Channel Pack | Isolated layer/group rendering, luminance planes over black, independently chosen RGBA channels, straight-alpha PNG encoding preserving hidden RGB |
| Filter layers, format 16 | Progressive Blur, Bloom / Glow and RGB Split; document-coordinate settings, native rendering, editing, validation, persistence and undo |
| Line styles, format 15 | Solid/Dotted/Dashed/Long Dash and Square/Round, shared drawing geometry, style metadata and version gates |
| Pen pressure | Native Qt tablet events; independent size/opacity toggles after pen detection; interpolated variable coverage; mouse strokes retain their established rendering |
| Font search | Fixed search box above all installed family/style faces, face previews and selected-letter formatting |
| Layer search | Name and type filters, Selected, ancestors and folder contents; filtered results cannot be reordered by ambiguous row positions |
| Automatic tool columns | One column at normal heights, two when tools would overflow, scrolling retained for exceptionally small screens |
| PSD artboards | Parse artb/artd/abdd rectangles; independent documents with shifted layer/mask coordinates and preserved hierarchy |
| View and input polish | Image/Canvas Size preserve zoom, initial fit capped at 100%, non-Latin physical tool keys, PDF/GIF desktop associations |
| Existing behavior improvements | Save pixel totals counted once; GIF edited assets counted in document/history budgets |

## Platform and implementation differences

- Metal/Core Image, AppKit, Quick Look integration and Sparkle are macOS-specific. LX uses Qt, Poppler and CPU rendering. Existing 16-bit, HDR and CMYK support is retained.
- RGB Split follows the spectral weighting and bilinear sampling in upstream MetalDispersion, but uses CPU arithmetic and a bounded sample count. It is not certified pixel-identical to Metal. Large filter previews and GIF exports can take time on the CPU.
- GIF export uses giflib palette quantization (128 colors plus transparency, or 256 without transparency). Transparent GIF is binary alpha; a partially transparent pixel is thresholded. The encoder differs from ImageIO.
- GIF full frames are decoded on demand from the embedded original; very long animations may play slower than their requested delay. Playback schedules the next frame after drawing completes.
- PDF rasterization and fonts depend on Poppler and the installed font set. Password-protected PDFs are rejected with a clear message.
- Pen-pressure behavior is tested with synthetic input/coverage. Physical tablet hardware and a live Wayland pointer session require separate verification.
- Filters remain synchronous CPU operations in their preview panels. The existing asynchronous project save path is retained.

## Verification

On Linux, 2026-10-22, the Release build succeeds and **19/19 CTest suites pass** (76.37 seconds in the clean packaging build). The new suite contains 24 regression cases covering GIF editing, undo, persistence, short and merged export clocks, frame growth, filter-layer rendering and dialog cancellation, channel packing, line styles, PDF pages, PSD artboards, pressure coverage and Qt tablet-event delivery, font search and new-text faces, layer search, adaptive tools, and non-Latin shortcuts. Existing painting, text, deep-color, project-format, PSD, selection, and filter suites also pass.

`git diff --check`, desktop-file validation, AppStream validation, and the upstream C-kernel sync check pass. The Compositor source checkout remains clean. Rendered UI screenshots were inspected and refreshed in `docs/screenshots/`. The workspace executable reports `CompositorLX 0.5.5`.

The build environment is Ubuntu 26.04, Qt 6.10.2, and GCC 15.2.0. Poppler Qt6 and giflib development packages were extracted into a temporary prefix for this build; source files contain no dependency on that path. The Debian package declares its runtime dependencies. The AppImage includes the offscreen Qt plugin for standalone validation.

Build and validation logs are `/tmp/055-packaging-final.log` and `/tmp/055-workspace-build.log`. Passing Linux tests does not establish a macOS runtime or pixel-for-pixel comparison.

The AppImage passes its version and bundled subject-model checks with `LD_LIBRARY_PATH` and `QT_PLUGIN_PATH` removed. Extracted binary inspection finds no missing libraries or temporary development-prefix dependency. AppImage and Debian checksums verify; packaged dependency notices and model assets are present. Both binaries require glibc 2.43 or newer. Release assets are `CompositorLX-0.5.5-x86_64.AppImage`, `compositorlx_0.5.5_amd64.deb`, and `SHA256SUMS`.

## Subsequent GPU work

The CPU-rendering statements above describe the released 0.5.5 build. Current development adds optional Vulkan RGB Split acceleration with automatic CPU fallback. See [the GPU implementation and validation notes](RGB_SPLIT_GPU.md) for scope, quality tolerances, and measurements.
