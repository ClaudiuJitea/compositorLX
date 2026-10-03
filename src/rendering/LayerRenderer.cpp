#include "rendering/LayerRenderer.h"
#include "rendering/LayerEffectsRenderer.h"
#include "rendering/RasterOperations.h"

#include <QPainter>
#include <QSet>

#include <algorithm>
#include <array>
#include <functional>

namespace compositor {

QImage LayerRenderer::applyMask(const QImage &image, const QImage &mask, Sampling sampling)
{
    if (image.isNull() || mask.isNull()) return image;
    QImage result = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const QImage coverage = mask.scaled(result.size(), Qt::IgnoreAspectRatio,
                                        sampling == Sampling::Nearest
                                            ? Qt::FastTransformation : Qt::SmoothTransformation)
                                .convertToFormat(QImage::Format_RGBA64);
    QImage alpha(result.size(), QImage::Format_RGBA64_Premultiplied);
    alpha.fill(Qt::transparent);
    for (int y = 0; y < coverage.height(); ++y) {
        const auto *source = reinterpret_cast<const QRgba64 *>(coverage.constScanLine(y));
        auto *destination = reinterpret_cast<QRgba64 *>(alpha.scanLine(y));
        for (int x = 0; x < coverage.width(); ++x) {
            const quint16 gray = quint16((quint32(source[x].red()) * 299U
                                        + quint32(source[x].green()) * 587U
                                        + quint32(source[x].blue()) * 114U) / 1000U);
            destination[x] = QRgba64::fromRgba64(gray, gray, gray, gray);
        }
    }
    QPainter maskPainter(&result);
    maskPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    maskPainter.drawImage(result.rect(), alpha);
    return result;
}

bool LayerRenderer::effectivelyVisible(const Document &document, const Layer &layer)
{
    if (!layer.visible) return false;
    QSet<QUuid> visited{layer.id};
    std::optional<QUuid> parent = layer.parentId;
    while (parent) {
        if (visited.contains(*parent)) return false;
        visited.insert(*parent);
        const auto it = std::find_if(document.layers.cbegin(), document.layers.cend(), [&](const Layer &candidate) {
            return candidate.id == *parent;
        });
        if (it == document.layers.cend() || !it->visible) return false;
        parent = it->parentId;
    }
    return true;
}

QTransform LayerRenderer::pixelToDocument(const LayerTransform &placement,const QSize &pixels)
{
    QTransform result;result.translate(placement.center().x(),placement.center().y());result.rotate(placement.rotation);result.scale((placement.flipX?-1:1)*placement.size.width()/pixels.width(),(placement.flipY?-1:1)*placement.size.height()/pixels.height());result.translate(-pixels.width()/2.0,-pixels.height()/2.0);return result;
}

QImage LayerRenderer::placedMask(const Layer &owner,const Layer &target,const QSize &size)
{
    if(owner.mask.isNull())return {};
    const QImage source=owner.mask.convertToFormat(QImage::Format_Grayscale8);const LayerTransform placement=owner.maskPlacement.value_or(owner.transform);
    if(owner.id==target.id&&!owner.maskPlacement&&owner.transform==target.transform)return source.scaled(size,Qt::IgnoreAspectRatio,target.transform.sampling==Sampling::Nearest?Qt::FastTransformation:Qt::SmoothTransformation);
    int edgeTotal=0,edgeCount=0;for(int y=0;y<source.height();++y)for(int x=0;x<source.width();++x)if(x==0||y==0||x+1==source.width()||y+1==source.height()){edgeTotal+=source.constScanLine(y)[x];++edgeCount;}const int background=edgeCount&&edgeTotal*2<edgeCount*255?0:255;
    QImage result(size,QImage::Format_Grayscale8);result.fill(background);bool ok=false;const QTransform toMask=pixelToDocument(placement,source.size()).inverted(&ok);if(!ok)return result;const QTransform toDocument=pixelToDocument(target.transform,size);
    for(int y=0;y<size.height();++y){uchar *out=result.scanLine(y);for(int x=0;x<size.width();++x){const QPointF sample=toMask.map(toDocument.map(QPointF(x+.5,y+.5)));const int sx=qFloor(sample.x()),sy=qFloor(sample.y());if(QRect(QPoint(),source.size()).contains(sx,sy))out[x]=source.constScanLine(sy)[sx];}}
    return result;
}



// Folders are pass-through (mac LayerGroups.swift, LayerOpacity): a folder's opacity multiplies into every layer inside
// it, folders further out included, and nothing inside is composited as a unit.
static double ancestorFactor(const Document &document, const Layer &layer)
{
    double factor = 1.0;
    std::optional<QUuid> parent = layer.parentId;
    for (int depth = 0; parent && depth < 64; ++depth) {
        const auto it = std::find_if(document.layers.cbegin(), document.layers.cend(), [&](const Layer &candidate) {
            return candidate.id == *parent;
        });
        if (it == document.layers.cend()) break;
        factor *= it->opacity;
        parent = it->parentId;
    }
    return factor;
}

static void drawLayer(QPainter &painter, const Document &document,const Layer &layer, QPainter::CompositionMode mode, double factor = -1)
{
    painter.save();
    painter.setOpacity(layer.opacity * (factor < 0 ? ancestorFactor(document, layer) : factor));
    painter.setCompositionMode(mode);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, layer.transform.sampling != Sampling::Nearest);
    const QImage mask = (layer.maskEnabled && !layer.mask.isNull())
                            ? LayerRenderer::placedMask(layer, layer, layer.image.size())
                            : QImage();

