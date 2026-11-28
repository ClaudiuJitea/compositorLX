# CompositorLX 0.7.6 port audit

Reference: upstream Compositor `ff42e5d` ("Publish update feed for Compositor 1.7.6", 2026-11-27); `3fa8b44` is the 1.7.6 version bump. Previous Linux baseline: `9e6fe04` (1.7.0). The range has 23 commits and covers every macOS release from 1.7.1 to 1.7.6. CompositorLX goes from 0.7.2 straight to 0.7.6.

The local `Compositor/` folder is a plain copy with no `.git` (it matches `ff42e5d` file for file, except the appcast), so the range was read from a fresh clone of the upstream repository. Every commit's diff was read, along with the six appcast entries.

The release also polishes LX's own look: every slider is now the slim slider macOS has had since 1.6 (`c214ee6`, not ported in 0.7.0).

## What changed upstream

| Mac commits | Change | Classification |
| --- | --- | --- |
| `a242ce6` | A gradient dragged on a mask placed apart from its layer shows while it's dragged on the GPU canvas | Already right in LX (see 1); pinned by a test |
| `ac4f304`, `01b4e66` | Softer Brush: at low hardness an airbrush feather, (1 − hardness)², widens the tip by up to 35% and the dab spacing from 2.5% to 16% of it | In scope |
| `b469ec1` | Movie layers: MOV and MP4 import, a timeline that plays (with sound and mute) and scrubs on the canvas, Grab Frame, Relink, format 23 | In scope (FFmpeg + Qt Multimedia) |
| `590c386`, `c0d4446` | Movie colors: playback frames keep the paused frame's colors; HDR movies tone-mapped the same way playing, scrubbing and paused | In scope (one decode path for all three) |
| `e234dd4` | Distorting a movie layer turns its frame into plain pixels; moving or scaling keeps the link | In scope |
| `2fc7936`, `475acdd` | Toolbar: solid pills (Fit, 100%, Zoom In, Zoom Out, New Canvas); active tab white 0.19, inactive 45/255 with dimmed text, no outlines | In scope |
| `a44be45` | RAW import's action buttons are capsules | In scope |
| `8c6ebeb` | GIF timeline: 56-point thumbnails over a playback bar (play, delay menu, frame scrubber, "03 / 12"); the Original label keeps the source delays | In scope (the label was already right in LX) |
| Entitlements, Info.plist (`com.apple.security.files.bookmarks.app-scope`, movie document types) | Sandbox bookmarks; Finder's Open With | The document types map to the desktop file's `MimeType`; the entitlement is Mac-only |
| Version bumps and appcasts (`989e3d1` … `ff42e5d`) | Sparkle | Mac-only |

No shared C kernel changed in the range, so `scripts/sync-kernels.sh` had nothing to bring in.

## Plan and what was done, per change

### 1. Gradient on a mask placed apart (`a242ce6`)

