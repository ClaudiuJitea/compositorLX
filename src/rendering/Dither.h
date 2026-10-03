#pragma once

#include <QImage>
#include <QString>
#include <array>
#include <vector>

// Filter > Dither (macOS Dither.swift). The pixel work is in vendor/compositor-rendering/DitherPixels.c.
enum class DitherStyle {
    Atkinson, FloydSteinberg, Bayer2, Bayer4, Bayer8,
    Dots, Lines, Diamonds, Patterns, Ascii, Scanlines
};

enum class DitherPixelShape { Square, Dot };
enum class DitherColors { BlackWhite, TwoColors, Original };

struct DitherColor {
    double red = 0, green = 0, blue = 0;
    bool operator==(const DitherColor &) const = default;
};

namespace DitherInfo {
// Menu order, grouped as the panel lists them.
inline const std::vector<std::vector<DitherStyle>> &groups()
{
    static const std::vector<std::vector<DitherStyle>> value{
        {DitherStyle::Atkinson, DitherStyle::FloydSteinberg},
        {DitherStyle::Bayer2, DitherStyle::Bayer4, DitherStyle::Bayer8},
        {DitherStyle::Dots, DitherStyle::Lines, DitherStyle::Diamonds},
        {DitherStyle::Patterns, DitherStyle::Ascii, DitherStyle::Scanlines}};
    return value;
}
QString styleName(DitherStyle style);
bool diffuses(DitherStyle style);
bool hasTones(DitherStyle style);
bool isHalftone(DitherStyle style);
bool drawsMarks(DitherStyle style);
bool usesPixelSize(DitherStyle style);
}

struct DitherSettings {
    static constexpr double pixelSizeMin = 1, pixelSizeMax = 32;
    static constexpr double cellSizeMin = 4, cellSizeMax = 64;
    static constexpr double textSizeMin = 6, textSizeMax = 64;
    static constexpr double levelsMin = 2, levelsMax = 8;
    static constexpr double lineSpacingMin = 2, lineSpacingMax = 32;
    static constexpr double wobbleMin = 0, wobbleMax = 64;
    static QString defaultCharacters() { return QStringLiteral(" .:-=+*#%@"); }

    DitherStyle style = DitherStyle::Atkinson;
    double pixelSize = 2;
    DitherPixelShape pixelShape = DitherPixelShape::Square;
    double cellSize = 8;
    double textSize = 14;
    double lineSpacing = 4;
    double glow = 35;
    double dots = 0;
    double wobble = 0;
    double angle = 45;
    double levels = 2;
    double diffusion = 100;
    double density = 0;
    double contrast = 0;
    DitherColors colors = DitherColors::BlackWhite;
    DitherColor dark{0, 0, 0};
    DitherColor light{1, 1, 1};
    bool lightOnDark = true;
    QString characters = defaultCharacters();

    bool operator==(const DitherSettings &) const = default;
    // Clamps and rounds every field exactly as DitherSettings.normalized does on macOS.
    [[nodiscard]] DitherSettings normalized() const;
    // Returns the dithered image (same size), or a null image if the work could not be done.
    [[nodiscard]] QImage apply(const QImage &image) const;
};
