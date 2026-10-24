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
// Automatic costs from tools/GpuBenchmark (1080p, cold, Iris Xe): Black & White and Color Balance gain
// ~30-40×, Exposure only ~1.1× since its CPU lookup table; Invert is faster on the CPU (memory bound).
static double cost(GpuOp op) {
    switch (op) {
    case GpuOp::BlackWhite:
    case GpuOp::ColorBalance:
    case GpuOp::Exposure:
        return 16;
    case GpuOp::Invert:
        return 1;
    default:
        return 8;
    }
}
QImage simple(const QImage &image, GpuOp op, const GpuParameters &p, unsigned quantization) {
    if (!GpuRenderer::shouldAttempt(image.size(), cost(op)))
        return {};
    return runLocal(image, 0, [&](const QImage &tile) {
        GpuGraph graph;
        return graph.execute(graph.pass(op, graph.source(tile), -1, p, quantization), tile.format());
    });
}
QImage lookup(const QImage &image, const float *tables) {
    // Levels/Curves: ~10× faster than the single-threaded CPU table at 1080p.
    if (!GpuRenderer::shouldAttempt(image.size(), 8))
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
    GpuParameters p{};
    p[GpuParam::Cube::grid] = grid;
    p[GpuParam::Cube::clipping] = clipping;
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
    GpuParameters p{};
    p[GpuParam::Bloom::amount] = float(std::clamp(amount, 0., 100.) / 50);
    p[GpuParam::Bloom::deep] = deep ? 1 : 0;
    return runLocal(image, int(std::ceil(radius * 3)) + 6, [&](const QImage &tile) {
        GpuGraph graph;
        int in = graph.source(tile), blur = graph.gaussian(in, radius, !deep);
        return graph.execute(graph.pass(GpuOp::Bloom, in, blur, p, deep ? 0 : 1), tile.format());
    });
}
int appendMotion(GpuGraph &graph, int input, double angle, double distance, unsigned quantization, QPoint origin) {
    if (!std::isfinite(angle) || !std::isfinite(distance) || distance <= 0)
        return input;
    const double radians = std::clamp(angle, -90., 90.) * M_PI / 180, dx = std::cos(radians), dy = -std::sin(radians);
    const bool horizontal = std::abs(dx) >= std::abs(dy);
    const double major = horizontal ? dx : dy;
    GpuParameters p{};
    p[GpuParam::MotionBlur::horizontal] = horizontal ? 1 : 0;
    p[GpuParam::MotionBlur::slope] = float((horizontal ? dy : dx) / major);
    p[GpuParam::MotionBlur::reach] = float(std::lround(std::clamp(distance, 1., 2000.) * std::abs(major) / 2));
    p[GpuParam::MotionBlur::origin] = float(horizontal ? origin.x() : origin.y());
    return graph.pass(GpuOp::MotionBlur, input, -1, p, quantization);
}
QImage motion(const QImage &image, double angle, double distance) {
    if (!std::isfinite(angle) || !std::isfinite(distance) || distance <= 0 ||
        !GpuRenderer::shouldAttempt(image.size(), 16))
        return {};
    GpuGraph graph;
    return graph.execute(appendMotion(graph, graph.source(image), angle, distance, image.depth() <= 32 ? 1 : 0),
                         image.format());
}
int appendHueSaturation(GpuGraph &graph, int input, const HueSaturationSettings &settings, unsigned quantization) {
    const auto responses = RasterOperations::hueSaturationResponses(settings);
    QImage table(361, 1, QImage::Format_RGBA32FPx4_Premultiplied);
    auto *values = reinterpret_cast<float *>(table.bits());
    for (int i = 0; i <= 360; ++i) {
        for (int c = 0; c < 3; ++c)
            values[i * 4 + c] = float(responses[size_t(i)][size_t(c)]);
        values[i * 4 + 3] = 1;
    }
    const auto &master = settings.adjustments[size_t(settings.range)];
    const double hue = std::fmod(master.hue, 360.);
    GpuParameters p{};
    p[GpuParam::HueSaturation::colorize] = settings.colorize;
    p[GpuParam::HueSaturation::hue] = float((hue < 0 ? hue + 360 : hue) / 360);
    p[GpuParam::HueSaturation::saturation] = float(std::clamp(master.saturation / 100, 0., 1.));
    p[GpuParam::HueSaturation::lightness] = float(master.lightness / 100);
    return graph.pass(GpuOp::HueSaturation, input, graph.source(table), p, quantization);
}
int appendGradientMap(GpuGraph &graph, int input, const std::array<uint8_t, 768> &table, unsigned quantization) {
    QImage ramp(256, 1, QImage::Format_RGBA32FPx4_Premultiplied);
    auto *values = reinterpret_cast<float *>(ramp.bits());
    for (int i = 0; i < 256; ++i) {
        for (int c = 0; c < 3; ++c)
            values[i * 4 + c] = table[size_t(i) * 3 + size_t(c)] / 255.f;
        values[i * 4 + 3] = 1;
    }
    return graph.pass(GpuOp::GradientMap, input, graph.source(ramp), {}, quantization);
}
int appendBlackWhite(GpuGraph &graph, int input, const float *weights, bool tint, double tintHue, double tintSaturation,
                     unsigned quantization) {
    GpuParameters p{};
    for (int i = 0; i < 6; ++i)
        p[GpuParam::BlackWhite::red + i] = weights[i];
    p[GpuParam::BlackWhite::tint] = tint ? 1 : 0;
    p[GpuParam::BlackWhite::tintHue] = float(tintHue / 360);
    p[GpuParam::BlackWhite::tintSaturation] = float(tintSaturation);
    return graph.pass(GpuOp::BlackWhite, input, -1, p, quantization);
}
int appendColorBalance(GpuGraph &graph, int input, const float *shadows, const float *midtones, const float *highlights,
                       bool preserveLuminosity, unsigned quantization) {
    GpuParameters p{};
    for (int i = 0; i < 3; ++i) {
        p[GpuParam::ColorBalance::shadowRed + i] = shadows[i];
        p[GpuParam::ColorBalance::midRed + i] = midtones[i];
        p[GpuParam::ColorBalance::highlightRed + i] = highlights[i];
    }
    p[GpuParam::ColorBalance::preserveLuminosity] = preserveLuminosity ? 1 : 0;
    return graph.pass(GpuOp::ColorBalance, input, -1, p, quantization);
}
int appendNoise(GpuGraph &graph, int input, float amount, bool gaussian, bool monochromatic, quint32 seed, QPoint origin,
                unsigned quantization) {
    GpuParameters p{};
    p[GpuParam::Noise::amount] = amount;
    p[GpuParam::Noise::gaussian] = gaussian ? 1 : 0;
    p[GpuParam::Noise::monochromatic] = monochromatic ? 1 : 0;
    p[GpuParam::Noise::seedLow] = float(seed & 0xffffu);
    p[GpuParam::Noise::seedHigh] = float(seed >> 16);
    p[GpuParam::Noise::originX] = float(origin.x());
    p[GpuParam::Noise::originY] = float(origin.y());
    return graph.pass(GpuOp::Noise, input, -1, p, quantization);
}
int appendGrain(GpuGraph &graph, int input, double amount, double size, double roughness, quint32 seed, QPointF origin,
                double unitsPerPixel, unsigned quantization) {
    if (!(amount > 0) || !(unitsPerPixel > 0))
        return input;
    GpuParameters p{};
    p[GpuParam::Grain::amount] = float(std::clamp(amount, 0., 100.));
    p[GpuParam::Grain::size] = float(std::clamp(size, .5, 20.));
    p[GpuParam::Grain::roughness] = float(std::clamp(roughness, 0., 100.));
    p[GpuParam::Grain::seedLow] = float(seed & 0xffffu);
    p[GpuParam::Grain::seedHigh] = float(seed >> 16);
    p[GpuParam::Grain::originX] = float(origin.x());
    p[GpuParam::Grain::originY] = float(origin.y());
    p[GpuParam::Grain::unitsPerPixel] = float(unitsPerPixel);
    return graph.pass(GpuOp::Grain, input, -1, p, quantization);
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
        using P = GpuParam::ProgressiveMix;
        GpuParameters p{};
        p[P::startX] = s.start.x();
        p[P::startY] = s.start.y();
        p[P::deltaX] = delta.x();
        p[P::deltaY] = delta.y();
        p[P::radial] = s.radial ? 1 : 0;
        p[P::low] = low;
        p[P::high] = high;
        p[P::strongest] = strongest;
        p[P::level] = level;
        int candidate = graph.pass(GpuOp::ProgressiveMix, previous, blurred, p);
        if (result < 0)
            result = candidate;
        else {
            GpuParameters add{};
            add[GpuParam::Mix::amount] = -1;
            result = graph.pass(GpuOp::Mix, result, candidate, add);
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
int appendRgbSplit(GpuGraph &graph, int input, const RgbSplitSettings &s, unsigned edges, QPoint origin) {
    const QSize size = graph.nodes()[input].size;
    for (double n : {s.distance, s.blur, s.angle, s.reach, s.center.x(), s.center.y()})
        if (!std::isfinite(n) || std::abs(n) > 1e7) {
            return -1;
        }
    // The deterministic sample jitter depends only on image coordinates. Cache one size
    // as a single float channel so slider changes reuse both its CPU data and its upload.
    static std::mutex mutex;
    static std::shared_ptr<const std::vector<float>> cached;
    static QSize cachedSize;
    static QPoint cachedOrigin;
    std::shared_ptr<const std::vector<float>> noise;
    {
        std::lock_guard lock(mutex);
        if (!cached || cachedSize != size || cachedOrigin != origin) {
            auto next = std::make_shared<std::vector<float>>(size_t(size.width()) * size.height());
            for (int y = 0; y < size.height(); ++y)
                for (int x = 0; x < size.width(); ++x) {
                    const double hash = std::sin((x + origin.x()) * 12.9898 + (y + origin.y()) * 78.233) * 43758.5453;
                    (*next)[size_t(y) * size.width() + x] = float(hash - std::floor(hash));
                }
            cached = next;
            cachedSize = size;
            cachedOrigin = origin;
        }
        noise = cached;
    }
    using P = GpuParam::RgbSplit;
    GpuParameters p{};
    const double radians = s.angle * M_PI / 180;
    p[P::distance] = s.distance;
    p[P::blur] = s.blur;
    p[P::directionX] = std::cos(radians);
    p[P::directionY] = std::sin(radians);
    p[P::centerX] = s.center.x();
    p[P::centerY] = s.center.y();
    p[P::reach] = s.reach;
    p[P::radial] = s.radial ? 1 : 0;
    p[P::spectralSamples] = std::clamp(std::ceil(s.distance * 2) + 1, 1., 48.);
    p[P::smearSamples] = s.blur > 0 ? std::clamp(std::ceil(s.blur / 3), 2., 24.) : 1;
    p[P::edges] = edges & 15;
    return graph.pass(GpuOp::RgbSplit, input, graph.channel(noise, size), p);
}
QImage rgbSplit(const QImage &image, const RgbSplitSettings &s, unsigned edges, GpuExecution *report) {
    GpuGraph graph;
    const int output = appendRgbSplit(graph, graph.source(image), s, edges);
    return graph.execute(output, image.format(), RenderBackend::Vulkan, report);
}
QImage composite(const QImage &image, const QImage &top, int mode, float opacity, unsigned quantization) {
    if (image.size() != top.size() || !GpuRenderer::shouldAttempt(image.size(), 8))
        return {};
    GpuGraph graph;
    int in = graph.source(image), over = graph.source(top);
    GpuParameters p{};
    p[GpuParam::Composite::mode] = mode;
    p[GpuParam::Composite::opacity] = opacity;
    return graph.execute(graph.pass(GpuOp::Composite, in, over, p, quantization), image.format());
}
} // namespace compositor::GpuOperations