    if (layer.effects && !layer.effects->visible().isEmpty() && layer.effects->isValid()) {
        const auto [effectsImage, inset] = LayerEffectsRenderer::cached(layer.image, mask, *layer.effects);
        if (!effectsImage.isNull()) {
            const LayerTransform grown = LayerEffectsRenderer::placed(layer.transform, effectsImage.size(), inset);
            const QPointF center = grown.center();
            painter.translate(center);
            painter.rotate(grown.rotation);
            painter.scale(grown.flipX ? -1.0 : 1.0, grown.flipY ? -1.0 : 1.0);
            const QRectF bounds(-grown.size.width() / 2.0, -grown.size.height() / 2.0,
                                grown.size.width(), grown.size.height());
            painter.drawImage(bounds, effectsImage);
            painter.restore();
            return;
        }
    }

    const QPointF center = layer.transform.center();
    painter.translate(center);
    painter.rotate(layer.transform.rotation);
    painter.scale(layer.transform.flipX ? -1.0 : 1.0, layer.transform.flipY ? -1.0 : 1.0);
    const QRectF bounds(-layer.transform.size.width() / 2.0, -layer.transform.size.height() / 2.0,
                        layer.transform.size.width(), layer.transform.size.height());
    const QImage source = mask.isNull() ? layer.image : LayerRenderer::applyMask(layer.image, mask, layer.transform.sampling);
    painter.drawImage(bounds, source);
    painter.restore();
}

static bool drawable(const Layer &layer)
{
    return !layer.group && !layer.image.isNull() && layer.adjustment.isEmpty();
}

