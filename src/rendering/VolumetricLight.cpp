#include "rendering/VolumetricLight.h"
#include "rendering/DeepImage.h"
#include "rendering/RimLight.h"
#include "rendering/SubjectRemoval.h"

#include <onnxruntime_cxx_api.h>

#include <QColorSpace>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>
#include <thread>

namespace compositor {
namespace {
double clamped(double value, double lo, double hi, double fallback) { return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback; }
float toLinear(float c) { return c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f); }
float toGamma(float c) { return c <= .0031308f ? c * 12.92f : 1.055f * std::pow(c, 1 / 2.4f) - .055f; }
float smoothstep(float a, float b, float x) { const float t = std::clamp((x - a) / (b - a), 0.f, 1.f); return t * t * (3 - 2 * t); }
constexpr double fullIntensity = 5, fullHaze = 1;

// Planes of `channels` floats a pixel, the Core Image steps as operations on them. Blurs are the Gaussian as Core
// Image's applyingGaussianBlur makes it after clampedToExtent: the exact kernel to a standard deviation of 10, three box
// passes past that, the edges carried on.
struct Plane {
    int w = 0, h = 0, channels = 1;
    std::vector<float> v;
    Plane() = default;
    Plane(int width, int height, int c, float fill = 0) : w(width), h(height), channels(c), v(size_t(width) * height * c, fill) {}
    float *at(int x, int y) { return v.data() + (size_t(y) * w + x) * channels; }
    const float *at(int x, int y) const { return v.data() + (size_t(y) * w + x) * channels; }
    const float *clampAt(int x, int y) const { return at(std::clamp(x, 0, w - 1), std::clamp(y, 0, h - 1)); }
};

Plane boxAxis(const Plane &in, int radius, int axis)
{
    if (radius <= 0) return in;
    Plane out(in.w, in.h, in.channels);
    const int length = axis ? in.h : in.w, lines = axis ? in.w : in.h, c = in.channels;
    const float divisor = float(2 * radius + 1);
    #pragma omp parallel for schedule(static)
    for (int line = 0; line < lines; ++line) {
        const auto px = [&](int pos) { return axis ? in.clampAt(line, pos) : in.clampAt(pos, line); };
        std::vector<float> sum(size_t(c), 0.f);
        for (int i = -radius; i <= radius; ++i) for (int k = 0; k < c; ++k) sum[size_t(k)] += px(i)[k];
        for (int pos = 0; pos < length; ++pos) {
            float *o = axis ? out.at(line, pos) : out.at(pos, line);
            for (int k = 0; k < c; ++k) o[k] = sum[size_t(k)] / divisor;
            const float *add = px(pos + radius + 1), *sub = px(pos - radius);
            for (int k = 0; k < c; ++k) sum[size_t(k)] += add[k] - sub[k];
        }
    }
    return out;
}

Plane blurred(Plane in, double sigma)
{
    if (!std::isfinite(sigma) || sigma <= 0) return in;
    sigma = std::max(.1, sigma);
    if (sigma <= 10) {
        const int reach = std::max(1, int(std::ceil(sigma * 3)));
        std::vector<float> weights(size_t(reach) * 2 + 1);
        double total = 0;
        for (int i = -reach; i <= reach; ++i) total += std::exp(-.5 * i * i / (sigma * sigma));
        for (int i = -reach; i <= reach; ++i) weights[size_t(i + reach)] = float(std::exp(-.5 * i * i / (sigma * sigma)) / total);
        for (int axis = 0; axis < 2; ++axis) {
            Plane out(in.w, in.h, in.channels);
            #pragma omp parallel for schedule(static)
            for (int y = 0; y < in.h; ++y)
                for (int x = 0; x < in.w; ++x) {
                    float *o = out.at(x, y);
                    for (int i = -reach; i <= reach; ++i) {
                        const float *p = axis ? in.clampAt(x, y + i) : in.clampAt(x + i, y);
                        for (int k = 0; k < in.channels; ++k) o[k] += p[k] * weights[size_t(i + reach)];
                    }
                }
            in = std::move(out);
        }
        return in;
    }
    int lower = int(std::floor(std::sqrt(4 * sigma * sigma + 1)));
    if (!(lower & 1)) --lower;
    const int count = int(std::lround((12 * sigma * sigma - 3 * lower * lower - 12 * lower - 9) / (-4. * lower - 4)));
    for (int iteration = 0; iteration < 3; ++iteration)
        for (int axis = 0; axis < 2; ++axis) in = boxAxis(in, std::max(0, ((iteration < count ? lower : lower + 2) - 1) / 2), axis);
    return in;
}

template <class F> Plane mapped(const Plane &a, int channels, F f)
{
    Plane out(a.w, a.h, channels);
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < a.w * a.h; ++i) f(a.v.data() + size_t(i) * a.channels, out.v.data() + size_t(i) * channels, i);
    return out;
}

