# CompositorLX project formats 1–17

The schema reference is Compositor's `docs/project-format.md`. A `.comp` project is a directory containing `manifest.json` and `images/` assets. Layers are stored bottom to top. The working space is sRGB; layer transforms and optional text, shapes, effects, masks, and adjustment metadata stay separate from source pixels.

CompositorLX reads versions 1–23 and 27, as the macOS app does (27 was written by builds of the macOS GIF branch; it passes every later-version rule and is saved again as 17). Any other version is refused with "This project uses format version N. This app supports versions 1–23 and 27." Existing projects keep their version until an edit requires a later feature. Version promotion belongs to the edit's undo step. Saving does not silently discard unsupported metadata.

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

Version 18 adds three adjustment kinds from the Filter menu: `Vignette`, `Lens Effects` and `Tonal Contrast`. Each keeps its settings in an object of its own inside the adjustment's `filterLayerSettings`: `vignette` (`amount` 0–100, `color` with `red`, `green`, `blue` 0–1, `midpoint` 0–100, `roundness` −100–100, `feather` 0–100, `highlights` 0–100; it frames the whole canvas and recolors only pixels that are there), `tonal` (`amount` 0–100, `radius` 1–100 document pixels, `shadows`, `midtones`, `highlights` −100–100) and `lens` (`effect`: `Anamorphic Flare`, `Starburst`, `Mist`, `Prism` or `Bokeh`, with that effect's settings: `threshold` 50–100; `flareAmount` 0–100, `flareLength` 1–100 and `flareBlue` 0–100; `starAmount` 0–100, `starLength` 2–40, `starPoints` 4, 6, 8, 12 or 14, `starAngle` −180–180; `mistStrength` and `mistSize` 0–100; `prismAmount` 0–100, `prismCopies` 1–8, `prismOffset` 0–50, `prismAngle` −180–180, `prismBothWays`, `prismBlur` 0–100; `bokehRadius` 0.5–6, `bokehBrightness` and `bokehRing` 0–100, `bokehShape` `Round`, `Hexagon`, `Oval` or `Cat's Eye`). Lengths given as percentages are of the document's longer side. Any `lens` setting may be left out and takes its default. Files declaring 1–17 cannot contain these kinds or objects, and older app builds reject v18.

Version 19 adds the `Sharpen` adjustment kind: an unsharp mask, its settings in a `sharpen` object inside `filterLayerSettings`, with `amount` 0–300 (percent of the detail added back) and `radius` 0.1–20 document pixels. Files declaring 1–18 cannot contain it, and older app builds reject v19.

Version 20 adds `prismFringe` 0–100 to the `lens` settings: how far Prism splits each copy's red and blue apart, in hundredths of a percent of the document's longer side (0 keeps the copies crisp). Left out, it takes 30, the split earlier versions always drew. Older app builds reject v20.

Version 21 adds the `Reduce Noise` adjustment kind, its settings in a `reduceNoise` object inside `filterLayerSettings`: `strength` (how much grain is smoothed from the brightness), `details` (how much texture is kept from it) and `color` (how much colored speckle is smoothed), each 0–100. Files declaring 1–20 cannot contain it, and older app builds reject v21.

Version 22 lets a rectangle or ellipse carry an inside stroke on its `shape` style: `strokeWidth` in document pixels (above 0, at most 5000) and `strokeRed`, `strokeGreen`, `strokeBlue` (0–1). The stroke is drawn inside the shape's box, so the layer keeps the size it was dragged out to, and the width stays in document pixels when the shape is scaled, as the corner radius does. Missing means no stroke. Lines cannot carry these fields. Files declaring 1–21 cannot contain them, and older app builds reject v22.

Version 23 adds an optional movie link with a read-only sandbox bookmark, duration and playhead in seconds. The layer image stores the selected frame. Relinking restores scrubbing when the original movie is unavailable.

On Linux, a movie layer's `bookmark` names the movie's path in the same `book` layout macOS uses (the path components under key `0x1004`), without a sandbox scope. CompositorLX reads the path out of a bookmark either app wrote; macOS can't open a bookmark without its scope, and offers Relink for it, as it does for a movie that has moved. A version 23 manifest is the only one that may carry `movie`, on a layer with an image, not a folder or a GIF, with `duration` above 0 and `seconds` within it.
