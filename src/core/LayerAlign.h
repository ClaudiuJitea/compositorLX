#pragma once
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVector>
#include <array>

namespace compositor {
// The Move tool's Align and Distribute commands, as in Photoshop (mac LayerAlign.swift, 4ae432b / 93a8aeb).
enum class LayerAlignment { Left, HorizontalCenter, Right, Top, VerticalCenter, Bottom };
enum class LayerDistribution { HorizontalCenters, VerticalCenters, HorizontalSpacing, VerticalSpacing };

inline constexpr std::array<LayerAlignment, 6> allLayerAlignments{LayerAlignment::Left, LayerAlignment::HorizontalCenter,
    LayerAlignment::Right, LayerAlignment::Top, LayerAlignment::VerticalCenter, LayerAlignment::Bottom};
inline constexpr std::array<LayerDistribution, 4> allLayerDistributions{LayerDistribution::HorizontalCenters,
    LayerDistribution::VerticalCenters, LayerDistribution::HorizontalSpacing, LayerDistribution::VerticalSpacing};

// "Left Edges", "Horizontal Centers", …: the menu items' names, and "Align " + it the undo step's.
QString layerAlignmentName(LayerAlignment alignment);
QString layerDistributionName(LayerDistribution distribution);
// How far `box` moves to line up with `target`.
QSizeF alignmentOffset(LayerAlignment alignment, const QRectF &box, const QRectF &target);
// How far each of `boxes` moves, in their order: the outermost two stay put and the rest are spread evenly between
// them, their middles or the gaps between them. Fewer than three: none move.
QVector<QSizeF> distributionOffsets(LayerDistribution distribution, const QVector<QRectF> &boxes);
} // namespace compositor
