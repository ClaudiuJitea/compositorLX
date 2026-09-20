#pragma once

#include "core/Document.h"

#include <QImage>

class QPainter;

namespace compositor {

class LayerRenderer final {
public:
    static void draw(QPainter &painter, const Document &document);
    [[nodiscard]] static QImage flattened(const Document &document);
    [[nodiscard]] static QImage applyMask(const QImage &image, const QImage &mask, Sampling sampling);
    [[nodiscard]] static QImage bakeLiveMask(const Document &document, const Layer &layer);

private:
    [[nodiscard]] static bool effectivelyVisible(const Document &document, const Layer &layer);
};

} // namespace compositor
