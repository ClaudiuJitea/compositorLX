#pragma once

#include <QJsonObject>
#include <QString>

namespace compositor {

// macOS decodes a layer's `adjustment` with a synthesized Codable (LayerAdjustment.swift), which treats every
// non-optional member as required: `hue`, `saturation`, `lightness`, `colorize`, `levels` and `curves` must always be
// present, and each present settings object needs all of its own keys. LX keeps adjustments as compact JSON that only
// carries what its editors touch, so the writer completes them with identity values before saving.
[[nodiscard]] QJsonObject completedAdjustmentJson(const QJsonObject &adjustment);

// The first reason macOS (LayerAdjustment.isValid) would refuse this adjustment, or an empty string. Checked on the
// completed object, so it covers Levels, Curves, Exposure, Gradient Map, Grain and Hue/Saturation ranges too.
[[nodiscard]] QString adjustmentJsonError(const QJsonObject &adjustment);

} // namespace compositor
