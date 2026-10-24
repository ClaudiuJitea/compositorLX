#include "rendering/LensEffects.h"
#include "rendering/DeepImage.h"
#include <QColorSpace>
#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <vector>

// Filter › Lens Effects, as macOS LensEffects.swift and MetalBokeh.swift. The macOS app builds each effect as a chain
// of Core Image filters in linear light; here each of those filters is the same arithmetic on float planes:
// CIGaussianBlur (an unclamped image reads as clear outside, a clamped one repeats its edges), the morphology filters,
// CIToneCurve's spline, the blend modes, and CILanczosScaleTransform's squeeze. Core Image's y axis points up, so its
// angles are mirrored into this y-down grid: an angle still turns counterclockwise on screen. As Core Image runs those
// filters on the GPU, each effect also runs as Vulkan compute (one graph per color channel, see "Vulkan" below) when
// the renderer allows it, with these float planes as the CPU fallback and reference.
namespace compositor {
namespace {
double clampValue(double value, double lo, double hi, double fallback)
{
    return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
}

// One color channel of light, row-major.
struct Plane {
    int width = 0, height = 0;
    std::vector<float> v;
    Plane() = default;
    Plane(int w, int h, float fill = 0) : width(w), height(h), v(size_t(w) * h, fill) {}
    float *row(int y) { return v.data() + size_t(y) * width; }
    const float *row(int y) const { return v.data() + size_t(y) * width; }
};
using Light = std::array<Plane, 3>;

bool parallel(const Plane &p) { return qint64(p.width) * p.height > 65536; }

Plane transposed(const Plane &p)
{
    Plane t(p.height, p.width);
    #pragma omp parallel for if(parallel(p))
    for (int x = 0; x < p.width; ++x)
        for (int y = 0; y < p.height; ++y) t.v[size_t(x) * p.height + y] = p.v[size_t(y) * p.width + x];
    return t;
}

void addInto(Plane &to, const Plane &from, double gain = 1)
{
    const float g = float(gain);
    #pragma omp parallel for if(parallel(to))
    for (qsizetype i = 0; i < qsizetype(to.v.size()); ++i) to.v[size_t(i)] += from.v[size_t(i)] * g;
}
Plane scaled(Plane p, double gain)
{
    const float g = float(gain);
    #pragma omp parallel for if(parallel(p))
    for (qsizetype i = 0; i < qsizetype(p.v.size()); ++i) p.v[size_t(i)] *= g;
    return p;
}

// ---- Blurs ------------------------------------------------------------------------------------------------------

// The widths of three box blurs that together make a Gaussian of `sigma` (Kovesi).
std::array<int, 3> boxWidths(double sigma)
{
    const double ideal = std::sqrt(12 * sigma * sigma / 3 + 1);
    int lower = int(std::floor(ideal));
    if (lower % 2 == 0) --lower;
    const int upper = lower + 2;
    const int m = int(std::lround((12 * sigma * sigma - 3 * lower * lower - 12 * lower - 9) / (-4.0 * lower - 4)));
    return {0 < m ? lower : upper, 1 < m ? lower : upper, 2 < m ? lower : upper};
}

// A Gaussian along each row. `clamp` repeats the edge pixels outward (clampedToExtent); otherwise the outside is clear.
void blurRows(Plane &p, double sigma, bool clamp)
{
    if (!(sigma > .05) || p.v.empty()) return;
    const int w = p.width;
    if (sigma <= 8) {
        const int r = int(std::ceil(sigma * 3));
        std::vector<float> kernel(size_t(2 * r + 1));
        double sum = 0;
        for (int i = -r; i <= r; ++i) sum += kernel[size_t(i + r)] = float(std::exp(-i * i / (2 * sigma * sigma)));
        for (float &k : kernel) k = float(k / sum);
        #pragma omp parallel for if(parallel(p))
        for (int y = 0; y < p.height; ++y) {
            std::vector<float> line(size_t(w + 2 * r), 0.f);
            float *row = p.row(y);
            for (int x = -r; x < w + r; ++x)
                line[size_t(x + r)] = x >= 0 && x < w ? row[x] : clamp ? row[std::clamp(x, 0, w - 1)] : 0.f;
            for (int x = 0; x < w; ++x) {
                float acc = 0;
                const float *at = line.data() + x;
                for (int i = 0; i <= 2 * r; ++i) acc += at[i] * kernel[size_t(i)];
                row[x] = acc;
            }
        }
        return;
    }
    // Wider blurs: three box passes of Kovesi's widths, the same σ at a fixed cost per pixel.
    const std::array<int, 3> boxes = boxWidths(sigma);
    int pad = 0;
    for (int box : boxes) pad += box / 2;
    #pragma omp parallel for if(parallel(p))
    for (int y = 0; y < p.height; ++y) {
        std::vector<float> line(size_t(w + 2 * pad)), next(line.size());
        float *row = p.row(y);
        for (int x = -pad; x < w + pad; ++x)
            line[size_t(x + pad)] = x >= 0 && x < w ? row[x] : clamp ? row[std::clamp(x, 0, w - 1)] : 0.f;
        const int n = int(line.size());
        for (int box : boxes) {
            const int r = box / 2;
            double acc = 0;
            for (int i = -r; i <= r; ++i) acc += i >= 0 && i < n ? line[size_t(i)] : (clamp ? line[size_t(std::clamp(i, 0, n - 1))] : 0.f);
            for (int x = 0; x < n; ++x) {
                next[size_t(x)] = float(acc / box);
                const int in = x + r + 1, out = x - r;
                acc += in < n ? line[size_t(in)] : (clamp ? line[size_t(n - 1)] : 0.f);
                acc -= out >= 0 ? line[size_t(out)] : (clamp ? line[0] : 0.f);
            }
            std::swap(line, next);
        }
        std::copy_n(line.begin() + pad, w, row);
    }
}
// A wide blur runs on a smaller copy, which it hides: each pixel of the copy is the average of a block of the plane,
// the copy is blurred by what is left of σ, and bilinear scaling brings it back. Its cost then stays the same however
// wide the blur, as MetalBokeh's discs do; the GPU's chain does the same arithmetic.
int reduction(double sigma) { return sigma > 16 ? int(std::ceil(sigma / 7)) : 1; }
double reducedSigma(double sigma, int factor)
{
    // The block average and the bilinear scaling spread the light too, by (f² − 1) / 12 and f² / 6.
    const double f = factor;
    return std::sqrt(std::max(0.0, sigma * sigma - (f * f - 1) / 12 - f * f / 6)) / f;
}
// `clamp`: a block past the edge averages the pixels it has; otherwise the outside counts as clear.
Plane reduced(const Plane &p, int factor, bool clamp)
{
    Plane small((p.width + factor - 1) / factor, (p.height + factor - 1) / factor);
    #pragma omp parallel for if(parallel(small))
    for (int y = 0; y < small.height; ++y)
        for (int x = 0; x < small.width; ++x) {
            const int left = x * factor, top = y * factor;
            const int right = std::min(left + factor, p.width), bottom = std::min(top + factor, p.height);
            float sum = 0;
            for (int row = top; row < bottom; ++row)
                for (int column = left; column < right; ++column) sum += p.v[size_t(row) * p.width + column];
            small.v[size_t(y) * small.width + x] = sum / float(clamp ? (right - left) * (bottom - top) : factor * factor);
        }
    return small;
}
float sample(const Plane &p, double x, double y, bool clamp);
Plane enlarged(const Plane &small, int width, int height, int factor, bool clamp)
{
    Plane p(width, height);
    const float scale = 1.f / float(factor);
    #pragma omp parallel for if(parallel(p))
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) p.v[size_t(y) * width + x] = sample(small, (x + .5f) * scale, (y + .5f) * scale, clamp);
    return p;
}
Plane gaussian(Plane p, double sigma, bool clamp)
{
    if (!(sigma > .05)) return p;
    if (const int factor = reduction(sigma); factor > 1)
        return enlarged(gaussian(reduced(p, factor, clamp), reducedSigma(sigma, factor), clamp), p.width, p.height, factor, clamp);
    blurRows(p, sigma, clamp);
    Plane t = transposed(p);
    blurRows(t, sigma, clamp);
    return transposed(t);
}
Light gaussian(const Light &light, double sigma, bool clamp)
{
    return {gaussian(light[0], sigma, clamp), gaussian(light[1], sigma, clamp), gaussian(light[2], sigma, clamp)};
}

// CIMorphologyRectangleMinimum / Maximum along one row, a centered window `size` wide; the outside is clear. Van
// Herk / Gil-Werman running extremes: three comparisons a pixel whatever the window. A row of nothing stays nothing.
void morphologyLine(float *row, int w, int size, bool maximum, std::vector<float> &scratch)
{
    const int r = size / 2, k = 2 * r + 1;
    if (r <= 0 || std::all_of(row, row + w, [](float value) { return value == 0; })) return;
    // Padded with clear on both sides, then split into blocks of the window's width.
    const int padded = (w + 2 * r + k - 1) / k * k;
    scratch.assign(size_t(padded) * 3, 0.f);
    float *line = scratch.data(), *forward = line + padded, *backward = forward + padded;
    std::copy_n(row, w, line + r);
    const auto pick = [maximum](float a, float b) { return maximum ? std::max(a, b) : std::min(a, b); };
    for (int b = 0; b < padded; b += k) {
        forward[b] = line[b];
        for (int i = b + 1; i < b + k; ++i) forward[i] = pick(forward[i - 1], line[i]);
        backward[b + k - 1] = line[b + k - 1];
        for (int i = b + k - 2; i >= b; --i) backward[i] = pick(backward[i + 1], line[i]);
    }
    // The window centered on x covers padded positions x … x + 2r.
    for (int x = 0; x < w; ++x) row[x] = pick(backward[x], forward[x + 2 * r]);
}
void morphologyRows(Plane &p, int size, bool maximum)
{
    if (size / 2 <= 0) return;
    #pragma omp parallel if(parallel(p))
    {
        std::vector<float> scratch;
        #pragma omp for
        for (int y = 0; y < p.height; ++y) morphologyLine(p.row(y), p.width, size, maximum, scratch);
    }
}
Plane morphologyColumns(const Plane &p, int size, bool maximum)
{
    Plane t = transposed(p);
    morphologyRows(t, size, maximum);
    return transposed(t);
}

// CIMorphologyMaximum: the brightest value within a disc of `radius` pixels; the outside is clear.
Plane discMaximum(const Plane &p, double radius)
{
    const int reach = int(std::floor(radius));
    if (reach < 1) return p;
    const int w = p.width, h = p.height;
    std::vector<uint8_t> occupied(static_cast<size_t>(h));
    for (int y = 0; y < h; ++y) occupied[size_t(y)] = std::any_of(p.row(y), p.row(y) + w, [](float value) { return value > 0; });
    Plane result(w, h);
    #pragma omp parallel for schedule(dynamic, 8) if(parallel(p))
    for (int y = 0; y < h; ++y) {
        float *out = result.row(y);
        Plane line(w, 1);
        for (int dy = -reach; dy <= reach; ++dy) {
            const int source = y + dy;
            if (source < 0 || source >= h || !occupied[size_t(source)]) continue;
            const int half = int(std::floor(std::sqrt(std::max(0.0, radius * radius - double(dy) * dy))));
            std::copy_n(p.row(source), w, line.v.begin());
            morphologyRows(line, 2 * half + 1, true);
            for (int x = 0; x < w; ++x) out[x] = std::max(out[x], line.v[size_t(x)]);
        }
    }
    return result;
}

// ---- Tones ------------------------------------------------------------------------------------------------------

// CIToneCurve: a natural cubic spline through five points, read for inputs between 0 and 1, kept between 0 and 1, on
// square roots of the values and squared back, as Core Image documents its gamma-2 application.
class ToneCurve {
  public:
    ToneCurve(std::initializer_list<std::pair<double, double>> points)
    {
        for (const auto &[x, y] : points)
            if (x_.empty() || x > x_.back()) { x_.push_back(x); y_.push_back(y); }
        const size_t n = x_.size();
        m_.assign(n, 0);
        if (n < 3) return;
        std::vector<double> c(n, 0), d(n, 0);
        for (size_t i = 1; i + 1 < n; ++i) {
            const double h0 = x_[i] - x_[i - 1], h1 = x_[i + 1] - x_[i];
            const double a = h0, b = 2 * (h0 + h1), rhs = 6 * ((y_[i + 1] - y_[i]) / h1 - (y_[i] - y_[i - 1]) / h0);
            const double denominator = b - a * c[i - 1];
            c[i] = h1 / denominator;
            d[i] = (rhs - a * d[i - 1]) / denominator;
        }
        for (size_t i = n - 2; i >= 1; --i) m_[i] = d[i] - c[i] * m_[i + 1];
    }
    float operator()(double x) const
    {
        // Core Image applies the curve "in a perceptual (gamma 2) version of the working space".
        const float y = curve(std::sqrt(std::clamp(std::isfinite(x) ? x : 0.0, 0.0, 1.0)));
        return y * y;
    }
    float curve(double x) const
    {
        x = std::clamp(std::isfinite(x) ? x : 0.0, 0.0, 1.0);
        if (x <= x_.front()) return float(std::clamp(y_.front(), 0.0, 1.0));
        if (x >= x_.back()) return float(std::clamp(y_.back(), 0.0, 1.0));
        size_t i = 0;
        while (i + 2 < x_.size() && x > x_[i + 1]) ++i;
        const double h = x_[i + 1] - x_[i], a = (x_[i + 1] - x) / h, b = (x - x_[i]) / h;
        const double y = a * y_[i] + b * y_[i + 1] + ((a * a * a - a) * m_[i] + (b * b * b - b) * m_[i + 1]) * h * h / 6;
        return float(std::clamp(y, 0.0, 1.0));
    }
    // The spline in GpuOp::LensPoint's slots: its points and their second derivatives.
    void encode(GpuParameters &p) const
    {
        p[GpuParam::LensPoint::count] = float(x_.size());
        for (size_t i = 0; i < x_.size() && i < 5; ++i) {
            p[GpuParam::LensPoint::x0 + i] = float(x_[i]);
            p[GpuParam::LensPoint::y0 + i] = float(y_[i]);
            p[GpuParam::LensPoint::m0 + i] = float(m_[i]);
        }
    }
    void apply(Plane &p, double scale = 1) const
    {
        #pragma omp parallel for if(parallel(p))
        for (qsizetype i = 0; i < qsizetype(p.v.size()); ++i) p.v[size_t(i)] = (*this)(p.v[size_t(i)] * scale);
    }

