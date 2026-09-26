#pragma once

#include "rendering/RasterOperations.h"

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QString>
#include <QVector>
#include <array>
#include <optional>
#include <tuple>

namespace compositor {

enum class CameraRawWhiteBalance { Custom, Auto };
enum class CameraRawGlowStyle { Diffusion = 0, Bloom = 1, Halation = 2 };
enum class CameraRawVignetteStyle { HighlightPriority = 0, ColorPriority = 1, PaintOverlay = 2 };
enum class CameraRawClipping { None = 0, Highlights = 1, Shadows = 2 };
enum class CameraRawScopeMode { Histogram, Vectorscope };
enum class CameraRawCurvePage { Parametric, Point };
enum class CameraRawPointChannel { RGB, Red, Green, Blue };
enum class CameraRawMixerPage { HSL, Color, PointColor };
enum class CameraRawMixerTab { Hue, Saturation, Luminance };
enum class CameraRawGradePage { ThreeWay, Shadows, Midtones, Highlights, Global };
enum class CameraRawUprightMode { Off, Guided };
enum class CameraRawProjection { Perspective, Rectilinear };
enum class CameraRawProcessVersion { Version1 = 1, Version2 = 2, Version3 = 3, Version4 = 4, Version5 = 5, Version6 = 6 };

struct CameraRawCurveSettings {
    double shadows = 0;
    double darks = 0;
    double lights = 0;
    double highlights = 0;
    double shadowSplit = 25;
    double darkSplit = 50;
    double lightSplit = 75;
    QVector<CurvePoint> rgb = {{0, 0}, {1, 1}};
    QVector<CurvePoint> red = {{0, 0}, {1, 1}};
    QVector<CurvePoint> green = {{0, 0}, {1, 1}};
    QVector<CurvePoint> blue = {{0, 0}, {1, 1}};
    double refineSaturation = 0;

    static const QVector<CurvePoint> linear;
    static const QVector<CurvePoint> mediumContrast;
    static const QVector<CurvePoint> strongContrast;

    [[nodiscard]] bool adjusts() const;
    static bool isLinear(const QVector<CurvePoint> &pts);
    [[nodiscard]] double parametric(double tone) const;
    [[nodiscard]] std::array<float, 256> lumaTable() const;
    [[nodiscard]] std::array<float, 256> channelTable(const QVector<CurvePoint> &pts) const;
    [[nodiscard]] CameraRawCurveSettings nudged(CameraRawPointChannel ch, double tone, double delta) const;
    [[nodiscard]] CameraRawCurveSettings normalized() const;
    static QVector<CurvePoint> repair(const QVector<CurvePoint> &pts);
    double evaluatePoint(double x, const QVector<CurvePoint> &pts) const;
    bool operator==(const CameraRawCurveSettings &) const = default;
};

struct CameraRawPointColor {
    double hue = 0;
    double saturation = 0;
    double luminance = 0;
    double hueShift = 0;
    double saturationShift = 0;
    double luminanceShift = 0;
    double hueRange = 30;
    double saturationRange = 0.4;
    double luminanceRange = 0.4;
    bool visualize = false;

    [[nodiscard]] CameraRawPointColor normalized() const;
    bool operator==(const CameraRawPointColor &) const = default;
};

struct CameraRawMixerSettings {
    static const std::array<double, 8> centers;
    std::array<double, 8> hue{};
    std::array<double, 8> saturation{};
    std::array<double, 8> luminance{};
    QVector<CameraRawPointColor> points;

    [[nodiscard]] bool adjusts() const;
    static std::array<double, 8> weights(double hueDegrees);
    [[nodiscard]] std::array<float, 24> mixerFloats() const;
    [[nodiscard]] QVector<float> pointFloats() const;
    [[nodiscard]] CameraRawMixerSettings normalized() const;
    bool operator==(const CameraRawMixerSettings &) const = default;
};

struct CameraRawGradeWheel {
    double hue = 0;
    double saturation = 0;
    double luminance = 0;
    [[nodiscard]] CameraRawGradeWheel normalized() const;
    bool operator==(const CameraRawGradeWheel &) const = default;
};

struct CameraRawGradingSettings {
    CameraRawGradeWheel shadows;
    CameraRawGradeWheel midtones;
    CameraRawGradeWheel highlights;
    CameraRawGradeWheel global;
    double blending = 50;
    double balance = 0;

