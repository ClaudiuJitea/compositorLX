#include "core/Document.h"

#include <QPainter>

#include <cmath>

namespace compositor {

bool LayerTransform::isValid() const
{
    return std::isfinite(origin.x()) && std::isfinite(origin.y())
        && std::isfinite(size.width()) && std::isfinite(size.height())
        && std::isfinite(rotation)
        && size.width() >= 1.0 && size.width() <= 300000.0
        && size.height() >= 1.0 && size.height() <= 300000.0
        && std::abs(origin.x()) <= 1000000.0 && std::abs(origin.y()) <= 1000000.0;
}

QPointF LayerTransform::center() const
{
    return origin + QPointF(size.width() / 2.0, size.height() / 2.0);
}

static QTransform unitToDocument(const LayerTransform &value)
{
    QTransform map;
    map.translate(value.center().x(), value.center().y());
    map.rotate(value.rotation);
    map.scale((value.flipX ? -1 : 1) * value.size.width(), (value.flipY ? -1 : 1) * value.size.height());
    map.translate(-0.5, -0.5);
    return map;
}

LayerTransform LayerTransform::following(const LayerTransform &oldPlacement, const LayerTransform &newPlacement) const
{
    if (oldPlacement == newPlacement) return *this;
    if (oldPlacement.size == newPlacement.size && oldPlacement.rotation == newPlacement.rotation
        && oldPlacement.flipX == newPlacement.flipX && oldPlacement.flipY == newPlacement.flipY) {
        LayerTransform moved = *this;
        moved.origin += newPlacement.origin - oldPlacement.origin;
        return moved;
    }
    bool ok = false;
    const QTransform carry = unitToDocument(oldPlacement).inverted(&ok) * unitToDocument(newPlacement);
    if (!ok) return *this;
    const QTransform map = unitToDocument(*this) * carry;
    const QPointF middle = map.map(QPointF(0.5, 0.5));
    const QPointF x0 = map.map(QPointF(0, 0.5)), x1 = map.map(QPointF(1, 0.5));
    const QPointF y0 = map.map(QPointF(0.5, 0)), y1 = map.map(QPointF(0.5, 1));
    LayerTransform result = *this;
    result.size = QSizeF(QLineF(x0, x1).length(), QLineF(y0, y1).length());
    result.rotation = std::atan2(x1.y() - x0.y(), x1.x() - x0.x()) * 180 / M_PI + (flipX ? 180 : 0);
    result.origin = middle - QPointF(result.size.width() / 2.0, result.size.height() / 2.0);
    return result;
}

bool LayerTransform::samePlacement(const LayerTransform &other) const
{
    return origin == other.origin && size == other.size && rotation == other.rotation
        && flipX == other.flipX && flipY == other.flipY;
}

Sampling samplingFromString(const QString &value)
{
    if (value == QStringLiteral("Nearest")) return Sampling::Nearest;
    if (value == QStringLiteral("Smooth")) return Sampling::Smooth;
    return Sampling::HighQuality;
}

QString samplingToString(Sampling value)
{
    switch (value) {
    case Sampling::Nearest: return QStringLiteral("Nearest");
    case Sampling::Smooth: return QStringLiteral("Smooth");
    case Sampling::HighQuality: return QStringLiteral("High quality");
    }
    return QStringLiteral("High quality");
}

std::optional<BlendMode> blendModeFromString(const QString &value)
{
    if (value == QStringLiteral("Normal")) return BlendMode::Normal;
    if (value == QStringLiteral("Darken")) return BlendMode::Darken;
    if (value == QStringLiteral("Multiply")) return BlendMode::Multiply;
    if (value == QStringLiteral("Color Burn")) return BlendMode::ColorBurn;
    if (value == QStringLiteral("Linear Burn")) return BlendMode::LinearBurn;
    if (value == QStringLiteral("Lighten")) return BlendMode::Lighten;
    if (value == QStringLiteral("Screen")) return BlendMode::Screen;
    if (value == QStringLiteral("Color Dodge")) return BlendMode::ColorDodge;
    if (value == QStringLiteral("Linear Dodge (Add)")) return BlendMode::LinearDodge;
    if (value == QStringLiteral("Overlay")) return BlendMode::Overlay;
    if (value == QStringLiteral("Soft Light")) return BlendMode::SoftLight;
    if (value == QStringLiteral("Hard Light")) return BlendMode::HardLight;
    if (value == QStringLiteral("Vivid Light")) return BlendMode::VividLight;
    if (value == QStringLiteral("Linear Light")) return BlendMode::LinearLight;
    if (value == QStringLiteral("Pin Light")) return BlendMode::PinLight;
    if (value == QStringLiteral("Hard Mix")) return BlendMode::HardMix;
    if (value == QStringLiteral("Difference")) return BlendMode::Difference;
    if (value == QStringLiteral("Exclusion")) return BlendMode::Exclusion;
    if (value == QStringLiteral("Subtract")) return BlendMode::Subtract;
    if (value == QStringLiteral("Divide")) return BlendMode::Divide;
    if (value == QStringLiteral("Hue")) return BlendMode::Hue;
    if (value == QStringLiteral("Saturation")) return BlendMode::Saturation;
    if (value == QStringLiteral("Color")) return BlendMode::Color;
    if (value == QStringLiteral("Luminosity")) return BlendMode::Luminosity;
    return std::nullopt;
}

QString blendModeToString(BlendMode mode)
{
    switch (mode) {
    case BlendMode::Normal: return QStringLiteral("Normal");
    case BlendMode::Darken: return QStringLiteral("Darken");
    case BlendMode::Multiply: return QStringLiteral("Multiply");
    case BlendMode::ColorBurn: return QStringLiteral("Color Burn");
    case BlendMode::LinearBurn: return QStringLiteral("Linear Burn");
    case BlendMode::Lighten: return QStringLiteral("Lighten");
    case BlendMode::Screen: return QStringLiteral("Screen");
    case BlendMode::ColorDodge: return QStringLiteral("Color Dodge");
    case BlendMode::LinearDodge: return QStringLiteral("Linear Dodge (Add)");
    case BlendMode::Overlay: return QStringLiteral("Overlay");
    case BlendMode::SoftLight: return QStringLiteral("Soft Light");
    case BlendMode::HardLight: return QStringLiteral("Hard Light");
    case BlendMode::VividLight: return QStringLiteral("Vivid Light");
    case BlendMode::LinearLight: return QStringLiteral("Linear Light");
    case BlendMode::PinLight: return QStringLiteral("Pin Light");
    case BlendMode::HardMix: return QStringLiteral("Hard Mix");
    case BlendMode::Difference: return QStringLiteral("Difference");
    case BlendMode::Exclusion: return QStringLiteral("Exclusion");
    case BlendMode::Subtract: return QStringLiteral("Subtract");
    case BlendMode::Divide: return QStringLiteral("Divide");
    case BlendMode::Hue: return QStringLiteral("Hue");
    case BlendMode::Saturation: return QStringLiteral("Saturation");
    case BlendMode::Color: return QStringLiteral("Color");
    case BlendMode::Luminosity: return QStringLiteral("Luminosity");
    }
    return QStringLiteral("Normal");
}

QPainter::CompositionMode compositionMode(BlendMode mode)
{
    switch (mode) {
    case BlendMode::Multiply: return QPainter::CompositionMode_Multiply;
    case BlendMode::Screen: return QPainter::CompositionMode_Screen;
    case BlendMode::Overlay: return QPainter::CompositionMode_Overlay;
    case BlendMode::Darken: return QPainter::CompositionMode_Darken;
    case BlendMode::Lighten: return QPainter::CompositionMode_Lighten;
    case BlendMode::Difference: return QPainter::CompositionMode_Difference;
    case BlendMode::Exclusion: return QPainter::CompositionMode_Exclusion;
    case BlendMode::ColorDodge: return QPainter::CompositionMode_ColorDodge;
    case BlendMode::ColorBurn: return QPainter::CompositionMode_ColorBurn;
    case BlendMode::SoftLight: return QPainter::CompositionMode_SoftLight;
    case BlendMode::HardLight: return QPainter::CompositionMode_HardLight;
    case BlendMode::Hue:
    case BlendMode::Saturation:
    case BlendMode::Color:
    case BlendMode::Luminosity:
    case BlendMode::LinearBurn:
    case BlendMode::LinearDodge:
    case BlendMode::VividLight:
    case BlendMode::LinearLight:
    case BlendMode::PinLight:
    case BlendMode::HardMix:
    case BlendMode::Subtract:
    case BlendMode::Divide:
    case BlendMode::Normal:
        return QPainter::CompositionMode_SourceOver;
    }
    return QPainter::CompositionMode_SourceOver;
}

QString guideAxisToString(CanvasGuide::Axis axis)
{
    return axis == CanvasGuide::Axis::Horizontal ? QStringLiteral("horizontal") : QStringLiteral("vertical");
}

std::optional<CanvasGuide::Axis> guideAxisFromString(const QString &value)
{
    if (value == QStringLiteral("horizontal")) return CanvasGuide::Axis::Horizontal;
    if (value == QStringLiteral("vertical")) return CanvasGuide::Axis::Vertical;
    return std::nullopt;
}

QString textAlignmentToString(TextAlignment align)
{
    switch (align) {
    case TextAlignment::Left: return QStringLiteral("Left");
    case TextAlignment::Center: return QStringLiteral("Center");
    case TextAlignment::Right: return QStringLiteral("Right");
    }
    return QStringLiteral("Left");
}

std::optional<TextAlignment> textAlignmentFromString(const QString &value)
{
    if (value == QStringLiteral("Left")) return TextAlignment::Left;
    if (value == QStringLiteral("Center")) return TextAlignment::Center;
    if (value == QStringLiteral("Right")) return TextAlignment::Right;
    return std::nullopt;
}

QString shapeKindToString(ShapeKind kind)
{
    switch (kind) {
    case ShapeKind::Rectangle: return QStringLiteral("Rectangle");
    case ShapeKind::Ellipse: return QStringLiteral("Ellipse");
    case ShapeKind::Line: return QStringLiteral("Line");
    }
    return QStringLiteral("Rectangle");
}

std::optional<ShapeKind> shapeKindFromString(const QString &value)
{
    if (value == QStringLiteral("Rectangle")) return ShapeKind::Rectangle;
    if (value == QStringLiteral("Ellipse")) return ShapeKind::Ellipse;
    if (value == QStringLiteral("Line")) return ShapeKind::Line;
    return std::nullopt;
}

QString adjustmentKindToString(AdjustmentKind kind)
{
    switch (kind) {
    case AdjustmentKind::HueSaturation: return QStringLiteral("Hue/Saturation");
    case AdjustmentKind::Levels: return QStringLiteral("Levels");
    case AdjustmentKind::Curves: return QStringLiteral("Curves");
    case AdjustmentKind::Exposure: return QStringLiteral("Exposure");
    case AdjustmentKind::GradientMap: return QStringLiteral("Gradient Map");
    case AdjustmentKind::Grain: return QStringLiteral("Grain");
    case AdjustmentKind::Invert: return QStringLiteral("Invert");
    case AdjustmentKind::BlackWhite: return QStringLiteral("Black & White");
    case AdjustmentKind::ColorBalance: return QStringLiteral("Color Balance");
    case AdjustmentKind::GaussianBlur: return QStringLiteral("Gaussian Blur");
    case AdjustmentKind::MotionBlur: return QStringLiteral("Motion Blur");
    case AdjustmentKind::AddNoise: return QStringLiteral("Add Noise");
    }
    return QStringLiteral("Hue/Saturation");
}

std::optional<AdjustmentKind> adjustmentKindFromString(const QString &value)
{
    if (value == QStringLiteral("Hue/Saturation")) return AdjustmentKind::HueSaturation;
    if (value == QStringLiteral("Levels")) return AdjustmentKind::Levels;
    if (value == QStringLiteral("Curves")) return AdjustmentKind::Curves;
    if (value == QStringLiteral("Exposure")) return AdjustmentKind::Exposure;
    if (value == QStringLiteral("Gradient Map")) return AdjustmentKind::GradientMap;
    if (value == QStringLiteral("Grain")) return AdjustmentKind::Grain;
    if (value == QStringLiteral("Invert")) return AdjustmentKind::Invert;
    if (value == QStringLiteral("Black & White")) return AdjustmentKind::BlackWhite;
    if (value == QStringLiteral("Color Balance")) return AdjustmentKind::ColorBalance;
    if (value == QStringLiteral("Gaussian Blur")) return AdjustmentKind::GaussianBlur;
    if (value == QStringLiteral("Motion Blur")) return AdjustmentKind::MotionBlur;
    if (value == QStringLiteral("Add Noise")) return AdjustmentKind::AddNoise;
    return std::nullopt;
}

bool isVersion9Adjustment(AdjustmentKind kind)
{
    return kind == AdjustmentKind::GaussianBlur || kind == AdjustmentKind::MotionBlur || kind == AdjustmentKind::AddNoise;
}

QString layerEffectKindToString(LayerEffectKind kind)
{
    switch (kind) {
    case LayerEffectKind::Stroke: return QStringLiteral("Stroke");
    case LayerEffectKind::DropShadow: return QStringLiteral("Drop Shadow");
    case LayerEffectKind::ColorOverlay: return QStringLiteral("Color Overlay");
    case LayerEffectKind::InnerShadow: return QStringLiteral("Inner Shadow");
    case LayerEffectKind::OuterGlow: return QStringLiteral("Outer Glow");
    case LayerEffectKind::InnerGlow: return QStringLiteral("Inner Glow");
    }
    return QStringLiteral("Stroke");
}

std::optional<LayerEffectKind> layerEffectKindFromString(const QString &value)
{
    if (value == QStringLiteral("Stroke") || value == QStringLiteral("stroke")) return LayerEffectKind::Stroke;
    if (value == QStringLiteral("Drop Shadow") || value == QStringLiteral("shadow")) return LayerEffectKind::DropShadow;
    if (value == QStringLiteral("Color Overlay") || value == QStringLiteral("colorOverlay")) return LayerEffectKind::ColorOverlay;
    if (value == QStringLiteral("Inner Shadow") || value == QStringLiteral("innerShadow")) return LayerEffectKind::InnerShadow;
    if (value == QStringLiteral("Outer Glow") || value == QStringLiteral("outerGlow")) return LayerEffectKind::OuterGlow;
    if (value == QStringLiteral("Inner Glow") || value == QStringLiteral("innerGlow")) return LayerEffectKind::InnerGlow;
    return std::nullopt;
}

} // namespace compositor
