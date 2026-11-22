#include "core/LayerDistances.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace compositor::LayerDistances {
namespace {
bool overlap(double a0, double a1, double b0, double b1) { return a0 < b1 && a1 > b0; }

// Where a line between `box` and `other` runs across: the middle of the span they share, or of `box` if none.
double middle(const QRectF &box, const QRectF &other, bool horizontal)
{
    const double low = horizontal ? std::max(box.top(), other.top()) : std::max(box.left(), other.left());
    const double high = horizontal ? std::min(box.bottom(), other.bottom()) : std::min(box.right(), other.right());
    return high > low ? (low + high) / 2 : horizontal ? box.center().y() : box.center().x();
}
} // namespace

QVector<DistanceLine> moving(const QRectF &box, const QVector<QRectF> &others, const QSizeF &canvas)
{
    QVector<QRectF> all = others;
    all.push_back(QRectF(QPointF(), canvas));
    QVector<DistanceLine> lines;
    for (const bool horizontal : {true, false}) {
        const double low = horizontal ? box.left() : box.top(), high = horizontal ? box.right() : box.bottom();
        // Each side: the closest edge past it, and the middle of what the two have in common across.
        std::optional<std::pair<double, QRectF>> before, after;
        for (const QRectF &rect : std::as_const(all)) {
            if (!(horizontal ? overlap(rect.top(), rect.bottom(), box.top(), box.bottom()) : overlap(rect.left(), rect.right(), box.left(), box.right()))) continue;
            const std::array<double, 2> edges = horizontal ? std::array<double, 2>{rect.left(), rect.right()} : std::array<double, 2>{rect.top(), rect.bottom()};
            for (const double edge : edges) {
                if (edge <= low && edge > (before ? before->first : -std::numeric_limits<double>::infinity())) before = std::make_pair(edge, rect);
                if (edge >= high && edge < (after ? after->first : std::numeric_limits<double>::infinity())) after = std::make_pair(edge, rect);
            }
        }
        if (before && low - before->first >= .5) lines.push_back({horizontal, before->first, low, middle(box, before->second, horizontal)});
        if (after && after->first - high >= .5) lines.push_back({horizontal, high, after->first, middle(box, after->second, horizontal)});
    }
    return lines;
}

QVector<DistanceLine> between(const QRectF &box, const QRectF &other)
{
    QVector<DistanceLine> gaps, insets;
    for (const bool horizontal : {true, false}) {
        const double low = horizontal ? box.left() : box.top(), high = horizontal ? box.right() : box.bottom();
        const double otherLow = horizontal ? other.left() : other.top(), otherHigh = horizontal ? other.right() : other.bottom();
        const double across = middle(box, other, horizontal);
        if (otherLow >= high) gaps.push_back({horizontal, high, otherLow, across});
        else if (otherHigh <= low) gaps.push_back({horizontal, otherHigh, low, across});
        else for (const auto &[a, b] : {std::pair{low, otherLow}, std::pair{high, otherHigh}})
            if (std::abs(a - b) >= .5) insets.push_back({horizontal, std::min(a, b), std::max(a, b), across});
    }
    // Side by side, only the gap between them; the sides' insets count only when one overlaps the other.
    return gaps.isEmpty() ? insets : gaps;
}
} // namespace compositor::LayerDistances
