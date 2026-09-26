#pragma once

#include <QImage>
#include <QJsonObject>
#include <QPainter>
#include <QPointF>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QUuid>
#include <QVector>

#include <cmath>
#include <optional>

namespace compositor {

enum class Sampling { Nearest, Smooth, HighQuality };

enum class BlendMode {
    Normal,
    // Darkening
    Darken,
    Multiply,
    ColorBurn,
    LinearBurn,
    // Lightening
    Lighten,
    Screen,
    ColorDodge,
    LinearDodge, // "Linear Dodge (Add)"
    // Contrast
    Overlay,
    SoftLight,
    HardLight,
    VividLight,
    LinearLight,
    PinLight,
    HardMix,
    // Inversion / Comparative
    Difference,
    Exclusion,
    Subtract,
    Divide,
    // Component
    Hue,
    Saturation,
    Color,
    Luminosity
};

struct CanvasGuide {
    enum class Axis { Horizontal, Vertical };

    QUuid id;
    Axis axis = Axis::Horizontal;
    double position = 0.0; // document pixels

    [[nodiscard]] bool isValid() const {
        return !id.isNull() && std::isfinite(position) && std::abs(position) <= 1000000.0;
    }
    bool operator==(const CanvasGuide &) const = default;

    [[nodiscard]] CanvasGuide offset(double x, double y) const {
        CanvasGuide g = *this;
        g.position += (axis == Axis::Vertical ? x : y);
        return g;
    }

    [[nodiscard]] CanvasGuide scaled(double x, double y) const {
        CanvasGuide g = *this;
        g.position *= (axis == Axis::Vertical ? x : y);
        return g;
    }

    [[nodiscard]] CanvasGuide mirrored(bool horizontally, double center) const {
        CanvasGuide g = *this;
        if ((horizontally && axis == Axis::Vertical) || (!horizontally && axis == Axis::Horizontal)) {
            g.position = 2.0 * center - g.position;
        }
        return g;
    }
};

struct LayoutGrid {
    static constexpr double spacing = 64.0;
    static constexpr int subdivisions = 8;
    static constexpr double step = spacing / double(subdivisions);

    static QVector<double> lines(double length) {
        if (length < 0.0 || step <= 0.0) return {0.0};
        QVector<double> result;
        double value = 0.0;
        while (value <= length + 0.001) {
            result.push_back(std::round(value));
            value += step;
        }
        return result;
    }

    static bool isMajor(double value) {
        return std::abs(std::fmod(std::round(value), spacing)) < 0.001;
    }
};

enum class LayerEffectKind {
    Stroke,
    DropShadow,
    ColorOverlay,
    InnerShadow,
    OuterGlow,
    InnerGlow
};

QString layerEffectKindToString(LayerEffectKind kind);
std::optional<LayerEffectKind> layerEffectKindFromString(const QString &str);

struct StrokeEffect {
    std::optional<bool> enabled;
    double size = 4.0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double opacity = 1.0;
    bool inside = false;

    [[nodiscard]] bool isEnabled() const { return enabled.value_or(true); }
    [[nodiscard]] bool isValid() const {
        return std::isfinite(size) && size >= 0.0 && size <= 500.0
            && std::isfinite(opacity) && opacity >= 0.0 && opacity <= 1.0
            && std::isfinite(red) && red >= 0.0 && red <= 1.0
            && std::isfinite(green) && green >= 0.0 && green <= 1.0
            && std::isfinite(blue) && blue >= 0.0 && blue <= 1.0;
    }
    bool operator==(const StrokeEffect &) const = default;
};

struct ShadowEffect {
    std::optional<bool> enabled;
    double angle = 90.0;
    double distance = 20.0;
    double blur = 20.0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double opacity = 0.5;

