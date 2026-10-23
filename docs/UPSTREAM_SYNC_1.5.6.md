# CompositorLX 0.5.6 port audit

Reference: local Compositor `47cb96b` ("Publish update feed for Compositor 1.5.6", 2026-10-22). Previous Linux baseline: `bd76fc9`. The Swift checkout is unchanged.

## What changed upstream

Between `bd76fc9` and 1.5.6 the only upstream changes are the version number (1.5.6, build 52), the appcast, and two clarifications in `docs/writing-comp-files.md`. There are no application code changes in that range. The 1.5.6 release notes cover work merged before `bd76fc9`, which 0.5.5 already ported:

| 1.5.6 release note | State in 0.5.5 | 0.5.6 change |
| --- | --- | --- |
| Animated GIFs: one layer, frame strip to play, paint, duplicate, delete and retime; Export As GIF | Ported: model, format 17, playback, editing, export | Timeline rebuilt to the final macOS `GIFTimeline` design (below) |
| Search the Layers panel by name, or show one kind of layer or the selected ones | Ported as an always-visible field and a single-choice kind combo | Header and filter rebuilt to match macOS `LayersHeader` and `filteredLayerRows` (below) |
| Tool bar moves into two columns when the window is too short | Ported: automatic columns, scrolling for very small windows | None; it matches `ViewThatFits` |
| Canvas Size keeps the zoom | Ported, but the pan offset was kept as well | Zoom kept and the canvas recentered, as `viewport.recenter()` |

## Parity changes

**GIF timeline** (`src/ui/GifTimeline.*`, `src/core/EditorSessionGif.cpp`)

- The strip is 110 px tall, with a 70 × 40 capsule play/pause button that is disabled for a single frame.
- A delay menu labelled *Original* or *N ms*. Original plays each frame for its own delay, and it never discards per-frame overrides already stored in a project. The presets are 20, 33, 40, 50, 67, 80, 100, 125, 150, 200, 250, 500 and 1000 ms, each shown with its frame rate. Choosing the delay already in use adds no undo step. The undo name is "Change GIF Delays".
- Thumbnails are 72 px boxes, numbered from 1, with an accent outline on the frame shown. The strip scrolls to the selected frame. Each thumbnail is redrawn only when its pixels or its selection change.
- Right-click opens **Duplicate Frame** and **Delete Frame** for the clicked frame (`duplicateGifFrame(index)` / `deleteGifFrame(index)`). Delete is disabled on the last frame.
- A duplicated frame copies only the original frame's delay override, as on macOS. 0.5.5 copied the frame's effective delay.
- Source thumbnails are up to 96 px, scaled down so they total at most 16 MP (`GIFDecoder`). 0.5.5 used a fixed 48 px.

**Layers panel search** (`src/ui/MainWindow.cpp`, `src/ui/LayerListModel.*`)

- The heading's magnifier turns the title into a *Search Layers* field and itself into a close button. Escape clears the text, and on an empty field closes it and returns focus to the canvas. Leaving the field empty also closes it.
- The funnel menu has *All Layers*, then Images, Shapes, Text, Folders and Adjustments, then Selected. Several kinds combine. The funnel stays checked (accent) while a kind is on. Kinds follow macOS `ImageLayer.layerType`; rasterized text counts as an image, and a GIF counts as an image.
- Results follow `filteredLayerRows`: matching layers, plus everything inside a matching folder. 0.5.5 also listed the parent folders of every match; that extra is removed. The search text is trimmed.
- When the filter matches nothing, the list is replaced by "No layers match the filter." and a **Clear All** button.
- The Linux-only *GIF* kind is removed, since macOS has no such filter. GIF layers keep their GIF tag in the list.

**Canvas Size / Image Size** keep the zoom and reset the pan offset, so the resized canvas is centered (`MainWindow::keepZoomRecentered`).

## Not ported

- `docs/writing-comp-files.md` (two clarifications for hand-written `.comp` files) has no Linux counterpart. `docs/project-format.md` already documents format 16 filter-layer settings and format 17 GIF fields.
- The appcast and Sparkle build number are macOS-only.

## Verification

On Linux, 2026-10-23, the Release build succeeds and **27/27 CTest suites pass**. The new `upstream156_tests` suite covers:

- Canvas Size keeping the zoom and recentering through the real dialog
- combined layer kinds and folder contents
- opening, closing, Escape and Clear All in the window
- the delay menu, including the no-op undo and invalid values
- frame actions on a clicked frame and the copied delay
- the timeline controls in the window, with playback

`upstream155_tests` was updated for the new filter API and frame actions. `tests/ui_snapshot` now also renders `layers-search` and `gif-timeline`. Both were inspected in the dark and light themes.

Upstream code was compared with the local Swift checkout only. No macOS runtime or pixel-level comparison was made.

The clean packaging build also passes 27/27 suites. `CompositorLX-0.5.6-x86_64.AppImage` reports `CompositorLX 0.5.6` with `LD_LIBRARY_PATH` and `QT_PLUGIN_PATH` removed, and passes the bundled renderer self-check, falling back to the CPU without a Vulkan driver. `compositorlx_0.5.6_amd64.deb` declares version 0.5.6. `SHA256SUMS` verifies both. As before, the binaries were built on Ubuntu 26.04 (Qt 6.10.2, GCC 15.2.0) and require glibc 2.43 or newer.
