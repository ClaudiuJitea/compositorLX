#pragma once

#include <QImage>
#include <QPoint>
#include <QJsonObject>

#include <optional>
#include <array>

namespace compositor {

struct CameraRawSettings;
enum class CameraRawClipping;

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
    static QImage featherMask(const QImage &mask, double amount);
    static QImage motionBlur(const QImage &image, double angleDegrees, double distance);
    static QImage blackWhite(const QImage &image, const float *weights, bool tint, double tintHue, double tintSaturation);
    static QImage colorBalance(const QImage &image, const float *shadows, const float *midtones, const float *highlights, bool preserveLuminosity);
    static QImage vignette(const QImage &image, double amount, const QColor &color,
                           double midpoint = 50.0, double roundness = 100.0, double feather = 60.0,
                           double highlights = 25.0, const std::optional<QRectF> &canvasFrame = std::nullopt);
    static QImage bloomGlow(const QImage &image, double amount, double radius);
    static QImage tonalContrast(const QImage &image, double amount, double radius,
                                double shadows = 40.0, double midtones = 60.0, double highlights = 30.0);
    static std::optional<QImage> contentAwareFill(const QImage &image, const QImage &coverage);
    static std::optional<QImage> spotHeal(const QImage &image, const QImage &coverage,
                                          double opacity, int mode, quint32 seed);
    static QImage cameraRaw(const QImage &image, const CameraRawSettings &settings,
                            CameraRawClipping clipping = static_cast<CameraRawClipping>(0),
                            double scale = 1.0, quint32 seed = 0,
                            int visualizePointColor = -1, bool sharpenMask = false);
    // Select > Color Range (mac c3e360a): 255 where every color channel is within `fuzziness` of one of the `include`
    // colors (straight sRGB, 3 bytes each) and of none in `exclude`; transparent pixels never match; `invert` selects
    // what doesn't match. Same size as `image`, Grayscale8.
    static QImage colorRangeMask(const QImage &image, const QVector<quint8> &include, const QVector<quint8> &exclude,
                                 int fuzziness, bool invert);
    // The straight color at `point`, averaged over the 3 x 3 pixels around it; nullopt for fully transparent ones.
    static std::optional<std::array<quint8, 3>> colorRangeSample(const QImage &image, const QPoint &point);
    static QJsonObject levelsSettingsToJson(const LevelsSettings &settings);
    static QJsonObject curvesSettingsToJson(const CurvesSettings &settings);
    static QJsonObject hueSaturationSettingsToJson(const HueSaturationSettings &settings);
};

} // namespace compositor
