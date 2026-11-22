#include "rendering/RimLight.h"
#include "rendering/DeepImage.h"
#include "rendering/SubjectRemoval.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QMutex>
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

namespace compositor {
namespace {
double clamped(double value, double lo, double hi, double fallback) { return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback; }
float toLinear(float c) { return c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f); }
float toGamma(float c) { return c <= .0031308f ? c * 12.92f : 1.055f * std::pow(c, 1 / 2.4f) - .055f; }
constexpr double fullStrength = 10, fullGlow = 4;

// RimMath's modes, as rendering.comp numbers them.
enum Mode { Affine = 0, Multiply = 1, Divide = 2, Difference = 3, Add = 4, Masked = 5, Pack2 = 6, Pack4 = 7, Strongest = 8, Tint = 9, Combine = 10 };
struct MathParameters { double gain = 1, bias = 0; bool clamp = false; float red = 0, green = 0, blue = 0; };
struct Light { float dx, dy, dz, red, green, blue, width, reach, detailScale, choke; };

// The inverse of the error function (Giles's approximation), as MetalRimLight's kernel has it.
float inverseError(float x)
{
    float w = -std::log((1.f - x) * (1.f + x)), q;
    if (w < 5.f) {
        w -= 2.5f;
        q = 2.81022636e-08f; q = 3.43273939e-07f + q * w; q = -3.5233877e-06f + q * w; q = -4.39150654e-06f + q * w;
        q = 0.00021858087f + q * w; q = -0.00125372503f + q * w; q = -0.00417768164f + q * w; q = 0.246640727f + q * w;
        q = 1.50140941f + q * w;
    } else {
        w = std::sqrt(w) - 3.f;
        q = -0.000200214257f; q = 0.000100950558f + q * w; q = 0.00134934322f + q * w; q = -0.00367342844f + q * w;
        q = 0.00573950773f + q * w; q = -0.0076224613f + q * w; q = 0.00943887047f + q * w; q = 1.00167406f + q * w;
        q = 2.83297682f + q * w;
    }
    return q * x;
}
float smoothstep(float a, float b, float x) { const float t = std::clamp((x - a) / (b - a), 0.f, 1.f); return t * t * (3 - 2 * t); }

// The chain on the CPU: RGBA float planes, each step as rendering.comp's op of the same name computes it, the blurs as
// GpuGraph::gaussian blurs (the exact Gaussian to a standard deviation of 10, three box passes past that).
struct Cpu {
    int w = 0, h = 0;
    using Node = std::shared_ptr<std::vector<float>>;
    static inline const Node none = nullptr;
    Node make() const { return std::make_shared<std::vector<float>>(size_t(w) * h * 4, 0.f); }
    const float *at(const Node &n, int x, int y) const { return n->data() + (size_t(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)) * 4; }
    Node source(const QImage &image)
    {
        const QImage f = image.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
        Node n = make();
        for (int y = 0; y < h; ++y) std::copy_n(reinterpret_cast<const float *>(f.constScanLine(y)), size_t(w) * 4, n->data() + size_t(y) * w * 4);
        return n;
    }
    Node boxAxis(const Node &in, int radius, int axis) const
    {
        if (radius <= 0) return in;
        Node out = make();
        if (axis == 1) {
            // Down the columns a block of them at a time, a running sum per column, sweeping rows in memory order: walking
            // one column at a time strides a whole row each step and was most of Rim Light's time on a large photo.
            constexpr int block = 64;
            const float divisor = float(2 * radius + 1);
            #pragma omp parallel
            {
                std::vector<float> sums(size_t(block) * 4);
                #pragma omp for schedule(static)
                for (int first = 0; first < w; first += block) {
                    const int count = std::min(block, w - first);
                    std::fill(sums.begin(), sums.end(), 0.f);
                    for (int i = -radius; i <= radius; ++i) {
                        const float *row = at(in, first, i);
                        for (int k = 0; k < count * 4; ++k) sums[size_t(k)] += row[k];
                    }
                    for (int y = 0; y < h; ++y) {
                        float *o = out->data() + (size_t(y) * w + first) * 4;
                        for (int k = 0; k < count * 4; ++k) o[k] = sums[size_t(k)] / divisor;
                        const float *add = at(in, first, y + radius + 1), *sub = at(in, first, y - radius);
                        for (int k = 0; k < count * 4; ++k) sums[size_t(k)] += add[k] - sub[k];
                    }
                }
            }
            return out;
        }
        const int length = axis ? h : w, lines = axis ? w : h;
        const float divisor = float(2 * radius + 1);
        #pragma omp parallel for schedule(static)
        for (int line = 0; line < lines; ++line) {
            const auto px = [&](int pos) { return axis ? at(in, line, pos) : at(in, pos, line); };
            float sum[4] = {0, 0, 0, 0};
            for (int i = -radius; i <= radius; ++i) for (int c = 0; c < 4; ++c) sum[c] += px(i)[c];
            for (int pos = 0; pos < length; ++pos) {
                float *o = out->data() + (axis ? (size_t(pos) * w + line) : (size_t(line) * w + pos)) * 4;
                for (int c = 0; c < 4; ++c) o[c] = sum[c] / divisor;
                const float *add = px(pos + radius + 1), *sub = px(pos - radius);
                for (int c = 0; c < 4; ++c) sum[c] += add[c] - sub[c];
            }
        }
        return out;
    }
    Node blur(Node in, double radius)
    {
        if (!std::isfinite(radius) || radius <= 0) return in;
        radius = std::clamp(radius, .1, 250.);
        if (radius <= 10) {
            const int reach = std::max(1, int(std::ceil(radius * 3)));
            std::vector<float> weights(size_t(reach) * 2 + 1);
            double total = 0;
            for (int i = -reach; i <= reach; ++i) total += std::exp(-.5 * i * i / (radius * radius));
            for (int i = -reach; i <= reach; ++i) weights[size_t(i + reach)] = float(std::exp(-.5 * i * i / (radius * radius)) / total);
            for (int axis = 0; axis < 2; ++axis) {
                Node out = make();
                #pragma omp parallel for schedule(static)
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        float sum[4] = {0, 0, 0, 0};
                        for (int i = -reach; i <= reach; ++i) {
                            const float *p = axis ? at(in, x, y + i) : at(in, x + i, y);
                            for (int c = 0; c < 4; ++c) sum[c] += p[c] * weights[size_t(i + reach)];
                        }
                        std::copy_n(sum, 4, out->data() + (size_t(y) * w + x) * 4);
                    }
                in = out;
            }
            return in;
        }
        int lower = int(std::floor(std::sqrt(4 * radius * radius + 1)));
        if (!(lower & 1)) --lower;
        const int count = int(std::lround((12 * radius * radius - 3 * lower * lower - 12 * lower - 9) / (-4. * lower - 4)));
        for (int iteration = 0; iteration < 3; ++iteration)
            for (int axis = 0; axis < 2; ++axis) in = boxAxis(in, std::max(0, ((iteration < count ? lower : lower + 2) - 1) / 2), axis);
        return in;
    }
    Node math(Mode mode, const Node &a, const Node &b, const MathParameters &m = {}) const
    {
        Node out = make();
        #pragma omp parallel for schedule(static)
        for (int i = 0; i < w * h; ++i) {
            const float *x = a->data() + size_t(i) * 4, *y = b ? b->data() + size_t(i) * 4 : nullptr;
            float *o = out->data() + size_t(i) * 4;
            switch (mode) {
            case Affine: for (int c = 0; c < 3; ++c) { o[c] = float(x[c] * m.gain + m.bias); if (m.clamp) o[c] = std::clamp(o[c], 0.f, 1.f); } o[3] = x[3]; break;
            case Multiply: for (int c = 0; c < 3; ++c) o[c] = x[c] * y[c] + x[c] * (1 - y[3]) + y[c] * (1 - x[3]); o[3] = x[3] + y[3] - x[3] * y[3]; break;
            case Divide: for (int c = 0; c < 3; ++c) o[c] = x[c] > 0 ? y[c] / x[c] : 0.f; o[3] = y[3]; break;
            case Difference: for (int c = 0; c < 3; ++c) o[c] = std::abs(x[c] - y[c]); o[3] = x[3] + y[3] - x[3] * y[3]; break;
            case Add: for (int c = 0; c < 4; ++c) o[c] = x[c] + y[c]; break;
            case Masked: { const float k = std::clamp(y[0], 0.f, 1.f); for (int c = 0; c < 4; ++c) o[c] = x[c] * k; break; }
            case Pack2: o[0] = x[0]; o[1] = y[0]; o[2] = 0; o[3] = 1; break;
            case Pack4: o[0] = x[0]; o[1] = x[1]; o[2] = y[0]; o[3] = y[1]; break;
            case Strongest: o[0] = std::max({x[0], x[1], x[2]}); o[1] = std::max({y[0], y[1], y[2]}); o[2] = 0; o[3] = 1; break;
            case Tint: {
                const float color[3] = {m.red, m.green, m.blue};
                for (int c = 0; c < 3; ++c) { const float albedo = x[3] > 0 ? std::max(0.f, x[c] / x[3]) : 0.f; o[c] = color[c] * (albedo * .7f + .45f) * y[0] * x[3]; }
                o[3] = 0; break;
            }
            case Combine:
                if (x[3] <= 0) { std::copy_n(x, 4, o); break; }
                for (int c = 0; c < 3; ++c) {
                    const float base = std::clamp(x[c] / x[3], 0.f, 1.f), eased = 1 - std::exp(-std::max(0.f, y[c]));
                    o[c] = (1 - (1 - base) * (1 - eased)) * x[3];
                }
                o[3] = x[3]; break;
            }
        }
        return out;
    }
    std::pair<float, float> inwards(const Node &n, int x, int y, int c) const
    {
        const float dx = at(n, x + 1, y)[c] - at(n, x - 1, y)[c], dy = at(n, x, y - 1)[c] - at(n, x, y + 1)[c];
        const float length = std::hypot(dx, dy);
        return length > 1e-6f ? std::pair{dx / length, dy / length} : std::pair{0.f, 0.f};
    }
    Node surface(const Node &pack, const Light &l) const
    {
        Node out = make();
        #pragma omp parallel for schedule(static)
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float *a = at(pack, x, y);
                const float covered = a[0], level = std::clamp(a[1], .001f, .999f);
                const float depth = l.reach * std::sqrt(2.f) * inverseError(2 * level - 1);
                const auto [ix, iy] = inwards(pack, x, y, 1);
                const auto detail = [&](int px, int py) { const float *q = at(pack, px, py); return q[2] - q[3]; };
                const float bx = (detail(x + 1, y) - detail(x - 1, y)) / 2, by = (detail(x, y - 1) - detail(x, y + 1)) / 2;
                float sx = -ix * 2.75f - bx * l.detailScale, sy = -iy * 2.75f - by * l.detailScale, sz = 1;
                const float n = std::sqrt(sx * sx + sy * sy + sz * sz); sx /= n; sy /= n; sz /= n;
                const float lit = std::pow(std::max(0.f, sx * l.dx + sy * l.dy + sz * l.dz), 1.6f);
                const float t = std::max(depth, 0.f) / l.width;
                const float held = 1 - smoothstep(std::max(0.f, (l.choke - .3f) / .7f) * .95f, 1, t);
                const float tail = std::exp(-2.2f * t);
                const float reaches = (l.choke < .3f ? tail + (held - tail) * (l.choke / .3f) : held) * (1 - smoothstep(1.5f, 1.9f, t));
                const float catches = std::clamp(1 + (a[2] - a[3]) * 6, .3f, 1.8f);
                const float inside = smoothstep(-1.5f, 1.5f, depth);
                float *o = out->data() + (size_t(y) * w + x) * 4;
                o[0] = lit * reaches * catches * covered * inside; o[1] = inside; o[2] = covered; o[3] = 1;
            }
        return out;
    }
    Node strand(const Node &pack, const Node &lit, const Light &l) const
    {
        Node out = make();
        #pragma omp parallel for schedule(static)
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float *a = at(pack, x, y), *s = at(lit, x, y);
                float value = s[0];
                if (s[2] > .002f || a[0] > .002f) {
                    const float differs = std::isfinite(a[2]) ? a[2] : 0.f, varies = std::isfinite(a[3]) ? std::sqrt(std::max(0.f, a[3])) : 1.f;
                    const float strand = std::clamp((differs - .03f - 2.5f * varies) / .1f, 0.f, 1.f) * smoothstep(0, .1f, a[0]) * (1 - s[1]);
                    if (strand > 0) {
                        const auto [ax, ay] = inwards(pack, x, y, 0);
                        float sx = -ax * 2.75f, sy = -ay * 2.75f, sz = 1;
                        const float n = std::sqrt(sx * sx + sy * sy + sz * sz);
                        value += std::pow(std::max(0.f, (sx * l.dx + sy * l.dy + sz * l.dz) / n), 1.6f) * strand;
                    }
                } else value = 0;
                float *o = out->data() + (size_t(y) * w + x) * 4;
                o[0] = o[1] = o[2] = value; o[3] = 1;
            }
        return out;
    }
};

