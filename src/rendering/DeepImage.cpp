#include "rendering/DeepImage.h"
#include "rendering/GpuOperations.h"
#include "DeepPixels.h"
#include "DitherPixels.h"
#include <QColorSpace>
#include <algorithm>
#include <cmath>
#include <vector>

namespace compositor::DeepImage
{
bool isHDR(const QImage &i)
{
    if (i.text(QStringLiteral("compositorDepth")) == QStringLiteral("16"))
        return false;
    return i.format() == QImage::Format_RGBA16FPx4 || i.format() == QImage::Format_RGBA16FPx4_Premultiplied ||
           i.format() == QImage::Format_RGBX16FPx4 || i.format() == QImage::Format_RGBA32FPx4 ||
           i.format() == QImage::Format_RGBA32FPx4_Premultiplied || i.format() == QImage::Format_RGBX32FPx4;
}
bool isDeep(const QImage &i)
{
    return isHDR(i) || i.depth() > 32 || i.format() == QImage::Format_Grayscale16;
}
QImage canonical(const QImage &i)
{
    return i.convertToFormat(isHDR(i)    ? QImage::Format_RGBA16FPx4_Premultiplied
                             : isDeep(i) ? QImage::Format_RGBA64_Premultiplied
                                         : QImage::Format_RGBA8888_Premultiplied);
}
QImage floating(const QImage &image)
{
    // Qt 6.10's direct 8-bit premultiplied -> float-premultiplied conversion
    // can replace alpha with 1. The 16-bit bridge preserves both alpha and RGB.
    return (image.depth() <= 32 ? image.convertToFormat(QImage::Format_RGBA64_Premultiplied) : image)
        .convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
}
QImage dithered(const QImage &image)
{
    if (!isDeep(image))
        return image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const QImage wide = image.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    QImage out(image.size(), QImage::Format_RGBA8888_Premultiplied);
    // The upstream quantizer's 16-bit input is tightly packed.
    dither_quantize16(reinterpret_cast<const uint16_t *>(wide.constBits()), out.bits(), out.width(),
                      out.height(), out.bytesPerLine());
    return out;
}
QImage through(const QImage &image, const std::function<QImage(const QImage &)> &op, bool mixes)
{
    if (!isDeep(image))
        return op(image);
    const QImage input = canonical(image);
    if (isHDR(input)) {
        QImage normal(input.size(), QImage::Format_RGBA64_Premultiplied);
        std::vector<float> scales(size_t(input.width()) * input.height());
        deep_hdr_normalize(reinterpret_cast<const uint16_t *>(input.constBits()), input.bytesPerLine(),
                           reinterpret_cast<uint16_t *>(normal.bits()), normal.bytesPerLine(), scales.data(),
                           input.width(), input.height());
        const QImage result = through(normal, op, mixes);
        if (result.isNull())
            return {};
        QImage out(input.size(), input.format());
        deep_hdr_restore(reinterpret_cast<const uint16_t *>(result.constBits()), result.bytesPerLine(),
                         scales.data(), reinterpret_cast<uint16_t *>(out.bits()), out.bytesPerLine(),
                         out.width(), out.height());
        return out;
    }
    const auto *deep = reinterpret_cast<const uint16_t *>(input.constBits());
    const auto run = [&](int step, int channel) {
        QImage split(input.size(), QImage::Format_RGBA8888_Premultiplied);
        deep_split(deep, input.bytesPerLine(), split.bits(), split.bytesPerLine(), split.width(),
                   split.height(), step, channel);
        const QImage result = op(split);
        return result.isNull() || result.size() != input.size() ? QImage()
                                                                : result.convertToFormat(split.format());
    };
    const QImage low = run(0, -1);
    if (low.isNull())
        return {};
    QImage near[3], mid[3], far[3];
    const uint8_t *nearP[3]{}, *midP[3]{}, *farP[3]{};
    for (int i = 0; i < (mixes ? 3 : 1); ++i) {
        const int c = mixes ? i : -1;
        near[i] = run(1, c);
        mid[i] = run(2, c);
        far[i] = run(4, c);
        if (near[i].isNull() || mid[i].isNull() || far[i].isNull())
            return {};
        nearP[i] = near[i].constBits();
        midP[i] = mid[i].constBits();
        farP[i] = far[i].constBits();
    }
    QImage out(input.size(), input.format());
    deep_merge(deep, input.bytesPerLine(), low.constBits(), nearP, midP, farP, low.bytesPerLine(),
               reinterpret_cast<uint16_t *>(out.bits()), out.bytesPerLine(), out.width(), out.height(),
               mixes);
    return out;
}
QImage lookup(const QImage &image, const std::function<QImage(const QImage &)> &op)
{
    if (!isDeep(image))
        return op(image);
    if (isHDR(image)) {
        const QImage input = canonical(image);
        QImage normal(input.size(), QImage::Format_RGBA64_Premultiplied);
        std::vector<float> scales(size_t(input.width()) * input.height());
        deep_hdr_normalize(reinterpret_cast<const uint16_t *>(input.constBits()), input.bytesPerLine(),
                           reinterpret_cast<uint16_t *>(normal.bits()), normal.bytesPerLine(), scales.data(),
                           input.width(), input.height());
        const QImage result = lookup(normal, op);
        if (result.isNull())
            return {};
        QImage out(input.size(), input.format());
        deep_hdr_restore(reinterpret_cast<const uint16_t *>(result.constBits()), result.bytesPerLine(),
                         scales.data(), reinterpret_cast<uint16_t *>(out.bits()), out.bytesPerLine(),
                         out.width(), out.height());
        return out;
    }
    constexpr int side = 33;
    QImage samples(side * side, side, QImage::Format_RGBA8888_Premultiplied);
    for (int b = 0; b < side; ++b)
        for (int g = 0; g < side; ++g)
            for (int r = 0; r < side; ++r) {
                auto *p = samples.scanLine(b) + (g * side + r) * 4;
                p[0] = uchar(qRound(r * 255.0 / (side - 1)));
                p[1] = uchar(qRound(g * 255.0 / (side - 1)));
                p[2] = uchar(qRound(b * 255.0 / (side - 1)));
                p[3] = 255;
            }
    const QImage adjusted = op(samples).convertToFormat(samples.format());
    if (adjusted.isNull())
        return {};
    std::vector<float> cube(side * side * side * 4);
    for (int b = 0; b < side; ++b)
        for (int x = 0; x < side * side; ++x)
            for (int c = 0; c < 4; ++c)
                cube[(b * side * side + x) * 4 + c] = adjusted.constScanLine(b)[x * 4 + c] / 255.0f;
    QImage out = canonical(image);
    deep_cube_apply(reinterpret_cast<uint16_t *>(out.bits()), out.bytesPerLine(), out.width(), out.height(),
                    cube.data(), side);
    return out;
}
QImage takePainted(const QImage &deep, const QImage &painted, const QImage &base)
{
    if (!isDeep(deep) || deep.size() != painted.size() || base.size() != painted.size())
        return painted;
    QImage out = canonical(deep);
    const QImage p = painted.convertToFormat(QImage::Format_RGBA8888_Premultiplied),
                 b = base.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (isHDR(out))
        deep_take_painted_hdr(reinterpret_cast<uint16_t *>(out.bits()), out.bytesPerLine(), p.constBits(),
                              b.constBits(), p.bytesPerLine(), out.width(), out.height());
    else
        deep_take_painted(reinterpret_cast<uint16_t *>(out.bits()), out.bytesPerLine(), p.constBits(),
                          b.constBits(), p.bytesPerLine(), out.width(), out.height());
    return out;
}
QImage gaussianBlur(const QImage &image, double radius)
{
    if (auto gpu = GpuOperations::gaussian(image,radius,false); !gpu.isNull()) return gpu;
    QImage result = floating(image);
    if (!std::isfinite(radius) || radius <= 0)
        return image;
    radius = std::clamp(radius, .1, 250.0);
    // Small radii need the actual Gaussian: box widths jump at fractional radii and made the
    // sharp end of Progressive Blur noticeably soft. Accumulate in floats, quantize only on return.
    if (radius <= 10) {
        const int reach = std::max(1, int(std::ceil(3*radius)));
        std::vector<double> weights(size_t(reach)*2+1);
        double total = 0;
        for (int i = -reach; i <= reach; ++i) total += weights[size_t(i+reach)] = std::exp(-.5*i*i/(radius*radius));
        for (auto &weight : weights) weight /= total;
        for (bool horizontal : {true,false}) {
            QImage out(result.size(),result.format());
            for (int y = 0; y < out.height(); ++y) {
                auto *row = reinterpret_cast<float *>(out.scanLine(y));
                for (int x = 0; x < out.width(); ++x) {
                    double sum[4]{};
                    for (int i = -reach; i <= reach; ++i) {
                        const int sx = horizontal ? std::clamp(x+i,0,out.width()-1) : x;
                        const int sy = horizontal ? y : std::clamp(y+i,0,out.height()-1);
                        const auto *pixel = reinterpret_cast<const float *>(result.constScanLine(sy))+sx*4;
                        for (int c = 0; c < 4; ++c) sum[c] += pixel[c]*weights[size_t(i+reach)];
                    }
                    for (int c = 0; c < 4; ++c) row[x*4+c] = float(sum[c]);
                }
            }
            result = out;
        }
        return result.convertToFormat(image.format());
    }
    constexpr int passes = 3;
    int lower = int(std::floor(std::sqrt(12 * radius * radius / passes + 1)));
    if (!(lower & 1))
        --lower;
    const int upper = lower + 2,
              lowerCount =
                  qRound((12 * radius * radius - passes * lower * lower - 4 * passes * lower - 3 * passes) /
                         (-4.0 * lower - 4));
    for (int pass = 0; pass < passes; ++pass)
        for (bool horizontal : {true, false}) {
            const int r = std::max(0, ((pass < lowerCount ? lower : upper) - 1) / 2);
            if (!r)
                continue;
            QImage out(result.size(), result.format());
            const int lines = horizontal ? result.height() : result.width(),
                      length = horizontal ? result.width() : result.height();
            for (int line = 0; line < lines; ++line) {
                double sum[4]{};
                const auto pixel = [&](int pos) {
                    pos = std::clamp(pos, 0, length - 1);
                    return reinterpret_cast<const float *>(result.constScanLine(horizontal ? line : pos)) +
                           (horizontal ? pos : line) * 4;
                };
                for (int pos = -r; pos <= r; ++pos)
                    for (int c = 0; c < 4; ++c)
                        sum[c] += pixel(pos)[c];
                for (int pos = 0; pos < length; ++pos) {
                    auto *p = reinterpret_cast<float *>(out.scanLine(horizontal ? line : pos)) +
                              (horizontal ? pos : line) * 4;
                    for (int c = 0; c < 4; ++c) {
                        p[c] = float(sum[c] / (2 * r + 1));
                        sum[c] += pixel(pos + r + 1)[c] - pixel(pos - r)[c];
                    }
                }
            }
            result = out;
        }
    return result.convertToFormat(image.format());
}
QImage motionBlur(const QImage &image, double angle, double distance)
{
    if (auto gpu = GpuOperations::motion(image,angle,distance); !gpu.isNull()) return gpu;
    if (image.isNull() || !std::isfinite(angle) || !std::isfinite(distance) || distance <= 0) return image;
    const QImage input = floating(image);
    const double radians = std::clamp(angle,-90.0,90.0)*3.14159265358979323846/180;
    const double dx = std::cos(radians), dy = -std::sin(radians);
    const bool horizontal = std::abs(dx) >= std::abs(dy);
    const int width = horizontal ? input.width() : input.height(), height = horizontal ? input.height() : input.width();
    const double major = horizontal ? dx : dy, slope = (horizontal ? dy : dx)/major;
    const int reach = std::max(0,int(std::lround(std::clamp(distance,1.0,2000.0)*std::abs(major)/2)));
    std::vector<int> shifts(size_t(width),0);
    int low = 0, high = 0;
    for (int x = 0; x < width; ++x) {
        shifts[size_t(x)] = int(std::lround(slope*x));
        low = std::min(low,shifts[size_t(x)]); high = std::max(high,shifts[size_t(x)]);
    }
    QImage out(input.size(),input.format()); out.fill(Qt::transparent);
    std::vector<double> sums(size_t(width+1)*4,0);
    for (int r = -high; r < height-low; ++r) {
        for (int x = 0; x < width; ++x) {
            const int y = r+shifts[size_t(x)];
            const float *pixel = y >= 0 && y < height
                ? reinterpret_cast<const float *>(input.constScanLine(horizontal?y:x))+(horizontal?x:y)*4 : nullptr;
            for (int c=0;c<4;++c) sums[size_t(x+1)*4+c] = sums[size_t(x)*4+c]+(pixel?pixel[c]:0);
        }
        for (int x = 0; x < width; ++x) {
            const int y = r+shifts[size_t(x)]; if (y<0 || y>=height) continue;
            auto *pixel = reinterpret_cast<float *>(out.scanLine(horizontal?y:x))+(horizontal?x:y)*4;
            for (int c=0;c<4;++c) pixel[c] = float((sums[size_t(std::min(width,x+reach+1))*4+c]
                -sums[size_t(std::max(0,x-reach))*4+c])/(2*reach+1));
        }
    }
    return out.convertToFormat(image.format());
}
} // namespace compositor::DeepImage
