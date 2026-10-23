#include "rendering/GpuOperations.h"
#include "rendering/DeepImage.h"
#include "rendering/ProgressiveBlur.h"
#include <QColorSpace>
#include <algorithm>
#include <cmath>
#include <functional>
#include <mutex>

namespace compositor::GpuOperations {
namespace {
QImage runLocal(const QImage &image, int halo, const std::function<QImage(const QImage &)> &run) {
    QImage result = run(image);
    if (!result.isNull())
        return result;
    // Large convolutions use overlapping tiles. The halo covers every pass, so the
    // interior has exactly the same boundary samples as a full-surface evaluation.
    if (image.width() <= 1024 && image.height() <= 1024)
        return {};
    result = QImage(image.size(), image.format());
    if (result.isNull())
        return {};
    for (int y = 0; y < image.height(); y += 512)
        for (int x = 0; x < image.width(); x += 512) {
            QRect interior(x, y, std::min(512, image.width() - x), std::min(512, image.height() - y));
            const QRect region = interior.adjusted(-halo, -halo, halo, halo).intersected(image.rect());
            const QImage part = run(image.copy(region));
            if (part.isNull())
                return {};
            const int bytes = image.depth() / 8;
            for (int row = 0; row < interior.height(); ++row)
                std::copy_n(part.constScanLine(interior.y() - region.y() + row) + (interior.x() - region.x()) * bytes,
                            interior.width() * bytes, result.scanLine(interior.y() + row) + interior.x() * bytes);
        }
    result.setDotsPerMeterX(image.dotsPerMeterX());
    result.setDotsPerMeterY(image.dotsPerMeterY());
    result.setColorSpace(image.colorSpace());
    for (const auto &key : image.textKeys())
        result.setText(key, image.text(key));
    return result;
}
} // namespace
QImage gaussian(const QImage &image, double radius, bool bytes) {
    if (!std::isfinite(radius) || radius <= 0 || !GpuRenderer::shouldAttempt(image.size(), 32))
        return {};
    radius = std::clamp(radius, .1, 250.);
    return runLocal(image, int(std::ceil(radius * 3)) + 6, [&](const QImage &tile) {
        GpuGraph graph;
        int at = graph.gaussian(graph.source(tile), radius, bytes);
        return graph.execute(at, tile.format());
    });
}
QImage simple(const QImage &image, GpuOp op, const std::array<float, 28> &p, unsigned quantization) {
    if (!GpuRenderer::shouldAttempt(image.size(), op == GpuOp::Exposure ? 16 : 1))
        return {};
    return runLocal(image, 0, [&](const QImage &tile) {
        GpuGraph graph;
        return graph.execute(graph.pass(op, graph.source(tile), -1, p, quantization), tile.format());
    });
}
QImage lookup(const QImage &image, const float *tables) {
    if (!GpuRenderer::shouldAttempt(image.size(), 1))
        return {};
    QImage table(256, 1, QImage::Format_RGBA32FPx4_Premultiplied);
    auto *data = reinterpret_cast<float *>(table.bits());
    for (int i = 0; i < 256; ++i) {
        for (int c = 0; c < 3; ++c)
            data[i * 4 + c] = tables[c * 256 + i];
        data[i * 4 + 3] = 1;
    }
    return runLocal(image, 0, [&](const QImage &tile) {
        GpuGraph graph;
        int in = graph.source(tile), lut = graph.source(table);
        return graph.execute(graph.pass(GpuOp::Lookup, in, lut, {}, 1), tile.format());
    });
}
QImage cube(const QImage &image, const float *values, int grid, int clipping) {
    // This CPU kernel is already parallel and faster than upload/readback in the
    // measured standalone Camera RAW path. Keep GPU execution available for testing
    // and explicit GPU mode; resident graph cube passes do not need this boundary.
    if (!values || grid < 2 || grid > 65 || !GpuRenderer::shouldAttempt(image.size(), 1))
        return {};
    QImage table(grid * grid, grid, QImage::Format_RGBA32FPx4_Premultiplied);
    auto *data = reinterpret_cast<float *>(table.bits());
    for (int i = 0; i < grid * grid * grid; ++i) {
        for (int c = 0; c < 3; ++c)
            data[i * 4 + c] = values[i * 3 + c];
        data[i * 4 + 3] = 1;
    }
    std::array<float, 28> p{};
    p[0] = grid;
    p[1] = clipping;
    return runLocal(image, 0, [&](const QImage &tile) {
        GpuGraph graph;
        int in = graph.source(tile), lut = graph.source(table);
        return graph.execute(graph.pass(GpuOp::Cube, in, lut, p, 1), tile.format());
    });
}
QImage bloom(const QImage &image, double amount, double radius, bool deep) {
    if (!std::isfinite(amount) || amount <= 0 || !std::isfinite(radius) || radius <= 0 ||
        !GpuRenderer::shouldAttempt(image.size(), 32))
        return {};
    radius = std::clamp(radius, 1., 150.);
    std::array<float, 28> p{};
    p[0] = float(std::clamp(amount, 0., 100.) / 50);
    p[1] = deep ? 1 : 0;
    return runLocal(image, int(std::ceil(radius * 3)) + 6, [&](const QImage &tile) {
        GpuGraph graph;
        int in = graph.source(tile), blur = graph.gaussian(in, radius, !deep);
        return graph.execute(graph.pass(GpuOp::Bloom, in, blur, p, deep ? 0 : 1), tile.format());
    });
}
QImage motion(const QImage &image, double angle, double distance) {
    if (!std::isfinite(angle) || !std::isfinite(distance) || distance <= 0 ||
        !GpuRenderer::shouldAttempt(image.size(), 16))
        return {};
    const double radians = std::clamp(angle, -90., 90.) * M_PI / 180, dx = std::cos(radians), dy = -std::sin(radians);
    const bool horizontal = std::abs(dx) >= std::abs(dy);
    const double major = horizontal ? dx : dy;
    std::array<float, 28> p{};
    p[0] = horizontal ? 1 : 0;
    p[1] = float((horizontal ? dy : dx) / major);
    p[2] = float(std::lround(std::clamp(distance, 1., 2000.) * std::abs(major) / 2));
    GpuGraph graph;
    return graph.execute(graph.pass(GpuOp::MotionBlur, graph.source(image), -1, p, image.depth() <= 32 ? 1 : 0),
                         image.format());
}
int appendProgressive(GpuGraph &graph, int original, const ProgressiveBlurSettings &s, unsigned precision) {
    double low = std::clamp(s.from, 0., 250.), high = std::clamp(s.to, 0., 250.);
    if (s.inverted)
        std::swap(low, high);
    const double strongest = std::max(low, high);
    if (strongest <= 0)
        return original;
    QPointF delta = s.end - s.start;
    if (std::hypot(delta.x(), delta.y()) < 1)
        delta = {0, 1};
    int previous = original, result = -1;
    for (int level = 1; level <= 12; ++level) {
        int blurred = graph.gaussian(original, strongest * level / 12);
        // The CPU pyramid converts each blurred level back to the source format.
        blurred = graph.pass(GpuOp::Copy, blurred, -1, {}, precision);
        std::array<float, 28> p{};
        p[0] = s.start.x();
        p[1] = s.start.y();
        p[2] = delta.x();
        p[3] = delta.y();
        p[4] = s.radial ? 1 : 0;
        p[5] = low;
        p[6] = high;
        p[7] = strongest;
        p[8] = level;
        int candidate = graph.pass(GpuOp::ProgressiveMix, previous, blurred, p);
        if (result < 0)
            result = candidate;
        else {
            p = {};
            p[0] = -1;
            result = graph.pass(GpuOp::Mix, result, candidate, p);
        }
        previous = blurred;
    }
    return result;
}
QImage progressive(const QImage &image, const ProgressiveBlurSettings &s) {
    if (!GpuRenderer::shouldAttempt(image.size(), 100))
        return {};
    const unsigned precision = DeepImage::isHDR(image)    ? (image.depth() == 64 ? 3 : 0)
                               : DeepImage::isDeep(image) ? 2
                                                          : 1;
    GpuGraph graph;
    const int output = appendProgressive(graph, graph.source(image), s, precision);
    return graph.execute(output, DeepImage::canonical(image).format());
}
int appendRgbSplit(GpuGraph &graph, int input, const RgbSplitSettings &s, unsigned edges) {
    const QSize size = graph.nodes()[input].size;
    for (double n : {s.distance, s.blur, s.angle, s.reach, s.center.x(), s.center.y()})
        if (!std::isfinite(n) || std::abs(n) > 1e7) {
            return -1;
        }
    // The deterministic sample jitter depends only on image coordinates. Cache one
    // size so slider changes reuse both its CPU data and its GPU upload.
    static std::mutex mutex;
    static QImage cached;
    QImage noise;
    {
        std::lock_guard lock(mutex);
        if (cached.size() != size) {
            QImage next(size, QImage::Format_RGBA32FPx4_Premultiplied);
            if (next.isNull())
                return -1;
            for (int y = 0; y < next.height(); ++y) {
                auto *row = reinterpret_cast<float *>(next.scanLine(y));
                for (int x = 0; x < next.width(); ++x) {
                    const double hash = std::sin(x * 12.9898 + y * 78.233) * 43758.5453;
                    row[x * 4] = float(hash - std::floor(hash));
                    row[x * 4 + 1] = row[x * 4 + 2] = 0;
                    row[x * 4 + 3] = 1;
                }
            }
            cached = next;
        }
        noise = cached;
    }
    std::array<float, 28> p{};
    const double radians = s.angle * M_PI / 180;
    p[0] = s.distance;
    p[1] = s.blur;
    p[2] = std::cos(radians);
    p[3] = std::sin(radians);
    p[4] = s.center.x();
    p[5] = s.center.y();
    p[6] = s.reach;
    p[7] = s.radial ? 1 : 0;
    p[8] = std::clamp(std::ceil(s.distance * 2) + 1, 1., 48.);
    p[9] = s.blur > 0 ? std::clamp(std::ceil(s.blur / 3), 2., 24.) : 1;
    p[10] = edges & 15;
    return graph.pass(GpuOp::RgbSplit, input, graph.source(noise), p);
}
QImage rgbSplit(const QImage &image, const RgbSplitSettings &s, unsigned edges, GpuExecution *report) {
    GpuGraph graph;
    const int output = appendRgbSplit(graph, graph.source(image), s, edges);
    return graph.execute(output, image.format(), RenderBackend::Vulkan, report);
}
QImage masked(const QImage &image, const QImage &mask, float factor, bool alpha) {
    if (image.size() != mask.size() || !GpuRenderer::shouldAttempt(image.size(), 1))
        return {};
    GpuGraph graph;
    int in = graph.source(image), coverage = graph.source(mask);
    std::array<float, 28> p{};
    p[0] = alpha ? 1 : 0;
    p[1] = factor;
    return graph.execute(graph.pass(GpuOp::Mask, in, coverage, p), image.format());
}
QImage composite(const QImage &image, const QImage &top, int mode, float opacity, unsigned quantization) {
    if (image.size() != top.size() || !GpuRenderer::shouldAttempt(image.size(), 8))
        return {};
    GpuGraph graph;
    int in = graph.source(image), over = graph.source(top);
    std::array<float, 28> p{};
    p[0] = mode;
    p[1] = opacity;
    return graph.execute(graph.pass(GpuOp::Composite, in, over, p, quantization), image.format());
}
QImage transformed(const QImage &image, QSize size, const QTransform &inverse, bool smooth, float opacity,
                   unsigned quantization) {
    if (!inverse.isAffine() || !GpuRenderer::shouldAttempt(size, 8))
        return {};
    std::array<float, 28> p{};
    p[0] = inverse.m11();
    p[1] = inverse.m21();
    p[2] = inverse.dx();
    p[3] = inverse.m12();
    p[4] = inverse.m22();
    p[5] = inverse.dy();
    p[6] = image.width();
    p[7] = image.height();
    p[8] = smooth ? 1 : 0;
    p[9] = opacity;
    GpuGraph graph;
    return graph.execute(graph.pass(GpuOp::Transform, graph.source(image), -1, p, quantization, size), image.format());
}
} // namespace compositor::GpuOperations