// The same chain as passes of one GpuGraph: uploaded once, read back once.
struct Gpu {
    GpuGraph graph;
    using Node = int;
    static constexpr Node none = -1;
    QSize size;
    Node source(const QImage &image) { const int n = graph.source(image.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied)); graph.transient(n); return n; }
    Node blur(Node in, double radius) { return graph.gaussian(in, radius, false); }
    Node math(Mode mode, Node a, Node b, const MathParameters &m = {})
    {
        using R = GpuParam::RimMath;
        GpuParameters p{};
        p[R::mode] = float(mode); p[R::gain] = float(m.gain); p[R::bias] = float(m.bias); p[R::clamp] = m.clamp ? 1 : 0;
        p[R::red] = m.red; p[R::green] = m.green; p[R::blue] = m.blue;
        return graph.pass(GpuOp::RimMath, a, b, p);
    }
    Node surface(Node pack, const Light &l)
    {
        using S = GpuParam::RimSurface;
        GpuParameters p{};
        p[S::directionX] = l.dx; p[S::directionY] = l.dy; p[S::directionZ] = l.dz; p[S::width] = l.width; p[S::reach] = l.reach;
        p[S::detailScale] = l.detailScale; p[S::choke] = l.choke;
        return graph.pass(GpuOp::RimSurface, pack, -1, p);
    }
    Node strand(Node pack, Node lit, const Light &l)
    {
        using S = GpuParam::RimStrand;
        GpuParameters p{};
        p[S::directionX] = l.dx; p[S::directionY] = l.dy; p[S::directionZ] = l.dz;
        return graph.pass(GpuOp::RimStrand, pack, lit, p);
    }
};

