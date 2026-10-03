#include "core/SelectionOps.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <vector>

namespace compositor::SelectionOps {

QRectF dragBox(const QPointF &anchor, const QPointF &point, bool square, bool fromCenter)
{
    double dx = std::round(point.x()) - anchor.x(), dy = std::round(point.y()) - anchor.y();
    if (square) {
        const double side = std::max(std::abs(dx), std::abs(dy));
        dx = dx < 0 ? -side : side;
        dy = dy < 0 ? -side : side;
    }
    return fromCenter
        ? QRectF(anchor.x() - std::abs(dx), anchor.y() - std::abs(dy), std::abs(dx) * 2, std::abs(dy) * 2)
        : QRectF(std::min(anchor.x(), anchor.x() + dx), std::min(anchor.y(), anchor.y() + dy), std::abs(dx), std::abs(dy));
}

int modeForModifiers(bool shift, bool option, int chosen)
{
    return option ? 2 : shift ? 1 : chosen;
}

bool hasCoverage(const QImage &mask)
{
    if (mask.isNull()) return false;
    const QImage gray = mask.convertToFormat(QImage::Format_Grayscale8);
    for (int y = 0; y < gray.height(); ++y) {
        const uchar *row = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x) if (row[x]) return true;
    }
    return false;
}

QRect coverageBounds(const QImage &mask)
{
    if (mask.isNull()) return {};
    const QImage gray = mask.convertToFormat(QImage::Format_Grayscale8);
    int left = gray.width(), top = gray.height(), right = -1, bottom = -1;
    for (int y = 0; y < gray.height(); ++y) {
        const uchar *row = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x) if (row[x]) {
            left = std::min(left, x); right = std::max(right, x); top = std::min(top, y); bottom = std::max(bottom, y);
        }
    }
    return right < left ? QRect() : QRect(QPoint(left, top), QPoint(right, bottom));
}

QImage shifted(const QImage &mask, const QPoint &offset)
{
    const QImage source = mask.convertToFormat(QImage::Format_Grayscale8);
    QImage result(source.size(), QImage::Format_Grayscale8);
    result.fill(0);
    const int width = source.width(), height = source.height();
    const int x0 = std::max(0, offset.x()), x1 = std::min(width, width + offset.x());
    if (x1 <= x0) return result;
    for (int y = std::max(0, offset.y()); y < std::min(height, height + offset.y()); ++y)
        std::memcpy(result.scanLine(y) + x0, source.constScanLine(y - offset.y()) + (x0 - offset.x()), size_t(x1 - x0));
    return result;
}

namespace {

// out[i] = max over k in [lo, hi] of v(i + k), with v(j) = `outside` beyond [0, n). Elements are `stride` bytes apart.
void slidingMax(const uchar *in, int n, int stride, int lo, int hi, uchar outside, uchar *out, int outStride)
{
    std::deque<int> queue;
    const auto value = [&](int j) { return j < 0 || j >= n ? outside : in[size_t(j) * size_t(stride)]; };
    // Window of output i covers j in [i + lo, i + hi]; feed j up to i + hi.
    int next = lo;
    for (int i = 0; i < n; ++i) {
        for (; next <= i + hi; ++next) {
            const uchar v = value(next);
            while (!queue.empty() && value(queue.back()) <= v) queue.pop_back();
            queue.push_back(next);
        }
        while (!queue.empty() && queue.front() < i + lo) queue.pop_front();
        out[size_t(i) * size_t(outStride)] = value(queue.front());
    }
}

QImage diskMax(const QImage &input, int radius, uchar outside)
{
    const int width = input.width(), height = input.height();
    QImage result(input.size(), QImage::Format_Grayscale8);
    result.fill(0);
    // Half-width of the disk's row at vertical offset dy.
    std::vector<int> half(size_t(radius) + 1);
    for (int dy = 0; dy <= radius; ++dy) half[size_t(dy)] = int(std::floor(std::sqrt(double(radius) * radius - double(dy) * dy) + 1e-9));
    QImage horizontal(input.size(), QImage::Format_Grayscale8);
    std::vector<uchar> reduced(static_cast<size_t>(height));
    for (int w = 0; w <= radius; ++w) {
        // Rows whose offset magnitude has this half-width: a contiguous band [a, b].
        int a = -1, b = -1;
        for (int dy = 0; dy <= radius; ++dy) if (half[size_t(dy)] == w) { if (a < 0) a = dy; b = dy; }
        if (a < 0) continue;
        for (int y = 0; y < height; ++y) {
            if (w == 0) std::memcpy(horizontal.scanLine(y), input.constScanLine(y), size_t(width));
            else slidingMax(input.constScanLine(y), width, 1, -w, w, outside, horizontal.scanLine(y), 1);
        }
        for (int x = 0; x < width; ++x) {
            // Offsets [a, b] and [-b, -a]; they form one band [-b, b] when a is 0.
            slidingMax(horizontal.constBits() + x, height, int(horizontal.bytesPerLine()), a == 0 ? -b : a, b, outside, reduced.data(), 1);
            for (int y = 0; y < height; ++y) { uchar &out = result.scanLine(y)[x]; out = std::max(out, reduced[size_t(y)]); }
            if (a > 0) {
                slidingMax(horizontal.constBits() + x, height, int(horizontal.bytesPerLine()), -b, -a, outside, reduced.data(), 1);
                for (int y = 0; y < height; ++y) { uchar &out = result.scanLine(y)[x]; out = std::max(out, reduced[size_t(y)]); }
            }
        }
    }
    return result;
}

// Felzenszwalb & Huttenlocher squared distance transform of a 1-D function.
void distance1D(const float *f, int n, float *d, int *v, float *z)
{
    constexpr float infinity = std::numeric_limits<float>::infinity();
    int k = 0; v[0] = 0; z[0] = -infinity; z[1] = infinity;
    for (int q = 1; q < n; ++q) {
        const auto intersection = [&](int p) { return ((f[q] + float(q) * float(q)) - (f[p] + float(p) * float(p))) / (2.0f * float(q - p)); };
        float s = intersection(v[k]);
        while (k > 0 && s <= z[k]) { --k; s = intersection(v[k]); }
        if (s <= z[k]) { v[k] = q; z[k + 1] = infinity; continue; }
        ++k; v[k] = q; z[k] = s; z[k + 1] = infinity;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < float(q)) ++k;
        d[q] = float(q - v[k]) * float(q - v[k]) + f[v[k]];
    }
}

