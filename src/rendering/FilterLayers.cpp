#include "rendering/FilterLayers.h"
#include "io/AdjustmentJson.h"
#include "rendering/DeepImage.h"
#include "rendering/RasterOperations.h"

extern "C" {
#include "AdjustPixels.h"
}

#include <QColorSpace>
#include <QJsonArray>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <mutex>
#include <vector>

namespace compositor {
namespace {
double clamped(double value, double lo, double hi, double fallback) { return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback; }
double read(const QJsonObject &o, const char *key, double fallback) { return o.value(QLatin1String(key)).toDouble(fallback); }
QJsonObject colorJson(const QColor &c) { return {{QStringLiteral("red"), c.redF()}, {QStringLiteral("green"), c.greenF()}, {QStringLiteral("blue"), c.blueF()}}; }

float toLinear(float c) { return c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f); }
float toGamma(float c) { return c <= .0031308f ? c * 12.92f : 1.055f * std::pow(c, 1 / 2.4f) - .055f; }

// The normalized Gaussian weights CIGaussianBlur uses for a standard deviation of `sigma`, out to three of them.
std::vector<float> gaussianWeights(double sigma)
{
    const int reach = std::max(1, int(std::ceil(sigma * 3)));
    std::vector<float> weights(size_t(2 * reach + 1));
    double total = 0;
    for (int i = -reach; i <= reach; ++i) total += std::exp(-.5 * i * i / (sigma * sigma));
    for (int i = -reach; i <= reach; ++i) weights[size_t(i + reach)] = float(std::exp(-.5 * i * i / (sigma * sigma)) / total);
    return weights;
}

// One axis of a separable blur over premultiplied float RGBA, the edge pixels carried on past the sides.
void blurAxis(const std::vector<float> &in, std::vector<float> &out, int w, int h, const std::vector<float> &weights, bool vertical)
{
    const int reach = int(weights.size() / 2);
    #pragma omp parallel for schedule(static) if(qint64(w) * h > 65536)
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float sum[4] = {0, 0, 0, 0};
            for (int k = -reach; k <= reach; ++k) {
                const int sx = vertical ? x : std::clamp(x + k, 0, w - 1), sy = vertical ? std::clamp(y + k, 0, h - 1) : y;
                const float *p = &in[(size_t(sy) * w + sx) * 4];
                const float weight = weights[size_t(k + reach)];
                for (int c = 0; c < 4; ++c) sum[c] += p[c] * weight;
            }
            std::copy_n(sum, 4, &out[(size_t(y) * w + x) * 4]);
        }
}

QImage fromFloat(const std::vector<float> &pixels, const QImage &like, int w, int h)
{
    QImage out(w, h, QImage::Format_RGBA32FPx4_Premultiplied);
    if (out.isNull()) return {};
    for (int y = 0; y < h; ++y) std::copy_n(&pixels[size_t(y) * w * 4], size_t(w) * 4, reinterpret_cast<float *>(out.scanLine(y)));
    QImage result = DeepImage::isDeep(like) ? out.convertToFormat(DeepImage::canonical(like).format())
                                            : out.convertToFormat(QImage::Format_RGBA64_Premultiplied).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    result.setColorSpace(like.colorSpace());
    for (const auto &key : like.textKeys()) result.setText(key, like.text(key));
    return result;
}

QImage sharpenCpu(const QImage &image, double amount, double sigma)
{
    const bool hdr = DeepImage::isHDR(image);
    const QImage source = DeepImage::floating(image);
    const int w = source.width(), h = source.height();
    std::vector<float> linear(size_t(w) * h * 4), blurred(linear.size()), work(linear.size());
    #pragma omp parallel for schedule(static) if(qint64(w) * h > 65536)
    for (int y = 0; y < h; ++y) {
        const auto *in = reinterpret_cast<const float *>(source.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const float a = in[x * 4 + 3];
            float *p = &linear[(size_t(y) * w + x) * 4];
            for (int c = 0; c < 3; ++c) {
                const float straight = a > 0 ? in[x * 4 + c] / a : 0.f;
                p[c] = toLinear(hdr ? std::max(0.f, straight) : std::min(1.f, straight)) * a;
            }
            p[3] = a;
        }
    }
    const std::vector<float> weights = gaussianWeights(sigma);
    blurAxis(linear, work, w, h, weights, false);
    blurAxis(work, blurred, w, h, weights, true);
    const float k = float(amount);
    #pragma omp parallel for schedule(static) if(qint64(w) * h > 65536)
    for (int i = 0; i < w * h; ++i) {
        float *p = &linear[size_t(i) * 4];
        const float *b = &blurred[size_t(i) * 4];
        float s[4];
        for (int c = 0; c < 4; ++c) s[c] = p[c] + k * (p[c] - b[c]);
        const float alpha = std::clamp(s[3], 0.f, 1.f);
        for (int c = 0; c < 3; ++c) {
            float straight = alpha > 0 ? s[c] / alpha : 0.f;
            straight = hdr ? std::max(0.f, straight) : std::clamp(straight, 0.f, 1.f);
            p[c] = toGamma(straight) * alpha;
        }
        p[3] = alpha;
    }
    return fromFloat(linear, image, w, h);
}