  private:
    std::vector<double> x_, y_, m_;
};

float toLinear(float c)
{
    const float a = std::abs(c), l = a <= .04045f ? a / 12.92f : std::pow((a + .055f) / 1.055f, 2.4f);
    return std::copysign(l, c);
}
float toGamma(float l)
{
    const float a = std::abs(l), c = a <= .0031308f ? a * 12.92f : 1.055f * std::pow(a, 1 / 2.4f) - .055f;
    return std::copysign(c, l);
}
Light linear(Light light)
{
    for (Plane &p : light) {
        #pragma omp parallel for if(parallel(p))
        for (qsizetype i = 0; i < qsizetype(p.v.size()); ++i) p.v[size_t(i)] = toLinear(p.v[size_t(i)]);
    }
    return light;
}
Light gamma(Light light)
{
    for (Plane &p : light) {
        #pragma omp parallel for if(parallel(p))
        for (qsizetype i = 0; i < qsizetype(p.v.size()); ++i) p.v[size_t(i)] = toGamma(p.v[size_t(i)]);
    }
    return light;
}
Light added(Light light, const Light &more, double gain = 1)
{
    for (int c = 0; c < 3; ++c) addInto(light[size_t(c)], more[size_t(c)], gain);
    return light;
}

// How far each pixel is above `threshold` in brightness, 0–1, as a gray mask.
Plane brightness(const Light &image, double threshold)
{
    const double t = std::min(.95, std::max(0.0, threshold)), k = 1 - t;
    Plane gray(image[0].width, image[0].height);
    #pragma omp parallel for if(parallel(gray))
    for (qsizetype i = 0; i < qsizetype(gray.v.size()); ++i)
        gray.v[size_t(i)] = .2126f * image[0].v[size_t(i)] + .7152f * image[1].v[size_t(i)] + .0722f * image[2].v[size_t(i)];
    ToneCurve({{0, 0}, {t, 0}, {t + k * .4, .25}, {t + k * .75, .7}, {1, 1}}).apply(gray);
    return gray;
}
Light masked(Light light, const Plane &mask)
{
    for (Plane &p : light) {
        #pragma omp parallel for if(parallel(p))
        for (qsizetype i = 0; i < qsizetype(p.v.size()); ++i) p.v[size_t(i)] *= mask.v[size_t(i)];
    }
    return light;
}
// The light brighter than `threshold` (0–1, on the picture's own tones), in linear light, fading in above it.
Light highlights(const Light &image, double threshold)
{
    return masked(linear(image), brightness(image, threshold));
}

// The lights a flare or starburst is made from, by Threshold (0.5–1). Up to 0.95 it's how bright a light is. Past that,
// a photo's brightest lights are all clipped to white, so it's how big and solid a bright spot is: a pixel stays only
// where its surroundings are lit too, over a wider area the higher Threshold goes, until only the strongest one or two
// sources are left. This is the mask that picks them out of the picture's linear light.
Plane gpuGaussian(const Plane &p, double sigma, bool clamp);
Plane sourceMask(const Light &image, double threshold, bool gpu)
{
    if (!(threshold > .95)) return brightness(image, threshold);
    const int side = std::max(image[0].width, image[0].height);
    const double density = std::min(1.0, (threshold - .95) / .05);
    Plane lit = brightness(image, .95);
    Plane solid = gpu ? gpuGaussian(lit, side * .03 * density, true) : gaussian(lit, side * .03 * density, true);
    // Measured against the photo's own strongest source, so the top of the range always leaves it.
    const float peak = solid.v.empty() ? 0 : *std::max_element(solid.v.begin(), solid.v.end());
    if (!(peak > .001f)) return lit;
    const double cut = .25 + .6 * density, soft = .05;
    ToneCurve({{0, 0}, {std::max(0.0, cut - soft), 0}, {cut, .5}, {cut + soft, 1}, {std::max(1.0, cut + soft * 2), 1}})
        .apply(solid, 1 / double(peak));
    // A light that qualifies keeps all of itself, not just its middle.
    Plane mask = discMaximum(solid, side * .03 * density);
    #pragma omp parallel for if(parallel(mask))
    for (qsizetype i = 0; i < qsizetype(mask.v.size()); ++i) mask.v[size_t(i)] *= lit.v[size_t(i)];
    return mask;
}
Light sources(const Light &image, double threshold)
{
    return masked(linear(image), sourceMask(image, threshold, false));
}

// Light compressed smoothly toward `limit`: nearly unchanged when dim, never past it.
const ToneCurve &clipCurve()
{
    static const ToneCurve curve{{0, 0}, {.0625, .224}, {.125, .398}, {.25, .638}, {1, 1}};
    return curve;
}
Plane softClip(Plane light, double limit)
{
    clipCurve().apply(light, 1 / (4 * limit));
    return scaled(std::move(light), limit);
}

// ---- Geometry ---------------------------------------------------------------------------------------------------

// A plane's value at `x`, `y` in pixel coordinates (pixel centers at .5), bilinear; clear outside unless `clamp`.
float sample(const Plane &p, double x, double y, bool clamp = false);
float sample(const Plane &p, double x, double y, bool clamp)
{
    x -= .5;
    y -= .5;
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const float fx = float(x - x0), fy = float(y - y0);
    const auto at = [&](int px, int py) -> float {
        if (clamp) return p.v[size_t(std::clamp(py, 0, p.height - 1)) * p.width + size_t(std::clamp(px, 0, p.width - 1))];
        return px >= 0 && py >= 0 && px < p.width && py < p.height ? p.v[size_t(py) * p.width + px] : 0.f;
    };
    return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}

// `image` blurred along its rows with a standard deviation of `along` pixels and `across` pixels across them, added to
// `into` times `gain`: squeezed along the line, blurred evenly and stretched back, so a long streak costs no more than a
// short one. `spread` first draws each light out along the line, which lifts a small light far more than a wide
// bright area.
void addStreakRows(const Plane &image, double along, double across, double spread, Plane &into, double gain)
{
    across = std::max(.5, across);
    const double squeeze = std::max(1.0, along / across);
    const int window = spread >= 3 ? std::min(301, int(spread) | 1) : 1;
    if (squeeze <= 1.0001) {
        Plane spreadOut = image;
        morphologyRows(spreadOut, window, true);
        addInto(into, gaussian(std::move(spreadOut), across, false), gain);
        return;
    }
    // Squeezed: each small pixel averages the `squeeze` pixels it covers (CILanczosScaleTransform's low-pass).
    const int w = image.width, h = image.height, pad = 2;
    const int narrow = std::max(1, int(std::ceil(w / squeeze)));
    Plane small(narrow + 2 * pad, h);
    #pragma omp parallel if(parallel(image))
    {
        std::vector<float> line(static_cast<size_t>(w)), scratch;
        #pragma omp for
        for (int y = 0; y < h; ++y) {
            const float *source = image.row(y);
            if (std::all_of(source, source + w, [](float value) { return value == 0; })) continue;
            std::copy_n(source, w, line.begin());
            morphologyLine(line.data(), w, window, true, scratch);
            float *out = small.row(y) + pad;
            for (int k = 0; k < narrow; ++k) {
                const double from = k * squeeze, to = std::min(double(w), (k + 1) * squeeze);
                double sum = 0;
                for (int x = int(std::floor(from)); x < int(std::ceil(to)) && x < w; ++x)
                    sum += line[size_t(x)] * (std::min(to, x + 1.0) - std::max(from, double(x)));
                out[k] = float(sum / squeeze);
            }
        }
    }
    small = gaussian(std::move(small), across, false);
    const float g = float(gain);
    #pragma omp parallel for if(parallel(image))
    for (int y = 0; y < h; ++y) {
        const float *in = small.row(y);
        if (std::all_of(in, in + small.width, [](float value) { return value == 0; })) continue;
        float *out = into.row(y);
        for (int x = 0; x < w; ++x) {
            const double at = (x + .5) / squeeze - .5 + pad;
            const int k = int(std::floor(at));
            const float f = float(at - k);
            const auto value = [&](int i) { return i >= 0 && i < small.width ? in[i] : 0.f; };
            out[x] += g * (value(k) * (1 - f) + value(k + 1) * f);
        }
    }
}
Plane streakRows(const Plane &image, double along, double across, double spread = 0)
{
    Plane result(image.width, image.height);
    addStreakRows(image, along, across, spread, result, 1);
    return result;
}

// A frame turned so that the line at `angle` (Core Image's radians, counterclockwise with y up) runs along its rows.
struct Turn {
    double ux = 1, uy = 0, vx = 0, vy = 1, cx = 0, cy = 0, umin = 0, vmin = 0;
    int width = 0, height = 0;
    Turn(double angle, int w, int h) : ux(std::cos(angle)), uy(-std::sin(angle)), vx(std::sin(angle)), vy(std::cos(angle)), cx(w / 2.0), cy(h / 2.0)
    {
        double umax = -1e300, vmax = -1e300;
        umin = vmin = 1e300;
        for (const auto &[x, y] : {std::pair{0.0, 0.0}, {double(w), 0.0}, {0.0, double(h)}, {double(w), double(h)}}) {
            const double u = (x - cx) * ux + (y - cy) * uy, v = (x - cx) * vx + (y - cy) * vy;
            umin = std::min(umin, u); umax = std::max(umax, u);
            vmin = std::min(vmin, v); vmax = std::max(vmax, v);
        }
        width = int(std::ceil(umax - umin - 1e-6));
        height = int(std::ceil(vmax - vmin - 1e-6));
    }
    Plane into(const Plane &p) const
    {
        // Lights are sparse: a pixel between two source rows holding nothing gets nothing.
        std::vector<uint8_t> lit(static_cast<size_t>(p.height) + 2, 0);
        for (int y = 0; y < p.height; ++y) lit[size_t(y) + 1] = std::any_of(p.row(y), p.row(y) + p.width, [](float value) { return value != 0; });
        Plane t(width, height);
        #pragma omp parallel for if(parallel(t))
        for (int j = 0; j < height; ++j)
            for (int i = 0; i < width; ++i) {
                const double u = umin + i + .5, v = vmin + j + .5, y = cy + u * uy + v * vy;
                const int row = int(std::floor(y - .5));
                if (row < -1 || row >= p.height || (!lit[size_t(row + 1)] && !lit[size_t(row + 2)])) continue;
                t.v[size_t(j) * width + i] = sample(p, cx + u * ux + v * vx, y);
            }
        return t;
    }
    void addBack(const Plane &t, Plane &to, double gain = 1) const
    {
        // Rays are thin: most turned rows hold nothing, and a pixel between two such rows gets nothing.
        std::vector<uint8_t> lit(static_cast<size_t>(t.height) + 2, 0);
        for (int j = 0; j < t.height; ++j) lit[size_t(j) + 1] = std::any_of(t.row(j), t.row(j) + t.width, [](float value) { return value != 0; });
        #pragma omp parallel for if(parallel(to))
        for (int y = 0; y < to.height; ++y)
            for (int x = 0; x < to.width; ++x) {
                const double px = x + .5 - cx, py = y + .5 - cy, v = px * vx + py * vy - vmin;
                const int j = int(std::floor(v - .5));
                if (j < -1 || j >= t.height || (!lit[size_t(j + 1)] && !lit[size_t(j + 2)])) continue;
                to.v[size_t(y) * to.width + x] += float(gain) * sample(t, px * ux + py * uy - umin, v);
            }
    }
};

// ---- Effects ----------------------------------------------------------------------------------------------------

// Horizontal streaks as an anamorphic lens's cylindrical glass draws them, made from the picture's own light. Lights
// brighter than Threshold flare, the more the brighter they are, and concentrated ones count for far more than the
// same light scattered in glints. Each throws a thin line across the frame inside a softer band, a white core and a
// veil. Near the light a streak keeps the light's own color; along it the coatings turn it blue, reaching farthest in
// blue and least in red, as much as Blue asks. Each color is eased off before it's tinted, so a strong streak stays
// saturated rather than whitening.
Light anamorphic(const Light &image, const LensEffectSettings &s)
{
    const int w = image[0].width, h = image[0].height, side = std::max(w, h);
    const double reach = side * s.flareLength / 100, thin = std::max(.5, side * .0008);
    const double amount = s.flareAmount / 50, blue = s.flareBlue / 100;
    const Light spread = gaussian(sources(image, s.threshold / 100), side * .004, true);
    Light lights;
    for (int c = 0; c < 3; ++c) {
        lights[size_t(c)] = spread[size_t(c)];
        for (float &value : lights[size_t(c)].v) value = 1.6f * value * (value + .12f);
    }
    // The thin line comes from compact lights only: drawn from a big one it would be as tall as the light.
    Light lineSources;
    const int size = std::max(3, int(side * .012) | 1);
    for (int c = 0; c < 3; ++c) {
        const Plane opened = morphologyColumns(morphologyColumns(lights[size_t(c)], size, false), size, true);
        lineSources[size_t(c)] = lights[size_t(c)];
        for (size_t i = 0; i < opened.v.size(); ++i) lineSources[size_t(c)].v[i] = std::max(0.f, lights[size_t(c)].v[i] - opened.v[i]);
    }
    const std::array<double, 3> lengths{1 - .7 * blue, 1 - .4 * blue, 1}, coat{1 - blue * .9, 1 - blue * .62, 1};
    Light flare{Plane(w, h), Plane(w, h), Plane(w, h)};
    for (int c = 0; c < 3; ++c) {
        const Plane line = streakRows(lineSources[size_t(c)], reach * lengths[size_t(c)], thin);
        addInto(flare[size_t(c)], softClip(scaled(line, 4 * amount), .6), coat[size_t(c)]);
        // The band: brightest near the light, fading toward the frame's edges.
        Plane band(w, h);
        for (const auto &[fraction, weight] : {std::pair{.08, .8}, {.3, .35}, {.9, .15}}) {
            const double along = reach * fraction * lengths[size_t(c)];
            addStreakRows(lights[size_t(c)], along, side * .006, 0, band, weight * std::sqrt(along / (side * .08)) * amount);
        }
        addInto(flare[size_t(c)], softClip(std::move(band), .7), coat[size_t(c)]);
    }
    for (int c = 0; c < 3; ++c) {
        addStreakRows(lights[size_t(c)], side * .015, thin * 3, 0, flare[size_t(c)], 1.2 * amount);
        addInto(flare[size_t(c)], gaussian(lights[size_t(c)], side * .04, true), .5 * amount * coat[size_t(c)]);
    }
    return gamma(added(linear(image), flare));
}

// Thin spikes from each light, as an aperture's blades diffract it: fading quickly from the light, and a little longer
// in red than in blue, as diffraction spreads longer waves farther.
Light starburst(const Light &image, const LensEffectSettings &s)
{
    const int w = image[0].width, h = image[0].height, side = std::max(w, h);
    const Light light = sources(image, s.threshold / 100);
    const double reach = side * s.starLength / 100, thin = std::max(.5, side * .0006);
    const int spokes = std::max(1, s.starPoints / 2);
    Light result = linear(image);
    for (int spoke = 0; spoke < spokes; ++spoke) {
        const double theta = s.starAngle * M_PI / 180 + spoke * M_PI / spokes;
        const Turn turn(theta, w, h);
        for (int c = 0; c < 3; ++c) {
            const Plane turned = turn.into(light[size_t(c)]);
            Plane rays(turned.width, turned.height);
            for (const auto &[fraction, weight] : {std::pair{.04, 1.8}, {.12, .9}, {.35, .35}, {1.0, .1}}) {
                const double along = std::max(thin * 2, reach * fraction * (1 - .1 * c));
                addStreakRows(turned, along, thin, along * .8, rays, weight * s.starAmount / 50);
            }
            turn.addBack(rays, result[size_t(c)]);
        }
    }
    return gamma(std::move(result));
}

// A diffusion filter, as a black mist one: a share of every pixel's light scatters into a near halo and a wide veil
// rather than being added, so bright areas soften and spill and the shadows lift a little, without the whole picture
// brightening. The brightest lights scatter a little extra.
Light mist(const Light &image, const LensEffectSettings &s)
{
    const int side = std::max(image[0].width, image[0].height);
    const Light light = linear(image);
    const double scale = side * (.2 + s.mistSize / 100 * 1.8) / 100;
    const double strength = s.mistStrength / 100, scatter = strength * .4;
    Light result = light;
    for (Plane &p : result) p = scaled(std::move(p), 1 - scatter);
    result = added(std::move(result), gaussian(light, scale * .6, true), scatter * .6);
    result = added(std::move(result), gaussian(light, scale * 4, true), scatter * .4);
    result = added(std::move(result), gaussian(highlights(image, .75), scale * 1.5, true), strength * .35);
    return gamma(std::move(result));
}

// A prism held before the lens: copies of the picture stepped along a line, one way or both, each fainter than the
// last, with a thin color fringe from the glass. A copy shows where it's brighter than what's behind it, so lit shapes
// echo into the shadows and nothing darkens. Blur softens the copies, more the farther they're pushed.
Light prism(const Light &image, const LensEffectSettings &s)
{
    const int w = image[0].width, h = image[0].height, side = std::max(w, h);
    const Light light = linear(image);
    const double theta = s.prismAngle * M_PI / 180, dx = std::cos(theta), dy = -std::sin(theta);
    const double shift = side * s.prismOffset / 100;
    const std::vector<double> ways = s.prismBothWays ? std::vector<double>{1, -1} : std::vector<double>{1};
    const std::array<double, 3> fringes{.003, 0, -.003};
    Light result = light;
    // Farthest first, so each nearer copy shows over it.
    for (int copy = int(s.prismCopies); copy >= 1; --copy) {
        const double sigma = s.prismBlur > 0 ? side * s.prismBlur / 10000 * copy : 0;
        // The copies together reach as far as their furthest shift; blurred, they're read past that as their edge.
        double left = 1e300, top = 1e300, right = -1e300, bottom = -1e300;
        for (double way : ways)
            for (double fringe : fringes) {
                const double along = way * (shift * copy + side * fringe);
                left = std::min(left, dx * along); right = std::max(right, w + dx * along);
                top = std::min(top, dy * along); bottom = std::max(bottom, h + dy * along);
            }
        const int margin = sigma > .05 ? int(std::ceil(sigma * 3)) + 1 : 0;
        const double fade = std::pow(.78, copy - 1) * s.prismAmount / 100;
        for (int c = 0; c < 3; ++c) {
            Plane ghost(w + 2 * margin, h + 2 * margin);
            #pragma omp parallel for if(parallel(ghost))
            for (int y = 0; y < ghost.height; ++y)
                for (int x = 0; x < ghost.width; ++x) {
                    // Unblurred, a copy is clear past its edge; blurred, its edge carries on (clampedToExtent).
                    double px = x - margin + .5, py = y - margin + .5;
                    if (margin) { px = std::clamp(px, left + .5, right - .5); py = std::clamp(py, top + .5, bottom - .5); }
                    float value = 0;
                    for (double way : ways) {
                        const double along = way * (shift * copy + side * fringes[size_t(c)]);
                        value = std::max(value, sample(light[size_t(c)], px - dx * along, py - dy * along));
                    }
                    ghost.v[size_t(y) * ghost.width + x] = value;
                }
            if (margin) ghost = gaussian(std::move(ghost), sigma, true);
            Plane &out = result[size_t(c)];
            for (int y = 0; y < h; ++y) {
                const float *in = ghost.row(y + margin) + margin;
                float *row = out.row(y);
                for (int x = 0; x < w; ++x) row[x] = std::max(row[x], float(fade) * in[x]);
            }
        }
    }
    return gamma(std::move(result));
}

QImage straightFloat(int width, int height) { return QImage(width, height, QImage::Format_RGBA32FPx4_Premultiplied); }

// Whether this thread's last Bokeh ran its discs on the GPU.
thread_local bool bokehAccelerated = false;

// MetalBokeh.blur: `light` blurred into discs of `radius` pixels. A big blur runs on a smaller copy, which it hides, so
// its cost stays the same however wide the discs.
Light disc(const Light &light, double radius, BokehShape shape, double ring)
{
    const int w = light[0].width, h = light[0].height;
    const double shrink = std::min(1.0, 20 / std::max(radius, .5)), small = radius * shrink;
    // Room for the discs reaching in from past the edge, which carries its edge pixels on.
    const double margin = std::ceil(small * 1.7) + 2;
    const int left = int(std::floor(-margin)), top = int(std::floor(-margin));
    const int width = int(std::ceil(w * shrink + margin)) - left, height = int(std::ceil(h * shrink + margin)) - top;
    QImage source = straightFloat(width, height);
    if (source.isNull()) return light;
    #pragma omp parallel for if(qint64(width) * height > 65536)
    for (int y = 0; y < height; ++y) {
        auto *out = reinterpret_cast<float *>(source.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const double fx = (left + x + .5) / shrink, fy = (top + y + .5) / shrink;
            for (int c = 0; c < 3; ++c) out[x * 4 + c] = sample(light[size_t(c)], fx, fy, true);
            out[x * 4 + 3] = 1;
        }
    }
    bool accelerated = false;
    const QImage blurred = LensOptics::bokehKernel(source, small, shape, ring, RenderBackend::Auto, &accelerated);
    bokehAccelerated = bokehAccelerated || accelerated;
    Light result{Plane(w, h), Plane(w, h), Plane(w, h)};
    if (blurred.isNull()) return light;
    std::array<Plane, 3> planes{Plane(width, height), Plane(width, height), Plane(width, height)};
    #pragma omp parallel for if(qint64(width) * height > 65536)
    for (int y = 0; y < height; ++y) {
        const auto *in = reinterpret_cast<const float *>(blurred.constScanLine(y));
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < 3; ++c) planes[size_t(c)].v[size_t(y) * width + x] = in[x * 4 + c];
    }
    for (int c = 0; c < 3; ++c) {
        #pragma omp parallel for if(parallel(result[0]))
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                result[size_t(c)].v[size_t(y) * w + x] =
                    sample(planes[size_t(c)], (x + .5) * shrink - left, (y + .5) * shrink - top, true);
    }
    return result;
}