// A gray plane at `size`, bilinear from a smaller map (Core Image's samplingLinear enlargement).
Plane enlarged(const std::vector<float> &values, int width, int height, QSize size)
{
    Plane out(size.width(), size.height(), 1);
    #pragma omp parallel for schedule(static)
    for (int y = 0; y < size.height(); ++y) {
        const float fy = std::clamp((y + .5f) * height / size.height() - .5f, 0.f, float(height - 1));
        const int y0 = int(fy), y1 = std::min(height - 1, y0 + 1);
        const float ty = fy - y0;
        for (int x = 0; x < size.width(); ++x) {
            const float fx = std::clamp((x + .5f) * width / size.width() - .5f, 0.f, float(width - 1));
            const int x0 = int(fx), x1 = std::min(width - 1, x0 + 1);
            const float tx = fx - x0;
            const auto v = [&](int px, int py) { return values[size_t(py) * width + px]; };
            *out.at(x, y) = (v(x0, y0) * (1 - tx) + v(x1, y0) * tx) * (1 - ty) + (v(x0, y1) * (1 - tx) + v(x1, y1) * tx) * ty;
        }
    }
    return out;
}

// GuidedMatte.box and .filter (He et al.'s guided filter with a square box), as macOS refines its depth.
std::vector<float> box(const std::vector<float> &source, int width, int height, int radius)
{
    Plane p(width, height, 1); p.v = source;
    // Two running-sum passes, the edges carried on; boxAxis is the same box.
    return boxAxis(boxAxis(p, radius, 0), radius, 1).v;
}
std::vector<float> guidedFilter(const std::vector<float> &mask, const std::vector<float> &guide, int width, int height, int radius, float epsilon)
{
    const size_t count = size_t(width) * height;
    const auto meanGuide = box(guide, width, height, radius), meanMask = box(mask, width, height, radius);
    std::vector<float> squares(count), products(count);
    for (size_t i = 0; i < count; ++i) { squares[i] = guide[i] * guide[i]; products[i] = guide[i] * mask[i]; }
    const auto meanSquares = box(squares, width, height, radius), meanProducts = box(products, width, height, radius);
    std::vector<float> slope(count), offset(count);
    for (size_t i = 0; i < count; ++i) {
        const float variance = meanSquares[i] - meanGuide[i] * meanGuide[i], covariance = meanProducts[i] - meanGuide[i] * meanMask[i];
        slope[i] = covariance / (variance + epsilon);
        offset[i] = meanMask[i] - slope[i] * meanGuide[i];
    }
    const auto meanSlope = box(slope, width, height, radius), meanOffset = box(offset, width, height, radius);
    std::vector<float> result(count);
    for (size_t i = 0; i < count; ++i) result[i] = std::clamp(meanSlope[i] * guide[i] + meanOffset[i], 0.f, 1.f);
    return result;
}