std::once_flag nonlocalOnce;
} // namespace

QJsonObject VignetteLayerSettings::json() const
{
    return {{QStringLiteral("amount"), amount}, {QStringLiteral("color"), colorJson(color)}, {QStringLiteral("midpoint"), midpoint},
            {QStringLiteral("roundness"), roundness}, {QStringLiteral("feather"), feather}, {QStringLiteral("highlights"), highlights}};
}
VignetteLayerSettings VignetteLayerSettings::fromJson(const QJsonObject &o)
{
    VignetteLayerSettings s;
    s.amount = clamped(read(o, "amount", 35), 0, 100, 35); s.midpoint = clamped(read(o, "midpoint", 50), 0, 100, 50);
    s.roundness = clamped(read(o, "roundness", 100), -100, 100, 100); s.feather = clamped(read(o, "feather", 60), 0, 100, 60);
    s.highlights = clamped(read(o, "highlights", 25), 0, 100, 25);
    const QJsonObject c = o.value(QStringLiteral("color")).toObject();
    s.color = QColor::fromRgbF(float(clamped(read(c, "red", 0), 0, 1, 0)), float(clamped(read(c, "green", 0), 0, 1, 0)),
                               float(clamped(read(c, "blue", 0), 0, 1, 0)));
    return s;
}
QJsonObject TonalLayerSettings::json() const
{
    return {{QStringLiteral("amount"), amount}, {QStringLiteral("radius"), radius}, {QStringLiteral("shadows"), shadows},
            {QStringLiteral("midtones"), midtones}, {QStringLiteral("highlights"), highlights}};
}
TonalLayerSettings TonalLayerSettings::fromJson(const QJsonObject &o)
{
    TonalLayerSettings s;
    s.amount = clamped(read(o, "amount", 50), 0, 100, 50); s.radius = clamped(read(o, "radius", 16), 1, 100, 16);
    s.shadows = clamped(read(o, "shadows", 40), -100, 100, 40); s.midtones = clamped(read(o, "midtones", 60), -100, 100, 60);
    s.highlights = clamped(read(o, "highlights", 30), -100, 100, 30);
    return s;
}
SharpenSettings SharpenSettings::normalized() const
{
    return {clamped(amount, 0, 300, 100), clamped(radius, .1, 20, 1)};
}
QJsonObject SharpenSettings::json() const { return {{QStringLiteral("amount"), amount}, {QStringLiteral("radius"), radius}}; }
SharpenSettings SharpenSettings::fromJson(const QJsonObject &o) { return SharpenSettings{read(o, "amount", 100), read(o, "radius", 1)}.normalized(); }
ReduceNoiseSettings ReduceNoiseSettings::normalized() const
{
    return {clamped(strength, 0, 100, 50), clamped(details, 0, 100, 50), clamped(color, 0, 100, 50)};
}
QJsonObject ReduceNoiseSettings::json() const
{
    return {{QStringLiteral("strength"), strength}, {QStringLiteral("details"), details}, {QStringLiteral("color"), color}};
}
ReduceNoiseSettings ReduceNoiseSettings::fromJson(const QJsonObject &o)
{
    return ReduceNoiseSettings{read(o, "strength", 50), read(o, "details", 50), read(o, "color", 50)}.normalized();
}