// macOS RimLighting.apply, step by step, on either backend. `image` is the layer in linear light, premultiplied;
// `coverage` the subject's mask and `gray` the photo's brightness as it's seen (both as they are, not converted).
template <class B>
typename B::Node chain(B &b, typename B::Node image, typename B::Node coverage, typename B::Node gray, double side, const RimLightSettings &s)
{
    using Node = typename B::Node;
    const Node outline = b.math(Affine, coverage, B::none, {1.6, -.3, true});
    const Node fine = b.blur(gray, 1), coarse = b.blur(gray, std::max(2.0, side / 400));
    // The backdrop behind the subject, worked out from around it, and how busy it is there.
    const Node open = b.math(Affine, outline, B::none, {-1, 1});
    const double spread = side * .01;
    const Node openBlurred = b.math(Affine, b.blur(open, spread), B::none, {1, .001});
    const Node weighted = b.blur(b.math(Multiply, image, open), spread);
    const Node behind = b.math(Divide, openBlurred, weighted);
    const Node backdrop = b.math(Difference, image, behind);
    const Node squares = b.math(Multiply, image, image);
    const Node meanSquares = b.math(Divide, openBlurred, b.blur(b.math(Multiply, squares, open), spread));
    const Node busy = b.math(Difference, meanSquares, b.math(Multiply, behind, behind));
    const Node near = b.blur(outline, side * .015);
    // The strands' pack: the wide outline, then how far each pixel stands out from the backdrop and that backdrop's variance.
    const Node strandPack = b.math(Pack4, b.math(Pack2, near, near), b.math(Strongest, backdrop, busy));
    std::optional<Node> rims;
    for (const RimLight &light : s.lights) {
        if (light.strength <= 0) continue;
        const double width = side * RimLighting::widthShare(light.width), reach = std::max(1.0, width / 1.5);
        const double angle = light.angle * M_PI / 180;
        double dx = std::cos(angle), dy = std::sin(angle), dz = -.2;
        const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float strength = float(light.strength / 100 * fullStrength);
        const Light l{float(dx / length), float(dy / length), float(dz / length), toLinear(light.color.redF()) * strength,
                      toLinear(light.color.greenF()) * strength, toLinear(light.color.blueF()) * strength, float(width), float(reach),
                      float(side / 250), float(light.choke / 100)};
        const Node surfacePack = b.math(Pack4, b.math(Pack2, coverage, b.blur(outline, reach)), b.math(Pack2, fine, coarse));
        const Node lit = b.strand(strandPack, b.surface(surfacePack, l), l);
        MathParameters tint; tint.red = l.red; tint.green = l.green; tint.blue = l.blue;
        const Node rim = b.math(Tint, image, lit, tint);
        rims = rims ? b.math(Add, rim, *rims) : rim;
    }
    if (!rims) return image;
    Node light = *rims;
    if (s.glow > 0) {
        // The rims' light in the air behind the subject, a halo and a wider haze, wrapping a little way in across the edge.
        const double amount = s.glow / 100 * fullGlow;
        const Node haze = b.math(Add, b.math(Affine, b.blur(light, side * .012), B::none, {amount * .6, 0}),
                                 b.math(Affine, b.blur(light, side * .035), B::none, {amount, 0}));
        const Node around = b.math(Affine, b.blur(outline, side * .008), B::none, {-2, 2, true});
        light = b.math(Add, light, b.math(Masked, haze, around));
    }
    return b.math(Combine, image, light);
}