// `image` drawn at `size` in 8 bits (premultiplied, high quality), as macOS's pixels(of:width:height:) draws it.
QImage drawn(const QImage &image, QSize size)
{
    return DeepImage::canonical(image).convertToFormat(QImage::Format_RGBA8888_Premultiplied)
        .scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

struct DepthRuntime {
    Ort::Env environment{ORT_LOGGING_LEVEL_WARNING, "CompositorLX depth"};
    std::unique_ptr<Ort::Session> session;
    std::string error;
    bool tried = false;
    QMutex lock;
    qint64 cachedKey = 0;
    std::optional<DepthMap> cached;
};
DepthRuntime &depthRuntime() { static DepthRuntime runtime; return runtime; }

QImage linearPremultiplied(const QImage &layer)
{
    const QImage f = DeepImage::floating(layer);
    QImage linear(f.size(), QImage::Format_RGBA32FPx4_Premultiplied);
    #pragma omp parallel for schedule(static)
    for (int y = 0; y < f.height(); ++y) {
        const auto *in = reinterpret_cast<const float *>(f.constScanLine(y));
        auto *o = reinterpret_cast<float *>(linear.scanLine(y));
        for (int x = 0; x < f.width(); ++x) {
            const float a = in[x * 4 + 3];
            for (int c = 0; c < 3; ++c) { const float s = a > 0 ? in[x * 4 + c] / a : 0.f; o[x * 4 + c] = toLinear(std::max(0.f, s)) * a; }
            o[x * 4 + 3] = a;
        }
    }
    return linear;
}
Plane planeOf(const QImage &linear)
{
    Plane p(linear.width(), linear.height(), 4);
    for (int y = 0; y < p.h; ++y) std::copy_n(reinterpret_cast<const float *>(linear.constScanLine(y)), size_t(p.w) * 4, p.at(0, y));
    return p;
}
Plane grayOf(const QImage &mask, QSize size)
{
    const QImage g = mask.convertToFormat(QImage::Format_Grayscale8).scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    Plane p(size.width(), size.height(), 1);
    for (int y = 0; y < p.h; ++y) { const uchar *row = g.constScanLine(y); for (int x = 0; x < p.w; ++x) *p.at(x, y) = row[x] / 255.f; }
    return p;
}

// The subject's outline for the depth: the general subject mask, smoothed a touch and made crisp (mac firmed), so the
// haze meets the subject cleanly. macOS refines it further with Vision's person segmentation and body pose, which have
// no counterpart here.
std::optional<Plane> outline(const QImage &image)
{
    const QImage general = SubjectRemoval::rawMask(DeepImage::canonical(image).convertToFormat(QImage::Format_RGBA8888_Premultiplied));
    if (general.isNull()) return std::nullopt;
    const double side = std::max(image.width(), image.height());
    Plane mask = blurred(grayOf(general, image.size()), std::max(1.0, side * .0015));
    for (float &value : mask.v) value = std::clamp(value * 2.5f - 1.1f, 0.f, 1.f);
    return mask;
}

// What doesn't change as the sliders move, kept for the last image.
struct Prepared {
    QMutex lock;
    qint64 key = 0;
    QSize size;
    bool hasScene = false;
    Plane subject, near, glows, ambient, linear, half, halfSubject;
    float brightest = .05f;
    bool hasDepth = false;
    double depthContrast = -1;
    bool keepsSubjectClear = true;
    std::optional<Plane> depth;
    std::vector<float> distances;
    bool hasBeams = false;
    QPointF beamsLight;
    double beamsSize = -1, beamsLength = -1, beamsShafts = -1;
    Plane beams;
};
Prepared &prepared() { static Prepared value; return value; }

// The beams' gathering on the CPU, as the VolumetricBeams op works it on the GPU.
// rendering.comp's volumetricHash: an integer hash, the same on the CPU and every device.
float hash(uint32_t n)
{
    n ^= n >> 16; n *= 0x7feb352du; n ^= n >> 15; n *= 0x846ca68bu; n ^= n >> 16;
    return float(n >> 8) / 16777216.f;
}
float around(float angle, float cells, uint32_t seed)
{
    const float u = (angle / 6.2831853f + .5f) * cells, i = std::floor(u);
    float f = u - i;
    f = f * f * (3 - 2 * f);
    const int count = int(cells);
    int k = int(i) % count;
    if (k < 0) k += count;
    const float a = hash(uint32_t(k) * 2654435761u + seed), b = hash(uint32_t((k + 1) % count) * 2654435761u + seed);
    return a + (b - a) * f;
}
float sampled(const Plane &p, float x, float y, int c)
{
    const float px = std::clamp(x - .5f, 0.f, float(p.w - 1)), py = std::clamp(y - .5f, 0.f, float(p.h - 1));
    const int x0 = int(px), y0 = int(py), x1 = std::min(p.w - 1, x0 + 1), y1 = std::min(p.h - 1, y0 + 1);
    const float fx = px - x0, fy = py - y0;
    return (p.at(x0, y0)[c] * (1 - fx) + p.at(x1, y0)[c] * fx) * (1 - fy) + (p.at(x0, y1)[c] * (1 - fx) + p.at(x1, y1)[c] * fx) * fy;
}
} // namespace

VolumetricLightSettings VolumetricLightSettings::normalized() const
{
    VolumetricLightSettings r = *this;
    r.color = QColor::fromRgbF(std::clamp(color.redF(), 0.f, 1.f), std::clamp(color.greenF(), 0.f, 1.f), std::clamp(color.blueF(), 0.f, 1.f));
    r.intensity = clamped(intensity, 0, 100, 30); r.length = clamped(length, 0, 100, 0); r.haze = clamped(haze, 0, 100, 60);
    r.size = clamped(size, 0, 100, 20); r.shafts = clamped(shafts, 0, 100, 0); r.distance = clamped(distance, 0, 100, 30);
    r.depthContrast = clamped(depthContrast, 0, 100, 0);
    if (center && !(std::isfinite(center->x()) && std::isfinite(center->y()))) r.center.reset();
    return r;
}

DepthMap DepthMap::equalized(float amount) const
{
    if (amount <= 0 || values.size() < 2) return *this;
    std::vector<size_t> order(values.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return values[a] < values[b]; });
    DepthMap spread = *this;
    const float last = float(values.size() - 1);
    for (size_t rank = 0; rank < order.size(); ++rank) spread.values[order[rank]] += (float(rank) / last - values[order[rank]]) * amount;
    return spread;
}