static bool customMode(BlendMode mode)
{
    return mode == BlendMode::ColorDodge
        || mode == BlendMode::ColorBurn
        || mode == BlendMode::LinearBurn
        || mode == BlendMode::LinearDodge
        || mode == BlendMode::VividLight
        || mode == BlendMode::LinearLight
        || mode == BlendMode::PinLight
        || mode == BlendMode::HardMix
        || mode == BlendMode::SoftLight
        || mode == BlendMode::Subtract
        || mode == BlendMode::Divide
        || mode == BlendMode::Hue
        || mode == BlendMode::Saturation
        || mode == BlendMode::Color
        || mode == BlendMode::Luminosity;
}
static double luminance(const std::array<double,3>&c){return .3*c[0]+.59*c[1]+.11*c[2];}
static double saturation(const std::array<double,3>&c){return *std::max_element(c.begin(),c.end())-*std::min_element(c.begin(),c.end());}
static std::array<double,3> setLum(std::array<double,3> c,double value){const double d=value-luminance(c);for(double &v:c)v+=d;const double low=*std::min_element(c.begin(),c.end()),high=*std::max_element(c.begin(),c.end());const double l=luminance(c);if(low<0)for(double &v:c)v=l+(v-l)*l/(l-low);if(high>1)for(double &v:c)v=l+(v-l)*(1-l)/(high-l);for(double &v:c)v=std::clamp(v,0.0,1.0);return c;}
static std::array<double,3> setSat(std::array<double,3> c,double value){std::array<int,3> order{0,1,2};std::sort(order.begin(),order.end(),[&](int a,int b){return c[a]<c[b];});const int low=order[0],mid=order[1],high=order[2];if(c[high]>c[low]){c[mid]=(c[mid]-c[low])*value/(c[high]-c[low]);c[high]=value;}else c[mid]=c[high]=0;c[low]=0;return c;}
static std::array<double,3> blendColor(std::array<double,3> backdrop, std::array<double,3> source, BlendMode mode)
{
    if (mode == BlendMode::Hue) return setLum(setSat(source, saturation(backdrop)), luminance(backdrop));
    if (mode == BlendMode::Saturation) return setLum(setSat(backdrop, saturation(source)), luminance(backdrop));
    if (mode == BlendMode::Color) return setLum(source, luminance(backdrop));
    if (mode == BlendMode::Luminosity) return setLum(backdrop, luminance(source));

    for (int c = 0; c < 3; ++c) {
        switch (mode) {
        case BlendMode::ColorDodge:
            source[c] = (source[c] >= 1.0) ? 1.0 : std::min(1.0, backdrop[c] / (1.0 - source[c]));
            break;
        case BlendMode::ColorBurn:
            source[c] = (source[c] <= 0.0) ? 0.0 : std::max(0.0, 1.0 - std::min(1.0, (1.0 - backdrop[c]) / source[c]));
            break;
        case BlendMode::LinearBurn:
            source[c] = std::max(0.0, backdrop[c] + source[c] - 1.0);
            break;
        case BlendMode::LinearDodge:
            source[c] = std::min(1.0, backdrop[c] + source[c]);
            break;
        case BlendMode::Subtract:
            source[c] = std::max(0.0, backdrop[c] - source[c]);
            break;
        case BlendMode::Divide:
            source[c] = (source[c] <= 0.0) ? ((backdrop[c] > 0.0) ? 1.0 : 0.0) : std::min(1.0, backdrop[c] / source[c]);
            break;
        case BlendMode::VividLight:
            if (source[c] <= 0.5) {
                const double s2 = 2.0 * source[c];
                source[c] = (s2 <= 0.0) ? 0.0 : std::max(0.0, 1.0 - std::min(1.0, (1.0 - backdrop[c]) / s2));
            } else {
                const double s2 = 2.0 * (source[c] - 0.5);
                source[c] = (s2 >= 1.0) ? 1.0 : std::min(1.0, backdrop[c] / (1.0 - s2));
            }
            break;
        case BlendMode::LinearLight:
            source[c] = std::clamp(backdrop[c] + 2.0 * source[c] - 1.0, 0.0, 1.0);
            break;
        case BlendMode::PinLight:
            if (source[c] > 0.5) {
                source[c] = std::max(backdrop[c], 2.0 * (source[c] - 0.5));
            } else {
                source[c] = std::min(backdrop[c], 2.0 * source[c]);
            }
            break;
        case BlendMode::HardMix:
            source[c] = (backdrop[c] + source[c] >= 1.0) ? 1.0 : 0.0;
            break;
        case BlendMode::SoftLight: {
            // Photoshop's / the PDF formula. QPainter's SoftLight runs up to ~25 levels lighter with a light source
            // color, so the same layer would not match a Photoshop or macOS render (mac 5a8f6ce).
            const double cb = backdrop[c], cs = source[c];
            if (cs <= 0.5) source[c] = cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb);
            else {
                const double d = cb <= 0.25 ? ((16.0 * cb - 12.0) * cb + 4.0) * cb : std::sqrt(cb);
                source[c] = cb + (2.0 * cs - 1.0) * (d - cb);
            }
            break;
        }
        default:
            break;
        }
    }
    return source;
}
static void customComposite(QImage &backdrop,const QImage &source,BlendMode mode)
{
    for(int y=0;y<backdrop.height();++y){uchar *out=backdrop.scanLine(y);const uchar *top=source.constScanLine(y);for(int x=0;x<backdrop.width();++x){uchar *b=out+x*4;const uchar *s=top+x*4;const double ab=b[3]/255.0,as=s[3]/255.0;if(as<=0)continue;std::array<double,3> cb{},cs{};for(int c=0;c<3;++c){cb[c]=ab?b[c]/255.0/ab:0;cs[c]=as?s[c]/255.0/as:0;}const auto mixed=blendColor(cb,cs,mode);const double ao=as+ab-as*ab;for(int c=0;c<3;++c){const double premult=(1-as)*b[c]/255.0+(1-ab)*s[c]/255.0+as*ab*mixed[c];b[c]=uchar(std::clamp(qRound(premult*255),0,255));}b[3]=uchar(std::clamp(qRound(ao*255),0,255));}}
}