// The layer's pixels in linear light, premultiplied float, and its gray as seen (sRGB values, opaque), as Core Image
// takes them in (`CIImage(cgImage:)` converts to its linear working space; the raw inputs are left as they are).
std::pair<QImage, QImage> inputs(const QImage &layer)
{
    const QImage f = DeepImage::floating(layer);
    const int w = f.width(), h = f.height();
    QImage linear(w, h, QImage::Format_RGBA32FPx4_Premultiplied), gray(w, h, QImage::Format_RGBA32FPx4_Premultiplied);
    #pragma omp parallel for schedule(static)
    for (int y = 0; y < h; ++y) {
        const auto *in = reinterpret_cast<const float *>(f.constScanLine(y));
        auto *l = reinterpret_cast<float *>(linear.scanLine(y)), *g = reinterpret_cast<float *>(gray.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const float a = in[x * 4 + 3];
            for (int c = 0; c < 3; ++c) { const float straight = a > 0 ? in[x * 4 + c] / a : 0.f; l[x * 4 + c] = toLinear(std::max(0.f, straight)) * a; }
            l[x * 4 + 3] = a;
            const float lum = .2126f * in[x * 4] + .7152f * in[x * 4 + 1] + .0722f * in[x * 4 + 2];
            g[x * 4] = g[x * 4 + 1] = g[x * 4 + 2] = lum; g[x * 4 + 3] = 1;
        }
    }
    return {linear, gray};
}