// The blur of a lens focused elsewhere: every point spreads into a disc of the aperture's shape, not the soft smear of
// a Gaussian blur. In a real photo a light is far brighter than white, so its disc stays bright: the lights are
// boosted, blurred on their own and eased back, so a wide bright area doesn't blow out.
Light bokeh(const Light &image, const LensEffectSettings &s)
{
    const int side = std::max(image[0].width, image[0].height);
    const double radius = side * s.bokehRadius / 100;
    // Even at its least, lights open into clear discs: Brightness says how much they glow, not whether.
    const Light light = linear(image);
    Light lights = masked(light, brightness(image, .8));
    for (Plane &p : lights) p = scaled(std::move(p), 12 + s.bokehBrightness * .7);
    lights = disc(lights, radius, s.bokehShape, s.bokehRing / 100);
    for (Plane &p : lights) p = softClip(std::move(p), .8);
    return gamma(added(disc(light, radius, s.bokehShape, s.bokehRing / 100), lights));
}

// ---- Vulkan -----------------------------------------------------------------------------------------------------

// The effects again as Vulkan compute, as Core Image runs them on the Mac's GPU. They go one color channel at a time on
// single-channel float planes, so a 24-megapixel photo fits the renderer's default working memory. Only the masks, which
// need all three channels at once, are made on the CPU above; linear light and every blur, streak, turn and copy run on
// the GPU, and each channel's chain is one graph: its planes go up once and its result comes back once. Any failure
// (no device, over the memory budget, the GPU busy) leaves the whole effect to the CPU, which draws the same.
using Shared = std::shared_ptr<const std::vector<float>>;
Shared shared(const Plane &p) { return std::make_shared<const std::vector<float>>(p.v); }
// The picture's three channels (straight color), handed to the uploads rather than copied.
struct Pictures {
    int width = 0, height = 0;
    std::array<Shared, 3> planes;
};