    [[nodiscard]] bool isEnabled() const { return enabled.value_or(true); }
    [[nodiscard]] QPointF offset() const {
        const double radians = angle * M_PI / 180.0;
        return QPointF(-std::cos(radians) * distance, std::sin(radians) * distance);
    }
    [[nodiscard]] bool isValid() const {
        return std::isfinite(angle) && angle >= -360.0 && angle <= 360.0
            && std::isfinite(distance) && distance >= 0.0 && distance <= 5000.0
            && std::isfinite(blur) && blur >= 0.0 && blur <= 500.0
            && std::isfinite(opacity) && opacity >= 0.0 && opacity <= 1.0
            && std::isfinite(red) && red >= 0.0 && red <= 1.0
            && std::isfinite(green) && green >= 0.0 && green <= 1.0
            && std::isfinite(blue) && blue >= 0.0 && blue <= 1.0;
    }
    bool operator==(const ShadowEffect &) const = default;
};

struct ColorOverlayEffect {
    std::optional<bool> enabled;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double opacity = 1.0;

    [[nodiscard]] bool isEnabled() const { return enabled.value_or(true); }
    [[nodiscard]] bool isValid() const {
        return std::isfinite(opacity) && opacity >= 0.0 && opacity <= 1.0
            && std::isfinite(red) && red >= 0.0 && red <= 1.0
            && std::isfinite(green) && green >= 0.0 && green <= 1.0
            && std::isfinite(blue) && blue >= 0.0 && blue <= 1.0;
    }
    bool operator==(const ColorOverlayEffect &) const = default;
};

struct InnerShadowEffect {
    std::optional<bool> enabled;
    double angle = 90.0;
    double distance = 10.0;
    double blur = 10.0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double opacity = 0.5;

    [[nodiscard]] bool isEnabled() const { return enabled.value_or(true); }
    [[nodiscard]] QPointF offset() const {
        const double radians = angle * M_PI / 180.0;
        return QPointF(-std::cos(radians) * distance, std::sin(radians) * distance);
    }
    [[nodiscard]] bool isValid() const {
        return std::isfinite(angle) && angle >= -360.0 && angle <= 360.0
            && std::isfinite(distance) && distance >= 0.0 && distance <= 5000.0
            && std::isfinite(blur) && blur >= 0.0 && blur <= 500.0
            && std::isfinite(opacity) && opacity >= 0.0 && opacity <= 1.0
            && std::isfinite(red) && red >= 0.0 && red <= 1.0
            && std::isfinite(green) && green >= 0.0 && green <= 1.0
            && std::isfinite(blue) && blue >= 0.0 && blue <= 1.0;
    }
    bool operator==(const InnerShadowEffect &) const = default;
};

struct OuterGlowEffect {
    std::optional<bool> enabled;
    double size = 20.0;
    double red = 1.0;
    double green = 1.0;
    double blue = 1.0;
    double opacity = 0.75;

    [[nodiscard]] bool isEnabled() const { return enabled.value_or(true); }
    [[nodiscard]] bool isValid() const {
        return std::isfinite(size) && size >= 0.0 && size <= 500.0
            && std::isfinite(opacity) && opacity >= 0.0 && opacity <= 1.0
            && std::isfinite(red) && red >= 0.0 && red <= 1.0
            && std::isfinite(green) && green >= 0.0 && green <= 1.0
            && std::isfinite(blue) && blue >= 0.0 && blue <= 1.0;
    }
    bool operator==(const OuterGlowEffect &) const = default;
};

struct InnerGlowEffect {
    std::optional<bool> enabled;
    double size = 10.0;
    double red = 1.0;
    double green = 1.0;
    double blue = 1.0;
    double opacity = 0.75;

    [[nodiscard]] bool isEnabled() const { return enabled.value_or(true); }
    [[nodiscard]] bool isValid() const {
        return std::isfinite(size) && size >= 0.0 && size <= 500.0
            && std::isfinite(opacity) && opacity >= 0.0 && opacity <= 1.0
            && std::isfinite(red) && red >= 0.0 && red <= 1.0
            && std::isfinite(green) && green >= 0.0 && green <= 1.0
            && std::isfinite(blue) && blue >= 0.0 && blue <= 1.0;
    }
    bool operator==(const InnerGlowEffect &) const = default;
};

struct LayerEffects {
    std::optional<StrokeEffect> stroke;
    std::optional<ShadowEffect> shadow;
    std::optional<ColorOverlayEffect> colorOverlay;
    std::optional<InnerShadowEffect> innerShadow;
    std::optional<OuterGlowEffect> outerGlow;
    std::optional<InnerGlowEffect> innerGlow;

