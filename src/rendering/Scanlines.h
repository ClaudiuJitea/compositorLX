#pragma once
#include "rendering/Dither.h"

struct ScanlinesSettings {
    double lineSpacing = 4, thickness = 70, glow = 0, dots = 0, wobble = 0;
    double displace = 0, smoothness = 50, threshold = 0, split = 0;
    double density = 0, contrast = 0, blackLevel = 0;
    DitherColors colors = DitherColors::BlackWhite;
    DitherColor dark{0, 0, 0}, light{1, 1, 1};
    [[nodiscard]] ScanlinesSettings normalized() const;
    [[nodiscard]] QImage apply(const QImage &image) const;
    bool operator==(const ScanlinesSettings &) const = default;
};