class Chain {
  public:
    GpuGraph graph;

    // A plane uploaded for this chain alone: its memory goes to later passes once they have read it.
    int plane(const Shared &values, int width, int height)
    {
        const int node = graph.channel(values, QSize(width, height));
        graph.transient(node);
        return node;
    }
    QSize size(int node) const { return graph.nodes()[size_t(node)].size; }
    int passes() const
    {
        return int(std::count_if(graph.nodes().begin(), graph.nodes().end(), [](const GpuNode &n) { return n.operation != GpuOp::Source; }));
    }
    // 0 to linear light, 1 to gamma, 3 the flare's 1.6·v·(v + .12), 4 linear light times the `aux` mask.
    int point(int in, int mode, int aux = -1)
    {
        GpuParameters p{};
        p[GpuParam::LensPoint::mode] = float(mode);
        return graph.pass(GpuOp::LensPoint, in, aux, p);
    }
    // A tone curve read at value × inScale, times outScale (softClip's).
    int curve(int in, const ToneCurve &curve, double inScale, double outScale)
    {
        GpuParameters p{};
        p[GpuParam::LensPoint::mode] = 2;
        p[GpuParam::LensPoint::inScale] = float(inScale);
        p[GpuParam::LensPoint::outScale] = float(outScale);
        curve.encode(p);
        return graph.pass(GpuOp::LensPoint, in, -1, p);
    }
    // Mode 0 a·ka + b·kb, 1 the brighter of the two, 2 a·ka − b·kb down to nothing; `b` may be wider, read from `offset`.
    int combine(int a, int b, int mode, double ka, double kb, QPoint offset = {}) { return combined(a, b, mode, ka, kb, offset, false); }
    // The same into `sum`, in place when it can be.
    int add(int sum, int b, int mode, double ka, double kb, QPoint offset = {}) { return combined(sum, b, mode, ka, kb, offset, true); }
    int combined(int a, int b, int mode, double ka, double kb, QPoint offset, bool inPlace)
    {
        GpuParameters p{};
        p[GpuParam::LensCombine::mode] = float(mode);
        p[GpuParam::LensCombine::a] = float(ka);
        p[GpuParam::LensCombine::b] = float(kb);
        p[GpuParam::LensCombine::offsetX] = float(offset.x());
        p[GpuParam::LensCombine::offsetY] = float(offset.y());
        p[GpuParam::LensCombine::auxWidth] = float(size(b).width());
        return inPlace ? into(GpuOp::LensCombine, a, b, p) : graph.pass(GpuOp::LensCombine, a, b, p);
    }
    // A pass that adds to `sum`: in place when `sum` is itself a pass (which nothing else reads), saving a plane.
    int into(GpuOp op, int sum, int aux, const GpuParameters &p)
    {
        if (graph.nodes()[size_t(sum)].operation == GpuOp::Source) return graph.pass(op, sum, aux, p);
        return graph.update(op, sum, aux, p, QRect(QPoint(), size(sum)));
    }
    // The plane sampled into `out` pixels at x' = m11·x + m12·y + dx, y' = m21·x + m22·y + dy (pixel centers).
    int resample(int in, QSize out, double m11, double m12, double dx, double m21, double m22, double dy, bool clamp = false,
                 int channel = 0)
    {
        GpuParameters p{};
        p[GpuParam::LensResample::clamp] = clamp ? 1 : 0;
        p[GpuParam::LensResample::channel] = float(channel);
        p[GpuParam::LensResample::m11] = float(m11);
        p[GpuParam::LensResample::m12] = float(m12);
        p[GpuParam::LensResample::dx] = float(dx);
        p[GpuParam::LensResample::m21] = float(m21);
        p[GpuParam::LensResample::m22] = float(m22);
        p[GpuParam::LensResample::dy] = float(dy);
        p[GpuParam::LensResample::sourceWidth] = float(size(in).width());
        p[GpuParam::LensResample::sourceHeight] = float(size(in).height());
        return graph.pass(GpuOp::LensResample, in, -1, p, 0, out);
    }
    int shifted(int in, QSize out, int dx, int dy) { return resample(in, out, 1, 0, dx, 0, 1, dy); }
    // gaussian(): the same direct kernel up to σ 8, the same three boxes past it, and the same smaller copy past 16.
    int blur(int in, double sigma, bool clamp)
    {
        if (!(sigma > .05)) return in;
        if (const int factor = reduction(sigma); factor > 1) {
            const QSize original = size(in);
            GpuParameters p{};
            p[GpuParam::LensReduce::factor] = float(factor);
            p[GpuParam::LensReduce::sourceWidth] = float(original.width());
            p[GpuParam::LensReduce::sourceHeight] = float(original.height());
            p[GpuParam::LensReduce::clear] = clamp ? 0 : 1;
            const QSize small((original.width() + factor - 1) / factor, (original.height() + factor - 1) / factor);
            const int copy = blur(graph.pass(GpuOp::LensReduce, in, -1, p, 0, small), reducedSigma(sigma, factor), clamp);
            return resample(copy, original, 1. / factor, 0, 0, 0, 1. / factor, 0, clamp);
        }
        if (sigma <= 8) {
            const int r = int(std::ceil(sigma * 3));
            std::vector<float> kernel(size_t(2 * r + 1));
            double sum = 0;
            for (int i = -r; i <= r; ++i) sum += kernel[size_t(i + r)] = float(std::exp(-i * i / (2 * sigma * sigma)));
            QImage table(2 * r + 1, 1, QImage::Format_RGBA32FPx4_Premultiplied);
            auto *weights = reinterpret_cast<float *>(table.bits());
            for (int i = 0; i <= 2 * r; ++i) std::fill_n(weights + i * 4, 4, float(kernel[size_t(i)] / sum));
            const int lut = graph.source(table);
            for (int axis = 0; axis < 2; ++axis) {
                GpuParameters p{};
                p[GpuParam::Gaussian::reach] = float(r);
                p[GpuParam::Gaussian::axis] = float(axis);
                p[GpuParam::Gaussian::clear] = clamp ? 0 : 1;
                in = graph.pass(GpuOp::Gaussian, in, lut, p);
            }
            return in;
        }
        const auto boxes = boxWidths(sigma);
        int pad = 0;
        for (int box : boxes) pad += box / 2;
        // As blurRows: the boxes run on the plane set in its edge pixels carried on (or in nothing, outside clear) as
        // far as they reach, and spread into that margin as the CPU's do, not clamped afresh at each pass.
        const QSize original = size(in);
        in = resample(in, original + QSize(2 * pad, 2 * pad), 1, 0, -pad, 0, 1, -pad, clamp);
        for (int axis = 0; axis < 2; ++axis)
            for (int box : boxes) in = graph.boxBlur(in, box / 2, axis);
        return shifted(in, original, pad, pad);
    }
    // addStreakRows up to its stretch: the rows drawn out by `spread`, squeezed by along / across and blurred.
    struct Squeezed { int node; double squeeze; int pad; };
    Squeezed squeezed(int in, double along, double across, double spread)
    {
        across = std::max(.5, across);
        const double squeeze = std::max(1.0, along / across);
        const int window = spread >= 3 ? std::min(301, int(spread) | 1) : 1, radius = window / 2;
        if (squeeze <= 1.0001) return {blur(graph.morphologyAxis(in, radius, 0, false), across, false), 1, 0};
        // The squeeze takes the rows' maxima from runs of 1, 8 or 64 pixels, the longest the window holds; the runs
        // are worked out once per plane, so Starburst's four rays share them.
        int run = 1, table = in;
        for (int longer : {8, 64}) {
            if (longer > radius + 1) break;
            auto &cached = tables[{table, longer}];
            if (!cached) {
                GpuParameters step{};
                step[GpuParam::MorphologyStep::step] = float(run);
                step[GpuParam::MorphologyStep::count] = 8;
                cached = graph.pass(GpuOp::MorphologyStep, table, -1, step) + 1;
            }
            table = cached - 1;
            run = longer;
        }
        const QSize full = size(in);
        const int narrow = std::max(1, int(std::ceil(full.width() / squeeze))), pad = 2;
        GpuParameters p{};
        p[GpuParam::LensSqueeze::squeeze] = float(squeeze);
        p[GpuParam::LensSqueeze::pad] = float(pad);
        p[GpuParam::LensSqueeze::sourceWidth] = float(full.width());
        p[GpuParam::LensSqueeze::radius] = float(radius);
        p[GpuParam::LensSqueeze::run] = float(run);
        const int small = graph.pass(GpuOp::LensSqueeze, table, -1, p, 0, QSize(narrow + 2 * pad, full.height()));
        return {blur(small, across, false), squeeze, pad};
    }
    // Run tables by (plane, run), stored one past the node so that 0 means none yet.
    std::map<std::pair<int, int>, int> tables;
    // addStreakRows: the streak, times `gain`, added to `into` (or a plane of its own when there is none).
    int streak(int in, double along, double across, double spread, double gain, int into)
    {
        const QSize full = size(in);
        const Squeezed rays = squeezed(in, along, across, spread);
        GpuParameters p{};
        p[GpuParam::LensStretch::squeeze] = float(rays.squeeze);
        p[GpuParam::LensStretch::pad] = float(rays.pad);
        p[GpuParam::LensStretch::smallWidth] = float(size(rays.node).width());
        p[GpuParam::LensStretch::gain] = float(gain);
        p[GpuParam::LensStretch::accumulate] = into >= 0 ? 1 : 0;
        if (into >= 0) return this->into(GpuOp::LensStretch, into, rays.node, p);
        return graph.pass(GpuOp::LensStretch, rays.node, -1, p, 0, full);
    }
    std::optional<Plane> run(int node) const
    {
        std::vector<float> values = graph.executeChannel(node, RenderBackend::Vulkan);
        if (values.empty()) return std::nullopt;
        Plane p;
        p.width = size(node).width();
        p.height = size(node).height();
        p.v = std::move(values);
        return p;
    }
};