namespace FilterLayers {
QImage sharpen(const QImage &image, const SharpenSettings &settings, double scale, RenderBackend backend, bool *accelerated)
{
    if (accelerated) *accelerated = false;
    if (image.isNull()) return image;
    const SharpenSettings s = settings.normalized();
    if (s.amount <= 0) return DeepImage::canonical(image);
    const double sigma = std::max(.05, s.radius * scale);
    // On the GPU for 8- and 16-bit layers when the renderer allows it; HDR light past white stays on the CPU's floats.
    const bool gpu = !DeepImage::isHDR(image) && (backend == RenderBackend::Vulkan
                     || (backend == RenderBackend::Auto && GpuRenderer::shouldAttempt(image.size(), 8)));
    if (gpu) {
        const bool deep = DeepImage::isDeep(image);
        const QImage::Format format = deep ? QImage::Format_RGBA64_Premultiplied : QImage::Format_RGBA8888_Premultiplied;
        const QImage input = image.convertToFormat(format);
        const QJsonObject adjustment{{QStringLiteral("kind"), QStringLiteral("Sharpen")},
            {QStringLiteral("filterLayerSettings"), QJsonObject{{QStringLiteral("sharpen"), SharpenSettings{s.amount, sigma}.json()}}}};
        // A large picture goes in bands of rows, each with the blur's reach above and below, so the float planes the
        // graph works in stay within the GPU's memory budget; the edges past the picture's are carried on as before.
        const int margin = int(std::ceil(3.5 * std::min(sigma, 250.0))) + 4;
        const int bandRows = std::max(64, int((qint64(1) << 22) / std::max(1, input.width())));
        QImage result(input.size(), format);
        for (int top = 0; top < input.height() && !result.isNull(); top += bandRows) {
            const int bottom = std::min(input.height(), top + bandRows);
            const int from = std::max(0, top - margin), to = std::min(input.height(), bottom + margin);
            GpuGraph graph;
            const int source = graph.source(top == 0 && to == input.height() ? input : input.copy(0, from, input.width(), to - from));
            const int out = appendLayer(graph, source, adjustment, {}, QSize(input.width(), to - from));
            const QImage band = graph.execute(out, format, backend);
            if (band.isNull()) { result = QImage(); break; }
            const qsizetype bytes = qsizetype(input.width()) * (deep ? 8 : 4);
            for (int y = top; y < bottom; ++y) std::memcpy(result.scanLine(y), band.constScanLine(y - from), bytes);
        }
        if (!result.isNull()) {
            if (accelerated) *accelerated = true;
            result.setColorSpace(image.colorSpace());
            for (const auto &key : image.textKeys()) result.setText(key, image.text(key));
            return result;
        }
    }
    GpuRenderer::CpuScope cpu;
    return sharpenCpu(image, s.amount / 100, sigma);
}

QImage reduceNoise(const QImage &image, const ReduceNoiseSettings &settings, double scale)
{
    if (image.isNull()) return image;
    const ReduceNoiseSettings s = settings.normalized();
    installGpuNonlocalMeans();
    // The noise reduction mixes channels (its brightness and color are smoothed apart), as Camera Raw's does.
    return DeepImage::through(image, [&](const QImage &low) {
        QImage result = low.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        if (s.isIdentity()) return result;
        adjust_camera_raw_detail(result.bits(), size_t(result.width()), size_t(result.height()), size_t(result.bytesPerLine()),
                                 0, 0, 0, 0, s.strength, s.details, 0, s.color, 50, 50, scale);
        return result;
    }, true);
}

QImage vignetteLayer(const QImage &image, const VignetteLayerSettings &s, const QRectF &canvas)
{
    if (DeepImage::isDeep(image)) return DeepImage::through(image, [&](const QImage &low) { return vignetteLayer(low, s, canvas); }, false);
    QImage result = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (result.isNull() || s.amount <= 0 || canvas.width() <= 0 || canvas.height() <= 0) return result;
    adjust_colored_vignette(result.bits(), size_t(result.width()), size_t(result.height()), size_t(result.bytesPerLine()),
                            canvas.x(), canvas.y(), canvas.width(), canvas.height(), 0, s.amount, s.midpoint, s.roundness,
                            s.feather, s.highlights, s.color.redF(), s.color.greenF(), s.color.blueF());
    return result;
}

QImage applyLayer(const QImage &image, const QJsonObject &adjustment, const QRectF &canvas)
{
    const QString kind = adjustment.value(QStringLiteral("kind")).toString();
    const QJsonObject f = adjustment.value(QStringLiteral("filterLayerSettings")).toObject();
    if (kind == QStringLiteral("Vignette")) return vignetteLayer(image, VignetteLayerSettings::fromJson(f.value(QStringLiteral("vignette")).toObject()), canvas);
    if (kind == QStringLiteral("Tonal Contrast")) {
        const auto s = TonalLayerSettings::fromJson(f.value(QStringLiteral("tonal")).toObject());
        return RasterOperations::tonalContrast(image, s.amount, s.radius, s.shadows, s.midtones, s.highlights);
    }
    if (kind == QStringLiteral("Sharpen")) return sharpen(image, SharpenSettings::fromJson(f.value(QStringLiteral("sharpen")).toObject()));
    if (kind == QStringLiteral("Reduce Noise")) return reduceNoise(image, ReduceNoiseSettings::fromJson(f.value(QStringLiteral("reduceNoise")).toObject()));
    if (kind == QStringLiteral("Lens Effects")) {
        // Measured by the document's longer side, and only what's in the document (mac 338a609).
        const double side = std::max(canvas.width(), canvas.height());
        return lensSettingsFromJson(f.value(QStringLiteral("lens")).toObject()).apply(image, RenderBackend::Auto, nullptr, side);
    }
    return {};
}

int reach(const QJsonObject &adjustment)
{
    const QString kind = adjustment.value(QStringLiteral("kind")).toString();
    const QJsonObject f = adjustment.value(QStringLiteral("filterLayerSettings")).toObject();
    if (kind == QStringLiteral("Tonal Contrast")) return int(std::ceil(TonalLayerSettings::fromJson(f.value(QStringLiteral("tonal")).toObject()).radius * 3 + 2));
    if (kind == QStringLiteral("Sharpen")) return int(std::ceil(SharpenSettings::fromJson(f.value(QStringLiteral("sharpen")).toObject()).radius * 3 + 2));
    // The search, its patches and the color smoothing (mac ReduceNoiseKernel.margin).
    if (kind == QStringLiteral("Reduce Noise")) return 24;
    // A light anywhere can streak or scatter across the whole picture.
    if (kind == QStringLiteral("Lens Effects")) return 1000000;
    return 0;
}

bool drawsOnGraph(const QString &kind)
{
    return kind == QStringLiteral("Vignette") || kind == QStringLiteral("Tonal Contrast") || kind == QStringLiteral("Sharpen");
}

int appendLayer(GpuGraph &graph, int input, const QJsonObject &adjustment, QPoint origin, QSize canvas)
{
    const QString kind = adjustment.value(QStringLiteral("kind")).toString();
    const QJsonObject f = adjustment.value(QStringLiteral("filterLayerSettings")).toObject();
    constexpr unsigned bytes8 = unsigned(GpuQuantization::Unorm8);
    if (kind == QStringLiteral("Vignette")) {
        const auto s = VignetteLayerSettings::fromJson(f.value(QStringLiteral("vignette")).toObject());
        if (s.amount <= 0) return input;
        using V = GpuParam::Vignette;
        GpuParameters p{};
        p[V::frameX] = float(-origin.x()); p[V::frameY] = float(-origin.y());
        p[V::frameWidth] = float(canvas.width()); p[V::frameHeight] = float(canvas.height());
        p[V::amount] = float(s.amount); p[V::midpoint] = float(s.midpoint); p[V::roundness] = float(s.roundness);
        p[V::feather] = float(s.feather); p[V::highlights] = float(s.highlights);
        p[V::red] = float(s.color.redF()); p[V::green] = float(s.color.greenF()); p[V::blue] = float(s.color.blueF());
        return graph.pass(GpuOp::Vignette, input, -1, p, bytes8);
    }
    if (kind == QStringLiteral("Tonal Contrast")) {
        const auto s = TonalLayerSettings::fromJson(f.value(QStringLiteral("tonal")).toObject());
        if (s.amount <= 0 || (s.shadows == 0 && s.midtones == 0 && s.highlights == 0)) return input;
        using T = GpuParam::TonalContrast;
        GpuParameters p{};
        p[T::amount] = float(s.amount); p[T::shadows] = float(s.shadows); p[T::midtones] = float(s.midtones); p[T::highlights] = float(s.highlights);
        // The blur the CPU takes (RasterOperations::gaussianBlur, at 8 bits) is the byte algorithm's.
        return graph.pass(GpuOp::TonalContrast, input, graph.gaussian(input, s.radius, true), p, bytes8);
    }
    if (kind == QStringLiteral("Sharpen")) {
        // `radius` here is already in the graph's pixels.
        const QJsonObject o = f.value(QStringLiteral("sharpen")).toObject();
        const double amount = clamped(read(o, "amount", 100), 0, 300, 100), sigma = std::max(.05, read(o, "radius", 1));
        if (amount <= 0) return input;
        GpuParameters linear{};
        linear[GpuParam::Sharpen::stage] = 0;
        const int light = graph.pass(GpuOp::Sharpen, input, -1, linear);
        // The exact Gaussian at every size, as the CPU blurs (gaussianWeights).
        const std::vector<float> weights = gaussianWeights(sigma);
        QImage table(int(weights.size()), 1, QImage::Format_RGBA32FPx4_Premultiplied);
        auto *out = reinterpret_cast<float *>(table.bits());
        for (size_t i = 0; i < weights.size(); ++i) for (int c = 0; c < 4; ++c) out[i * 4 + c] = weights[i];
        const int lut = graph.source(table);
        int blurred = light;
        for (int axis = 0; axis < 2; ++axis) {
            GpuParameters g{};
            g[GpuParam::Gaussian::reach] = float(weights.size() / 2);
            g[GpuParam::Gaussian::axis] = float(axis);
            blurred = graph.pass(GpuOp::Gaussian, blurred, lut, g);
        }
        GpuParameters back{};
        back[GpuParam::Sharpen::stage] = 1;
        back[GpuParam::Sharpen::amount] = float(amount / 100);
        const unsigned depth = graph.nodes()[size_t(input)].image.depth() > 32 ? unsigned(GpuQuantization::Unorm16) : bytes8;
        return graph.pass(GpuOp::Sharpen, light, blurred, back, depth);
    }
    return -1;
}

bool nonlocalMeansGpu(const float *luma, const float *coverage, float *out, int width, int height, int search, int patch,
                      double h, double sigma)
{
    constexpr int margin = 9;
    if (width <= 0 || height <= 0 || search + patch > margin || patch > 2 || h <= 0) return false;
    if (GpuRenderer::backend() == RenderBackend::Cpu || !GpuRenderer::shouldAttempt(QSize(width, height), 64)) return false;
    // Bands of rows, each with the reach of its search above and below, so no one dispatch runs long.
    const int rows = std::max(64, int((qint64(1) << 21) / width));
    for (int first = 0; first < height; first += rows) {
        const int last = std::min(height, first + rows), top = std::max(0, first - margin), bottom = std::min(height, last + margin);
        const QSize size(width, bottom - top);
        auto values = std::make_shared<std::vector<float>>(luma + size_t(top) * width, luma + size_t(bottom) * width);
        auto cover = std::make_shared<std::vector<float>>(coverage + size_t(top) * width, coverage + size_t(bottom) * width);
        GpuGraph graph;
        GpuParameters p{};
        p[GpuParam::NonlocalMeans::search] = float(search);
        p[GpuParam::NonlocalMeans::patch] = float(patch);
        p[GpuParam::NonlocalMeans::inverseH2] = float(1 / (h * h));
        p[GpuParam::NonlocalMeans::allowance] = float(2 * sigma * sigma);
        p[GpuParam::NonlocalMeans::area] = float((2 * patch + 1) * (2 * patch + 1));
        const int pass = graph.pass(GpuOp::NonlocalMeans, graph.channel(values, size), graph.channel(cover, size), p, 0, size);
        const std::vector<float> result = graph.executeChannel(pass, RenderBackend::Vulkan);
        if (result.size() != size_t(size.width()) * size.height()) return false;
        std::copy_n(result.data() + size_t(first - top) * width, size_t(last - first) * width, out + size_t(first) * width);
    }
    return true;
}

void installGpuNonlocalMeans()
{
    std::call_once(nonlocalOnce, [] {
        adjust_nonlocal_means_gpu = [](const float *luma, const float *coverage, float *out, size_t width, size_t height, int search,
                                       int patch, double h, double sigma) -> int {
            return nonlocalMeansGpu(luma, coverage, out, int(width), int(height), search, patch, h, sigma) ? 1 : 0;
        };
    });
}
} // namespace FilterLayers
} // namespace compositor