    [[nodiscard]] bool isEmpty() const {
        return !stroke && !shadow && !colorOverlay && !innerShadow && !outerGlow && !innerGlow;
    }
    [[nodiscard]] bool isValid() const {
        return (!stroke || stroke->isValid())
            && (!shadow || shadow->isValid())
            && (!colorOverlay || colorOverlay->isValid())
            && (!innerShadow || innerShadow->isValid())
            && (!outerGlow || outerGlow->isValid())
            && (!innerGlow || innerGlow->isValid());
    }
    [[nodiscard]] bool contains(LayerEffectKind kind) const {
        switch (kind) {
        case LayerEffectKind::Stroke: return stroke.has_value();
        case LayerEffectKind::DropShadow: return shadow.has_value();
        case LayerEffectKind::ColorOverlay: return colorOverlay.has_value();
        case LayerEffectKind::InnerShadow: return innerShadow.has_value();
        case LayerEffectKind::OuterGlow: return outerGlow.has_value();
        case LayerEffectKind::InnerGlow: return innerGlow.has_value();
        }
        return false;
    }
    [[nodiscard]] bool isEnabled(LayerEffectKind kind) const {
        switch (kind) {
        case LayerEffectKind::Stroke: return stroke && stroke->isEnabled();
        case LayerEffectKind::DropShadow: return shadow && shadow->isEnabled();
        case LayerEffectKind::ColorOverlay: return colorOverlay && colorOverlay->isEnabled();
        case LayerEffectKind::InnerShadow: return innerShadow && innerShadow->isEnabled();
        case LayerEffectKind::OuterGlow: return outerGlow && outerGlow->isEnabled();
        case LayerEffectKind::InnerGlow: return innerGlow && innerGlow->isEnabled();
        }
        return false;
    }
    void setEnabled(LayerEffectKind kind, bool enabled) {
        switch (kind) {
        case LayerEffectKind::Stroke: if (stroke) stroke->enabled = enabled; break;
        case LayerEffectKind::DropShadow: if (shadow) shadow->enabled = enabled; break;
        case LayerEffectKind::ColorOverlay: if (colorOverlay) colorOverlay->enabled = enabled; break;
        case LayerEffectKind::InnerShadow: if (innerShadow) innerShadow->enabled = enabled; break;
        case LayerEffectKind::OuterGlow: if (outerGlow) outerGlow->enabled = enabled; break;
        case LayerEffectKind::InnerGlow: if (innerGlow) innerGlow->enabled = enabled; break;
        }
    }
    void remove(LayerEffectKind kind) {
        switch (kind) {
        case LayerEffectKind::Stroke: stroke.reset(); break;
        case LayerEffectKind::DropShadow: shadow.reset(); break;
        case LayerEffectKind::ColorOverlay: colorOverlay.reset(); break;
        case LayerEffectKind::InnerShadow: innerShadow.reset(); break;
        case LayerEffectKind::OuterGlow: outerGlow.reset(); break;
        case LayerEffectKind::InnerGlow: innerGlow.reset(); break;
        }
    }
    [[nodiscard]] QVector<LayerEffectKind> kinds() const {
        QVector<LayerEffectKind> list;
        if (stroke) list.append(LayerEffectKind::Stroke);
        if (shadow) list.append(LayerEffectKind::DropShadow);
        if (colorOverlay) list.append(LayerEffectKind::ColorOverlay);
        if (innerShadow) list.append(LayerEffectKind::InnerShadow);
        if (outerGlow) list.append(LayerEffectKind::OuterGlow);
        if (innerGlow) list.append(LayerEffectKind::InnerGlow);
        return list;
    }
    [[nodiscard]] LayerEffects visible() const {
        LayerEffects v;
        if (stroke && stroke->isEnabled()) v.stroke = stroke;
        if (shadow && shadow->isEnabled()) v.shadow = shadow;
        if (colorOverlay && colorOverlay->isEnabled()) v.colorOverlay = colorOverlay;
        if (innerShadow && innerShadow->isEnabled()) v.innerShadow = innerShadow;
        if (outerGlow && outerGlow->isEnabled()) v.outerGlow = outerGlow;
        if (innerGlow && innerGlow->isEnabled()) v.innerGlow = innerGlow;
        return v;
    }
    [[nodiscard]] std::optional<QColor> color(LayerEffectKind kind) const {
        switch (kind) {
        case LayerEffectKind::Stroke:
            if (stroke) return QColor::fromRgbF(stroke->red, stroke->green, stroke->blue);
            break;
        case LayerEffectKind::DropShadow:
            if (shadow) return QColor::fromRgbF(shadow->red, shadow->green, shadow->blue);
            break;
        case LayerEffectKind::ColorOverlay:
            if (colorOverlay) return QColor::fromRgbF(colorOverlay->red, colorOverlay->green, colorOverlay->blue);
            break;
        case LayerEffectKind::InnerShadow:
            if (innerShadow) return QColor::fromRgbF(innerShadow->red, innerShadow->green, innerShadow->blue);
            break;
        case LayerEffectKind::OuterGlow:
            if (outerGlow) return QColor::fromRgbF(outerGlow->red, outerGlow->green, outerGlow->blue);
            break;
        case LayerEffectKind::InnerGlow:
            if (innerGlow) return QColor::fromRgbF(innerGlow->red, innerGlow->green, innerGlow->blue);
            break;
        }
        return std::nullopt;
    }
    void setColor(LayerEffectKind kind, const QColor &c) {
        const double r = c.redF(), g = c.greenF(), b = c.blueF();
        switch (kind) {
        case LayerEffectKind::Stroke:
            if (stroke) { stroke->red = r; stroke->green = g; stroke->blue = b; }
            break;
        case LayerEffectKind::DropShadow:
            if (shadow) { shadow->red = r; shadow->green = g; shadow->blue = b; }
            break;
        case LayerEffectKind::ColorOverlay:
            if (colorOverlay) { colorOverlay->red = r; colorOverlay->green = g; colorOverlay->blue = b; }
            break;
        case LayerEffectKind::InnerShadow:
            if (innerShadow) { innerShadow->red = r; innerShadow->green = g; innerShadow->blue = b; }
            break;
        case LayerEffectKind::OuterGlow:
            if (outerGlow) { outerGlow->red = r; outerGlow->green = g; outerGlow->blue = b; }
            break;
        case LayerEffectKind::InnerGlow:
            if (innerGlow) { innerGlow->red = r; innerGlow->green = g; innerGlow->blue = b; }
            break;
        }
    }
    bool operator==(const LayerEffects &) const = default;
};

enum class TextAlignment { Left, Center, Right };

struct TextStyle {
    QString content = QStringLiteral("Text");
    QString fontName = QStringLiteral("Helvetica");
    double fontSize = 72.0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    TextAlignment alignment = TextAlignment::Left;
    double tracking = 0.0;
    double leading = 0.0;
    std::optional<QSizeF> boxSize;