QString DepthModel::modelPath()
{
    const QString environment = qEnvironmentVariable("COMPOSITOR_DEPTH_MODEL");
    if (QFileInfo::exists(environment)) return environment;
    const QDir executable(QCoreApplication::applicationDirPath());
    for (const QString &candidate : {executable.absoluteFilePath(QStringLiteral("../share/compositor-lx/models/depthmini.onnx")),
                                     executable.absoluteFilePath(QStringLiteral("models/depthmini.onnx")),
                                     executable.absoluteFilePath(QStringLiteral("../models/depthmini.onnx")),
                                     QDir::current().absoluteFilePath(QStringLiteral("models/depthmini.onnx")),
                                     QDir::current().absoluteFilePath(QStringLiteral("build/models/depthmini.onnx"))})
        if (QFileInfo::exists(candidate)) return candidate;
    return executable.absoluteFilePath(QStringLiteral("../share/compositor-lx/models/depthmini.onnx"));
}

bool DepthModel::isAvailable() { return QFileInfo::exists(modelPath()); }

QSize DepthModel::shape(QSize size)
{
    static const std::array<QSize, 5> shapes{QSize(512, 512), QSize(512, 384), QSize(512, 288), QSize(384, 512), QSize(288, 512)};
    const double aspect = std::log(double(size.width()) / std::max(1, size.height()));
    return *std::min_element(shapes.begin(), shapes.end(), [aspect](QSize a, QSize b) {
        return std::abs(std::log(double(a.width()) / a.height()) - aspect) < std::abs(std::log(double(b.width()) / b.height()) - aspect);
    });
}

std::optional<DepthMap> DepthModel::depth(const QImage &image, QString *error)
{
    if (image.isNull()) return std::nullopt;
    DepthRuntime &runtime = depthRuntime();
    QMutexLocker locker(&runtime.lock);
    if (runtime.cached && runtime.cachedKey == image.cacheKey()) return runtime.cached;
    if (!runtime.tried) {
        runtime.tried = true;
        try {
            Ort::SessionOptions options;
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            options.SetIntraOpNumThreads(int(std::max(1u, std::thread::hardware_concurrency() / 2)));
            const QByteArray path = QFile::encodeName(modelPath());
            runtime.session = std::make_unique<Ort::Session>(runtime.environment, path.constData(), options);
        } catch (const Ort::Exception &exception) { runtime.error = exception.what(); }
    }
    if (!runtime.session) { if (error) *error = QString::fromStdString(runtime.error); return std::nullopt; }
    const QSize shape = DepthModel::shape(image.size());
    const QImage small = drawn(image, shape);
    const int columns = shape.width(), rows = shape.height();
    std::vector<float> input(size_t(3) * columns * rows);
    for (int y = 0; y < rows; ++y) {
        const uchar *row = small.constScanLine(y);
        for (int x = 0; x < columns; ++x)
            for (int c = 0; c < 3; ++c) input[size_t(c) * columns * rows + size_t(y) * columns + x] = row[x * 4 + c];
    }
    std::vector<float> values;
    try {
        const std::array<int64_t, 4> dims{1, 3, rows, columns};
        Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), dims.data(), dims.size());
        const char *inputs[] = {"image"}, *outputs[] = {"depth"};
        auto result = runtime.session->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
        const float *data = result.front().GetTensorData<float>();
        values.assign(data, data + size_t(columns) * rows);
    } catch (const Ort::Exception &exception) { if (error) *error = QString::fromUtf8(exception.what()); return std::nullopt; }
    const auto [lowIt, highIt] = std::minmax_element(values.begin(), values.end());
    const float low = *lowIt, high = *highIt;
    if (!(high > low)) return std::nullopt;
    // Enlarged to the photo's shape, then pulled onto its edges, its brightness the guide.
    constexpr int refinedSide = 640;
    const double scale = double(refinedSide) / std::max(image.width(), image.height());
    const int width = std::max(8, int(std::lround(image.width() * scale))), height = std::max(8, int(std::lround(image.height() * scale)));
    const QImage guideImage = drawn(image, QSize(width, height));
    std::vector<float> guide(size_t(width) * height), enlargedValues(size_t(width) * height);
    for (int y = 0; y < height; ++y) {
        const uchar *row = guideImage.constScanLine(y);
        for (int x = 0; x < width; ++x) guide[size_t(y) * width + x] = (.2126f * row[x * 4] + .7152f * row[x * 4 + 1] + .0722f * row[x * 4 + 2]) / 255;
    }
    for (int y = 0; y < height; ++y) {
        const float fy = std::min(float(rows - 1), std::max(0.f, (y + .5f) * rows / height - .5f));
        const int y0 = int(fy), y1 = std::min(rows - 1, y0 + 1);
        const float ty = fy - y0;
        for (int x = 0; x < width; ++x) {
            const float fx = std::min(float(columns - 1), std::max(0.f, (x + .5f) * columns / width - .5f));
            const int x0 = int(fx), x1 = std::min(columns - 1, x0 + 1);
            const float tx = fx - x0;
            const float top = values[size_t(y0) * columns + x0] * (1 - tx) + values[size_t(y0) * columns + x1] * tx;
            const float bottom = values[size_t(y1) * columns + x0] * (1 - tx) + values[size_t(y1) * columns + x1] * tx;
            enlargedValues[size_t(y) * width + x] = ((top * (1 - ty) + bottom * ty) - low) / (high - low);
        }
    }
    DepthMap map;
    map.values = guidedFilter(enlargedValues, guide, width, height, std::max(2, refinedSide / 48), 1e-3f);
    map.width = width; map.height = height;
    runtime.cachedKey = image.cacheKey();
    runtime.cached = map;
    return map;
}

