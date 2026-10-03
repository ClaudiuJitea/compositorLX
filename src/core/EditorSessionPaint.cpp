// Painting-area session state ported from macOS: the palette (ColorPalette.swift) and its canvas sampling.
#include "core/EditorSession.h"

#include "rendering/LayerRenderer.h"

#include <QtMath>

namespace compositor {

QColor EditorSession::paletteColor(bool background) const
{
    if (maskSelected_) return (background ? !maskPaintWhite_ : maskPaintWhite_) ? QColor(Qt::white) : QColor(Qt::black);
    return background ? background_ : foreground_;
}

void EditorSession::setPaletteColor(const QColor &color, bool background)
{
    if (brush_ || warp_ || !color.isValid()) return;
    if (maskSelected_) {
        // A mask only has black and white: anything else other than white counts as black, as on the Mac.
        const bool white = color.rgb() == QColor(Qt::white).rgb();
        maskPaintWhite_ = background ? !white : white;
    } else if (background) background_ = color.toRgb();
    else foreground_ = color.toRgb();
}

void EditorSession::swapPaletteColors()
{
    if (brush_ || warp_) return;
    if (maskSelected_) { maskPaintWhite_ = !maskPaintWhite_; return; }
    std::swap(foreground_, background_);
}

void EditorSession::resetPaletteColors()
{
    if (brush_ || warp_) return;
    if (maskSelected_) { maskPaintWhite_ = false; return; }
    foreground_ = Qt::black;
    background_ = Qt::white;
}

std::optional<QColor> EditorSession::sampleCompositeColor(const QPointF &documentPoint) const
{
    if (!document_ || documentPoint.x() < 0 || documentPoint.y() < 0
        || documentPoint.x() >= document_->canvasSize.width() || documentPoint.y() >= document_->canvasSize.height()) return std::nullopt;
    const QImage flat = LayerRenderer::flattened(*document_);
    const QPoint pixel(qFloor(documentPoint.x()), qFloor(documentPoint.y()));
    if (!QRect(QPoint(), flat.size()).contains(pixel)) return std::nullopt;
    const QColor color = flat.pixelColor(pixel);
    if (!color.isValid() || color.alpha() == 0) return std::nullopt;
    return QColor(color.red(), color.green(), color.blue());
}

} // namespace compositor