Plane gpuGaussian(const Plane &p, double sigma, bool clamp)
{
    if (!(sigma > .05)) return p;
    Chain chain;
    if (auto out = chain.run(chain.blur(chain.plane(shared(p), p.width, p.height), sigma, clamp))) return std::move(*out);
    return gaussian(p, sigma, clamp);
}

std::optional<Light> anamorphicGpu(const Pictures &image, const Plane &mask, const LensEffectSettings &s)
{
    const int w = image.width, h = image.height, side = std::max(w, h);
    const double reach = side * s.flareLength / 100, thin = std::max(.5, side * .0008);
    const double amount = s.flareAmount / 50, blue = s.flareBlue / 100;
    const int size = std::max(3, int(side * .012) | 1);
    const std::array<double, 3> lengths{1 - .7 * blue, 1 - .4 * blue, 1}, coat{1 - blue * .9, 1 - blue * .62, 1};
    const Shared lit = shared(mask);
    Light result;
    for (int c = 0; c < 3; ++c) {
        Chain k;
        const int picture = k.plane(image.planes[size_t(c)], w, h);
        const int lights = k.point(k.blur(k.point(picture, 4, k.plane(lit, w, h)), side * .004, true), 3);
        // The thin line comes from compact lights only.
        const int opened = k.graph.morphologyAxis(k.graph.morphologyAxis(lights, size / 2, 1, true), size / 2, 1, false);
        const int line = k.streak(k.combine(lights, opened, 2, 1, 1), reach * lengths[size_t(c)], thin, 0, 1, -1);
        int out = k.curve(line, clipCurve(), 4 * amount / (4 * .6), .6 * coat[size_t(c)]);
        int band = -1;
        for (const auto &[fraction, weight] : {std::pair{.08, .8}, {.3, .35}, {.9, .15}}) {
            const double along = reach * fraction * lengths[size_t(c)];
            band = k.streak(lights, along, side * .006, 0, weight * std::sqrt(along / (side * .08)) * amount, band);
        }
        out = k.add(out, k.curve(band, clipCurve(), 1 / (4 * .7), .7), 0, 1, coat[size_t(c)]);
        out = k.streak(lights, side * .015, thin * 3, 0, 1.2 * amount, out);
        out = k.add(out, k.blur(lights, side * .04, true), 0, 1, .5 * amount * coat[size_t(c)]);
        auto plane = k.run(k.point(picture, 5, out));
        if (!plane) return std::nullopt;
        result[size_t(c)] = std::move(*plane);
    }
    return result;
}