    [[nodiscard]] bool isValid() const {
        if (content.size() > 100000) return false;
        if (!std::isfinite(fontSize) || fontSize < 1.0 || fontSize > 2000.0) return false;
        if (!std::isfinite(red) || red < 0.0 || red > 1.0) return false;
        if (!std::isfinite(green) || green < 0.0 || green > 1.0) return false;
        if (!std::isfinite(blue) || blue < 0.0 || blue > 1.0) return false;
        if (!std::isfinite(tracking) || tracking < -100.0 || tracking > 1000.0) return false;
        if (!std::isfinite(leading) || leading < 0.0 || leading > 5000.0) return false;
        if (boxSize) {
            const double w = boxSize->width();
            const double h = boxSize->height();
            if (!std::isfinite(w) || !std::isfinite(h) || w < 16.0 || w > 30000.0 || h < 16.0 || h > 30000.0) return false;
            if (w * h > 100000000.0) return false;
        }
        return true;
    }
    bool operator==(const TextStyle &) const = default;
};

enum class ShapeKind { Rectangle, Ellipse, Line };

struct LayerShapeStyle {
    ShapeKind kind = ShapeKind::Rectangle;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double cornerRadius = 0.0;
    std::optional<double> lineWidth;
    std::optional<QPointF> start;
    std::optional<QPointF> end;