QPointF VolumetricLighting::brightestPoint(const QImage &image)
{
    const int side = 64;
    const int width = std::max(1, image.width() >= image.height() ? side : side * image.width() / std::max(1, image.height()));
    const int height = std::max(1, image.height() >= image.width() ? side : side * image.height() / std::max(1, image.width()));
    const QPointF center(image.width() / 2.0, image.height() / 2.0);
    if (image.isNull()) return center;
    const QImage small = drawn(image, QSize(width, height));
    std::vector<double> gray(size_t(width) * height), bright(gray.size());
    for (int y = 0; y < height; ++y) {
        const uchar *row = small.constScanLine(y);
        for (int x = 0; x < width; ++x) gray[size_t(y) * width + x] = .2126 * row[x * 4] + .7152 * row[x * 4 + 1] + .0722 * row[x * 4 + 2];
    }
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            double sum = 0, count = 0;
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                const int sx = x + dx, sy = y + dy;
                if (sx < 0 || sy < 0 || sx >= width || sy >= height) continue;
                sum += gray[size_t(sy) * width + sx]; count += 1;
            }
            bright[size_t(y) * width + x] = sum / count;
        }
    const double peak = *std::max_element(bright.begin(), bright.end());
    if (!(peak > 0)) return center;
    const double threshold = peak * .75;
    std::vector<int> area(bright.size(), -1);
    double bestScore = -1; size_t bestAt = 0;
    for (size_t start = 0; start < bright.size(); ++start) {
        if (bright[start] < threshold || area[start] >= 0) continue;
        std::vector<size_t> stack{start};
        double score = 0; size_t brightest = start;
        area[start] = int(start);
        while (!stack.empty()) {
            const size_t index = stack.back(); stack.pop_back();
            score += bright[index];
            if (bright[index] > bright[brightest]) brightest = index;
            const int x = int(index % size_t(width)), y = int(index / size_t(width));
            for (const auto &[nx, ny] : {std::pair{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}}) {
                if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                const size_t next = size_t(ny) * width + nx;
                if (area[next] < 0 && bright[next] >= threshold) { area[next] = int(start); stack.push_back(next); }
            }
        }
        if (score > bestScore) { bestScore = score; bestAt = brightest; }
    }
    return QPointF((double(bestAt % size_t(width)) + .5) * image.width() / width, (double(bestAt / size_t(width)) + .5) * image.height() / height);
}

std::pair<float, float> VolumetricLighting::ramp(double distance, const std::vector<float> *distances)
{
    const float share = float(distance / 100);
    if (!distances || distances->empty()) return {share * .6f, share * .6f + .5f};
    const auto at = [&](float s) { return (*distances)[std::min(distances->size() - 1, size_t(std::max(0.f, s) * float(distances->size() - 1)))]; };
    const float start = std::min(at(share), .9f);
    return {start, std::max(at(std::min(1.f, share + .45f)), start + .45f)};
}