- Mac: `EditorCanvas.swift`. The Metal canvas reads a gradient's tiles before anything has filled them, so a mask placed apart showed nothing until Return.
- LX: the gradient tool applies the gradient to the document on every drag move (`MainWindow::previewGradient`), and both canvases render that document, so there are no unfilled tiles to read.
- Test: `upstream176_tests::aGradientOnAMaskPlacedApartShowsWhileDragging` (the mask moved 6 px up and 14 px taller, as in the Mac's `GPUCanvasTests`).

### 2. Soft brush (`ac4f304`, `01b4e66`)

- Mac: `BrushStroke.swift` (`BrushSettings.feather`, `tipDiameter`, `dabSpacing`), `MetalBrushCoverage.swift`, `EditorSession+Brush.swift` (`usesAirbrushFalloff = tool == .brush`).
- LX: `EditorSession::beginBrushStroke` works out the feather for the Brush (paint and erase, on pixels or a mask; not Clone, Healing or Blur). `BrushState` keeps `tipDiameter = diameter × (1 + 0.35 × feather)` and the spacing fraction `0.025 + 0.135 × feather`. The tip radius, the sparse-layer patch and the reach past a scaled-down layer all use the tip; the nominal diameter is kept for a restarted stroke. Anti-alias off (the pencil) has no feather, as on macOS.
- Tests: the Mac's new golden values in `paint_audit_tests` (half strength at x 53, faint at 64, nothing at 68; a 0% dab trims to 28 px; the nominal rim of a stroke still feathered, 25–140). Plus a port of `eightHundredPixelSoftStrokeHasNoPeriodicRidges` (see Linux differences for its tolerance).

### 3. Movie layers (`b469ec1`, `590c386`, `e234dd4`, `c0d4446`)

- Mac: `MovieFramePicker.swift` (`MovieLink`, `MovieFramePicker`, `MovieTimeline`), `EditorSession.swift` (import, `moviePreview`, `displayedAsset(onCanvas:)`), `ProjectStore.swift` (format 23), `EditorCanvas.swift`, `Distort.swift`, `NativeLayerList.swift` (MOV), `ContentView.swift`.
- LX:
  - `src/io/MovieDecoder.{h,cpp}`: the AVAssetImageGenerator counterpart, on FFmpeg (libavformat, libavcodec, libswscale).
    - A frame is the one shown at the time asked for, decoded exactly.
    - The track's end is exclusive, so asking for it gives the last frame (`frameTime`: end − 1/max(timescale, 60000) s).
    - Frames are turned upright by the display matrix, with FFmpeg's own autorotate rules (`appliesPreferredTrackTransform`).
    - The track's declared matrix and range give full-range RGB; untagged video is BT.709 from 720 lines up, else BT.601.
    - Decoding continues from the last frame when the next request is under a second ahead, so playback doesn't seek.
  - HDR (PQ and HLG): BT.2100 to linear light, BT.2020 to BT.709 primaries, reference white (203 nits) at 1, a soft shoulder above 0.8, then sRGB. Still frames, scrub frames and playback frames all come from this one path, which is what `590c386` and `c0d4446` fix on macOS (playback and paused frames now match).
  - `MovieBookmark`: reads the path out of a macOS bookmark (the `book` layout, key `0x1004`), and writes one in the same layout naming the movie.
  - `Layer::movie` (`MovieLink`: bookmark, duration, seconds). The writer emits it at version 23 only. The reader takes it only from a version 23 manifest (not 27), on a layer with an image file, not a folder or a GIF, with a duration above 0 and the seconds within it, as `ProjectStore.load` does.
  - `EditorSession` (`EditorSessionMovie.cpp`):
    - `insertMovie` ("Import Movie"): the first frame, named for the file, checked against the document budget. It raises the format version to 23.
    - `setMovieFrame` ("Select Movie Frame"): only while the layer is active.
    - `grabMovieFrame` ("Grab Movie Frame"): a still just above the movie, named "Name · m:ss.ff", same transform and parent. The movie stays active, and it refuses past the budget or 10,000 layers.
    - `relinkMovie` ("Relink Movie").
    - Distort drops the link when it replaces the layer's pixels (a mask-only distort keeps it).
  - `src/ui/MovieTimeline.{h,cpp}`: the bar under the canvas, as tall as the Layers footer.
    - Controls: play/pause, mute (remembered as `moviePlaybackMuted`), the playhead, and the time as "m:ss.ff" (rounded half to even, as Duration's formatter does).
    - Right-click: Grab Frame (disabled while decoding or playing) and Relink…
    - The error messages are macOS's: "Movie unavailable. Right-click the timeline to relink and play." (and "…and scrub.").
    - Dragging decodes preview frames (at most 1920 px) one at a time, the newest request replacing any waiting.
    - Letting go, an arrow key or a pause settles on an exact full-size frame. A pause keeps the playing frame on screen until then, and playing to the end settles on the last frame.
    - Sound plays through Qt Multimedia (`QMediaPlayer` with its video track off); its clock paces the pictures when it's running.
  - `CanvasWidget::setMoviePreview`: the playing or scrubbed frame replaces the layer's image in each render's snapshot only (`applyPreviewState`, used by the CPU and the Vulkan canvas), so the document, its undo history and its modified state are untouched.
  - Import: Import… lists Movies (`*.mov *.mp4 *.m4v *.qt`); opening one starts a document and makes it the export source. The Layers row reads "64 × 48 px · MOV". The desktop file declares `video/quicktime`, `video/mp4` and `video/x-m4v`.
- Tests (`upstream176_tests`):
  - The Mac's `capturesTheSelectedFrameUprightIncludingAtTheEnd` on its own fixture (`frame-picker-portrait.mov`): red at 0.99 s, blue at 1.0 and 2.0 s, 48 × 80.
  - Rotations 90/180/270 and a mirror, against where ffmpeg's autorotate puts a marker.
  - PQ tone mapping, and the same color at preview size.
  - `linkedMovieRoundTripsWithItsStill`, and every reader gate.
  - Select, Grab, Relink and their undo names; Distort.
  - The MOV tag; scrub preview then settle; play, pause and play to the end; a missing movie then Relink; the time format.

### 4. Toolbar and tabs (`2fc7936`, `475acdd`), RAW buttons (`a44be45`)

- Mac: `ProjectTabs.swift` (`EditorToolbarPillStyle`), `ContentView.swift`, `RawDevelopSheet.swift`.
- LX: new theme tokens `pill` (white 0.19 on dark), `pillHover`, `pillPressed` (0.25), `tabInactive` (45/255) and `tabInactiveText`, with light-scheme counterparts.
  - Fit, 100%, Zoom In and Zoom Out are 28-pixel capsules six apart, in macOS's order (Zoom In before Zoom Out; LX had them the other way round). New Canvas is a 28-pixel circle.
  - Tabs are capsules with no outline.
  - The RAW dialog's button box takes `roundedControls`.
- Test: `toolbarPillsInMacOrder`; the screenshots below.

### 5. GIF timeline (`8c6ebeb`)

- Mac: `GIFTimeline.swift`, `LayersPanel.swift` (`LayersFooter.height`), `EditorSession.swift` (`gifTimelineFrames` from the source frames).
- LX: `GifTimeline` is now a strip of 56-pixel thumbnails (88 high) over a playback bar as tall as the Layers footer, holding play, the delay menu ("100 ms", now spaced), a frame scrubber and "03 / 12". Frame requests are shown one at a time, the newest winning (`requestFrame`), and play is disabled while one is pending. The Original label already read the GIF's own delays under a uniform delay; a test now pins it.
- Tests: `gifTimelineHasAScrubberAndFrameCounter`, `originalDelaysStayTheGifsOwnUnderAUniformDelay`; earlier tests updated for the spaced label, the new height and the deferred selection.

### 6. Slim sliders (LX polish; mac `c214ee6`, `SliderTrackView`)

- `SliderJumpStyle` now paints every slider:
  - a 3-pixel track (4 for color tracks) with the accent filling it to the knob's center, ramping in over the last 6 pixels at each end (`SliderGeometry.fillWidth`)
  - a round light knob with a soft shadow that grows by a fifth while held, and the whole slider at 45% when disabled
- The knob travels half a knob in from each end. The stylesheet's `QSlider` rules are gone. Colored tracks (Camera Raw, Hue/Saturation, Color Balance) pass their colors as a property, and the style paints them.
- The knob is round rather than macOS's 10 × 18 pill, by request; the rest of the geometry is macOS's.
- Test: `slidersAreSlimWithARoundKnob`.

## Linux differences

- **Movie decoding.** FFmpeg stands in for AVFoundation and Qt Multimedia for AVPlayer's sound. Pictures are always decoded by LX itself, and Qt Multimedia only gives the sound and its clock. Without Qt Multimedia (a build option), movies play silently on an internal clock and the mute button is hidden.
- **Bookmarks.** macOS stores a security-scoped bookmark. LX reads the path from it, but a project saved on a Mac names a `/Users/…` path, so on Linux it usually needs Relink. LX writes a bookmark naming the path without a sandbox scope, which macOS can't open, so a movie linked on Linux asks to be relinked on a Mac. Either way the frame is in the project, so it opens and renders everywhere.
- **HDR.** Apple's HDR-to-SDR conversion isn't public. LX uses BT.2100's curves with a soft shoulder; the frames match each other, but not macOS's pixel for pixel.
- **Undo name.** Importing a movie is "Import Movie" (macOS says "Import" for every import; LX already says "Import Image" and "Import GIF").
- **Soft brush ridges.** macOS's Metal brush integrates coverage continuously in float (`1 − exp(−∫density / spacing)`) and holds an 800-pixel 0% stroke to 1 level. LX lays 8-bit dabs at macOS's spacing, as macOS's software path does. With 1.7.2's wider spacing (16% of a 1080-pixel tip), their sum ripples by up to 4 levels in the faint rim, so the ported test allows 4. The continuous float accumulator is the first GPU candidate below.
- **Look.** The slider knob is round (requested) instead of macOS's pill, and doesn't turn to Liquid Glass while held. The toolbar pills are drawn by the stylesheet, so they have no SwiftUI press animation.
- **Mac-only.** Sparkle, the bookmark entitlement, Finder's document types and CI.

## GPU candidates (still on the CPU)

| Priority | Path | Why | Note |
| --- | --- | --- | --- |
| High | Brush and eraser coverage, with macOS's continuous deposition | Removes the soft-brush ripple and matches macOS pixel for pixel | A float density image per stroke; the segment integral is per pixel, embarrassingly parallel |
| High | Rim Light past the GPU budget; Volumetric Light's scene and composition | Unchanged from 0.7.0 | See UPSTREAM_SYNC_1.7.0.md |
| Medium | Movie frame conversion (YUV to RGB, HDR tone mapping) | 4K HDR frames are converted on the CPU (OpenMP for the tone curve, swscale for the rest) | A compute pass reading the decoded planes; pairs with hardware decoding |
| Medium | Hardware video decoding (VA-API) | macOS decodes on the media engine | FFmpeg's hwaccel, with the CPU decoder as fallback |
| Medium | Stroke effect distance transform | Unchanged | Jump flooding |
| Low | Adjustment-layer caching, DepthMini, PSD export | Unchanged | |

## Measurements

Release build, Intel Iris Xe, 12 threads, median of three runs. Movie decoding on a 3840 × 2160 H.264 clip (30 fps, keyframe every 2 s; `MOVIE_BENCH=<clip> upstream176_tests movieDecodingTimings`):

| Step | ms |
|---|---:|
| Open | 18 |
| First frame (full size) | 128 |
| Exact frame 2.9 s in, from a seek | 201 |
| Playback frame, decoded on from the last (at most 1920 px) | 20 |
| Scrub frame anywhere (at most 1920 px) | 188 |

A playback frame at 4K takes 20 ms, so 30 fps movies play in time. A scrub lands in under 0.2 s and settles on the exact full-size frame when it's let go. No before figure exists, since movies are new. The brush change alters output, not speed, and was not timed.

## Verification

- Release build; `ctest`: 34 of 34 suites pass (`upstream176_tests`: 23 cases counting data rows, including ported Mac golden values, plus a timing case run on request).
- GPU suites on the Intel GPU over Vulkan, on lavapipe, with forced staging, and under the Khronos validation layer with synchronization checks: all pass, 0 validation errors. No Vulkan code changed in this release.
- The app run offscreen through `ui_snapshot` (movie bar, GIF bar, toolbar, sliders in each dialog).
- The AppImage bundles Qt Multimedia, its FFmpeg backend plugin, Qt Quick's libraries (which the plugin links) and FFmpeg's. It passes `--version` and `--check-renderer` (with Vulkan and without), and it opened a format-23 project with a movie layer plus an MP4 given on its command line, with no errors. Export from the AppImage's own window was not clicked through (no input automation on this Wayland desktop); export is covered by the test suite, run in the same clean Release build.
- The .deb depends on `libqt6multimedia6` and FFmpeg 8's libavcodec62, libavformat62, libavutil60 and libswscale9.