    [[nodiscard]] bool adjusts() const;
    [[nodiscard]] std::array<float, 12> gradeFloats() const;
    [[nodiscard]] CameraRawGradingSettings normalized() const;
    bool operator==(const CameraRawGradingSettings &) const = default;
};

struct CameraRawDetailSettings {
    double sharpenAmount = 0;
    double sharpenRadius = 10;
    double sharpenDetail = 25;
    double sharpenMasking = 0;
    double noiseLuminance = 0;
    double noiseLuminanceDetail = 50;
    double noiseLuminanceContrast = 0;
    double noiseColor = 0;
    double noiseColorDetail = 50;
    double noiseColorSmoothness = 50;

    [[nodiscard]] bool adjustsSharpening() const { return sharpenAmount != 0; }
    [[nodiscard]] bool adjustsNoise() const { return noiseLuminance != 0 || noiseColor != 0; }
    [[nodiscard]] bool adjusts() const { return adjustsSharpening() || adjustsNoise(); }
    [[nodiscard]] CameraRawDetailSettings normalized() const;
    bool operator==(const CameraRawDetailSettings &) const = default;
};

struct CameraRawOpticsSettings {
    bool removeChromaticAberration = false;
    bool enableLensProfile = false;
    double profileDistortion = 100;
    double profileVignetting = 100;
    double distortion = 0;
    double purpleAmount = 0;
    double purpleHueLow = 270;
    double purpleHueHigh = 310;
    double greenAmount = 0;
    double greenHueLow = 60;
    double greenHueHigh = 120;
    double vignetteAmount = 0;
    double vignetteMidpoint = 50;

    [[nodiscard]] bool adjusts() const;
    [[nodiscard]] double distortionK(double profileStrength = 1.0) const;
    [[nodiscard]] CameraRawOpticsSettings normalized() const;
    bool operator==(const CameraRawOpticsSettings &) const = default;
};

struct CameraRawGeometryGuide {
    double startX = 0;
    double startY = 0;
    double endX = 0;
    double endY = 0;
    bool operator==(const CameraRawGeometryGuide &) const = default;
};

struct CameraRawGeometrySettings {
    CameraRawUprightMode upright = CameraRawUprightMode::Off;
    CameraRawProjection projection = CameraRawProjection::Perspective;
    double vertical = 0;
    double horizontal = 0;
    double rotate = 0;
    double aspect = 0;
    double scale = 0;
    double offsetX = 0;
    double offsetY = 0;
    bool constrainCrop = false;
    QVector<CameraRawGeometryGuide> guides;

    [[nodiscard]] bool adjusts() const;
    [[nodiscard]] CameraRawGeometrySettings normalized() const;
    [[nodiscard]] QImage apply(const QImage &image) const;
    bool operator==(const CameraRawGeometrySettings &) const = default;
};

struct CameraRawCalibrationSettings {
    CameraRawProcessVersion process = CameraRawProcessVersion::Version6;
    double shadowTint = 0;
    double redHue = 0;
    double redSaturation = 0;
    double greenHue = 0;
    double greenSaturation = 0;
    double blueHue = 0;
    double blueSaturation = 0;