std::vector<float> VolumetricLighting::composedDepth(const DepthMap &predicted, const std::vector<float> *subjectValues, QSize size, bool keepsSubjectClear)
{
    const double side = std::max(size.width(), size.height());
    const Plane depth = blurred(enlarged(predicted.values, predicted.width, predicted.height, size), side * .0015);
    if (!subjectValues) return depth.v;
    Plane subject(size.width(), size.height(), 1); subject.v = *subjectValues;
    const double reach = side * .02;
    const auto scaled = [](Plane p, float gain, float bias) { for (float &v : p.v) v = std::clamp(v * gain + bias, 0.f, 1.f); return p; };
    const auto product = [](const Plane &a, const Plane &b) { Plane p = a; for (size_t i = 0; i < p.v.size(); ++i) p.v[i] *= b.v[i]; return p; };
    // The backdrop read from beyond the band the model's nearness bleeds into, spread back in under the subject.
    const Plane beyond = scaled(blurred(subject, reach), -6, 1);
    const Plane weighted = blurred(product(depth, beyond), reach * 2.5);
    Plane behind = scaled(blurred(beyond, reach * 2.5), 1, .001f);
    for (size_t i = 0; i < behind.v.size(); ++i) behind.v[i] = behind.v[i] > 0 ? weighted.v[i] / behind.v[i] : 0.f;
    const Plane closeness = scaled(blurred(subject, reach * 1.5), 4, 0);
    Plane around = depth;
    for (size_t i = 0; i < around.v.size(); ++i) around.v[i] = behind.v[i] * closeness.v[i] + depth.v[i] * (1 - closeness.v[i]);
    Plane inside = depth;
    if (keepsSubjectClear) {
        for (float &v : inside.v) v = std::max(v, .9f);
    } else {
        const double span = side * .08;
        const Plane covered = scaled(blurred(subject, span), 1, .001f), sum = blurred(product(depth, subject), span);
        for (size_t i = 0; i < inside.v.size(); ++i) inside.v[i] = std::clamp(covered.v[i] > 0 ? sum.v[i] / covered.v[i] : 0.f, 0.f, 1.f);
    }
    std::vector<float> result(depth.v.size());
    for (size_t i = 0; i < result.size(); ++i) result[i] = inside.v[i] * subject.v[i] + around.v[i] * (1 - subject.v[i]);
    return result;
}

QImage VolumetricLighting::depthMap(const QImage &image, const VolumetricLightSettings &settings, QString *error)
{
    const auto depth = DepthModel::depth(image, error);
    if (!depth) return {};
    const auto subject = outline(image);
    const std::vector<float> map = composedDepth(depth->equalized(float(settings.normalized().depthContrast / 100)),
                                                 subject ? &subject->v : nullptr, image.size(), settings.keepsSubjectClear);
    QImage gray(image.size(), QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < gray.height(); ++y) {
        uchar *row = gray.scanLine(y);
        for (int x = 0; x < gray.width(); ++x) {
            const uchar v = uchar(std::lround(std::clamp(map[size_t(y) * gray.width() + x], 0.f, 1.f) * 255));
            row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = v; row[x * 4 + 3] = 255;
        }
    }
    return gray;
}

std::vector<float> VolumetricLighting::beams(const QImage &small, const std::vector<float> &subject, float brightest, QPointF light, double side,
                                             const VolumetricLightSettings &settings, RenderBackend backend, bool *accelerated)
{
    if (accelerated) *accelerated = false;
    const int w = small.width(), h = small.height();
    const float size = float(std::max(1.0, side * (.02 + settings.size / 100 * .4)));
    const float length = float(settings.length / 100), shafts = float(settings.shafts / 100);
    const bool gpu = backend == RenderBackend::Vulkan || (backend == RenderBackend::Auto && GpuRenderer::shouldAttempt(small.size(), 96));
    if (gpu) {
        GpuGraph graph;
        const int source = graph.source(small);
        const int blocked = graph.channel(std::make_shared<const std::vector<float>>(subject), small.size());
        using B = GpuParam::VolumetricBeams;
        GpuParameters p{};
        p[B::lightX] = float(light.x()); p[B::lightY] = float(light.y()); p[B::side] = float(side); p[B::size] = size;
        p[B::length] = length; p[B::shafts] = shafts; p[B::brightest] = brightest;
        const std::vector<float> result = graph.executeChannel(graph.pass(GpuOp::VolumetricBeams, source, blocked, p, 0, small.size()), backend);
        if (result.size() == size_t(w) * h) { if (accelerated) *accelerated = true; return result; }
    }
    const Plane image = planeOf(small.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied));
    Plane blockedPlane(w, h, 1); blockedPlane.v = subject;
    std::vector<float> result(size_t(w) * h);
    const float lx = float(light.x()), ly = float(light.y()), sideF = float(side);
    const auto emission = [&](float x, float y) {
        const float bright = std::clamp((.2126f * sampled(image, x, y, 0) + .7152f * sampled(image, x, y, 1) + .0722f * sampled(image, x, y, 2)) / brightest, 0.f, 1.f);
        const float blocked = sampled(blockedPlane, x, y, 0);
        const float ax = x - lx, ay = y - ly, distance = std::hypot(ax, ay);
        const float open = std::clamp((bright - .08f) / .3f, 0.f, 1.f);
        const float disc = std::exp(-2 * (distance / size) * (distance / size));
        const float openings = std::clamp((bright - .45f) / .45f, 0.f, 1.f) * std::exp(-distance / (sideF * .5f));
        const float angle = std::atan2(ay, ax);
        const float wave = .6f * around(angle, 97, 31u) + .4f * around(angle, 251, 77u);
        const float broken = 1 - shafts + shafts * wave * wave * 1.8f;
        return (disc * (.25f + .75f * open) + openings) * (1 - blocked) * broken;
    };
    #pragma omp parallel for schedule(dynamic, 8)
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float hx = x + .5f, hy = y + .5f;
            constexpr int samples = 96;
            const float sx = (lx - hx) * length / samples, sy = (ly - hy) * length / samples;
            const float jitter = hash((uint32_t(x) * 73856093u) ^ (uint32_t(y) * 19349663u));
            float sum = 0, weight = 1;
            for (int i = 0; i < samples; ++i) { sum += emission(hx + sx * (i + jitter), hy + sy * (i + jitter)) * weight; weight *= .985f; }
            result[size_t(y) * w + x] = sum / samples * 4;
        }
    return result;
}