static void applyMaskToImage(QImage &image, const QImage &mask);
static void compositeRendered(QImage &target,const Document &document,const Layer &layer,const QImage &clip={})
{
    QImage source(target.size(),QImage::Format_RGBA8888_Premultiplied);source.fill(Qt::transparent);QPainter render(&source);drawLayer(render,document,layer,QPainter::CompositionMode_SourceOver);render.end();if(!clip.isNull())applyMaskToImage(source,clip);customComposite(target,source,layer.blendMode);
}

static QImage adjustedComposite(const QImage &original, const Document &document,const Layer &layer, const QImage &clip = {})
{
    const double effectiveOpacity = layer.opacity * ancestorFactor(document, layer);
    QImage adjusted = RasterOperations::adjustment(original, layer.adjustment);
    if (adjusted.size() != original.size()) return original;
    QImage result = original.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const QImage mask = (layer.maskEnabled && !layer.mask.isNull())
                            ? LayerRenderer::placedMask(layer, layer, result.size())
                            : QImage();
    if (layer.blendMode != BlendMode::Normal) {
        QImage blended = original;
        if(customMode(layer.blendMode)) customComposite(blended,adjusted,layer.blendMode);
        else {
            QPainter blend(&blended);
            blend.setCompositionMode(compositionMode(layer.blendMode));
            blend.drawImage(0,0,adjusted);
            blend.end();
        }
        adjusted = blended;
    }
    for (int y = 0; y < result.height(); ++y) {
        uchar *out = result.scanLine(y);
        const uchar *base = original.constScanLine(y), *top = adjusted.constScanLine(y), *coverage = mask.isNull() ? nullptr : mask.constScanLine(y);
        const uchar *folderClip = clip.isNull() ? nullptr : clip.constScanLine(y);
        for (int x = 0; x < result.width(); ++x) {
            const int amount = std::clamp(qRound(255 * effectiveOpacity * (coverage ? coverage[x] / 255.0 : 1.0) * (folderClip ? folderClip[x] / 255.0 : 1.0)), 0, 255), inverse = 255 - amount;
            for (int c = 0; c < 4; ++c) out[x * 4 + c] = uchar((int(base[x * 4 + c]) * inverse + int(top[x * 4 + c]) * amount + 127) / 255);
        }
    }
    return result;
}