    [[nodiscard]] bool adjusts() const;
    [[nodiscard]] CameraRawCalibrationSettings normalized() const;
    bool operator==(const CameraRawCalibrationSettings &) const = default;
};

struct CameraRawSettings {
    CameraRawWhiteBalance whiteBalance = CameraRawWhiteBalance::Custom;
    double temperature = 0;
    double tint = 0;
    double exposure = 0;
    double contrast = 0;
    double highlights = 0;
    double shadows = 0;
    double whites = 0;
    double blacks = 0;
    double vibrance = 0;
    double saturation = 0;
    double texture = 0;
    double clarity = 0;
    double dehaze = 0;
    double glow = 0;
    CameraRawGlowStyle glowStyle = CameraRawGlowStyle::Diffusion;
    double glowRange = 0;
    double glowSpread = 0;
    double glowWarmth = 0;
    double vignetteAmount = 0;
    CameraRawVignetteStyle vignetteStyle = CameraRawVignetteStyle::HighlightPriority;
    double vignetteMidpoint = 50;
    double vignetteRoundness = 0;
    double vignetteFeather = 50;
    double vignetteHighlights = 0;
    double grainAmount = 0;
    double grainSize = 25;
    double grainRoughness = 50;

    CameraRawCurveSettings curve;
    CameraRawMixerSettings mixer;
    CameraRawGradingSettings grading;
    CameraRawDetailSettings detail;
    CameraRawOpticsSettings optics;
    CameraRawGeometrySettings geometry;
    CameraRawCalibrationSettings calibration;

    [[nodiscard]] bool adjustsLight() const;
    [[nodiscard]] bool adjustsColor() const;
    [[nodiscard]] bool adjustsEffects() const;
    [[nodiscard]] bool adjustsCurve() const { return curve.adjusts(); }
    [[nodiscard]] bool adjustsMixer() const { return mixer.adjusts(); }
    [[nodiscard]] bool adjustsGrading() const { return grading.adjusts(); }
    [[nodiscard]] bool adjustsDetail() const { return detail.adjusts(); }
    [[nodiscard]] bool adjustsOptics() const { return optics.adjusts(); }
    [[nodiscard]] bool adjustsGeometry() const { return geometry.adjusts(); }
    [[nodiscard]] bool adjustsCalibration() const { return calibration.adjusts(); }
    [[nodiscard]] bool isIdentity() const;
    [[nodiscard]] bool isValid() const;
    [[nodiscard]] CameraRawSettings normalized() const;
    [[nodiscard]] CameraRawSettings applying(bool showsLight, bool showsColor, bool showsEffects = true,
                                             bool showsCurve = true, bool showsMixer = true,
                                             bool showsGrading = true, bool showsDetail = true,
                                             bool showsOptics = true, bool showsGeometry = true,
                                             bool showsCalibration = true) const;

    [[nodiscard]] double grainKernelSize() const { return 0.5 + (grainSize / 100.0) * 19.5; }
    [[nodiscard]] std::tuple<double, double, double> gains() const;

    static std::optional<std::pair<double, double>> neutralize(double linearRed, double green, double blue);
    static std::optional<std::pair<double, double>> neutralizeStraight(double straightRed, double green, double blue);
    static std::optional<std::pair<double, double>> autoBalance(const QImage &image);

    bool operator==(const CameraRawSettings &) const = default;
};

struct CameraRawScope {
    static constexpr int binCount = 256;
    static constexpr int scopeSide = 64;
    std::array<double, binCount> red{};
    std::array<double, binCount> green{};
    std::array<double, binCount> blue{};
    std::array<double, scopeSide * scopeSide> vectorscope{};

    [[nodiscard]] double peak() const;
    static std::optional<CameraRawScope> make(const QImage &image);
    static QImage overlay(const QImage &image, bool shadows, bool highlights);
    bool operator==(const CameraRawScope &) const = default;
};

struct RawDevelopSettings {
    float exposure = 0.0f;
    float temperature = 5000.0f;
    float tint = 0.0f;
    float boost = 1.0f;
    float asShotTemperature = 5000.0f;
    float asShotTint = 0.0f;

    [[nodiscard]] bool isAsShot() const {
        return exposure == 0.0f && boost == 1.0f &&
               temperature == asShotTemperature && tint == asShotTint;
    }
    void reset() {
        exposure = 0.0f;
        boost = 1.0f;
        temperature = asShotTemperature;
        tint = asShotTint;
    }
    bool operator==(const RawDevelopSettings &) const = default;
};

} // namespace compositor
