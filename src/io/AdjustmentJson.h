#pragma once

#include <QJsonObject>
#include <QString>
#include "rendering/LensEffects.h"

namespace compositor {

// macOS decodes a layer's `adjustment` with a synthesized Codable (LayerAdjustment.swift), which treats every
// non-optional member as required: `hue`, `saturation`, `lightness`, `colorize`, `levels` and `curves` must always be
// present, and each present settings object needs all of its own keys. LX keeps adjustments as compact JSON that only
// carries what its editors touch, so the writer completes them with identity values before saving.
[[nodiscard]] QJsonObject completedAdjustmentJson(const QJsonObject &adjustment);

// The first reason macOS (LayerAdjustment.isValid) would refuse this adjustment, or an empty string. Checked on the
// completed object, so it covers Levels, Curves, Exposure, Gradient Map, Grain and Hue/Saturation ranges too.
[[nodiscard]] QString adjustmentJsonError(const QJsonObject &adjustment);

// A Lens Effects layer's `lens` object (mac LensEffectSettings Codable): every setting, by its macOS name.
[[nodiscard]] QJsonObject lensSettingsJson(const LensEffectSettings &settings);
// Each setting `object` has, its default where it has none, as macOS reads it.
[[nodiscard]] LensEffectSettings lensSettingsFromJson(const QJsonObject &object);

} // namespace compositor