// Distance (pixels) from each pixel centre to the nearest pixel where featureMask is nonzero; features are
// in a (width+2)x(height+2) frame whose one-pixel ring is a feature when ringIsFeature.
std::vector<float> distanceToFeature(const QImage &featureMask, bool ringIsFeature)
{
    const int w = featureMask.width() + 2, h = featureMask.height() + 2;
    constexpr float big = 1e20f;
    std::vector<float> grid(size_t(w) * size_t(h), big);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const bool ring = x == 0 || y == 0 || x == w - 1 || y == h - 1;
        const bool feature = ring ? ringIsFeature : featureMask.constScanLine(y - 1)[x - 1] != 0;
        if (feature) grid[size_t(y) * w + x] = 0;
    }
    const int length = std::max(w, h);
    std::vector<float> f(static_cast<size_t>(length)), d(static_cast<size_t>(length)), z(static_cast<size_t>(length) + 1);
    std::vector<int> v(static_cast<size_t>(length));
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) f[size_t(y)] = grid[size_t(y) * w + x];
        distance1D(f.data(), h, d.data(), v.data(), z.data());
        for (int y = 0; y < h; ++y) grid[size_t(y) * w + x] = d[size_t(y)];
    }
    for (int y = 0; y < h; ++y) {
        distance1D(grid.data() + size_t(y) * w, w, d.data(), v.data(), z.data());
        std::copy_n(d.data(), w, grid.data() + size_t(y) * w);
    }
    std::vector<float> result(size_t(featureMask.width()) * size_t(featureMask.height()));
    for (int y = 0; y < featureMask.height(); ++y) for (int x = 0; x < featureMask.width(); ++x)
        result[size_t(y) * featureMask.width() + x] = std::sqrt(grid[size_t(y + 1) * w + x + 1]);
    return result;
}

constexpr int exactRadiusLimit = 24;

// Larger radii: distance transform of the half-selected level set, with a one-pixel anti-aliased rim.
QImage largeDilate(const QImage &input, int radius, bool outsideSelected)
{
    QImage features(input.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < input.height(); ++y) {
        const uchar *in = input.constScanLine(y); uchar *out = features.scanLine(y);
        for (int x = 0; x < input.width(); ++x) out[x] = in[x] >= 128 ? 255 : 0;
    }
    const std::vector<float> distance = distanceToFeature(features, outsideSelected);
    QImage result = input;
    result.detach();
    for (int y = 0; y < input.height(); ++y) {
        uchar *out = result.scanLine(y);
        for (int x = 0; x < input.width(); ++x) {
            const float reach = std::clamp(float(radius) + 1.0f - distance[size_t(y) * input.width() + x], 0.0f, 1.0f);
            out[x] = std::max(out[x], uchar(std::lround(reach * 255.0f)));
        }
    }
    return result;
}

} // namespace

QImage dilated(const QImage &mask, int radius)
{
    const QImage input = mask.convertToFormat(QImage::Format_Grayscale8);
    if (radius <= 0 || input.isNull()) return input;
    return radius <= exactRadiusLimit ? diskMax(input, radius, 0) : largeDilate(input, radius, false);
}

QImage eroded(const QImage &mask, int radius)
{
    QImage inverse = mask.convertToFormat(QImage::Format_Grayscale8);
    if (radius <= 0 || inverse.isNull()) return inverse;
    inverse.detach();
    for (int y = 0; y < inverse.height(); ++y) { uchar *row = inverse.scanLine(y); for (int x = 0; x < inverse.width(); ++x) row[x] = uchar(255 - row[x]); }
    // Erosion is dilation of what is not selected; the canvas's outside is unselected, so it dilates in from every edge.
    QImage grown = radius <= exactRadiusLimit ? diskMax(inverse, radius, 255) : largeDilate(inverse, radius, true);
    for (int y = 0; y < grown.height(); ++y) { uchar *row = grown.scanLine(y); for (int x = 0; x < grown.width(); ++x) row[x] = uchar(255 - row[x]); }
    return grown;
}

} // namespace compositor::SelectionOps