QImage coveragePlane(const QImage &mask, QSize size)
{
    const QImage gray = mask.convertToFormat(QImage::Format_Grayscale8).scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage plane(size, QImage::Format_RGBA32FPx4_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        const uchar *in = gray.constScanLine(y);
        auto *o = reinterpret_cast<float *>(plane.scanLine(y));
        for (int x = 0; x < size.width(); ++x) { o[x * 4] = o[x * 4 + 1] = o[x * 4 + 2] = in[x] / 255.f; o[x * 4 + 3] = 1; }
    }
    return plane;
}

// Back from linear light to the layer's own format, its alpha as it was.
QImage output(const QImage &result, const QImage &layer)
{
    QImage out = result.copy();
    const bool hdr = DeepImage::isHDR(layer);
    #pragma omp parallel for schedule(static)
    for (int y = 0; y < out.height(); ++y) {
        auto *p = reinterpret_cast<float *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const float a = p[x * 4 + 3];
            for (int c = 0; c < 3; ++c) {
                const float straight = a > 0 ? p[x * 4 + c] / a : 0.f;
                p[x * 4 + c] = toGamma(hdr ? std::max(0.f, straight) : std::clamp(straight, 0.f, 1.f)) * a;
            }
        }
    }
    QImage converted = DeepImage::isDeep(layer) ? out.convertToFormat(DeepImage::canonical(layer).format())
                                                : out.convertToFormat(QImage::Format_RGBA64_Premultiplied).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    converted.setColorSpace(layer.colorSpace());
    return converted;
}

struct SubjectCache { QMutex lock; qint64 key = 0; QImage mask; };
SubjectCache &subjectCache() { static SubjectCache cache; return cache; }
} // namespace

RimLight RimLightSettings::nextLight() const
{
    RimLight light;
    light.angle = lights.isEmpty() ? 135 : (lights.last().angle > 0 ? lights.last().angle - 180 : lights.last().angle + 180);
    light.color = lights.size() <= 1 ? QColor::fromRgbF(.7f, .84f, 1) : QColor(Qt::white);
    light.strength = 50;
    light.width = lights.isEmpty() ? 30 : lights.last().width;
    light.choke = lights.isEmpty() ? 0 : lights.last().choke;
    return light;
}

