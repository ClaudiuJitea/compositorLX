#include "rendering/GpuDocument.h"
#include "rendering/DeepImage.h"
#include "rendering/GpuOperations.h"
#include "rendering/GpuRenderer.h"
#include "rendering/LayerEffectsRenderer.h"
#include "rendering/LayerRenderer.h"
#include "rendering/ProgressiveBlur.h"
#include "rendering/RasterOperations.h"
#include <QJsonArray>
#include <QPainter>
#include <cmath>

namespace compositor {
namespace {
unsigned precision(bool deep) { return deep ? 0 : 1; }
int adjustment(GpuGraph &g, int input, const QJsonObject &settings) {
    const QString kind = settings.value("kind").toString();
    std::array<float, 28> p{};
    if (kind == "Invert")
        return g.pass(GpuOp::Invert, input, -1, p, 1);
    if (kind == "Exposure") {
        auto s = settings.value("exposureSettings").toObject();
        if (s.isEmpty())
            s = settings.value("exposure").toObject();
        p[0] = std::pow(2., std::clamp(s.value("exposure").toDouble(), -20., 20.));
        p[1] = std::clamp(s.value("offset").toDouble(), -.5, .5);
        p[2] = 1 / std::clamp(s.value("gamma").toDouble(1), .01, 9.99);
        return g.pass(GpuOp::Exposure, input, -1, p, 1);
    }
    if (kind == "Gaussian Blur")
        return g.gaussian(input, settings.value("blurRadius").toDouble(10), true);
    if (kind == "Bloom / Glow") {
        auto s = settings.value("filterLayerSettings").toObject();
        double radius = s.value("bloomRadius").toDouble(24), amount = s.value("bloomAmount").toDouble(40);
        if (amount <= 0 || radius <= 0)
            return input;
        p[0] = std::clamp(amount, 0., 100.) / 50.;
        int soft = g.gaussian(input, std::clamp(radius, 1., 150.), true);
        return g.pass(GpuOp::Bloom, input, soft, p, 1);
    }
    if (kind == "Progressive Blur" || kind == "RGB Split") {
        const auto s = settings.value("filterLayerSettings").toObject();
        const QSize size = g.nodes()[input].size;
        const auto point = [&](const char *name, QPointF fallback) {
            const auto a = s.value(name).toArray();
            return a.size() == 2 ? QPointF(a[0].toDouble(), a[1].toDouble()) : fallback;
        };
        int output = -1;
        if (kind == "Progressive Blur") {
            ProgressiveBlurSettings value;
            value.from = s.value("progressiveFrom").toDouble();
            value.to = s.value("progressiveTo").toDouble(20);
            value.start = point("progressiveStart", {size.width() / 2., size.height() * .35});
            value.end = point("progressiveEnd", {size.width() / 2., double(size.height())});
            value.radial = s.value("progressiveRadial").toBool();
            value.inverted = s.value("progressiveInverted").toBool();
            output = GpuOperations::appendProgressive(g, input, value, 1);
        } else {
            RgbSplitSettings value;
            value.distance = s.value("rgbSplitDistance").toDouble(10);
            value.blur = s.value("rgbSplitBlur").toDouble();
            value.angle = s.value("rgbSplitAngle").toDouble();
            value.radial = s.value("rgbSplitRadial").toBool();
            value.center = point("rgbSplitCenter", {size.width() / 2., size.height() / 2.});
            value.reach = s.value("rgbSplitReach").toDouble(1000);
            output = GpuOperations::appendRgbSplit(g, input, value, 15);
        }
        return output < 0 ? -1 : g.pass(GpuOp::Copy, output, -1, {}, 1);
    }
    if (kind == "Levels" || kind == "Curves") {
        QImage table(256, 1, QImage::Format_RGBA32FPx4_Premultiplied);
        auto *out = reinterpret_cast<float *>(table.bits());
        if (kind == "Levels") {
            LevelsSettings s;
            auto ranges = settings.value("levels").toObject().value("ranges").toArray();
            for (int i = 0; i < std::min(4, int(ranges.size())); ++i) {
                auto r = ranges[i].toObject();
                s.ranges[i] = LevelRange{r.value("black").toDouble(), r.value("gamma").toDouble(1),
                                         r.value("white").toDouble(255), r.value("outputBlack").toDouble(),
                                         r.value("outputWhite").toDouble(255)}
                                  .normalized();
            }
            const auto apply = [](double x, const LevelRange &r) {
                return (r.outputBlack +
                        std::pow(std::clamp((x * 255 - r.black) / (r.white - r.black), 0., 1.), 1 / r.gamma) *
                            (r.outputWhite - r.outputBlack)) /
                       255;
            };
            for (int x = 0; x < 256; ++x) {
                for (int c = 0; c < 3; ++c)
                    out[x * 4 + c] = apply(apply(x / 255., s.ranges[c + 1]), s.ranges[0]);
                out[x * 4 + 3] = 1;
            }
        } else {
            CurvesSettings s;
            auto channels = settings.value("curves").toObject().value("channels").toArray();
            for (int c = 0; c < std::min(4, int(channels.size())); ++c) {
                QVector<CurvePoint> points;
                for (auto entry : channels[c].toArray()) {
                    auto v = entry.toObject();
                    points.push_back({v.value("x").toDouble(), v.value("y").toDouble()});
                }
                if (!points.empty())
                    s.channels[c] = points;
            }
            if (!s.isValid())
                return -1;
            for (int x = 0; x < 256; ++x) {
                for (int c = 0; c < 3; ++c)
                    out[x * 4 + c] = s.value(s.value(x, c + 1), 0) / 255;
                out[x * 4 + 3] = 1;
            }
        }
        return g.pass(GpuOp::Lookup, input, g.source(table), {}, 1);
    }
    return -1;
}
} // namespace
QImage tryGpuDocument(const Document &document) {
    if (!GpuRenderer::shouldAttempt(document.canvasSize, document.layers.size() * 8))
        return {};
    const bool deep = std::any_of(document.layers.begin(), document.layers.end(),
                                  [](const Layer &l) { return DeepImage::isDeep(l.image); });
    const auto format = deep ? QImage::Format_RGBA32FPx4_Premultiplied : QImage::Format_RGBA8888_Premultiplied;
    // Clipping stacks have special alpha-preservation semantics. Until represented
    // explicitly in the graph, they use the established renderer and its GPU filters.
    for (const auto &l : document.layers)
        if (l.maskSourceId)
            return {};
    GpuGraph graph;
    QImage empty(document.canvasSize, format);
    empty.fill(Qt::transparent);
    int canvas = graph.source(empty);
    bool supported = true;
    std::function<void(const std::optional<QUuid> &, int, int)> scope;
    scope = [&](const std::optional<QUuid> &parent, int groupMask, int depth) {
        if (depth > 64) {
            supported = false;
            return;
        }
        for (const auto &layer : document.layers) {
            if (layer.parentId != parent || !LayerRenderer::effectivelyVisible(document, layer))
                continue;
            if (layer.group) {
                int mask = groupMask;
                if (layer.maskEnabled && !layer.mask.isNull()) {
                    const QImage coverage = LayerRenderer::placedMask(layer, layer, document.canvasSize);
                    int own = graph.source(coverage);
                    if (mask >= 0) {
                        std::array<float, 28> p{};
                        p[1] = 1;
                        own = graph.pass(GpuOp::Mask, own, mask, p, 1);
                    }
                    mask = own;
                }
                scope(layer.id, mask, depth + 1);
                continue;
            }
            double opacity = layer.opacity;
            auto ancestor = layer.parentId;
            for (int count = 0; ancestor && count < 64; ++count) {
                auto it = std::find_if(document.layers.begin(), document.layers.end(),
                                       [&](const Layer &l) { return l.id == *ancestor; });
                if (it == document.layers.end())
                    break;
                opacity *= it->opacity;
                ancestor = it->parentId;
            }
            if (!layer.adjustment.isEmpty()) {
                if (deep || layer.blendMode != BlendMode::Normal || groupMask >= 0 ||
                    (layer.maskEnabled && !layer.mask.isNull())) {
                    supported = false;
                    return;
                }
                int changed = adjustment(graph, canvas, layer.adjustment);
                if (changed < 0) {
                    supported = false;
                    return;
                }
                std::array<float, 28> p{};
                p[0] = opacity;
                canvas = graph.pass(GpuOp::Mix, canvas, changed, p, 1);
                continue;
            }
            if (layer.image.isNull())
                continue;
            QImage image = layer.image;
            LayerTransform placement = layer.transform;
            QImage mask = layer.maskEnabled && !layer.mask.isNull()
                              ? LayerRenderer::placedMask(layer, layer, image.size())
                              : QImage();
            if (layer.effects && !layer.effects->visible().isEmpty() && layer.effects->isValid()) {
                const auto effect = LayerEffectsRenderer::cached(image, mask, *layer.effects);
                if (effect.first.isNull()) {
                    supported = false;
                    return;
                }
                image = effect.first;
                placement = LayerEffectsRenderer::placed(placement, image.size(), effect.second);
                mask = {};
            }
            int surface = graph.source(image);
            if (!mask.isNull()) {
                std::array<float, 28> p{};
                p[1] = 1;
                surface = graph.pass(GpuOp::Mask, surface, graph.source(mask), p, precision(deep));
            }
            bool invertible = false;
            const auto inverse = LayerRenderer::pixelToDocument(placement, image.size()).inverted(&invertible);
            if (!invertible)
                continue;
            // QPainter's transformed edge coverage is retained in a rasterized source
            // for rotated or fractional placements; simple pixel-aligned placement is GPU native.
            const bool aligned = placement.rotation == 0 && std::floor(placement.origin.x()) == placement.origin.x() &&
                                 std::floor(placement.origin.y()) == placement.origin.y() &&
                                 placement.size == QSizeF(image.size()) && opacity == 1;
            if (!aligned) {
                QImage plane(document.canvasSize, format);
                plane.fill(Qt::transparent);
                QPainter painter(&plane);
                painter.setRenderHint(QPainter::SmoothPixmapTransform, placement.sampling != Sampling::Nearest);
                painter.setOpacity(opacity);
                painter.setTransform(LayerRenderer::pixelToDocument(placement, image.size()));
                painter.drawImage(0, 0,
                                  mask.isNull() ? image : LayerRenderer::applyMask(image, mask, placement.sampling));
                painter.end();
                surface = graph.source(plane);
                opacity = 1;
            } else {
                std::array<float, 28> p{};
                p[0] = inverse.m11();
                p[1] = inverse.m21();
                p[2] = inverse.dx();
                p[3] = inverse.m12();
                p[4] = inverse.m22();
                p[5] = inverse.dy();
                p[6] = image.width();
                p[7] = image.height();
                p[8] = 0;
                p[9] = 1;
                surface = graph.pass(GpuOp::Transform, surface, -1, p, precision(deep), document.canvasSize);
            }
            if (groupMask >= 0) {
                std::array<float, 28> p{};
                p[1] = 1;
                surface = graph.pass(GpuOp::Mask, surface, groupMask, p, precision(deep));
            }
            std::array<float, 28> p{};
            p[0] = int(layer.blendMode);
            p[1] = opacity;
            canvas = graph.pass(GpuOp::Composite, canvas, surface, p, precision(deep));
        }
    };
    scope(std::nullopt, -1, 0);
    if (!supported)
        return {};
    QImage result = graph.execute(canvas, format);
    if (deep && std::none_of(document.layers.begin(), document.layers.end(),
                             [](const Layer &l) { return DeepImage::isHDR(l.image); }))
        result.setText("compositorDepth", "16");
    return result;
}
} // namespace compositor
