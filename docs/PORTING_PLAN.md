# Compositor Linux port plan

## Architectural boundary

`CompositorLX` is an independent Qt 6/C++20 application. The macOS source remains unchanged and acts as the behavior and project-format reference.

- `src/core`: platform-neutral document state and editing commands.
- `src/io`: validated project and image persistence.
- `src/rendering`: compositing, tiles, masks, filters, and raster kernels.
- `src/ui`: Qt Widgets windows, panels, dialogs, and input handling.
- `tests`: unit, file-format, rendering, and interaction tests.

UI classes may call the core, but the core must never depend on widgets. Rendering must be callable without a visible window so export and tests use the same implementation as the canvas.

## Milestones

### 1. Foundation — in progress

- [x] CMake/Ninja Qt 6 application and test targets.
- [x] Validated `.comp` v1–v7 reader.
- [x] PNG assets, layer placement, opacity, common blend modes, raster masks, group visibility.
- [x] Canvas, zoom controls, flat layer list, visibility toggles, file dialog, CLI open, drag-and-drop.
- [ ] Nested layer model and clipping/folder masks.
- [ ] Golden rendering fixtures produced by the macOS implementation.

### 2. File workflow

- New canvas, image import, clipboard import, and multi-project tabs.
- Atomic `.comp` v7 writer compatible with the macOS application.
- PNG/JPEG export, metadata, recent projects, and recovery files.
- HEIC via libheif and TIFF via the Qt image-format plugin.

### 3. Editing core

- Command-based undo/redo with immutable image snapshots.
- Layer add/delete/duplicate/rename/reorder/group and appearance controls.
- Move, resize, rotate, flip, snapping, crop, canvas size, and image size.
- Selections, clipboard operations, and transform-selected-pixels workflow.

### 4. Paint and retouch

- Port the existing C brush, healing, wand, noise, lens, levels, and content-fill kernels.
- Tiled raster snapshots and bounded memory use.
- Brush/eraser, clone stamp, spot healing, blur/smudge, gradient, and shape tools.
- Tablet pressure and correct high-frequency pointer handling on X11 and Wayland.

### 5. Adjustments and filters

- Levels, curves, hue/saturation, exposure, gradient map, grain, and invert.
- Gaussian and motion blur, add noise, lens correction, and content-aware fill.
- Adjustment layers and live previews.
- Portable background removal using ONNX Runtime plus a redistributable model.

### 6. Performance and release

- Profile before adding GPU code; retain CPU fallback for every operation.
- Port the Metal brush coverage path to a portable compute backend where it materially helps.
- AppImage first, then Debian and Flatpak packaging.
- Wayland/X11, HiDPI, color, large-document, crash-recovery, and project-round-trip testing.

## Compatibility rules

- Preserve the Mac document coordinate system and bottom-to-top layer ordering.
- Preserve project UUIDs and filenames byte-for-byte when practical.
- Do not silently flatten unsupported editable features when saving.
- Reject unsafe paths, symlinks, malformed metadata, and documented size-limit violations before replacing the current document.
- Any intentional rendering difference requires a regression fixture and a written rationale.