    [[nodiscard]] bool isValid() const {
        if (!std::isfinite(red) || red < 0.0 || red > 1.0) return false;
        if (!std::isfinite(green) || green < 0.0 || green > 1.0) return false;
        if (!std::isfinite(blue) || blue < 0.0 || blue > 1.0) return false;
        if (!std::isfinite(cornerRadius) || cornerRadius < 0.0) return false;
        if (kind == ShapeKind::Line) {
            if (!lineWidth || !std::isfinite(*lineWidth) || *lineWidth < 0.0 || *lineWidth > 500.0) return false;
            if (!start || !std::isfinite(start->x()) || !std::isfinite(start->y())) return false;
            if (!end || !std::isfinite(end->x()) || !std::isfinite(end->y())) return false;
        }
        return true;
    }
    bool operator==(const LayerShapeStyle &) const = default;
};

enum class AdjustmentKind {
    HueSaturation,
    Levels,
    Curves,
    Exposure,
    GradientMap,
    Grain,
    Invert,
    BlackWhite,
    ColorBalance,
    // Version 9 additions
    GaussianBlur,
    MotionBlur,
    AddNoise
};

struct LayerTransform {
    QPointF origin;
    QSizeF size;
    double rotation = 0.0;
    bool flipX = false;
    bool flipY = false;
    Sampling sampling = Sampling::HighQuality;

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] QPointF center() const;
    [[nodiscard]] LayerTransform following(const LayerTransform &oldPlacement, const LayerTransform &newPlacement) const;
    [[nodiscard]] bool samePlacement(const LayerTransform &other) const;
    bool operator==(const LayerTransform &) const = default;
};

struct Layer {
    QUuid id;
    QString name;
    bool visible = true;
    LayerTransform transform;
    QString imageFile;
    QImage image;
    std::optional<QUuid> parentId;
    bool group = false;
    double opacity = 1.0;
    BlendMode blendMode = BlendMode::Normal;
    QString maskFile;
    QImage mask;
    bool maskEnabled = true;
    std::optional<LayerTransform> maskPlacement;
    bool maskLinked = true;
    std::optional<QUuid> maskSourceId;
    QJsonObject adjustment;
    QJsonObject shape;
    std::optional<LayerEffects> effects;
    std::optional<TextStyle> text;
    std::optional<LayerShapeStyle> shapeStyle;

    bool operator==(const Layer &) const = default;
};

struct Document {
    int formatVersion = 0;
    QUuid id;
    quint64 generation = 0;
    QSize canvasSize;
    double resolution = 72.0;
    std::optional<QUuid> activeLayerId;
    QVector<Layer> layers; // Bottom to top, matching the project manifest.
    QVector<CanvasGuide> guides;
    /// Canvas-sized grayscale coverage. Null means no selection; an all-black image is an explicit empty selection.
    std::optional<QImage> selection;
    QString projectPath;

    bool operator==(const Document &other) const {
        return formatVersion == other.formatVersion
            && id == other.id
            && canvasSize == other.canvasSize
            && resolution == other.resolution
            && activeLayerId == other.activeLayerId
            && layers == other.layers
            && guides == other.guides
            && selection == other.selection
            && projectPath == other.projectPath;
    }
};

[[nodiscard]] Sampling samplingFromString(const QString &value);
[[nodiscard]] QString samplingToString(Sampling value);

[[nodiscard]] std::optional<BlendMode> blendModeFromString(const QString &value);
[[nodiscard]] QString blendModeToString(BlendMode mode);
[[nodiscard]] QPainter::CompositionMode compositionMode(BlendMode mode);

[[nodiscard]] QString guideAxisToString(CanvasGuide::Axis axis);
[[nodiscard]] std::optional<CanvasGuide::Axis> guideAxisFromString(const QString &value);

[[nodiscard]] QString textAlignmentToString(TextAlignment align);
[[nodiscard]] std::optional<TextAlignment> textAlignmentFromString(const QString &value);

[[nodiscard]] QString shapeKindToString(ShapeKind kind);
[[nodiscard]] std::optional<ShapeKind> shapeKindFromString(const QString &value);

[[nodiscard]] QString adjustmentKindToString(AdjustmentKind kind);
[[nodiscard]] std::optional<AdjustmentKind> adjustmentKindFromString(const QString &value);
[[nodiscard]] bool isVersion9Adjustment(AdjustmentKind kind);

} // namespace compositor
