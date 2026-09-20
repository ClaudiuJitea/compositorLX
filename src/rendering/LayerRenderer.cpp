#include "rendering/LayerRenderer.h"
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

static QTransform pixelToDocument(const LayerTransform &placement,const QSize &pixels)
{
    QTransform result;result.translate(placement.center().x(),placement.center().y());result.rotate(placement.rotation);result.scale((placement.flipX?-1:1)*placement.size.width()/pixels.width(),(placement.flipY?-1:1)*placement.size.height()/pixels.height());result.translate(-pixels.width()/2.0,-pixels.height()/2.0);return result;
}

static QImage placedMask(const Layer &owner,const Layer &target,const QSize &size)
{
    if(owner.mask.isNull())return {};
    const QImage source=owner.mask.convertToFormat(QImage::Format_Grayscale8);const LayerTransform placement=owner.maskPlacement.value_or(owner.transform);
    if(owner.id==target.id&&!owner.maskPlacement)return source.scaled(size,Qt::IgnoreAspectRatio,target.transform.sampling==Sampling::Nearest?Qt::FastTransformation:Qt::SmoothTransformation);
    int edgeTotal=0,edgeCount=0;for(int y=0;y<source.height();++y)for(int x=0;x<source.width();++x)if(x==0||y==0||x+1==source.width()||y+1==source.height()){edgeTotal+=source.constScanLine(y)[x];++edgeCount;}const int background=edgeCount&&edgeTotal*2<edgeCount*255?0:255;
    QImage result(size,QImage::Format_Grayscale8);result.fill(background);bool ok=false;const QTransform toMask=pixelToDocument(placement,source.size()).inverted(&ok);if(!ok)return result;const QTransform toDocument=pixelToDocument(target.transform,size);
    for(int y=0;y<size.height();++y){uchar *out=result.scanLine(y);for(int x=0;x<size.width();++x){const QPointF sample=toMask.map(toDocument.map(QPointF(x+.5,y+.5)));const int sx=qFloor(sample.x()),sy=qFloor(sample.y());if(QRect(QPoint(),source.size()).contains(sx,sy))out[x]=source.constScanLine(sy)[sx];}}
    return result;
}

static QImage combinedMask(const Document &document,const Layer &layer,const QSize &size)
{
    QImage result;if(layer.maskEnabled&&!layer.mask.isNull())result=placedMask(layer,layer,size);
    std::optional<QUuid> parent=layer.parentId;QSet<QUuid> seen;
    while(parent&&!seen.contains(*parent)){seen.insert(*parent);const auto it=std::find_if(document.layers.cbegin(),document.layers.cend(),[&](const Layer &candidate){return candidate.id==*parent;});if(it==document.layers.cend())break;if(it->maskEnabled&&!it->mask.isNull()){const QImage folder=placedMask(*it,layer,size);if(result.isNull())result=folder;else for(int y=0;y<result.height();++y){uchar *out=result.scanLine(y);const uchar *clip=folder.constScanLine(y);for(int x=0;x<result.width();++x)out[x]=uchar((int(out[x])*clip[x]+127)/255);}}parent=it->parentId;}
    return result;
}

static void drawLayer(QPainter &painter, const Document &document,const Layer &layer, QPainter::CompositionMode mode)
{
    painter.save();
    painter.setOpacity(layer.opacity);
    painter.setCompositionMode(mode);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, layer.transform.sampling != Sampling::Nearest);
    const QPointF center = layer.transform.center();
    painter.translate(center);
    painter.rotate(layer.transform.rotation);
    painter.scale(layer.transform.flipX ? -1.0 : 1.0, layer.transform.flipY ? -1.0 : 1.0);
    const QRectF bounds(-layer.transform.size.width() / 2.0, -layer.transform.size.height() / 2.0,
                        layer.transform.size.width(), layer.transform.size.height());
    const QImage mask=combinedMask(document,layer,layer.image.size());
    const QImage source = mask.isNull()?layer.image:LayerRenderer::applyMask(layer.image,mask,layer.transform.sampling);
    painter.drawImage(bounds, source);
    painter.restore();
}

static bool drawable(const Layer &layer)
{
    return !layer.group && !layer.image.isNull() && layer.adjustment.isEmpty();
}