static void applyMaskToImage(QImage &image, const QImage &mask)
{
    if (image.isNull() || mask.isNull()) return;
    const QImage m = mask.scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                         .convertToFormat(QImage::Format_Grayscale8);
    for (int y = 0; y < image.height(); ++y) {
        uchar *pixel = image.scanLine(y);
        const uchar *coverage = m.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const int a = coverage[x];
            uchar *p = pixel + x * 4;
            p[0] = uchar((int(p[0]) * a + 127) / 255);
            p[1] = uchar((int(p[1]) * a + 127) / 255);
            p[2] = uchar((int(p[2]) * a + 127) / 255);
            p[3] = uchar((int(p[3]) * a + 127) / 255);
        }
    }
}

static void applyOpacityToImage(QImage &image, double opacity)
{
    if (image.isNull() || opacity >= 0.999999) return;
    const int op = std::clamp(qRound(opacity * 255.0), 0, 255);
    if (op <= 0) {
        image.fill(Qt::transparent);
        return;
    }
    for (int y = 0; y < image.height(); ++y) {
        uchar *pixel = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            uchar *p = pixel + x * 4;
            p[0] = uchar((int(p[0]) * op + 127) / 255);
            p[1] = uchar((int(p[1]) * op + 127) / 255);
            p[2] = uchar((int(p[2]) * op + 127) / 255);
            p[3] = uchar((int(p[3]) * op + 127) / 255);
        }
    }
}

static QImage layerCoverage(const Document &document, const Layer &layer, QSet<QUuid> visiting)
{
    Layer ownLayer = layer;
    ownLayer.effects = std::nullopt;
    ownLayer.blendMode = BlendMode::Normal;
    QImage image(document.canvasSize, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter own(&image);
        drawLayer(own, document, ownLayer, QPainter::CompositionMode_SourceOver);
    }
    if (!layer.maskSourceId || visiting.contains(layer.id)) return image;
    const auto it = std::find_if(document.layers.cbegin(), document.layers.cend(), [&](const Layer &candidate) {
        return candidate.id == *layer.maskSourceId;
    });
    if (it == document.layers.cend() || !drawable(*it)) {
        image.fill(Qt::transparent);
        return image;
    }
    visiting.insert(layer.id);
    const QImage upstreamCoverage = layerCoverage(document, *it, visiting);
    QPainter clip(&image);
    clip.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    clip.drawImage(0, 0, upstreamCoverage);
    return image;
}

static QImage clippedLayerImage(const Document &document, const Layer &layer, QSet<QUuid> visiting, bool normalizeOwn = false)
{
    Layer ownLayer = layer; if (normalizeOwn) ownLayer.opacity = 1; ownLayer.blendMode = BlendMode::Normal;
    QImage image(document.canvasSize, QImage::Format_RGBA8888_Premultiplied); image.fill(Qt::transparent);
    { QPainter own(&image); drawLayer(own, document, ownLayer, QPainter::CompositionMode_SourceOver, normalizeOwn ? 1.0 : -1.0); }
    if (!layer.maskSourceId || visiting.contains(layer.id)) return image;
    const auto it = std::find_if(document.layers.cbegin(), document.layers.cend(), [&](const Layer &candidate){ return candidate.id == *layer.maskSourceId; });
    if (it == document.layers.cend() || !drawable(*it)) { image.fill(Qt::transparent); return image; }
    visiting.insert(layer.id);
    const QImage coverage = layerCoverage(document, *it, visiting);
    QPainter clip(&image); clip.setCompositionMode(QPainter::CompositionMode_DestinationIn); clip.drawImage(0, 0, coverage); return image;
}

