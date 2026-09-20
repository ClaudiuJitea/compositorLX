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
    QTransform map;map.translate(value.center().x(),value.center().y());map.rotate(value.rotation);map.scale((value.flipX?-1:1)*value.size.width(),(value.flipY?-1:1)*value.size.height());map.translate(-.5,-.5);return map;
}

LayerTransform LayerTransform::following(const LayerTransform &oldPlacement,const LayerTransform &newPlacement) const
{
    if(oldPlacement==newPlacement)return *this;
    if(oldPlacement.size==newPlacement.size&&oldPlacement.rotation==newPlacement.rotation&&oldPlacement.flipX==newPlacement.flipX&&oldPlacement.flipY==newPlacement.flipY){LayerTransform moved=*this;moved.origin+=newPlacement.origin-oldPlacement.origin;return moved;}
    bool ok=false;const QTransform carry=unitToDocument(oldPlacement).inverted(&ok)*unitToDocument(newPlacement);if(!ok)return *this;const QTransform map=unitToDocument(*this)*carry;const QPointF middle=map.map(QPointF(.5,.5));const QPointF x0=map.map(QPointF(0,.5)),x1=map.map(QPointF(1,.5)),y0=map.map(QPointF(.5,0)),y1=map.map(QPointF(.5,1));LayerTransform result=*this;result.size=QSizeF(QLineF(x0,x1).length(),QLineF(y0,y1).length());result.rotation=std::atan2(x1.y()-x0.y(),x1.x()-x0.x())*180/M_PI+(flipX?180:0);result.origin=middle-QPointF(result.size.width()/2,result.size.height()/2);return result;
}

bool LayerTransform::samePlacement(const LayerTransform &other) const
{
    return origin==other.origin&&size==other.size&&rotation==other.rotation&&flipX==other.flipX&&flipY==other.flipY;
}

Sampling samplingFromString(const QString &value)
{
    if (value == QStringLiteral("Nearest")) {
        return Sampling::Nearest;
    }
    if (value == QStringLiteral("Smooth")) {
        return Sampling::Smooth;
    }
    return Sampling::HighQuality;
}

BlendMode blendModeFromString(const QString &value)
{
    if (value == QStringLiteral("Multiply")) return BlendMode::Multiply;
    if (value == QStringLiteral("Screen")) return BlendMode::Screen;
    if (value == QStringLiteral("Overlay")) return BlendMode::Overlay;
    if (value == QStringLiteral("Darken")) return BlendMode::Darken;
    if (value == QStringLiteral("Lighten")) return BlendMode::Lighten;
    if (value == QStringLiteral("Difference")) return BlendMode::Difference;
    if (value == QStringLiteral("Color Dodge")) return BlendMode::ColorDodge;
    if (value == QStringLiteral("Color Burn")) return BlendMode::ColorBurn;
    if (value == QStringLiteral("Hue")) return BlendMode::Hue;
    if (value == QStringLiteral("Saturation")) return BlendMode::Saturation;
    if (value == QStringLiteral("Color")) return BlendMode::Color;
    if (value == QStringLiteral("Luminosity")) return BlendMode::Luminosity;
    return BlendMode::Normal;
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
    case BlendMode::ColorDodge: return QPainter::CompositionMode_ColorDodge;
    case BlendMode::ColorBurn: return QPainter::CompositionMode_ColorBurn;
    case BlendMode::Hue: return QPainter::CompositionMode_SourceOver;
    case BlendMode::Saturation: return QPainter::CompositionMode_SourceOver;
    case BlendMode::Color: return QPainter::CompositionMode_SourceOver;
    case BlendMode::Luminosity: return QPainter::CompositionMode_SourceOver;
    case BlendMode::Normal: return QPainter::CompositionMode_SourceOver;
    }
    return QPainter::CompositionMode_SourceOver;
}

} // namespace compositor