static bool customMode(BlendMode mode){return mode==BlendMode::ColorDodge||mode==BlendMode::ColorBurn||mode==BlendMode::Hue||mode==BlendMode::Saturation||mode==BlendMode::Color||mode==BlendMode::Luminosity;}
static double luminance(const std::array<double,3>&c){return .3*c[0]+.59*c[1]+.11*c[2];}
static double saturation(const std::array<double,3>&c){return *std::max_element(c.begin(),c.end())-*std::min_element(c.begin(),c.end());}
static std::array<double,3> setLum(std::array<double,3> c,double value){const double d=value-luminance(c);for(double &v:c)v+=d;const double low=*std::min_element(c.begin(),c.end()),high=*std::max_element(c.begin(),c.end());const double l=luminance(c);if(low<0)for(double &v:c)v=l+(v-l)*l/(l-low);if(high>1)for(double &v:c)v=l+(v-l)*(1-l)/(high-l);for(double &v:c)v=std::clamp(v,0.0,1.0);return c;}
static std::array<double,3> setSat(std::array<double,3> c,double value){std::array<int,3> order{0,1,2};std::sort(order.begin(),order.end(),[&](int a,int b){return c[a]<c[b];});const int low=order[0],mid=order[1],high=order[2];if(c[high]>c[low]){c[mid]=(c[mid]-c[low])*value/(c[high]-c[low]);c[high]=value;}else c[mid]=c[high]=0;c[low]=0;return c;}
static std::array<double,3> blendColor(std::array<double,3> backdrop,std::array<double,3> source,BlendMode mode){if(mode==BlendMode::Hue)return setLum(setSat(source,saturation(backdrop)),luminance(backdrop));if(mode==BlendMode::Saturation)return setLum(setSat(backdrop,saturation(source)),luminance(backdrop));if(mode==BlendMode::Color)return setLum(source,luminance(backdrop));if(mode==BlendMode::Luminosity)return setLum(backdrop,luminance(source));for(int c=0;c<3;++c)source[c]=mode==BlendMode::ColorDodge?(source[c]>=1?1:std::min(1.0,backdrop[c]/(1-source[c]))):(source[c]<=0?0:1-std::min(1.0,(1-backdrop[c])/source[c]));return source;}
static void customComposite(QImage &backdrop,const QImage &source,BlendMode mode)
{
    for(int y=0;y<backdrop.height();++y){uchar *out=backdrop.scanLine(y);const uchar *top=source.constScanLine(y);for(int x=0;x<backdrop.width();++x){uchar *b=out+x*4;const uchar *s=top+x*4;const double ab=b[3]/255.0,as=s[3]/255.0;if(as<=0)continue;std::array<double,3> cb{},cs{};for(int c=0;c<3;++c){cb[c]=ab?b[c]/255.0/ab:0;cs[c]=as?s[c]/255.0/as:0;}const auto mixed=blendColor(cb,cs,mode);const double ao=as+ab-as*ab;for(int c=0;c<3;++c){const double premult=(1-as)*b[c]/255.0+(1-ab)*s[c]/255.0+as*ab*mixed[c];b[c]=uchar(std::clamp(qRound(premult*255),0,255));}b[3]=uchar(std::clamp(qRound(ao*255),0,255));}}
}

static void compositeRendered(QImage &target,const Document &document,const Layer &layer)
{
    QImage source(target.size(),QImage::Format_RGBA8888_Premultiplied);source.fill(Qt::transparent);QPainter render(&source);drawLayer(render,document,layer,QPainter::CompositionMode_SourceOver);render.end();customComposite(target,source,layer.blendMode);
}

static QImage adjustedComposite(const QImage &original, const Document &document,const Layer &layer)
{
    QImage adjusted = RasterOperations::adjustment(original, layer.adjustment);
    if (adjusted.size() != original.size()) return original;
    QImage result = original.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const QImage mask=combinedMask(document,layer,result.size());
    if (layer.blendMode != BlendMode::Normal) {
        QImage blended = original;if(customMode(layer.blendMode))customComposite(blended,adjusted,layer.blendMode);else{QPainter blend(&blended);blend.setCompositionMode(compositionMode(layer.blendMode));blend.drawImage(0,0,adjusted);blend.end();}adjusted=blended;
    }
    for (int y = 0; y < result.height(); ++y) {
        uchar *out = result.scanLine(y); const uchar *base = original.constScanLine(y), *top = adjusted.constScanLine(y), *coverage = mask.isNull() ? nullptr : mask.constScanLine(y);
        for (int x = 0; x < result.width(); ++x) {
            const int amount = std::clamp(qRound(255 * layer.opacity * (coverage ? coverage[x] / 255.0 : 1.0)), 0, 255), inverse = 255 - amount;
            for (int c = 0; c < 4; ++c) out[x * 4 + c] = uchar((int(base[x * 4 + c]) * inverse + int(top[x * 4 + c]) * amount + 127) / 255);
        }
    }
    return result;
}