// `clip` is the product of the masks of every enabled folder enclosing the scope (Grayscale8, canvas sized), or null.
// Folders are pass-through: their children draw straight onto `scopeCanvas`, each clipped by the folder masks.
static void renderScope(const Document &document,
                        const std::optional<QUuid> &scopeId,
                        const QSize &canvasSize,
                        QImage &scopeCanvas,
                        const QImage &clip,
                        const std::function<QImage(const Layer &, QSet<QUuid>)> &clippedImage)
{
    const auto clipped = [&clip](QImage image) {
        if (!clip.isNull()) applyMaskToImage(image, clip);
        return image;
    };

    QVector<Layer> children;
    for (const Layer &layer : document.layers) {
        if (layer.parentId == scopeId) {
            children.append(layer);
        }
    }

    for (int i = 0; i < children.size(); ++i) {
        const Layer &base = children.at(i);
        if (!LayerRenderer::effectivelyVisible(document, base)) continue;

        if (base.group) {
            QImage childClip = clip;
            if (base.maskEnabled && !base.mask.isNull()) {
                QImage folderMask = LayerRenderer::placedMask(base, base, canvasSize).convertToFormat(QImage::Format_Grayscale8);
                if (!clip.isNull()) {
                    for (int y = 0; y < canvasSize.height(); ++y) {
                        uchar *m = folderMask.scanLine(y);
                        const uchar *c = clip.constScanLine(y);
                        for (int x = 0; x < canvasSize.width(); ++x) m[x] = uchar((int(m[x]) * c[x] + 127) / 255);
                    }
                }
                childClip = folderMask;
            }
            renderScope(document, base.id, canvasSize, scopeCanvas, childClip, clippedImage);
            continue;
        }

        if (!base.adjustment.isEmpty()) {
            if (!base.maskSourceId) {
                scopeCanvas = adjustedComposite(scopeCanvas, document, base, clip);
            } else {
                const auto it = std::find_if(document.layers.cbegin(), document.layers.cend(), [&](const Layer &candidate){ return candidate.id == *base.maskSourceId; });
                if (it != document.layers.cend() && drawable(*it)) {
                    const QImage baseCov = layerCoverage(document, *it, {});
                    QImage adjusted = adjustedComposite(scopeCanvas, document, base);
                    for (int y = 0; y < canvasSize.height(); ++y) {
                        const uchar *covRow = baseCov.constScanLine(y);
                        const uchar *clipRow = clip.isNull() ? nullptr : clip.constScanLine(y);
                        uchar *scRow = scopeCanvas.scanLine(y);
                        const uchar *adjRow = adjusted.constScanLine(y);
                        for (int x = 0; x < canvasSize.width(); ++x) {
                            const int a = clipRow ? (covRow[x * 4 + 3] * clipRow[x] + 127) / 255 : covRow[x * 4 + 3];
                            if (a == 255) {
                                scRow[x * 4 + 0] = adjRow[x * 4 + 0];
                                scRow[x * 4 + 1] = adjRow[x * 4 + 1];
                                scRow[x * 4 + 2] = adjRow[x * 4 + 2];
                                scRow[x * 4 + 3] = adjRow[x * 4 + 3];
                            } else if (a > 0) {
                                scRow[x * 4 + 0] = uchar((int(adjRow[x * 4 + 0]) * a + int(scRow[x * 4 + 0]) * (255 - a) + 127) / 255);
                                scRow[x * 4 + 1] = uchar((int(adjRow[x * 4 + 1]) * a + int(scRow[x * 4 + 1]) * (255 - a) + 127) / 255);
                                scRow[x * 4 + 2] = uchar((int(adjRow[x * 4 + 2]) * a + int(scRow[x * 4 + 2]) * (255 - a) + 127) / 255);
                                scRow[x * 4 + 3] = uchar((int(adjRow[x * 4 + 3]) * a + int(scRow[x * 4 + 3]) * (255 - a) + 127) / 255);
                            }
                        }
                    }
                }
            }
            continue;
        }

        if (!drawable(base)) continue;

        int end = i + 1;
        if (!base.maskSourceId) {
            while (end < children.size()) {
                const Layer &child = children.at(end);
                if (child.maskSourceId != std::optional<QUuid>(base.id)) break;
                ++end;
            }
        }

        if (end > i + 1) {
            QImage stack(canvasSize, QImage::Format_RGBA8888_Premultiplied);
            stack.fill(Qt::transparent);
            {
                QPainter own(&stack);
                drawLayer(own, document, base, QPainter::CompositionMode_SourceOver);
            }
            const QImage baseCoverage = layerCoverage(document, base, {});
            const QImage stackAlpha = stack;   // the base on its own: its alpha is what the stack keeps
            // Children draw over the base's color at full strength: lift the soft edge to opaque first.
            for (int y = 0; y < canvasSize.height(); ++y) {
                uchar *row = stack.scanLine(y);
                for (int x = 0; x < canvasSize.width(); ++x) {
                    uchar *px = row + x * 4;
                    const int a = px[3];
                    if (a == 0 || a == 255) continue;
                    for (int c = 0; c < 3; ++c) px[c] = uchar(std::min(255, int(px[c]) * 255 / a));
                    px[3] = 255;
                }
            }

            for (int childIdx = i + 1; childIdx < end; ++childIdx) {
                const Layer &layer = children.at(childIdx);
                if (!LayerRenderer::effectivelyVisible(document, layer)) continue;
                if (!layer.adjustment.isEmpty()) {
                    QImage adjusted = adjustedComposite(stack, document, layer);
                    for (int y = 0; y < canvasSize.height(); ++y) {
                        const uchar *covRow = baseCoverage.constScanLine(y);
                        uchar *stackRow = stack.scanLine(y);
                        const uchar *adjRow = adjusted.constScanLine(y);
                        for (int x = 0; x < canvasSize.width(); ++x) {
                            const int a = covRow[x * 4 + 3];
                            if (a == 255) {
                                stackRow[x * 4 + 0] = adjRow[x * 4 + 0];
                                stackRow[x * 4 + 1] = adjRow[x * 4 + 1];
                                stackRow[x * 4 + 2] = adjRow[x * 4 + 2];
                                stackRow[x * 4 + 3] = adjRow[x * 4 + 3];
                            } else if (a > 0) {
                                stackRow[x * 4 + 0] = uchar((int(adjRow[x * 4 + 0]) * a + int(stackRow[x * 4 + 0]) * (255 - a) + 127) / 255);
                                stackRow[x * 4 + 1] = uchar((int(adjRow[x * 4 + 1]) * a + int(stackRow[x * 4 + 1]) * (255 - a) + 127) / 255);
                                stackRow[x * 4 + 2] = uchar((int(adjRow[x * 4 + 2]) * a + int(stackRow[x * 4 + 2]) * (255 - a) + 127) / 255);
                                stackRow[x * 4 + 3] = uchar((int(adjRow[x * 4 + 3]) * a + int(stackRow[x * 4 + 3]) * (255 - a) + 127) / 255);
                            }
                        }
                    }
                } else if (drawable(layer)) {
                    QImage childImg(canvasSize, QImage::Format_RGBA8888_Premultiplied);
                    childImg.fill(Qt::transparent);
                    {
                        QPainter cp(&childImg);
                        drawLayer(cp, document, layer, QPainter::CompositionMode_SourceOver);
                    }
                    if (customMode(layer.blendMode)) {
                        customComposite(stack, childImg, layer.blendMode);
                    } else {
                        QPainter p(&stack);
                        p.setCompositionMode(compositionMode(layer.blendMode));
                        p.drawImage(0, 0, childImg);
                    }
                }
            }
            // Clipping stacks share the base's alpha instead of painting it over itself (mac LiveMaskRenderer stacks):
            // the clipped layers' colors are blended over the base's color, and the base's own alpha is put back.
            for (int y = 0; y < canvasSize.height(); ++y) {
                uchar *row = stack.scanLine(y);
                const uchar *cov = stackAlpha.constScanLine(y);
                for (int x = 0; x < canvasSize.width(); ++x) {
                    uchar *px = row + x * 4;
                    const int a = cov[x * 4 + 3], total = px[3];
                    if (a == 0 || total == 0) { px[0] = px[1] = px[2] = px[3] = 0; continue; }
                    for (int c = 0; c < 3; ++c) px[c] = uchar(std::min(255, int(px[c]) * 255 / total) * a / 255);
                    px[3] = uchar(a);
                }
            }
            if (!clip.isNull()) applyMaskToImage(stack, clip);
            if (customMode(base.blendMode)) {
                customComposite(scopeCanvas, stack, base.blendMode);
            } else {
                QPainter p(&scopeCanvas);
                p.setCompositionMode(compositionMode(base.blendMode));
                p.drawImage(0, 0, stack);
            }
            i = end - 1;
        } else if (base.maskSourceId) {
            const QImage image = clipped(clippedImage(base, {}));
            if (customMode(base.blendMode)) {
                customComposite(scopeCanvas, image, base.blendMode);
            } else {
                QPainter p(&scopeCanvas);
                p.setCompositionMode(compositionMode(base.blendMode));
                p.drawImage(0, 0, image);
            }
        } else if (customMode(base.blendMode)) {
            compositeRendered(scopeCanvas, document, base, clip);
        } else if (!clip.isNull()) {
            QImage plane(canvasSize, QImage::Format_RGBA8888_Premultiplied);
            plane.fill(Qt::transparent);
            { QPainter own(&plane); drawLayer(own, document, base, QPainter::CompositionMode_SourceOver); }
            applyMaskToImage(plane, clip);
            QPainter p(&scopeCanvas);
            p.setCompositionMode(compositionMode(base.blendMode));
            p.drawImage(0, 0, plane);
        } else {
            QPainter p(&scopeCanvas);
            drawLayer(p, document, base, compositionMode(base.blendMode));
        }
    }
}