// How many rows a blur of `sigma` reaches.
int blurReach(double sigma)
{
    if (!(sigma > .05)) return 0;
    if (const int factor = reduction(sigma); factor > 1) return (blurReach(reducedSigma(sigma, factor)) + 2) * factor;
    if (sigma <= 8) return int(std::ceil(sigma * 3));
    int reach = 0;
    for (int box : boxWidths(sigma)) reach += box / 2;
    return reach;
}

// Runs of lit pixels, row by row: the only places a ray starts from.
struct Run { int y, x0, x1; };
std::vector<Run> litRuns(const Plane &mask)
{
    std::vector<Run> runs;
    for (int y = 0; y < mask.height; ++y) {
        const float *row = mask.row(y);
        for (int x = 0; x < mask.width; ++x) {
            if (!(row[x] > 0)) continue;
            const int start = x;
            while (x + 1 < mask.width && row[x + 1] > 0) ++x;
            runs.push_back({y, start, x});
        }
    }
    return runs;
}
// The turned frame's rows a ray can be on: within reach of a lit pixel's bilinear taps, then `halo` either side for
// the rays' blur. The CPU passes over the rest as empty rows; here they are never computed.
std::vector<uint8_t> rayRows(const Turn &turn, const std::vector<Run> &runs, int halo)
{
    std::vector<int> marks(size_t(turn.height) + 1, 0);
    for (const Run &run : runs) {
        const auto row = [&](int x) { return (x + .5 - turn.cx) * turn.vx + (run.y + .5 - turn.cy) * turn.vy - turn.vmin - .5; };
        const double a = row(run.x0), b = row(run.x1);
        const int first = std::max(0, int(std::floor(std::min(a, b) - 1.5)) - halo);
        const int last = std::min(turn.height - 1, int(std::ceil(std::max(a, b) + 1.5)) + halo);
        if (first > last) continue;
        ++marks[size_t(first)];
        --marks[size_t(last) + 1];
    }
    std::vector<uint8_t> rows(static_cast<size_t>(turn.height));
    for (int j = 0, open = 0; j < turn.height; ++j) rows[size_t(j)] = (open += marks[size_t(j)]) > 0;
    return rows;
}

std::optional<Light> starburstGpu(const Pictures &image, const Plane &mask, const LensEffectSettings &s)
{
    const int w = image.width, h = image.height, side = std::max(w, h);
    const double reach = side * s.starLength / 100, thin = std::max(.5, side * .0006);
    const int spokes = std::max(1, s.starPoints / 2);
    // The turned frame goes in bands of rows, each with the rows its rays' blur reaches, so the working memory stays
    // the same however large the picture.
    const int halo = blurReach(std::max(.5, thin)) + 1;
    const qint64 bandPixels = std::clamp<qint64>(GpuRenderer::memoryBudgetBytes() / (4 * 20), 1 << 20, 1 << 24);
    const std::vector<Run> runs = litRuns(mask);
    const Shared lit = shared(mask);
    const double across = std::max(.5, thin);
    static constexpr std::array<double, 4> fractions{.04, .12, .35, 1.0}, weights{1.8, .9, .35, .1};
    Light result;
    for (int c = 0; c < 3; ++c) {
        const auto along = [&](int f) { return std::max(thin * 2, reach * fractions[size_t(f)] * (1 - .1 * c)); };
        const Shared picture = image.planes[size_t(c)];
        Chain k;
        // The rays are added into the picture's light in place, each band only where it lands.
        const auto lightsOf = [&] { return k.point(k.plane(picture, w, h), 4, k.plane(lit, w, h)); };
        int lights = lightsOf(), out = k.point(k.plane(picture, w, h), 0);
        for (int spoke = 0; spoke < spokes; ++spoke) {
            // A graph has room for some 2000 passes: past that, this channel's sum so far comes back and goes on.
            if (k.passes() > 1200) {
                auto sum = k.run(out);
                if (!sum) return std::nullopt;
                k = Chain();
                lights = lightsOf();
                const int plane = k.plane(shared(*sum), w, h);
                out = k.combine(plane, plane, 0, 1, 0);
            }
            const Turn turn(s.starAngle * M_PI / 180 + spoke * M_PI / spokes, w, h);
            const std::vector<uint8_t> needed = rayRows(turn, runs, halo);
            const int rows = int(std::clamp<qint64>(bandPixels / std::max(1, turn.width), 64, std::max(64, turn.height)));
            std::vector<std::pair<int, int>> bands;
            for (int j = 0; j < turn.height;) {
                if (!needed[size_t(j)]) { ++j; continue; }
                int end = j;
                while (end < turn.height && needed[size_t(end)]) ++end;
                for (int first = j; first < end; first += rows) bands.emplace_back(first, std::min(end, first + rows));
                j = end;
            }
            if (bands.empty()) continue;
            // The four rays' squeezed rows sit side by side in one plane the turned frame's height, which every band
            // fills in, so they are all turned back onto the picture in one pass.
            GpuParameters back{};
            int packedWidth = 0;
            for (int f = 0; f < 4; ++f) {
                const double squeeze = std::max(1.0, along(f) / across);
                back[GpuParam::LensStreakBack::squeeze0 + f] = float(squeeze);
                back[GpuParam::LensStreakBack::gain0 + f] = float(weights[size_t(f)] * s.starAmount / 50);
                back[GpuParam::LensStreakBack::offset0 + f] = float(packedWidth);
                packedWidth += std::max(1, int(std::ceil(turn.width / squeeze))) + 4;
            }
            int packed = k.resample(lights, QSize(packedWidth, turn.height), 0, 0, -1e6, 0, 0, -1e6); // all clear
            for (const auto &[first, last] : bands) {
                const int top = std::max(0, first - halo), bottom = std::min(turn.height, last + halo);
                const int turned = k.resample(lights, QSize(turn.width, bottom - top), turn.ux, turn.vx,
                                              turn.cx + turn.umin * turn.ux + (turn.vmin + top) * turn.vx, turn.uy, turn.vy,
                                              turn.cy + turn.umin * turn.uy + (turn.vmin + top) * turn.vy);
                for (int f = 0; f < 4; ++f) {
                    const auto rays = k.squeezed(turned, along(f), thin, along(f) * .8);
                    // The band's own rows (not the ones its blur needed) into the packed plane.
                    const int offset = int(back[GpuParam::LensStreakBack::offset0 + f]);
                    GpuParameters paste{};
                    paste[GpuParam::LensCombine::b] = 1;
                    paste[GpuParam::LensCombine::offsetX] = float(-offset);
                    paste[GpuParam::LensCombine::offsetY] = float(-top);
                    paste[GpuParam::LensCombine::auxWidth] = float(k.size(rays.node).width());
                    packed = k.graph.update(GpuOp::LensCombine, packed, rays.node, paste,
                                            QRect(offset, first, k.size(rays.node).width(), last - first));
                }
            }
            back[GpuParam::LensStreakBack::ux] = float(turn.ux);
            back[GpuParam::LensStreakBack::uy] = float(turn.uy);
            back[GpuParam::LensStreakBack::vx] = float(turn.vx);
            back[GpuParam::LensStreakBack::vy] = float(turn.vy);
            back[GpuParam::LensStreakBack::cx] = float(turn.cx);
            back[GpuParam::LensStreakBack::cy] = float(turn.cy);
            back[GpuParam::LensStreakBack::umin] = float(turn.umin);
            back[GpuParam::LensStreakBack::vmin] = float(turn.vmin);
            back[GpuParam::LensStreakBack::turnedWidth] = float(turn.width);
            back[GpuParam::LensStreakBack::turnedHeight] = float(turn.height);
            back[GpuParam::LensStreakBack::pad] = 2;
            back[GpuParam::LensStreakBack::packedWidth] = float(packedWidth);
            out = k.graph.update(GpuOp::LensStreakBack, out, packed, back, QRect(0, 0, w, h));
        }
        auto plane = k.run(k.point(out, 1));
        if (!plane) return std::nullopt;
        result[size_t(c)] = std::move(*plane);
    }
    return result;
}

