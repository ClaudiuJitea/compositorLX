#pragma once

#include <QImage>
#include <QPoint>
#include <QRectF>

namespace compositor::SelectionOps {

/// The box a drag from @p anchor to @p point spans, in whole pixels (mac Selection.swift DragBox.rect).
/// @p square evens the sides, @p fromCenter grows the box around the anchor. Shared by the Marquee and the Shape tool.
/// The result is exclusive of its far edge, so a drag of 40 pixels selects 40 pixels (QRect(QPoint, QPoint) would add one).
[[nodiscard]] QRectF dragBox(const QPointF &anchor, const QPointF &point, bool square, bool fromCenter);

/// Shift adds, Option (with or without Shift) subtracts; otherwise the options-bar @p chosen mode (0 New, 1 Add, 2 Subtract).
[[nodiscard]] int modeForModifiers(bool shift, bool option, int chosen);

/// Grayscale morphology with a round structuring element (mac Expand / Contract grow the outline with rounded corners).
/// Pixels outside the canvas count as unselected, so Contract also pulls in from the canvas edges while Expand stays on it.
[[nodiscard]] QImage dilated(const QImage &mask, int radius);
[[nodiscard]] QImage eroded(const QImage &mask, int radius);

/// @p mask moved by @p offset on a canvas of the same size; whatever leaves the canvas is dropped.
[[nodiscard]] QImage shifted(const QImage &mask, const QPoint &offset);

/// True when any pixel of @p mask is selected.
[[nodiscard]] bool hasCoverage(const QImage &mask);

/// Whole-pixel bounds of the pixels with coverage, or an empty rect.
[[nodiscard]] QRect coverageBounds(const QImage &mask);

} // namespace compositor::SelectionOps