static QImage clippedLayerImage(const Document &document, const Layer &layer, QSet<QUuid> visiting, bool normalizeOwn = false)
{
    Layer ownLayer = layer; if (normalizeOwn) ownLayer.opacity = 1; ownLayer.blendMode = BlendMode::Normal;
    QImage image(document.canvasSize, QImage::Format_RGBA8888_Premultiplied); image.fill(Qt::transparent);
    { QPainter own(&image); drawLayer(own, document, ownLayer, QPainter::CompositionMode_SourceOver); }
    if (!layer.maskSourceId || visiting.contains(layer.id)) return image;
    const auto it = std::find_if(document.layers.cbegin(), document.layers.cend(), [&](const Layer &candidate){ return candidate.id == *layer.maskSourceId; });
    if (it == document.layers.cend() || !drawable(*it)) { image.fill(Qt::transparent); return image; }
    visiting.insert(layer.id); const QImage coverage = clippedLayerImage(document, *it, visiting);
    QPainter clip(&image); clip.setCompositionMode(QPainter::CompositionMode_DestinationIn); clip.drawImage(0, 0, coverage); return image;
}

void LayerRenderer::draw(QPainter &outputPainter, const Document &document)
{
    QImage canvas(document.canvasSize, QImage::Format_RGBA8888_Premultiplied); canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
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
        const QImage coverage = clippedImage(*source, visiting);
        QPainter clip(&image); clip.setCompositionMode(QPainter::CompositionMode_DestinationIn); clip.drawImage(0, 0, coverage); return image;
    };

    for (int i = 0; i < document.layers.size(); ++i) {
        const Layer &base = document.layers.at(i);
        if (!effectivelyVisible(document, base)) continue;
        if (!base.adjustment.isEmpty()) {
            if (!base.maskSourceId) { painter.end(); canvas = adjustedComposite(canvas,document, base); painter.begin(&canvas); }
            continue;
        }
        if (!drawable(base)) continue;
        int end = i + 1;
        if (!base.maskSourceId) while (end < document.layers.size()) {
            const Layer &child = document.layers.at(end);
            if (child.parentId != base.parentId || child.maskSourceId != std::optional<QUuid>(base.id)) break;
            ++end;
        }
        if (end > i + 1) {
            QImage group(document.canvasSize, QImage::Format_RGBA8888_Premultiplied); group.fill(Qt::transparent);
            { QPainter own(&group); drawLayer(own, document,base, QPainter::CompositionMode_SourceOver); }
            QVector<uchar> alpha(group.width() * group.height());
            for (int y = 0; y < group.height(); ++y) {
                uchar *row = group.scanLine(y);
                for (int x = 0; x < group.width(); ++x) {
                    uchar *pixel = row + x * 4; const int a = pixel[3]; alpha[y * group.width() + x] = uchar(a);
                    if (a) { pixel[0] = uchar(std::min(255, (int(pixel[0]) * 255 + a / 2) / a)); pixel[1] = uchar(std::min(255, (int(pixel[1]) * 255 + a / 2) / a)); pixel[2] = uchar(std::min(255, (int(pixel[2]) * 255 + a / 2) / a)); pixel[3] = 255; }
                }
            }
            { QPainter stack(&group); for (int child = i + 1; child < end; ++child) {
                const Layer &layer = document.layers.at(child);
                if (!effectivelyVisible(document, layer)) continue;
                if (!layer.adjustment.isEmpty()) { stack.end(); group = adjustedComposite(group,document, layer); stack.begin(&group); }
                else if (drawable(layer)) {if(customMode(layer.blendMode)){stack.end();compositeRendered(group,document,layer);stack.begin(&group);}else drawLayer(stack, document,layer, compositionMode(layer.blendMode));}
            } }
            for (int y = 0; y < group.height(); ++y) {
                uchar *row = group.scanLine(y);
                for (int x = 0; x < group.width(); ++x) { uchar *pixel = row + x * 4; const int a = alpha.at(y * group.width() + x); pixel[0] = uchar((int(pixel[0]) * a + 127) / 255); pixel[1] = uchar((int(pixel[1]) * a + 127) / 255); pixel[2] = uchar((int(pixel[2]) * a + 127) / 255); pixel[3] = uchar(a); }
            }
            if(customMode(base.blendMode)){painter.end();customComposite(canvas,group,base.blendMode);painter.begin(&canvas);}else{painter.save(); painter.setCompositionMode(compositionMode(base.blendMode)); painter.drawImage(0, 0, group); painter.restore();}
            i = end - 1;
        } else if (base.maskSourceId) {
            const QImage image = clippedImage(base, {});
            if(customMode(base.blendMode)){painter.end();customComposite(canvas,image,base.blendMode);painter.begin(&canvas);}else{painter.save(); painter.setCompositionMode(compositionMode(base.blendMode)); painter.drawImage(0, 0, image); painter.restore();}
        } else if(customMode(base.blendMode)){painter.end();compositeRendered(canvas,document,base);painter.begin(&canvas);}else drawLayer(painter, document,base, compositionMode(base.blendMode));
    }
    painter.end();
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