std::optional<Light> mistGpu(const Pictures &image, const Plane &bright, const LensEffectSettings &s)
{
    const int w = image.width, h = image.height, side = std::max(w, h);
    const Shared lit = shared(bright);
    const double scale = side * (.2 + s.mistSize / 100 * 1.8) / 100;
    const double strength = s.mistStrength / 100, scatter = strength * .4;
    Light result;
    for (int c = 0; c < 3; ++c) {
        Chain k;
        const int picture = k.plane(image.planes[size_t(c)], w, h);
        const int l = k.point(picture, 0);
        int out = k.combine(l, k.blur(l, scale * .6, true), 0, 1 - scatter, scatter * .6);
        out = k.add(out, k.blur(l, scale * 4, true), 0, 1, scatter * .4);
        out = k.add(out, k.blur(k.point(picture, 4, k.plane(lit, w, h)), scale * 1.5, true), 0, 1, strength * .35);
        auto plane = k.run(k.point(out, 1));
        if (!plane) return std::nullopt;
        result[size_t(c)] = std::move(*plane);
    }
    return result;
}

std::optional<Light> prismGpu(const Pictures &image, const LensEffectSettings &s)
{
    const int w = image.width, h = image.height, side = std::max(w, h);
    const double theta = s.prismAngle * M_PI / 180, dx = std::cos(theta), dy = -std::sin(theta);
    const double shift = side * s.prismOffset / 100;
    const std::vector<double> ways = s.prismBothWays ? std::vector<double>{1, -1} : std::vector<double>{1};
    const std::array<double, 3> fringes{.003, 0, -.003};
    Light result;
    for (int c = 0; c < 3; ++c) {
        Chain k;
        const int source = k.point(k.plane(image.planes[size_t(c)], w, h), 0);
        int out = source;
        // Farthest first, so each nearer copy shows over it.
        for (int copy = int(s.prismCopies); copy >= 1; --copy) {
            const double sigma = s.prismBlur > 0 ? side * s.prismBlur / 10000 * copy : 0;
            double left = 1e300, top = 1e300, right = -1e300, bottom = -1e300;
            for (double way : ways)
                for (double fringe : fringes) {
                    const double along = way * (shift * copy + side * fringe);
                    left = std::min(left, dx * along); right = std::max(right, w + dx * along);
                    top = std::min(top, dy * along); bottom = std::max(bottom, h + dy * along);
                }
            const int margin = sigma > .05 ? int(std::ceil(sigma * 3)) + 1 : 0;
            const double fade = std::pow(.78, copy - 1) * s.prismAmount / 100;
            GpuParameters p{};
            p[GpuParam::LensPrism::dx] = float(dx);
            p[GpuParam::LensPrism::dy] = float(dy);
            for (size_t way = 0; way < ways.size(); ++way)
                p[GpuParam::LensPrism::along0 + int(way)] = float(ways[way] * (shift * copy + side * fringes[size_t(c)]));
            p[GpuParam::LensPrism::ways] = float(ways.size());
            p[GpuParam::LensPrism::margin] = float(margin);
            p[GpuParam::LensPrism::left] = float(left);
            p[GpuParam::LensPrism::top] = float(top);
            p[GpuParam::LensPrism::right] = float(right);
            p[GpuParam::LensPrism::bottom] = float(bottom);
            p[GpuParam::LensPrism::sourceWidth] = float(w);
            p[GpuParam::LensPrism::sourceHeight] = float(h);
            int ghost = k.graph.pass(GpuOp::LensPrism, source, -1, p, 0, QSize(w + 2 * margin, h + 2 * margin));
            if (margin) ghost = k.blur(ghost, sigma, true);
            // The first copy starts the sum (the light itself stays for the copies still to come).
            out = out == source ? k.combine(out, ghost, 1, 1, fade, QPoint(margin, margin)) : k.add(out, ghost, 1, 1, fade, QPoint(margin, margin));
        }
        auto plane = k.run(k.point(out, 1));
        if (!plane) return std::nullopt;
        result[size_t(c)] = std::move(*plane);
    }
    return result;
}

std::optional<Light> bokehGpu(const Pictures &image, const Plane &bright, const LensEffectSettings &s)
{
    const int w = image.width, h = image.height, side = std::max(w, h);
    const double radius = side * s.bokehRadius / 100;
    // disc()'s smaller copy, with room for the discs reaching in from past the edge (more than the kernel reaches,
    // so two copies side by side never read each other where it matters).
    const double shrink = std::min(1.0, 20 / std::max(radius, .5)), small = radius * shrink;
    const double margin = std::ceil(small * 1.7) + 2;
    const int left = int(std::floor(-margin)), top = int(std::floor(-margin));
    const QSize reduced(int(std::ceil(w * shrink + margin)) - left, int(std::ceil(h * shrink + margin)) - top);
    const Shared lit = shared(bright);
    // Each channel's lights and picture onto the smaller copy, side by side.
    QImage copies(reduced.width() * 2, reduced.height(), QImage::Format_RGBA32FPx4_Premultiplied);
    if (copies.isNull()) return std::nullopt;
    copies.fill(Qt::white); // alpha 1, as disc() gives the kernel
    for (int c = 0; c < 3; ++c) {
        Chain k;
        const int picture = k.plane(image.planes[size_t(c)], w, h);
        const auto onto = [&](int plane) { return k.resample(plane, reduced, 1 / shrink, 0, left / shrink, 0, 1 / shrink, top / shrink, true); };
        const int lights = k.resample(onto(k.point(picture, 4, k.plane(lit, w, h))), QSize(reduced.width() * 2, reduced.height()), 1, 0, 0, 0, 1, 0);
        GpuParameters paste{};
        paste[GpuParam::LensCombine::b] = 1;
        paste[GpuParam::LensCombine::offsetX] = float(-reduced.width());
        paste[GpuParam::LensCombine::auxWidth] = float(reduced.width());
        const int both = k.graph.update(GpuOp::LensCombine, lights, onto(k.point(picture, 0)), paste,
                                        QRect(QPoint(reduced.width(), 0), reduced));
        const auto plane = k.run(both);
        if (!plane) return std::nullopt;
        for (int y = 0; y < copies.height(); ++y) {
            auto *row = reinterpret_cast<float *>(copies.scanLine(y));
            for (int x = 0; x < copies.width(); ++x) row[x * 4 + c] = plane->v[size_t(y) * copies.width() + x];
        }
    }
    // MetalBokeh's discs, all three channels at once.
    bool accelerated = false;
    const QImage blurred = LensOptics::bokehKernel(copies, small, s.bokehShape, s.bokehRing / 100, RenderBackend::Vulkan, &accelerated);
    if (!accelerated) return std::nullopt;
    Light result;
    for (int c = 0; c < 3; ++c) {
        Chain k;
        const int discs = k.graph.source(blurred);
        const auto back = [&](int offset) { return k.resample(discs, QSize(w, h), shrink, 0, offset - left, 0, shrink, -top, true, c); };
        // The lights' discs, boosted (folded into softClip's curve, as the discs are linear) and eased back.
        const int lights = k.curve(back(0), clipCurve(), (12 + s.bokehBrightness * .7) / (4 * .8), .8);
        auto plane = k.run(k.point(k.add(back(reduced.width()), lights, 0, 1, 1), 1));
        if (!plane) return std::nullopt;
        result[size_t(c)] = std::move(*plane);
    }
    return result;
}

// MetalBokeh's `aperture`: how far `o` (in disc radii) is toward the aperture's edge, 1 on it.
float aperture(float ox, float oy, BokehShape shape, float outX, float outY)
{
    switch (shape) {
    case BokehShape::Hexagon: {
        const float qx = std::abs(ox), qy = std::abs(oy);
        return std::max(qx * .866025f + qy * .5f, qy) / .866025f * .93f;
    }
    case BokehShape::Oval: return std::hypot(ox, oy * .62f);
    // The disc clipped by a second one shifted toward the frame's middle, more so farther out.
    case BokehShape::CatsEye: return std::max(std::hypot(ox, oy), std::hypot(ox + outX * 1.25f, oy + outY * 1.25f));
    case BokehShape::Round: break;
    }
    return std::hypot(ox, oy);
}
float smoothstep(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}
} // namespace

QString LensEffectSettings::effectName(LensEffect effect)
{
    switch (effect) {
    case LensEffect::AnamorphicFlare: return QCoreApplication::translate("LensEffects", "Anamorphic Flare");
    case LensEffect::Starburst: return QCoreApplication::translate("LensEffects", "Starburst");
    case LensEffect::Mist: return QCoreApplication::translate("LensEffects", "Mist");
    case LensEffect::Prism: return QCoreApplication::translate("LensEffects", "Prism");
    case LensEffect::Bokeh: break;
    }
    return QCoreApplication::translate("LensEffects", "Bokeh");
}
QString LensEffectSettings::shapeName(BokehShape shape)
{
    switch (shape) {
    case BokehShape::Hexagon: return QCoreApplication::translate("LensEffects", "Hexagon");
    case BokehShape::Oval: return QCoreApplication::translate("LensEffects", "Oval");
    case BokehShape::CatsEye: return QCoreApplication::translate("LensEffects", "Cat's Eye");
    case BokehShape::Round: break;
    }
    return QCoreApplication::translate("LensEffects", "Round");
}

bool LensEffectSettings::isIdentity() const
{
    switch (effect) {
    case LensEffect::AnamorphicFlare: return flareAmount == 0;
    case LensEffect::Starburst: return starAmount == 0;
    case LensEffect::Mist: return mistStrength == 0;
    case LensEffect::Prism: return prismAmount == 0;
    case LensEffect::Bokeh: break;
    }
    return false;
}

