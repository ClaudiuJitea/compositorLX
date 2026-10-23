#include "rendering/GpuDocument.h"
#include "rendering/DeepImage.h"
#include "rendering/GpuOperations.h"
#include "rendering/GpuRenderer.h"
#include "rendering/LayerEffectsRenderer.h"
#include "rendering/LayerRenderer.h"
#include "rendering/ProgressiveBlur.h"
#include "rendering/RasterOperations.h"
#include "rendering/RgbSplit.h"
#include <QColorSpace>
#include <QHash>
#include <QPainter>
#include <cmath>
#include <mutex>
#include <set>

namespace compositor {
static_assert(GpuBlend::Normal == int(BlendMode::Normal) && GpuBlend::HardMix == int(BlendMode::HardMix) &&
                  GpuBlend::Difference == int(BlendMode::Difference) && GpuBlend::Luminosity == int(BlendMode::Luminosity),
              "GpuBlend IDs must follow BlendMode; regenerate GpuContract.h");
static_assert(GpuBlend::Darken == int(BlendMode::Darken) && GpuBlend::Multiply == int(BlendMode::Multiply) &&
                  GpuBlend::ColorBurn == int(BlendMode::ColorBurn) && GpuBlend::LinearBurn == int(BlendMode::LinearBurn) &&
                  GpuBlend::Lighten == int(BlendMode::Lighten) && GpuBlend::Screen == int(BlendMode::Screen) &&
                  GpuBlend::ColorDodge == int(BlendMode::ColorDodge) &&
                  GpuBlend::LinearDodge == int(BlendMode::LinearDodge) && GpuBlend::Overlay == int(BlendMode::Overlay) &&
                  GpuBlend::SoftLight == int(BlendMode::SoftLight) && GpuBlend::HardLight == int(BlendMode::HardLight) &&
                  GpuBlend::VividLight == int(BlendMode::VividLight) &&
                  GpuBlend::LinearLight == int(BlendMode::LinearLight) && GpuBlend::PinLight == int(BlendMode::PinLight) &&
                  GpuBlend::Exclusion == int(BlendMode::Exclusion) && GpuBlend::Subtract == int(BlendMode::Subtract) &&
                  GpuBlend::Divide == int(BlendMode::Divide) && GpuBlend::Hue == int(BlendMode::Hue) &&
                  GpuBlend::Saturation == int(BlendMode::Saturation) && GpuBlend::Color == int(BlendMode::Color),
              "GpuBlend IDs must follow BlendMode; regenerate GpuContract.h");
namespace {
constexpr unsigned bytes8 = unsigned(GpuQuantization::Unorm8);
unsigned precision(bool deep) { return deep ? 0 : bytes8; }

// The document flattened into render steps, in drawing order (folders are pass-through).
struct Step {
    const Layer *layer = nullptr;
    double opacity = 1;
    int groupMask = -1; // Index into Plan::groupMasks (product of the enclosing folder masks).
    // Raster layers, filled by prepare():
    QImage image, mask;
    LayerTransform placement;
    bool aligned = false;
};
struct GroupMask {
    const Layer *group = nullptr;
    int parent = -1;
    QImage coverage; // Canvas-sized Grayscale8, filled by prepare().
};
struct Plan {
    bool supported = true, deep = false;
    std::vector<Step> steps;
    std::vector<GroupMask> groupMasks;
    int halo = 0; // Sum of the spatial reach of filter layers, for tiles.
};
bool supportedAdjustment(const QString &kind) {
    return kind == "Invert" || kind == "Exposure" || kind == "Levels" || kind == "Curves" || kind == "Gaussian Blur" ||
           kind == "Bloom / Glow" || kind == "Progressive Blur" || kind == "RGB Split";
}
int adjustmentReach(const QJsonObject &settings, QSize canvas) {
    const QString kind = settings.value("kind").toString();
    const auto blur = [](double radius) { return int(std::ceil(3 * std::clamp(radius, .1, 250.))) + 6; };
    if (kind == "Gaussian Blur")
        return blur(settings.value("blurRadius").toDouble(10));
    if (kind == "Bloom / Glow")
        return blur(std::clamp(bloomFromJson(settings).radius, 1., 150.));
    if (kind == "Progressive Blur") {
        const auto s = progressiveFromJson(settings, canvas);
        return blur(std::max(std::clamp(s.from, 0., 250.), std::clamp(s.to, 0., 250.)));
    }
    if (kind == "RGB Split") {
        const auto s = rgbSplitFromJson(settings, canvas);
        const double reach = std::abs(s.distance) + std::abs(s.blur);
        const double scale = s.radial ? std::hypot(canvas.width(), canvas.height()) / std::max(1., s.reach) : 1;
        return int(std::ceil(std::min(reach * scale, 1e6))) + 2;
    }
    return 0;
}
Plan plan(const Document &document, int firstTopLevel) {
    Plan p;
    p.deep = std::any_of(document.layers.begin(), document.layers.end(),
                         [](const Layer &l) { return DeepImage::isDeep(l.image); });
    std::function<void(const std::optional<QUuid> &, int, int, double)> scope;
    scope = [&](const std::optional<QUuid> &parent, int groupMask, int depth, double factor) {
        if (depth > 64) {
            p.supported = false;
            return;
        }
        int topLevel = -1;
        for (const auto &layer : document.layers) {
            if (!p.supported || layer.parentId != parent)
                continue;
            if (!parent && ++topLevel < firstTopLevel)
                continue;
            if (!LayerRenderer::effectivelyVisible(document, layer))
                continue;
            // Clipping stacks have special alpha-preservation semantics. Until represented
            // explicitly in the graph, they use the established renderer and its GPU filters.
            if (layer.maskSourceId) {
                p.supported = false;
                return;
            }
            if (layer.group) {
                int mask = groupMask;
                if (layer.maskEnabled && !layer.mask.isNull()) {
                    p.groupMasks.push_back({&layer, groupMask, {}});
                    mask = int(p.groupMasks.size()) - 1;
                }
                scope(layer.id, mask, depth + 1, factor * layer.opacity);
                continue;
            }
            Step step;
            step.layer = &layer;
            step.opacity = layer.opacity * factor;
            step.groupMask = groupMask;
            if (!layer.adjustment.isEmpty()) {
                if (p.deep || layer.blendMode != BlendMode::Normal || groupMask >= 0 ||
                    (layer.maskEnabled && !layer.mask.isNull()) ||
                    !supportedAdjustment(layer.adjustment.value("kind").toString())) {
                    p.supported = false;
                    return;
                }
                p.halo += adjustmentReach(layer.adjustment, document.canvasSize);
            } else if (layer.image.isNull())
                continue;
            p.steps.push_back(step);
        }
    };
    scope(std::nullopt, -1, 0, 1);
    return p;
}
quint64 estimate(const Plan &p, QSize region) {
    const quint64 area = quint64(region.width()) * region.height();
    const quint64 canvasBytes = p.deep ? 16 : 4;
    // The running canvas, the incoming surface and its masked copy.
    quint64 total = area * canvasBytes * 3;
    quint64 widest = 0; // Filter layers run one at a time; their intermediates reuse slots.
    total += (area + area * 4) * p.groupMasks.size(); // Coverage and the combined mask.
    for (const auto &s : p.steps) {
        const Layer &l = *s.layer;
        if (!l.adjustment.isEmpty()) {
            const QString kind = l.adjustment.value("kind").toString();
            const quint64 copies = kind == "Progressive Blur" ? 8 : kind == "RGB Split" ? 3 : 4;
            widest = std::max(widest, area * canvasBytes * copies);
            continue;
        }
        const quint64 layerBpp = DeepImage::isHDR(l.image) ? 16 : DeepImage::isDeep(l.image) ? 8 : 4;
        const quint64 pixels = std::min(area, quint64(l.image.width()) * l.image.height());
        const bool simple = l.transform.rotation == 0 && l.transform.size == QSizeF(l.image.size()) &&
                            s.opacity == 1 && !(l.effects && !l.effects->visible().isEmpty());
        // Rotated, scaled, translucent or effect layers arrive as a rasterized region plane.
        total += simple ? pixels * layerBpp + (l.mask.isNull() ? 0 : pixels * 5) : area * (p.deep ? 16 : 4);
    }
    return total + widest + 64 * 1024;
}
// CPU preparation: effects, placed masks and folder coverage. Runs only once the graph is
// known to fit, so an over-budget document costs nothing before the CPU renders it.
bool prepare(const Document &document, Plan &p) {
    for (auto &g : p.groupMasks)
        g.coverage = LayerRenderer::placedMask(*g.group, *g.group, document.canvasSize);
    for (auto &s : p.steps) {
        const Layer &layer = *s.layer;
        if (!layer.adjustment.isEmpty())
            continue;
        s.image = layer.image;
        s.placement = layer.transform;
        s.mask = layer.maskEnabled && !layer.mask.isNull() ? LayerRenderer::placedMask(layer, layer, s.image.size())
                                                           : QImage();
        if (layer.effects && !layer.effects->visible().isEmpty() && layer.effects->isValid()) {
            const auto effect = LayerEffectsRenderer::cached(s.image, s.mask, *layer.effects);
            if (effect.first.isNull())
                return false;
            s.image = effect.first;
            s.placement = LayerEffectsRenderer::placed(s.placement, s.image.size(), effect.second);
            s.mask = {};
        }
        // QPainter's transformed edge coverage is kept by rasterizing rotated or fractional
        // placements; simple pixel-aligned placement is GPU native.
        s.aligned = s.placement.rotation == 0 && std::floor(s.placement.origin.x()) == s.placement.origin.x() &&
                    std::floor(s.placement.origin.y()) == s.placement.origin.y() &&
                    s.placement.size == QSizeF(s.image.size()) && s.opacity == 1;
    }
    return true;
}
int adjustment(GpuGraph &g, int input, const QJsonObject &settings, QPoint origin, QSize canvas) {
    const QString kind = settings.value("kind").toString();
    GpuParameters p{};
    if (kind == "Invert")
        return g.pass(GpuOp::Invert, input, -1, p, bytes8);
    if (kind == "Exposure") {
        const auto s = RasterOperations::exposureFromJson(settings);
        p[GpuParam::Exposure::multiplier] = std::pow(2., std::clamp(s.stops, -20., 20.));
        p[GpuParam::Exposure::offset] = std::clamp(s.offset, -.5, .5);
        p[GpuParam::Exposure::inverseGamma] = 1 / std::clamp(s.gamma, .01, 9.99);
        return g.pass(GpuOp::Exposure, input, -1, p, bytes8);
    }
    if (kind == "Gaussian Blur")
        return g.gaussian(input, settings.value("blurRadius").toDouble(10), true);
    if (kind == "Bloom / Glow") {
        const auto s = bloomFromJson(settings);
        if (s.amount <= 0 || s.radius <= 0)
            return input;
        p[GpuParam::Bloom::amount] = std::clamp(s.amount, 0., 100.) / 50.;
        const int soft = g.gaussian(input, std::clamp(s.radius, 1., 150.), true);
        return g.pass(GpuOp::Bloom, input, soft, p, bytes8);
    }
    if (kind == "Progressive Blur" || kind == "RGB Split") {
        // Positions are in canvas pixels; a tile at `origin` sees them shifted.
        int output = -1;
        if (kind == "Progressive Blur") {
            auto value = progressiveFromJson(settings, canvas);
            value.start -= origin;
            value.end -= origin;
            output = GpuOperations::appendProgressive(g, input, value, bytes8);
        } else {
            auto value = rgbSplitFromJson(settings, canvas);
            value.center -= origin;
            output = GpuOperations::appendRgbSplit(g, input, value, 15, origin);
        }
        return output < 0 ? -1 : g.pass(GpuOp::Copy, output, -1, {}, bytes8);
    }
    if (kind == "Levels" || kind == "Curves") {
        std::array<float, 768> tables;
        if (kind == "Levels")
            tables = RasterOperations::levelsTables(RasterOperations::levelsFromJson(settings));
        else {
            const auto curves = RasterOperations::curvesFromJson(settings);
            if (!curves.isValid())
                return input; // The CPU leaves the image unchanged.
            tables = RasterOperations::curvesTables(curves);
        }
        QImage table(256, 1, QImage::Format_RGBA32FPx4_Premultiplied);
        auto *out = reinterpret_cast<float *>(table.bits());
        for (int x = 0; x < 256; ++x) {
            for (int c = 0; c < 3; ++c)
                out[x * 4 + c] = tables[c * 256 + x];
            out[x * 4 + 3] = 1;
        }
        return g.pass(GpuOp::Lookup, input, g.source(table), {}, bytes8);
    }
    return -1;
}
// One graph over `region` (canvas pixels).
QImage renderRegion(const Document &document, const Plan &p, QRect region, const QImage &base, QImage::Format format) {
    GpuGraph graph;
    // The result takes its color space and resolution from the first source; the CPU
    // renderer's output has neither, so the first source is a neutral pixel.
    static const QImage neutral = [] {
        QImage pixel(1, 1, QImage::Format_RGBA8888_Premultiplied);
        pixel.fill(Qt::transparent);
        return pixel;
    }();
    graph.source(neutral);
    const QSize size = region.size();
    // The whole base keeps its identity, so the upload cache skips it on repeated renders.
    const bool whole = region == QRect(QPoint(), document.canvasSize);
    int canvas = base.isNull() ? graph.clear(size) : graph.source(whole ? base : base.copy(region));
    const QTransform toCanvas = QTransform::fromTranslate(-region.x(), -region.y());
    std::vector<int> groupNodes(p.groupMasks.size(), -1);
    for (size_t i = 0; i < p.groupMasks.size(); ++i) {
        const auto &g = p.groupMasks[i];
        int own = graph.source(whole ? g.coverage : g.coverage.copy(region));
        if (g.parent >= 0) {
            GpuParameters m{};
            m[GpuParam::Mask::factor] = 1;
            own = graph.pass(GpuOp::Mask, own, groupNodes[size_t(g.parent)], m, bytes8);
        }
        groupNodes[i] = own;
    }
    for (const auto &s : p.steps) {
        const Layer &layer = *s.layer;
        if (!layer.adjustment.isEmpty()) {
            const int changed = adjustment(graph, canvas, layer.adjustment, region.topLeft(), document.canvasSize);
            if (changed < 0)
                return {};
            GpuParameters m{};
            m[GpuParam::Mix::amount] = s.opacity;
            canvas = graph.pass(GpuOp::Mix, canvas, changed, m, bytes8);
            continue;
        }
        const QTransform toDocument = LayerRenderer::pixelToDocument(s.placement, s.image.size());
        if (!toDocument.mapRect(QRectF(s.image.rect())).intersects(QRectF(region)))
            continue;
        int surface;
        if (!s.aligned) {
            QImage plane(size, p.deep ? QImage::Format_RGBA32FPx4_Premultiplied : QImage::Format_RGBA8888_Premultiplied);
            if (plane.isNull())
                return {};
            plane.fill(Qt::transparent);
            QPainter painter(&plane);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, s.placement.sampling != Sampling::Nearest);
            painter.setOpacity(s.opacity);
            painter.setTransform(toDocument * toCanvas);
            painter.drawImage(0, 0, s.mask.isNull() ? s.image : LayerRenderer::applyMask(s.image, s.mask, s.placement.sampling));
            painter.end();
            surface = graph.source(plane);
        } else {
            bool invertible = false;
            const QTransform inverse = toDocument.inverted(&invertible);
            if (!invertible)
                continue;
            // Upload only the part of the layer this region shows.
            const QRect used = (toCanvas.inverted() * inverse).mapRect(QRectF(QPointF(), QSizeF(size))).toAlignedRect()
                                   .adjusted(-1, -1, 1, 1)
                                   .intersected(s.image.rect());
            if (used.isEmpty())
                continue;
            const bool entire = used == s.image.rect();
            surface = graph.source(entire ? s.image : s.image.copy(used));
            if (!s.mask.isNull()) {
                GpuParameters m{};
                m[GpuParam::Mask::factor] = 1;
                surface = graph.pass(GpuOp::Mask, surface, graph.source(entire ? s.mask : s.mask.copy(used)), m,
                                     precision(p.deep));
            }
            const QTransform map = toCanvas.inverted() * inverse * QTransform::fromTranslate(-used.x(), -used.y());
            using T = GpuParam::Transform;
            GpuParameters t{};
            t[T::m11] = map.m11();
            t[T::m21] = map.m21();
            t[T::dx] = map.dx();
            t[T::m12] = map.m12();
            t[T::m22] = map.m22();
            t[T::dy] = map.dy();
            t[T::sourceWidth] = used.width();
            t[T::sourceHeight] = used.height();
            t[T::smooth] = 0;
            t[T::opacity] = 1;
            surface = graph.pass(GpuOp::Transform, surface, -1, t, precision(p.deep), size);
        }
        if (s.groupMask >= 0) {
            GpuParameters m{};
            m[GpuParam::Mask::factor] = 1;
            surface = graph.pass(GpuOp::Mask, surface, groupNodes[size_t(s.groupMask)], m, precision(p.deep));
        }
        GpuParameters c{};
        c[GpuParam::Composite::mode] = int(layer.blendMode);
        c[GpuParam::Composite::opacity] = 1; // Opacity is already in the surface.
        canvas = graph.pass(GpuOp::Composite, canvas, surface, c, precision(p.deep));
    }
    return graph.execute(canvas, format);
}
// Negative decisions (graph did not fit) for recently seen document shapes.
std::mutex rejectedMutex;
std::set<size_t> rejected;
size_t shapeKey(const Document &document, const Plan &p) {
    size_t key = qHash(document.canvasSize.width()) ^ (qHash(document.canvasSize.height()) << 1) ^
                 qHash(GpuRenderer::memoryBudgetMiB()) << 2 ^ size_t(p.deep);
    for (const auto &s : p.steps)
        key = key * 1000003u ^ qHash(s.layer->image.width()) ^ qHash(s.layer->image.height()) << 7 ^
              qHash(s.layer->adjustment.value("kind").toString());
    return key;
}
} // namespace
quint64 estimateGpuDocumentBytes(const Document &document, QSize region) {
    const Plan p = plan(document, 0);
    return p.supported ? estimate(p, region) : 0;
}
QImage tryGpuDocument(const Document &document, const QImage &base, int firstTopLevel) {
    if (!GpuRenderer::shouldAttempt(document.canvasSize, std::max<qsizetype>(1, document.layers.size()) * 8))
        return {};
    Plan p = plan(document, firstTopLevel);
    if (!p.supported || (p.steps.empty() && base.isNull()))
        return {};
    const auto format = p.deep ? QImage::Format_RGBA32FPx4_Premultiplied : QImage::Format_RGBA8888_Premultiplied;
    if (!base.isNull() && (base.size() != document.canvasSize || base.format() != format))
        return {};
    const QRect canvas(QPoint(), document.canvasSize);
    // Decide the tiling from the document alone, before any CPU preparation (A1).
    const quint64 budget = GpuRenderer::memoryBudgetBytes() * 8 / 10;
    int tile = 0;
    if (estimate(p, canvas.size()) > budget) {
        // Tiles need every filter's reach as overlap; a huge reach is not worth tiling.
        if (p.halo > 512)
            return {};
        for (int side : {4096, 2048, 1024, 512})
            if (estimate(p, QSize(side + 2 * p.halo, side + 2 * p.halo)) <= budget) {
                tile = side;
                break;
            }
        if (!tile)
            return {};
    }
    const size_t key = shapeKey(document, p);
    {
        std::lock_guard lock(rejectedMutex);
        if (rejected.count(key))
            return {};
    }
    if (!prepare(document, p))
        return {};
    QImage result;
    if (!tile)
        result = renderRegion(document, p, canvas, base, format);
    else {
        result = QImage(document.canvasSize, format);
        if (result.isNull())
            return {};
        const int bytes = result.depth() / 8;
        for (int y = 0; y < canvas.height() && !result.isNull(); y += tile)
            for (int x = 0; x < canvas.width(); x += tile) {
                const QRect interior = QRect(x, y, tile, tile).intersected(canvas);
                const QRect region = interior.adjusted(-p.halo, -p.halo, p.halo, p.halo).intersected(canvas);
                const QImage part = renderRegion(document, p, region, base, format);
                if (part.isNull()) {
                    result = {};
                    break;
                }
                for (int row = 0; row < interior.height(); ++row)
                    std::memcpy(result.scanLine(interior.y() + row) + interior.x() * bytes,
                                part.constScanLine(interior.y() - region.y() + row) + (interior.x() - region.x()) * bytes,
                                size_t(interior.width()) * bytes);
                if (y == 0 && x == 0) {
                    result.setColorSpace(part.colorSpace());
                    result.setDotsPerMeterX(part.dotsPerMeterX());
                    result.setDotsPerMeterY(part.dotsPerMeterY());
                }
            }
    }
    if (result.isNull()) {
        if (GpuRenderer::lastExecution().reason.contains(QStringLiteral("budget"))) {
            std::lock_guard lock(rejectedMutex);
            if (rejected.size() > 64)
                rejected.clear();
            rejected.insert(key);
        }
        return {};
    }
    if (p.deep && std::none_of(document.layers.begin(), document.layers.end(),
                               [](const Layer &l) { return DeepImage::isHDR(l.image); }))
        result.setText("compositorDepth", "16");
    return result;
}
} // namespace compositor
