# CompositorLX project formats 1–14

The schema reference is Compositor's `docs/project-format.md`. A `.comp` project is a directory containing `manifest.json` and `images/` assets. Layers are stored bottom to top. The working space is sRGB; layer transforms and optional text, shapes, effects, masks, and adjustment metadata stay separate from source pixels.

CompositorLX reads versions 1–14. Existing projects keep their version until an edit requires a later feature. Version promotion belongs to the edit's undo step. Saving does not silently discard unsupported metadata.

| Version | Added capability |
| --- | --- |
| 1–9 | Pixel layers, groups, appearance, masks, clipping, text, adjustments, effects, shapes, guides, blur/noise adjustments |
| 10 | Per-letter text color runs using UTF-16 offsets |
| 11 | Per-letter text font runs using UTF-16 offsets |
| 12 | 16-bit PNG layer assets; PNG bit depth identifies precision |
| 13 | Half-float HDR OpenEXR layer assets named `<layer UUID>.exr` |
| 14 | Optional `shape.smoothing`; true enables 60% smooth rectangle corners |

Versions 1–11 cannot contain 16-bit PNG layers. Versions 1–12 cannot contain EXR layers. Versions 1–13 cannot enable shape smoothing. Missing or false smoothing retains circular corners. Masks are written as 8-bit PNGs. HDR layer values are extended sRGB, with white at 1; EXR storage converts to linear light. An EXR with no values above white can load as a 16-bit layer.

Reader validation checks asset names and canonical paths, dimensions and budgets, metadata ranges, hierarchy/clipping graphs, and version gates. The writer stages a complete package and atomically replaces the destination using the existing recovery mechanism. Unknown package files, such as macOS Quick Look thumbnails, are not retained when LX rewrites a package.

Synthetic fixtures and programmatic round trips verify the Linux reader/writer. Cross-application validation on a Mac remains unverified.

## Embedded preview

Saves may include `QuickLook/Preview.jpg`, the flattened canvas over white, at most 1024 pixels on the long side. This is optional derived data and is ignored when loading the editable document. Preview generation is skipped for canvases larger than 50 megapixels, matching upstream.
