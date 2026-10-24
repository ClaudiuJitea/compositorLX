#include "rendering/LensEffects.h"
#include "rendering/DeepImage.h"
#include <QColorSpace>
#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <vector>

// Filter › Lens Effects, as macOS LensEffects.swift and MetalBokeh.swift. The macOS app builds each effect as a chain
// of Core Image filters in linear light; here each of those filters is the same arithmetic on float planes:
// CIGaussianBlur (an unclamped image reads as clear outside, a clamped one repeats its edges), the morphology filters,
// CIToneCurve's spline, the blend modes, and CILanczosScaleTransform's squeeze. Core Image's y axis points up, so its
// angles are mirrored into this y-down grid: an angle still turns counterclockwise on screen.
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
    for (float &value : p.v) value *= float(gain);
    return p;
}

// ---- Blurs ------------------------------------------------------------------------------------------------------

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
    const double ideal = std::sqrt(12 * sigma * sigma / 3 + 1);
    int lower = int(std::floor(ideal));
    if (lower % 2 == 0) --lower;
    const int upper = lower + 2;
    const int m = int(std::lround((12 * sigma * sigma - 3 * lower * lower - 12 * lower - 9) / (-4.0 * lower - 4)));
    const std::array<int, 3> boxes{0 < m ? lower : upper, 1 < m ? lower : upper, 2 < m ? lower : upper};
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
Plane gaussian(Plane p, double sigma, bool clamp)
{
    if (!(sigma > .05)) return p;
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
    for (size_t i = 0; i < gray.v.size(); ++i)
        gray.v[i] = .2126f * image[0].v[i] + .7152f * image[1].v[i] + .0722f * image[2].v[i];
    ToneCurve({{0, 0}, {t, 0}, {t + k * .4, .25}, {t + k * .75, .7}, {1, 1}}).apply(gray);
    return gray;
}
Light masked(Light light, const Plane &mask)
{
    for (Plane &p : light)
        for (size_t i = 0; i < p.v.size(); ++i) p.v[i] *= mask.v[i];
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
// sources are left.
Light sources(const Light &image, double threshold)
{
    if (!(threshold > .95)) return highlights(image, threshold);
    const int side = std::max(image[0].width, image[0].height);
    const double density = std::min(1.0, (threshold - .95) / .05);
    const Plane lit = brightness(image, .95);
    Plane solid = gaussian(lit, side * .03 * density, true);
    // Measured against the photo's own strongest source, so the top of the range always leaves it.
    const float peak = solid.v.empty() ? 0 : *std::max_element(solid.v.begin(), solid.v.end());
    if (!(peak > .001f)) return highlights(image, .95);
    const double cut = .25 + .6 * density, soft = .05;
    ToneCurve({{0, 0}, {std::max(0.0, cut - soft), 0}, {cut, .5}, {cut + soft, 1}, {std::max(1.0, cut + soft * 2), 1}})
        .apply(solid, 1 / double(peak));
    // A light that qualifies keeps all of itself, not just its middle.
    Plane mask = discMaximum(solid, side * .03 * density);
    for (size_t i = 0; i < mask.v.size(); ++i) mask.v[i] *= lit.v[i];
    return masked(linear(image), mask);
}

// Light compressed smoothly toward `limit`: nearly unchanged when dim, never past it.
Plane softClip(Plane light, double limit)
{
    static const ToneCurve curve{{0, 0}, {.0625, .224}, {.125, .398}, {.25, .638}, {1, 1}};
    curve.apply(light, 1 / (4 * limit));
    return scaled(std::move(light), limit);
}

// ---- Geometry ---------------------------------------------------------------------------------------------------

// A plane's value at `x`, `y` in pixel coordinates (pixel centers at .5), bilinear; clear outside unless `clamp`.
float sample(const Plane &p, double x, double y, bool clamp = false)
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
    const QImage blurred = LensOptics::bokehKernel(source, small, shape, ring);
    Light result{Plane(w, h), Plane(w, h), Plane(w, h)};
    if (blurred.isNull()) return light;
    std::array<Plane, 3> planes{Plane(width, height), Plane(width, height), Plane(width, height)};
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
    Light lights = highlights(image, .8);
    for (Plane &p : lights) p = scaled(std::move(p), 12 + s.bokehBrightness * .7);
    lights = disc(lights, radius, s.bokehShape, s.bokehRing / 100);
    for (Plane &p : lights) p = softClip(std::move(p), .8);
    return gamma(added(disc(linear(image), radius, s.bokehShape, s.bokehRing / 100), lights));
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

QImage LensEffectSettings::apply(const QImage &image) const
{
    if (image.isNull()) return image;
    const LensEffectSettings s = normalized();
    const bool hdr = DeepImage::isHDR(image), deep = DeepImage::isDeep(image);
    const QImage source = DeepImage::floating(image);
    if (source.isNull()) return {};
    const int w = source.width(), h = source.height();
    // The light arithmetic runs on opaque colors, and the layer's own transparency goes back on at the end.
    Light light{Plane(w, h), Plane(w, h), Plane(w, h)};
    for (int y = 0; y < h; ++y) {
        const auto *in = reinterpret_cast<const float *>(source.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const float a = in[x * 4 + 3];
            for (int c = 0; c < 3; ++c) light[size_t(c)].v[size_t(y) * w + x] = a > 0 ? in[x * 4 + c] / a : 0.f;
        }
    }
    Light result;
    switch (s.effect) {
    case LensEffect::AnamorphicFlare: result = anamorphic(light, s); break;
    case LensEffect::Starburst: result = starburst(light, s); break;
    case LensEffect::Mist: result = mist(light, s); break;
    case LensEffect::Prism: result = prism(light, s); break;
    case LensEffect::Bokeh: result = bokeh(light, s); break;
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
