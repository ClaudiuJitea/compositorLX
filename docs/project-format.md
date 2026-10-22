# CompositorLX project formats 1–17

The schema reference is Compositor's `docs/project-format.md`. A `.comp` project is a directory containing `manifest.json` and `images/` assets. Layers are stored bottom to top. The working space is sRGB; layer transforms and optional text, shapes, effects, masks, and adjustment metadata stay separate from source pixels.

CompositorLX reads versions 1–17. Existing projects keep their version until an edit requires a later feature. Version promotion belongs to the edit's undo step. Saving does not silently discard unsupported metadata.

| Version | Added capability |
| --- | --- |
| 1–9 | Pixel layers, groups, appearance, masks, clipping, text, adjustments, effects, shapes, guides, blur/noise adjustments |
| 10 | Per-letter text color runs using UTF-16 offsets |
| 11 | Per-letter text font runs using UTF-16 offsets |
| 12 | 16-bit PNG layer assets; PNG bit depth identifies precision |
| 13 | Half-float HDR OpenEXR layer assets named `<layer UUID>.exr` |
| 14 | Optional `shape.smoothing`; true enables 60% smooth rectangle corners |
| 15 | Line patterns and caps |
| 16 | Progressive Blur, Bloom / Glow and RGB Split filter layers |
| 17 | GIF sources, editable frames, timing and geometry |

Versions 1–11 cannot contain 16-bit PNG layers. Versions 1–12 cannot contain EXR layers. Versions 1–13 cannot enable shape smoothing. Missing or false smoothing retains circular corners. Masks are written as 8-bit PNGs. HDR layer values are extended sRGB, with white at 1; EXR storage converts to linear light. An EXR with no values above white can load as a 16-bit layer.

Reader validation checks asset names and canonical paths, dimensions and budgets, metadata ranges, hierarchy/clipping graphs, and version gates. The writer stages a complete package and atomically replaces the destination using the existing recovery mechanism. Unknown package files, such as macOS Quick Look thumbnails, are not retained when LX rewrites a package.

Synthetic fixtures and programmatic round trips verify the Linux reader/writer. Cross-application validation on a Mac remains unverified.

## Embedded preview

Saves may include `QuickLook/Preview.jpg`, the flattened canvas over white, at most 1024 pixels on the long side. This is optional derived data and is ignored when loading the editable document. Preview generation is skipped for canvases larger than 50 megapixels, matching upstream.

## Formats 15–17

Version 15 lets a line carry `lineStyle` (`Solid`, `Dashed`, `Long Dash` or `Dotted`, with dashes measured in multiples of the line's width) and `lineCap` (`Square` or `Round`, applied to the line's ends and to every dash and dot) in its `shape` style. Missing means solid with round caps, as every line was before version 15 (new lines start square-capped); other shape kinds cannot carry them. Files declaring 1–14 cannot contain them, and older app builds reject v15 rather than failing to read the unknown values.

Version 16 adds three adjustment kinds from the Filter menu: `Progressive Blur`, `Bloom / Glow` and `RGB Split`. Their settings are in the adjustment's optional `filterLayerSettings`: `progressiveFrom` and `progressiveTo` (0–250 document pixels), `progressiveStart` and `progressiveEnd` (document points), `progressiveRadial`, `progressiveInverted`; `bloomAmount` (0–100) and `bloomRadius` (1–150); `rgbSplitDistance` (0–200), `rgbSplitBlur` (0–400), `rgbSplitAngle` (−180 to 180 degrees), `rgbSplitRadial`, `rgbSplitCenter` (a document point) and `rgbSplitReach` (document pixels from the center at which Radial's full distance applies). Files declaring 1–15 cannot contain these kinds or `filterLayerSettings`, and older app builds reject v16.

Version 17 lets an image layer be an animated GIF, through an optional `gif` object (groups, adjustments, text and shapes cannot carry it). `sourceFile` is `<layer UUID>.gif` under `images/`, the original GIF kept unchanged, and `selectedFrameIndex` is the frame shown, whose pixels the layer's ordinary `imageFile` holds; the layer's transform, opacity, effects and mask apply to every frame. Optional fields: `geometry` (Image Size's output `width` and `height` and the affine map `a`, `b`, `c`, `d`, `tx`, `ty` from the source's normalized coordinates to the output's, with optional `sampling`), `framePixels` (edited frames, each `{ "index": 1, "imageFile": "<layer UUID>.frame-1.png" }`; other frames decode from the source), `deletedFrameIndices` (source frames removed), `frameOrder` (frames in playing order, each `{ "index": 4, "sourceIndex": 1 }`; a copied frame takes an index from the source's frame count up, below 8,192) and `timing` (`mode` `Frame delay` or `Same for all`, `uniformDelay` in seconds, and `frameDelays` overrides `{ "index": 4, "duration": 0.16 }`; delays are 0.02–655.35 seconds). A GIF holds at most 4,096 frames. Files declaring 1–16 cannot contain `gif`, and older app builds reject v17.
