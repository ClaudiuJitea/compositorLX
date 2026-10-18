#include "rendering/Scanlines.h"
#include "rendering/DeepImage.h"
#include "rendering/RasterOperations.h"
#include "DitherPixels.h"
#include <algorithm>
#include <cmath>

ScanlinesSettings ScanlinesSettings::normalized() const
{
    auto r = *this;
    const auto clamp = [](double x, double lo, double hi, double fallback) {
        return std::isfinite(x) ? std::clamp(x, lo, hi) : fallback;
    };
    r.lineSpacing = std::round(clamp(lineSpacing, 2, 32, 4));
    r.thickness = clamp(thickness, 5, 100, 70);
    r.glow = clamp(glow, 0, 100, 0);
    r.dots = clamp(dots, 0, 100, 0);
    r.wobble = clamp(wobble, 0, 64, 0);
    r.displace = clamp(displace, -100, 100, 0);
    r.smoothness = clamp(smoothness, 0, 100, 50);
    r.threshold = clamp(threshold, 0, 100, 0);
    r.split = std::round(clamp(split, 0, 16, 0));
    r.density = clamp(density, -100, 100, 0);
    r.contrast = clamp(contrast, -100, 100, 0);
    r.blackLevel = clamp(blackLevel, 0, 100, 0);
    for (auto *color : {&r.dark, &r.light}) {
        color->red = clamp(color->red, 0, 1, 0);
        color->green = clamp(color->green, 0, 1, 0);
        color->blue = clamp(color->blue, 0, 1, 0);
    }
    return r;
}
QImage ScanlinesSettings::apply(const QImage &image) const
{
    if (compositor::DeepImage::isDeep(image))
        return compositor::DeepImage::through(image, [&](const QImage &low) { return apply(low); }, true);
    if (image.isNull())
        return {};
    const auto s = normalized();
    QImage out = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    ScanlinesParams p{};
    p.spacing = int(s.lineSpacing);
    p.thickness = float(s.thickness / 100);
    p.dots = float(s.dots / 100);
    p.wobble = float(s.wobble);
    p.displace = float(s.displace);
    p.smoothness = float(s.smoothness / 100);
    p.threshold = float(s.threshold / 100);
    p.split = float(s.split);
    p.density = float(s.density / 100);
    p.contrast = float(s.contrast / 100);
    p.blackLevel = float(s.blackLevel / 100);
    p.originalColors = s.colors == DitherColors::Original;
    const auto byte = [](double x) { return uint8_t(std::lround(x * 255)); };
    const auto dark = s.colors == DitherColors::TwoColors ? s.dark : DitherColor{0, 0, 0};
    const auto light = s.colors == DitherColors::TwoColors ? s.light : DitherColor{1, 1, 1};
    p.dark[0] = byte(dark.red);
    p.dark[1] = byte(dark.green);
    p.dark[2] = byte(dark.blue);
    p.light[0] = byte(light.red);
    p.light[1] = byte(light.green);
    p.light[2] = byte(light.blue);
    if (!scanlines_apply(out.bits(), out.width(), out.height(), out.bytesPerLine(), &p))
        return {};
    if (s.glow > 0) {
        const double sigma = s.lineSpacing * 3 + 3;
        const int shrink = std::max(1, int(std::floor(sigma / 4)));
        QImage small = out.scaled(std::max(1, out.width() / shrink), std::max(1, out.height() / shrink),
                                  Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const QImage bloom = compositor::RasterOperations::gaussianBlur(small, sigma / shrink)
                                 .scaled(out.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                 .convertToFormat(out.format());
        dither_glow(out.bits(), bloom.constBits(), out.width(), out.height(), out.bytesPerLine(),
                    float(s.glow / 100 * 2.5));
    }
    return out;
}