void LayerRenderer::draw(QPainter &outputPainter, const Document &document)
{
    const auto findLayer = [&document](const QUuid &id) -> const Layer * {
        const auto it = std::find_if(document.layers.cbegin(), document.layers.cend(), [&](const Layer &layer) { return layer.id == id; });
        return it == document.layers.cend() ? nullptr : &*it;
    };
    std::function<QImage(const Layer &, QSet<QUuid>)> clippedImage;
    clippedImage = [&](const Layer &layer, QSet<QUuid> visiting) {
        QImage image(document.canvasSize, QImage::Format_RGBA8888_Premultiplied); image.fill(Qt::transparent);
        { QPainter own(&image); drawLayer(own, document,layer, QPainter::CompositionMode_SourceOver); }
        if (!layer.maskSourceId || visiting.contains(layer.id)) return image;
        const Layer *source = findLayer(*layer.maskSourceId);
        if (!source || !drawable(*source)) { image.fill(Qt::transparent); return image; }
        visiting.insert(layer.id);
        const QImage coverage = layerCoverage(document, *source, visiting);
        QPainter clip(&image); clip.setCompositionMode(QPainter::CompositionMode_DestinationIn); clip.drawImage(0, 0, coverage); return image;
    };

    QImage canvas(document.canvasSize, QImage::Format_RGBA8888_Premultiplied);
    canvas.fill(Qt::transparent);
    renderScope(document, std::nullopt, document.canvasSize, canvas, QImage(), clippedImage);
    outputPainter.drawImage(0, 0, canvas);
}

QImage LayerRenderer::flattened(const Document &document)
{
    QImage output(document.canvasSize, QImage::Format_RGBA8888_Premultiplied);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    draw(painter, document);
    return output;
}

QImage LayerRenderer::bakeLiveMask(const Document &document, const Layer &layer)
{
    return clippedLayerImage(document, layer, {}, true);
}

} // namespace compositor