LensEffectSettings LensEffectSettings::normalized() const
{
    LensEffectSettings r = *this;
    r.threshold = clampValue(threshold, 50, 100, 90);
    r.flareAmount = clampValue(flareAmount, 0, 100, 50);
    r.flareLength = clampValue(flareLength, 1, 100, 60);
    r.flareBlue = clampValue(flareBlue, 0, 100, 80);
    r.starAmount = clampValue(starAmount, 0, 100, 50);
    r.starLength = clampValue(starLength, 2, 40, 12);
    r.starPoints = std::find(starPointChoices.begin(), starPointChoices.end(), starPoints) != starPointChoices.end() ? starPoints : 6;
    r.starAngle = clampValue(starAngle, -180, 180, 15);
    r.mistStrength = clampValue(mistStrength, 0, 100, 50);
    r.mistSize = clampValue(mistSize, 0, 100, 30);
    r.prismAmount = clampValue(prismAmount, 0, 100, 80);
    r.prismCopies = std::round(clampValue(prismCopies, 1, 8, 5));
    r.prismOffset = clampValue(prismOffset, 0, 50, 6);
    r.prismAngle = clampValue(prismAngle, -180, 180, 180);
    r.prismBlur = clampValue(prismBlur, 0, 100, 0);
    r.bokehRadius = clampValue(bokehRadius, bokehSizeMin, bokehSizeMax, 2);
    r.bokehBrightness = clampValue(bokehBrightness, 0, 100, 50);
    r.bokehRing = clampValue(bokehRing, 0, 100, 100);
    return r;
}

QImage LensEffectSettings::apply(const QImage &image, RenderBackend backend, bool *accelerated) const
{
    if (accelerated) *accelerated = false;
    if (image.isNull()) return image;
    const LensEffectSettings s = normalized();
    const bool hdr = DeepImage::isHDR(image), deep = DeepImage::isDeep(image);
    const QImage source = DeepImage::floating(image);
    if (source.isNull()) return {};
    const int w = source.width(), h = source.height();
    // The light arithmetic runs on opaque colors, and the layer's own transparency goes back on at the end.
    Light light{Plane(w, h), Plane(w, h), Plane(w, h)};
    #pragma omp parallel for if(qint64(w) * h > 65536)
    for (int y = 0; y < h; ++y) {
        const auto *in = reinterpret_cast<const float *>(source.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const float a = in[x * 4 + 3];
            for (int c = 0; c < 3; ++c) light[size_t(c)].v[size_t(y) * w + x] = a > 0 ? in[x * 4 + c] / a : 0.f;
        }
    }
    // On the GPU where the renderer allows it (Automatic: pictures big enough to repay the upload), else the CPU.
    const bool gpu = backend == RenderBackend::Vulkan || (backend == RenderBackend::Auto && GpuRenderer::shouldAttempt(QSize(w, h), 32));
    std::optional<Light> fast;
    if (gpu) {
        // The masks need all three channels, so they come first; then the channels go to the uploads as they are.
        Plane mask;
        if (s.effect == LensEffect::AnamorphicFlare || s.effect == LensEffect::Starburst) mask = sourceMask(light, s.threshold / 100, true);
        else if (s.effect != LensEffect::Prism) mask = brightness(light, s.effect == LensEffect::Mist ? .75 : .8);
        Pictures pictures{w, h, {}};
        for (int c = 0; c < 3; ++c) pictures.planes[size_t(c)] = std::make_shared<const std::vector<float>>(std::move(light[size_t(c)].v));
        switch (s.effect) {
        case LensEffect::AnamorphicFlare: fast = anamorphicGpu(pictures, mask, s); break;
        case LensEffect::Starburst: fast = starburstGpu(pictures, mask, s); break;
        case LensEffect::Mist: fast = mistGpu(pictures, mask, s); break;
        case LensEffect::Prism: fast = prismGpu(pictures, s); break;
        case LensEffect::Bokeh: fast = bokehGpu(pictures, mask, s); break;
        }
        if (!fast) // The CPU goes on from the same planes.
            for (int c = 0; c < 3; ++c) light[size_t(c)].v = *pictures.planes[size_t(c)];
    }
    Light result;
    if (fast) {
        result = std::move(*fast);
        if (accelerated) *accelerated = true;
    } else {
        std::optional<GpuRenderer::CpuScope> cpu;
        if (backend == RenderBackend::Cpu) cpu.emplace();
        bokehAccelerated = false;
        switch (s.effect) {
        case LensEffect::AnamorphicFlare: result = anamorphic(light, s); break;
        case LensEffect::Starburst: result = starburst(light, s); break;
        case LensEffect::Mist: result = mist(light, s); break;
        case LensEffect::Prism: result = prism(light, s); break;
        case LensEffect::Bokeh: result = bokeh(light, s); break;
        }
        if (accelerated) *accelerated = bokehAccelerated;
    }
    const auto color = [&](int c, int x, int y) {
        const float value = result[size_t(c)].v[size_t(y) * w + x];
        return std::isfinite(value) ? (hdr ? std::max(0.f, value) : std::clamp(value, 0.f, 1.f)) : 0.f;
    };
    if (!deep) {
        // 8-bit layers keep their own alpha byte, and colors round to the nearest level.
        const QImage original = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        QImage out(w, h, QImage::Format_RGBA8888_Premultiplied);
        if (out.isNull()) return {};
        #pragma omp parallel for if(qint64(w) * h > 65536)
        for (int y = 0; y < h; ++y) {
            const uchar *in = original.constScanLine(y);
            uchar *row = out.scanLine(y);
            for (int x = 0; x < w; ++x) {
                const int a = in[x * 4 + 3];
                for (int c = 0; c < 3; ++c) row[x * 4 + c] = uchar(std::lround(color(c, x, y) * a));
                row[x * 4 + 3] = uchar(a);
            }
        }
        out.setColorSpace(image.colorSpace());
        return out;
    }
    QImage out(w, h, QImage::Format_RGBA32FPx4_Premultiplied);
    if (out.isNull()) return {};
    #pragma omp parallel for if(qint64(w) * h > 65536)
    for (int y = 0; y < h; ++y) {
        const auto *in = reinterpret_cast<const float *>(source.constScanLine(y));
        auto *row = reinterpret_cast<float *>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const float a = in[x * 4 + 3];
            for (int c = 0; c < 3; ++c) row[x * 4 + c] = color(c, x, y) * a;
            row[x * 4 + 3] = a;
        }
    }
    QImage canonical = out.convertToFormat(DeepImage::canonical(image).format());
    canonical.setColorSpace(image.colorSpace());
    for (const auto &key : image.textKeys()) canonical.setText(key, image.text(key));
    return canonical;
}

QImage LensOptics::bokehKernel(const QImage &image, double radius, BokehShape shape, double ring, RenderBackend backend,
                               bool *accelerated)
{
    if (accelerated) *accelerated = false;
    if (image.isNull()) return {};
    const QImage source = image.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
    const float r = float(std::max(radius, .5));
    const int reachX = int(std::ceil(r)), reachY = int(std::ceil(r * (shape == BokehShape::Oval ? 1.62f : 1.f)));
    const qint64 work = qint64(source.width()) * source.height() * (2 * reachX + 1) * (2 * reachY + 1);
    if (backend == RenderBackend::Auto && GpuRenderer::backend() != RenderBackend::Cpu
        && (GpuRenderer::backend() == RenderBackend::Vulkan || work >= (qint64(1) << 24)))
        backend = RenderBackend::Vulkan;
    if (backend == RenderBackend::Vulkan) {
        GpuGraph graph;
        GpuParameters p{};
        p[GpuParam::Bokeh::radius] = r;
        p[GpuParam::Bokeh::shape] = float(int(shape));
        p[GpuParam::Bokeh::ring] = float(ring);
        const QImage result = graph.execute(graph.pass(GpuOp::Bokeh, graph.source(source), -1, p),
                                            QImage::Format_RGBA32FPx4_Premultiplied, RenderBackend::Vulkan);
        if (!result.isNull()) {
            if (accelerated) *accelerated = true;
            return result;
        }
    }
    // The same gather on the CPU. Except for the cat's eye, whose shape changes across the frame, every pixel's taps
    // and weights are the same, so they are worked out once.
    const int w = source.width(), h = source.height();
    QImage result(w, h, QImage::Format_RGBA32FPx4_Premultiplied);
    if (result.isNull()) return {};
    struct Tap { int dx, dy; float weight; };
    const float soft = 1 / r;
    const auto taps = [&](float outX, float outY) {
        std::vector<Tap> list;
        for (int dy = -reachY; dy <= reachY; ++dy)
            for (int dx = -reachX; dx <= reachX; ++dx) {
                const float d = aperture(dx / r, dy / r, shape, outX, outY);
                const float inside = 1 - smoothstep(1 - soft, 1, d);
                if (inside <= 0) continue;
                // A lens that draws its discs with a bright rim, as many fast ones do.
                list.push_back({dx, dy, inside * (1 + float(ring) * 2.5f * smoothstep(.6f, .97f, d))});
            }
        return list;
    };
    const std::vector<Tap> fixed = shape == BokehShape::CatsEye ? std::vector<Tap>{} : taps(0, 0);
    #pragma omp parallel for schedule(dynamic, 4) if(qint64(w) * h > 4096)
    for (int y = 0; y < h; ++y) {
        auto *out = reinterpret_cast<float *>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            std::vector<Tap> local;
            if (shape == BokehShape::CatsEye) {
                const float mx = (x + .5f - w * .5f) / (std::max(w, h) * .5f), my = (y + .5f - h * .5f) / (std::max(w, h) * .5f);
                const float out = std::min(1.f, std::hypot(mx, my));
                local = taps(mx * out, my * out);
            }
            const std::vector<Tap> &list = shape == BokehShape::CatsEye ? local : fixed;
            float sum[4] = {0, 0, 0, 0}, total = 0;
            for (const Tap &t : list) {
                const auto *in = reinterpret_cast<const float *>(source.constScanLine(std::clamp(y + t.dy, 0, h - 1)))
                                 + std::clamp(x + t.dx, 0, w - 1) * 4;
                for (int c = 0; c < 4; ++c) sum[c] += in[c] * t.weight;
                total += t.weight;
            }
            for (int c = 0; c < 4; ++c) out[x * 4 + c] = sum[c] / std::max(total, 1e-5f);
        }
    }
    return result;
}
} // namespace compositor