bool RimLightSettings::isIdentity() const
{
    return std::all_of(lights.begin(), lights.end(), [](const RimLight &light) { return light.strength == 0; });
}

RimLightSettings RimLightSettings::normalized() const
{
    RimLightSettings result = *this;
    result.glow = clamped(glow, 0, 100, 10);
    result.lights = lights.mid(0, maximumLights);
    for (RimLight &light : result.lights) {
        light.angle = clamped(light.angle, -180, 180, 135);
        light.strength = clamped(light.strength, 0, 100, 50);
        light.width = clamped(light.width, 0, 100, 30);
        light.choke = clamped(light.choke, 0, 100, 0);
        light.color = QColor::fromRgbF(std::clamp(light.color.redF(), 0.f, 1.f), std::clamp(light.color.greenF(), 0.f, 1.f), std::clamp(light.color.blueF(), 0.f, 1.f));
    }
    return result;
}

double RimLighting::widthShare(double width) { return .002 + width / 100 * .03; }

bool RimLighting::hasSubject(const QImage &image)
{
    QMutexLocker locker(&subjectCache().lock);
    return subjectCache().key == image.cacheKey() && !subjectCache().mask.isNull();
}

QImage RimLighting::subject(const QImage &image, QString *error, std::atomic<bool> *cancelled)
{
    {
        QMutexLocker locker(&subjectCache().lock);
        if (subjectCache().key == image.cacheKey() && !subjectCache().mask.isNull()) return subjectCache().mask;
    }
    const QImage raw = SubjectRemoval::rawMask(DeepImage::canonical(image).convertToFormat(QImage::Format_RGBA8888_Premultiplied), error, cancelled);
    if (raw.isNull()) return {};
    const double side = std::max(image.width(), image.height());
    SubjectRemovalSettings refine; refine.advanced = true; refine.refineEdges = std::max(2.0, side * .008);
    const QImage made = SubjectRemoval::refined(raw, image.convertToFormat(QImage::Format_RGBA8888_Premultiplied), refine, 2048);
    QMutexLocker locker(&subjectCache().lock);
    subjectCache().key = image.cacheKey();
    subjectCache().mask = made;
    return made;
}

QImage RimLighting::applyWithSubject(const QImage &image, const QImage &coverageMask, const RimLightSettings &settings,
                                     RenderBackend backend, bool *accelerated)
{
    if (accelerated) *accelerated = false;
    const RimLightSettings s = settings.normalized();
    if (image.isNull() || coverageMask.isNull() || s.isIdentity()) return image;
    const auto [linear, gray] = inputs(image);
    const QImage coverage = coveragePlane(coverageMask, image.size());
    const double side = std::max(image.width(), image.height());
    const bool gpu = backend == RenderBackend::Vulkan || (backend == RenderBackend::Auto && GpuRenderer::shouldAttempt(image.size(), 64));
    if (gpu) {
        Gpu g; g.size = image.size();
        const int out = chain(g, g.source(linear), g.source(coverage), g.source(gray), side, s);
        const QImage result = g.graph.execute(out, QImage::Format_RGBA32FPx4_Premultiplied, backend);
        if (!result.isNull()) {
            if (accelerated) *accelerated = true;
            return output(result, image);
        }
    }
    Cpu c; c.w = image.width(); c.h = image.height();
    const Cpu::Node out = chain(c, c.source(linear), c.source(coverage), c.source(gray), side, s);
    QImage result(image.size(), QImage::Format_RGBA32FPx4_Premultiplied);
    for (int y = 0; y < c.h; ++y) std::copy_n(out->data() + size_t(y) * c.w * 4, size_t(c.w) * 4, reinterpret_cast<float *>(result.scanLine(y)));
    return output(result, image);
}

QImage RimLighting::apply(const QImage &image, const RimLightSettings &settings, RenderBackend backend, bool *accelerated, QString *error)
{
    if (accelerated) *accelerated = false;
    if (image.isNull() || settings.normalized().isIdentity()) return image;
    const QImage mask = subject(image, error);
    if (mask.isNull()) return {};
    return applyWithSubject(image, mask, settings, backend, accelerated);
}
} // namespace compositor
