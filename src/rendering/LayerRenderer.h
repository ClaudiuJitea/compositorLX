#pragma once

#include "core/Document.h"

#include <QImage>

class QPainter;

namespace compositor {

// Committed pixels (export, merge, flatten, project composite, sampling) render on the CPU
// reference unless GPU rendering is chosen explicitly, so a file renders identically on every
// machine and setting. The canvas display may use the GPU and its incremental caches.
enum class RenderPurpose { Commit, Display };

class LayerRenderer final {
public:
    static void draw(QPainter &painter, const Document &document);
    [[nodiscard]] static QImage flattened(const Document &document, RenderPurpose purpose = RenderPurpose::Commit);
    // Whether `flattenedRegion` gives exactly what `flattened` gives in a region: every pixel then depends only on the
    // pixels under it (no adjustment layers or layer effects) and the document is 8-bit.
    [[nodiscard]] static bool regionRenderable(const Document &document);
    // The document's composite in `region` (canvas pixels) alone, on the CPU: each layer cropped to what the region
    // shows of it, with a margin for its sampling, so the cost follows the region and not the layers' sizes. The canvas
    // redraws a brush stroke's dirty rectangles with it.
    [[nodiscard]] static QImage flattenedRegion(const Document &document, const QRect &region);
    [[nodiscard]] static QImage applyMask(const QImage &image, const QImage &mask, Sampling sampling);
    [[nodiscard]] static QImage bakeLiveMask(const Document &document, const Layer &layer);
    [[nodiscard]] static bool effectivelyVisible(const Document &document, const Layer &layer);
    [[nodiscard]] static QTransform pixelToDocument(const LayerTransform &placement, const QSize &pixels);
    [[nodiscard]] static QImage placedMask(const Layer &owner, const Layer &target, const QSize &size);
};

} // namespace compositor