QImage VolumetricLighting::apply(const QImage &layer, const VolumetricLightSettings &settings, QPointF light, RenderBackend backend, bool *accelerated)
{
    if (accelerated) *accelerated = false;
    const VolumetricLightSettings s = settings.normalized();
    if (layer.isNull() || s.isIdentity()) return layer;
    const int w = layer.width(), h = layer.height();
    const double side = std::max(w, h);
    Prepared &cache = prepared();
    QMutexLocker locker(&cache.lock);
    if (cache.key != layer.cacheKey() || cache.size != layer.size()) {
        cache.key = layer.cacheKey(); cache.size = layer.size();
        cache.hasScene = cache.hasDepth = cache.hasBeams = false;
    }
    if (!cache.hasScene) {
        cache.linear = planeOf(linearPremultiplied(layer));
        const QImage mask = RimLighting::subject(layer);
        cache.subject = mask.isNull() ? Plane(w, h, 1, 0) : grayOf(mask, layer.size());
        // The photo's own lights, each glowing into the haze in its own color: brighter than half white and than half
        // again the light around it.
        const Plane around = blurred(cache.linear, side * .04);
        const Plane lights = mapped(cache.linear, 4, [&](const float *p, float *o, int i) {
            const float *c = around.v.data() + size_t(i) * 4;
            for (int k = 0; k < 3; ++k) o[k] = std::min(100.f, std::max(0.f, p[k] - std::clamp(c[k] * 1.5f, .5f, 100.f)));
            o[3] = 1;
        });
        const Plane halo = blurred(lights, side * .012), bloom = blurred(lights, side * .05);
        cache.glows = mapped(halo, 4, [&](const float *p, float *o, int i) { for (int k = 0; k < 3; ++k) o[k] = p[k] + 2 * bloom.v[size_t(i) * 4 + k]; o[3] = 1; });
        cache.ambient = blurred(cache.linear, side * .12);
        cache.near = blurred(cache.subject, std::max(1.0, side * .003));
        // The beams are soft, so they're gathered at half size (each pixel the mean of the four it covers).
        const int hw = std::max(1, (w + 1) / 2), hh = std::max(1, (h + 1) / 2);
        cache.half = Plane(hw, hh, 4); cache.halfSubject = Plane(hw, hh, 1);
        for (int y = 0; y < hh; ++y)
            for (int x = 0; x < hw; ++x) {
                for (int k = 0; k < 4; ++k) {
                    float sum = 0;
                    for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) sum += cache.linear.clampAt(x * 2 + dx, y * 2 + dy)[k];
                    cache.half.at(x, y)[k] = sum / 4;
                }
                float sum = 0;
                for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) sum += cache.subject.clampAt(x * 2 + dx, y * 2 + dy)[0];
                *cache.halfSubject.at(x, y) = sum / 4;
            }
        // The photo's brightest, lightly blurred so one hot pixel doesn't set it, to tell its openings by.
        const Plane soft = blurred(cache.linear, 2);
        float peak = 0;
        for (int i = 0; i < w * h; ++i) peak = std::max(peak, soft.v[size_t(i) * 4 + 1]);
        cache.brightest = std::max(.05f, peak);
        cache.hasScene = true;
    }
    if (!cache.hasDepth || cache.depthContrast != s.depthContrast || cache.keepsSubjectClear != s.keepsSubjectClear) {
        const auto predicted = DepthModel::depth(layer);
        if (predicted) {
            const DepthMap spread = predicted->equalized(float(s.depthContrast / 100));
            const auto subject = outline(layer);
            Plane depth(w, h, 1);
            depth.v = composedDepth(spread, subject ? &subject->v : nullptr, layer.size(), s.keepsSubjectClear);
            cache.depth = std::move(depth);
            cache.distances.resize(spread.values.size());
            std::transform(spread.values.begin(), spread.values.end(), cache.distances.begin(), [](float v) { return 1 - v; });
            std::sort(cache.distances.begin(), cache.distances.end());
        } else {
            cache.depth.reset(); cache.distances.clear();
        }
        cache.hasDepth = true; cache.depthContrast = s.depthContrast; cache.keepsSubjectClear = s.keepsSubjectClear;
    }
    if (!cache.hasBeams || cache.beamsLight != light || cache.beamsSize != s.size || cache.beamsLength != s.length || cache.beamsShafts != s.shafts) {
        QImage small(cache.half.w, cache.half.h, QImage::Format_RGBA32FPx4_Premultiplied);
        for (int y = 0; y < small.height(); ++y) std::copy_n(cache.half.at(0, y), size_t(small.width()) * 4, reinterpret_cast<float *>(small.scanLine(y)));
        Plane gathered(cache.half.w, cache.half.h, 1);
        gathered.v = beams(small, cache.halfSubject.v, cache.brightest, light / 2, side / 2, s, backend, accelerated);
        cache.beams = blurred(gathered, 1);
        cache.hasBeams = true; cache.beamsLight = light; cache.beamsSize = s.size; cache.beamsLength = s.length; cache.beamsShafts = s.shafts;
    }
    // The haze and beams over the photo (mac volumetricCombine).
    const auto [rampStart, rampEndRaw] = ramp(s.distance, cache.depth ? &cache.distances : nullptr);
    const float rampEnd = std::max(rampEndRaw, rampStart + .01f);
    const float tint[3] = {toLinear(s.color.redF()), toLinear(s.color.greenF()), toLinear(s.color.blueF())};
    const float strength = float(s.intensity / 100 * fullIntensity), haze = float(s.haze / 100 * fullHaze);
    const bool hdr = DeepImage::isHDR(layer);
    QImage out(w, h, QImage::Format_RGBA32FPx4_Premultiplied);
    const Plane &image = cache.linear;
    #pragma omp parallel for schedule(static)
    for (int y = 0; y < h; ++y) {
        auto *row = reinterpret_cast<float *>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const float *color = image.at(x, y);
            float *o = row + x * 4;
            if (color[3] <= 0) { std::copy_n(color, 4, o); continue; }
            const float gathered = sampled(cache.beams, (x + .5f) / 2, (y + .5f) / 2, 0);
            const float front = std::clamp(*cache.near.at(x, y), 0.f, 1.f);
            const float *bloom = cache.glows.at(x, y), *air = cache.ambient.at(x, y);
            float far;
            if (cache.depth) far = 1 - std::clamp(*cache.depth->at(x, y), 0.f, 1.f);
            else {
                const float down = (y + .5f) / std::max(1, h);
                far = (1 - smoothstep(.45f, 1.05f, down)) * (1 - .8f * front);
            }
            const float glow = std::exp(-std::hypot(x + .5f - float(light.x()), y + .5f - float(light.y())) / float(side * .35));
            const float rampValue = smoothstep(rampStart, rampEnd, far);
            const float fog = 1 - std::exp(-3 * haze * (.12f + .88f * rampValue));
            for (int c = 0; c < 3; ++c) {
                const float fogColor = std::max(0.f, air[c]) * (1 + (tint[c] - 1) * .6f) * .9f + tint[c] * glow * .45f;
                const float base = std::max(0.f, color[c] / color[3]);
                const float veiled = base + (fogColor - base) * fog + std::max(0.f, bloom[c]) * haze * 1.6f * (.3f + .7f * rampValue);
                const float added = tint[c] * strength * gathered * .5f * (1 - .92f * front);
                float lit = 1 - (1 - std::min(veiled, 1.f)) * std::exp(-added);
                lit = std::max(lit, veiled + (lit - std::min(veiled, 1.f)));
                const float straight = hdr ? std::max(0.f, lit) : std::clamp(lit, 0.f, 1.f);
                o[c] = toGamma(straight) * color[3];
            }
            o[3] = color[3];
        }
    }
    QImage converted = DeepImage::isDeep(layer) ? out.convertToFormat(DeepImage::canonical(layer).format())
                                                : out.convertToFormat(QImage::Format_RGBA64_Premultiplied).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    converted.setColorSpace(layer.colorSpace());
    return converted;
}
} // namespace compositor
