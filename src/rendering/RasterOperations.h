#pragma once

#include <QImage>
#include <QPoint>
#include <QJsonObject>

#include <optional>
#include <array>

namespace compositor {

struct LevelRange {
    double black = 0, gamma = 1, white = 255, outputBlack = 0, outputWhite = 255;
    bool operator==(const LevelRange &) const = default;
};

struct LevelsSettings {
    std::array<LevelRange, 4> ranges{}; // RGB, red, green, blue
};
using LevelsHistogram = std::array<std::array<double, 256>, 4>;

enum class ColorRange { Master, Reds, Yellows, Greens, Cyans, Blues, Magentas, Count };

struct HueBand {
    double falloffStart = 0, rangeStart = 0, rangeEnd = 360, falloffEnd = 360;
    [[nodiscard]] static double forward(double from, double to);
    [[nodiscard]] double weight(double hue) const;
    [[nodiscard]] HueBand centered(double hue) const;
    void include(double hue);
    void exclude(double hue);
    bool setHandle(int index, double degrees);
    [[nodiscard]] static HueBand defaultFor(ColorRange range);
    bool operator==(const HueBand &) const = default;
};

struct RangeAdjustment {
    double hue = 0, saturation = 0, lightness = 0;
    bool operator==(const RangeAdjustment &) const = default;
};

struct HueSaturationSettings {
    ColorRange range = ColorRange::Master;
    bool colorize = false;
    bool invertRange = false;
    std::array<RangeAdjustment, size_t(ColorRange::Count)> adjustments{};
    std::array<HueBand, size_t(ColorRange::Count)> bands{};

    HueSaturationSettings();
    HueSaturationSettings(double hue, double saturation = 0, double lightness = 0,
                          bool colorize = false, ColorRange range = ColorRange::Master);
    [[nodiscard]] bool isIdentity() const;
    [[nodiscard]] double weight(ColorRange colorRange, double hue) const;
};

struct CurvePoint {
    double x = 0, y = 0;
    bool operator==(const CurvePoint &) const = default;
};

struct CurvesSettings {
    int channel = 0;
    std::array<QVector<CurvePoint>, 4> channels;
    CurvesSettings();
    [[nodiscard]] bool isValid() const;
    [[nodiscard]] double value(double x, int channelIndex) const;
};

class RasterOperations final {
public:
    static std::optional<QImage> magicWandMask(const QImage &image, const QPoint &seed,
                                                int tolerance = 32, int radius = 0,
                                                bool contiguous = true);
    static QImage inverted(const QImage &image);
    static QImage addNoise(const QImage &image, float amount, bool gaussian,
                           bool monochromatic, quint32 seed);
    static QImage lensDistorted(const QImage &image, double amount);
    static QImage levels(const QImage &image, const LevelsSettings &settings);
    static LevelsHistogram levelsHistogram(const QImage &image, const QImage &coverage = {});
    static LevelsSettings automaticLevels(const LevelsHistogram &histogram, int mode);
    static LevelsSettings sampledLevels(const LevelsSettings &settings, const QColor &sample, int mode);
    static QImage exposure(const QImage &image, double stops, double offset, double gamma);
    static QImage hueSaturation(const QImage &image, const HueSaturationSettings &settings);
    static QImage curves(const QImage &image, const CurvesSettings &settings);
    static QImage adjustment(const QImage &image, const QJsonObject &settings,
                             const QPointF &origin = {}, double unitsPerPixel = 1);
    static QImage gradientMap(const QImage &image, const QColor &shadows, const QColor &highlights, bool reversed);
    static QImage grain(const QImage &image, double amount, double size, double roughness, quint32 seed,
                        const QPointF &origin = {}, double unitsPerPixel = 1);
    static QImage gaussianBlur(const QImage &image, double radius);
    static QImage motionBlur(const QImage &image, double angleDegrees, double distance);
    static std::optional<QImage> contentAwareFill(const QImage &image, const QImage &coverage);
    static std::optional<QImage> spotHeal(const QImage &image, const QImage &coverage,
                                          double opacity, int mode, quint32 seed);
};

} // namespace compositor
