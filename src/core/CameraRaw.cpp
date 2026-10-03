#include "core/CameraRaw.h"

extern "C" {
#include "AdjustPixels.h"
#include "LevelsPixels.h"
}

#include <QPainter>
#include <QTransform>
#include <algorithm>
#include <cmath>

namespace compositor {

namespace {

inline double clampDouble(double val, double low, double high, double def = 0.0) {
    if (!std::isfinite(val)) return def;
    return std::clamp(val, low, high);
}

inline double srgbDecode(double encoded) {
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

} // namespace

const QVector<CurvePoint> CameraRawCurveSettings::linear = {{0.0, 0.0}, {1.0, 1.0}};
const QVector<CurvePoint> CameraRawCurveSettings::mediumContrast = {
    {0.0, 0.0}, {0.25, 0.18}, {0.75, 0.82}, {1.0, 1.0}
};
const QVector<CurvePoint> CameraRawCurveSettings::strongContrast = {
    {0.0, 0.0}, {0.25, 0.10}, {0.75, 0.90}, {1.0, 1.0}
};

const std::array<double, 8> CameraRawMixerSettings::centers = {
    0.0, 30.0, 60.0, 120.0, 180.0, 240.0, 270.0, 300.0
};

bool CameraRawCurveSettings::isLinear(const QVector<CurvePoint> &pts) {
    return pts.size() == 2 &&
           std::abs(pts[0].x) < 1e-4 && std::abs(pts[0].y) < 1e-4 &&
           std::abs(pts[1].x - 1.0) < 1e-4 && std::abs(pts[1].y - 1.0) < 1e-4;
}

bool CameraRawCurveSettings::adjusts() const {
    return shadows != 0.0 || darks != 0.0 || lights != 0.0 || highlights != 0.0 ||
           refineSaturation != 0.0 ||
           !isLinear(rgb) || !isLinear(red) || !isLinear(green) || !isLinear(blue);
}

// Camera Raw's parametric curve, matched to Photoshop's (mac 60d9117): Darks bends the whole range below the middle
// divider and Lights the whole range above it, Shadows and Highlights just the ranges past the outer dividers. Each
// bend is a gamma curve across its range, which keeps the curve rising however far the sliders go; the result is run
// through the same smooth curve as Image > Curves so the halves meet without a corner.
namespace {
// Bends tones below `lower` by `low` and above `upper` by `high` (-100..100), leaving black, white and the dividers.
// Fitted to Photoshop: Darks -51 dips the curve about 0.1 at a quarter of the way up.
double bendTone(double tone, double lower, double low, double upper, double high) {
    constexpr double strength = 1.66;
    if (tone < lower && lower > 0.0) return lower * std::pow(tone / lower, std::pow(2.0, -low / 100.0 * strength));
    if (tone > upper && upper < 1.0) {
        const double rest = 1.0 - upper;
        return 1.0 - rest * std::pow((1.0 - tone) / rest, std::pow(2.0, high / 100.0 * strength));
    }
    return tone;
}
} // namespace

double CameraRawCurveSettings::parametric(double tone) const {
    if (shadows == 0.0 && darks == 0.0 && lights == 0.0 && highlights == 0.0) return tone;
    QVector<CurvePoint> anchors;
    anchors.reserve(33);
    for (int i = 0; i <= 32; ++i) {
        const double x = double(i) / 32.0;
        const double y = bendTone(bendTone(x, shadowSplit / 100.0, shadows, lightSplit / 100.0, highlights),
                                  darkSplit / 100.0, darks, darkSplit / 100.0, lights);
        anchors.push_back({x, y});
    }
    return evaluatePoint(tone, anchors);
}

// The same monotone cubic as Image > Curves (CurvesSettings::value), over 0..1 points and without its 32-point cap,
// because the parametric curve is sampled at 33 anchors.
double CameraRawCurveSettings::evaluatePoint(double x, const QVector<CurvePoint> &pts) const {
    if (pts.size() < 2) return x;
    int index = 0;
    while (index + 2 < pts.size() && pts[index + 1].x <= x) ++index;
    QVector<double> differences;
    differences.reserve(pts.size() - 1);
    for (int i = 0; i + 1 < pts.size(); ++i) differences.push_back((pts[i + 1].y - pts[i].y) / (pts[i + 1].x - pts[i].x));
    const auto slope = [&](int i) {
        if (i == 0) return differences.constFirst();
        if (i == pts.size() - 1) return differences.constLast();
        if (differences[i - 1] * differences[i] <= 0) return 0.0;
        return 2 / (1 / differences[i - 1] + 1 / differences[i]);
    };
    const double h = pts[index + 1].x - pts[index].x;
    const double t = std::clamp((x - pts[index].x) / h, 0.0, 1.0);
    const double y = (2*t*t*t - 3*t*t + 1) * pts[index].y + (t*t*t - 2*t*t + t) * h * slope(index)
        + (-2*t*t*t + 3*t*t) * pts[index + 1].y + (t*t*t - t*t) * h * slope(index + 1);
    return std::clamp(y, 0.0, 1.0);
}

std::array<float, 256> CameraRawCurveSettings::toneTable() const {
    std::array<float, 256> table{};
    for (int i = 0; i < 256; ++i) {
        const double p = parametric(double(i) / 255.0);
        table[size_t(i)] = static_cast<float>(evaluatePoint(p, rgb));
    }
    return table;
}

std::array<float, 256> CameraRawCurveSettings::channelTable(const QVector<CurvePoint> &pts) const {
    std::array<float, 256> table{};
    for (int i = 0; i < 256; ++i) {
        table[size_t(i)] = static_cast<float>(evaluatePoint(double(i) / 255.0, pts));
    }
    return table;
}

CameraRawCurveSettings CameraRawCurveSettings::nudged(CameraRawPointChannel ch, double tone, double delta) const {
    CameraRawCurveSettings res = *this;
    QVector<CurvePoint> *points = nullptr;
    switch (ch) {
    case CameraRawPointChannel::RGB: points = &res.rgb; break;
    case CameraRawPointChannel::Red: points = &res.red; break;
    case CameraRawPointChannel::Green: points = &res.green; break;
    case CameraRawPointChannel::Blue: points = &res.blue; break;
    }
    if (!points || points->isEmpty()) return *this;

    int nearestIdx = 0;
    double minDist = 1e9;
    for (int i = 0; i < points->size(); ++i) {
        double d = std::abs((*points)[i].x - tone);
        if (d < minDist) {
            minDist = d;
            nearestIdx = i;
        }
    }
    (*points)[nearestIdx].y = std::clamp((*points)[nearestIdx].y + delta, 0.0, 1.0);
    return res;
}

QVector<CurvePoint> CameraRawCurveSettings::repair(const QVector<CurvePoint> &pts) {
    QVector<CurvePoint> sorted;
    for (const auto &p : pts) {
        if (std::isfinite(p.x) && std::isfinite(p.y)) sorted.push_back(p);
    }
    std::sort(sorted.begin(), sorted.end(), [](const CurvePoint &a, const CurvePoint &b) {
        return a.x < b.x;
    });
    if (sorted.size() < 2) return linear;
    sorted.first() = {0.0, std::clamp(sorted.first().y, 0.0, 1.0)};
    sorted.last() = {1.0, std::clamp(sorted.last().y, 0.0, 1.0)};

    QVector<CurvePoint> kept = {sorted.first()};
    for (int i = 1; i < sorted.size() - 1; ++i) {
        const double x = std::clamp(sorted[i].x, 0.01, 0.99);
        if (x > kept.last().x + 0.01) {
            kept.push_back({x, std::clamp(sorted[i].y, 0.0, 1.0)});
        }
    }
    kept.push_back(sorted.last());
    return kept;
}

CameraRawCurveSettings CameraRawCurveSettings::normalized() const {
    CameraRawCurveSettings res = *this;
    res.shadows = clampDouble(shadows, -100.0, 100.0, 0.0);
    res.darks = clampDouble(darks, -100.0, 100.0, 0.0);
    res.lights = clampDouble(lights, -100.0, 100.0, 0.0);
    res.highlights = clampDouble(highlights, -100.0, 100.0, 0.0);
    res.refineSaturation = clampDouble(refineSaturation, -100.0, 100.0, 0.0);
    res.shadowSplit = clampDouble(shadowSplit, 5.0, 90.0, 25.0);
    res.darkSplit = clampDouble(darkSplit, res.shadowSplit + 2.0, 95.0, 50.0);
    res.lightSplit = clampDouble(lightSplit, res.darkSplit + 2.0, 98.0, 75.0);
    res.rgb = repair(rgb);
    res.red = repair(red);
    res.green = repair(green);
    res.blue = repair(blue);
    return res;
}

CameraRawPointColor CameraRawPointColor::normalized() const {
    CameraRawPointColor res = *this;
    res.hue = clampDouble(hue, 0.0, 360.0, 0.0);
    res.saturation = clampDouble(saturation, 0.0, 1.0, 0.0);
    res.luminance = clampDouble(luminance, 0.0, 1.0, 0.0);
    res.hueShift = clampDouble(hueShift, -100.0, 100.0, 0.0);
    res.saturationShift = clampDouble(saturationShift, -100.0, 100.0, 0.0);
    res.luminanceShift = clampDouble(luminanceShift, -100.0, 100.0, 0.0);
    res.hueRange = clampDouble(hueRange, 5.0, 180.0, 30.0);
    res.saturationRange = clampDouble(saturationRange, 0.05, 1.0, 0.4);
    res.luminanceRange = clampDouble(luminanceRange, 0.05, 1.0, 0.4);
    return res;
}

bool CameraRawMixerSettings::adjusts() const {
    for (double h : hue) if (h != 0.0) return true;
    for (double s : saturation) if (s != 0.0) return true;
    for (double l : luminance) if (l != 0.0) return true;
    for (const auto &p : points) {
        if (p.hueShift != 0.0 || p.saturationShift != 0.0 || p.luminanceShift != 0.0) return true;
    }
    return false;
}

std::array<double, 8> CameraRawMixerSettings::weights(double hueDegrees) {
    std::array<double, 8> res{};
    for (size_t i = 0; i < 8; ++i) {
        double dist = std::abs(hueDegrees - centers[i]);
        if (dist > 180.0) dist = 360.0 - dist;
        res[i] = std::max(0.0, 1.0 - dist / 40.0);
    }
    return res;
}

std::array<float, 24> CameraRawMixerSettings::mixerFloats() const {
    std::array<float, 24> res{};
    for (size_t i = 0; i < 8; ++i) {
        res[i] = static_cast<float>(hue[i] / 100.0);
        res[8 + i] = static_cast<float>(saturation[i] / 100.0);
        res[16 + i] = static_cast<float>(luminance[i] / 100.0);
    }
    return res;
}

QVector<float> CameraRawMixerSettings::pointFloats() const {
    QVector<float> res;
    res.reserve(points.size() * 9);
    for (const auto &pt : points) {
        res.push_back(static_cast<float>(pt.hue / 360.0));
        res.push_back(static_cast<float>(pt.saturation));
        res.push_back(static_cast<float>(pt.luminance));
        res.push_back(static_cast<float>(pt.hueShift / 100.0));
        res.push_back(static_cast<float>(pt.saturationShift / 100.0));
        res.push_back(static_cast<float>(pt.luminanceShift / 100.0));
        res.push_back(static_cast<float>(pt.hueRange / 360.0));
        res.push_back(static_cast<float>(pt.saturationRange));
        res.push_back(static_cast<float>(pt.luminanceRange));
    }
    return res;
}

CameraRawMixerSettings CameraRawMixerSettings::normalized() const {
    CameraRawMixerSettings res = *this;
    for (size_t i = 0; i < 8; ++i) {
        res.hue[i] = clampDouble(hue[i], -100.0, 100.0, 0.0);
        res.saturation[i] = clampDouble(saturation[i], -100.0, 100.0, 0.0);
        res.luminance[i] = clampDouble(luminance[i], -100.0, 100.0, 0.0);
    }
    QVector<CameraRawPointColor> normPoints;
    for (int i = 0; i < std::min(8, int(points.size())); ++i) {
        normPoints.push_back(points[i].normalized());
    }
    res.points = normPoints;
    return res;
}

CameraRawGradeWheel CameraRawGradeWheel::normalized() const {
    CameraRawGradeWheel res;
    res.hue = clampDouble(hue, 0.0, 360.0, 0.0);
    res.saturation = clampDouble(saturation, 0.0, 100.0, 0.0);
    res.luminance = clampDouble(luminance, -100.0, 100.0, 0.0);
    return res;
}

bool CameraRawGradingSettings::adjusts() const {
    for (const auto &w : {shadows, midtones, highlights, global}) {
        if (w.saturation != 0.0 || w.luminance != 0.0) return true;
    }
    return false;
}

std::array<float, 12> CameraRawGradingSettings::gradeFloats() const {
    std::array<float, 12> res{};
    const CameraRawGradeWheel wheels[4] = {shadows, midtones, highlights, global};
    for (size_t i = 0; i < 4; ++i) {
        res[i * 3 + 0] = static_cast<float>(wheels[i].hue / 360.0);
        res[i * 3 + 1] = static_cast<float>(wheels[i].saturation / 100.0);
        res[i * 3 + 2] = static_cast<float>(wheels[i].luminance / 100.0);
    }
    return res;
}

CameraRawGradingSettings CameraRawGradingSettings::normalized() const {
    CameraRawGradingSettings res = *this;
    res.shadows = shadows.normalized();
    res.midtones = midtones.normalized();
    res.highlights = highlights.normalized();
    res.global = global.normalized();
    res.blending = clampDouble(blending, 0.0, 100.0, 50.0);
    res.balance = clampDouble(balance, -100.0, 100.0, 0.0);
    return res;
}

CameraRawDetailSettings CameraRawDetailSettings::normalized() const {
    CameraRawDetailSettings res = *this;
    res.sharpenAmount = clampDouble(sharpenAmount, 0.0, 150.0, 0.0);
    res.sharpenRadius = clampDouble(sharpenRadius, 0.0, 100.0, 10.0);
    res.sharpenDetail = clampDouble(sharpenDetail, 0.0, 100.0, 25.0);
    res.sharpenMasking = clampDouble(sharpenMasking, 0.0, 100.0, 0.0);
    res.noiseLuminance = clampDouble(noiseLuminance, 0.0, 100.0, 0.0);
    res.noiseLuminanceDetail = clampDouble(noiseLuminanceDetail, 0.0, 100.0, 50.0);
    res.noiseLuminanceContrast = clampDouble(noiseLuminanceContrast, 0.0, 100.0, 0.0);
    res.noiseColor = clampDouble(noiseColor, 0.0, 100.0, 0.0);
    res.noiseColorDetail = clampDouble(noiseColorDetail, 0.0, 100.0, 50.0);
    res.noiseColorSmoothness = clampDouble(noiseColorSmoothness, 0.0, 100.0, 50.0);
    return res;
}

bool CameraRawOpticsSettings::adjusts() const {
    return removeChromaticAberration || enableLensProfile || distortion != 0.0 ||
           purpleAmount != 0.0 || greenAmount != 0.0 || vignetteAmount != 0.0;
}

double CameraRawOpticsSettings::distortionK(double profileStrength) const {
    const double manual = (distortion / 100.0) * profileStrength;
    const double profile = enableLensProfile ? (profileDistortion / 100.0) * profileStrength : 0.0;
    return manual + profile;
}

CameraRawOpticsSettings CameraRawOpticsSettings::normalized() const {
    CameraRawOpticsSettings res = *this;
    res.profileDistortion = clampDouble(profileDistortion, 0.0, 100.0, 100.0);
    res.profileVignetting = clampDouble(profileVignetting, 0.0, 100.0, 100.0);
    res.distortion = clampDouble(distortion, -100.0, 100.0, 0.0);
    res.purpleAmount = clampDouble(purpleAmount, 0.0, 100.0, 0.0);
    res.greenAmount = clampDouble(greenAmount, 0.0, 100.0, 0.0);
    res.vignetteAmount = clampDouble(vignetteAmount, -100.0, 100.0, 0.0);
    res.vignetteMidpoint = clampDouble(vignetteMidpoint, 0.0, 100.0, 50.0);
    res.purpleHueLow = clampDouble(purpleHueLow, 0.0, 360.0, 270.0);
    res.purpleHueHigh = clampDouble(purpleHueHigh, 0.0, 360.0, 310.0);
    res.greenHueLow = clampDouble(greenHueLow, 0.0, 360.0, 60.0);
    res.greenHueHigh = clampDouble(greenHueHigh, 0.0, 360.0, 120.0);
    if (res.purpleHueLow > res.purpleHueHigh) std::swap(res.purpleHueLow, res.purpleHueHigh);
    if (res.greenHueLow > res.greenHueHigh) std::swap(res.greenHueLow, res.greenHueHigh);
    return res;
}

bool CameraRawGeometrySettings::adjusts() const {
    const bool usesGuides = (upright == CameraRawUprightMode::Guided) &&
        std::any_of(guides.begin(), guides.end(), [](const CameraRawGeometryGuide &g) {
            return std::hypot(g.endX - g.startX, g.endY - g.startY) > 0.01;
        });
    return usesGuides || vertical != 0.0 || horizontal != 0.0 || rotate != 0.0 ||
           aspect != 0.0 || scale != 0.0 || offsetX != 0.0 || offsetY != 0.0;
}

CameraRawGeometrySettings CameraRawGeometrySettings::normalized() const {
    CameraRawGeometrySettings res = *this;
    res.vertical = clampDouble(vertical, -100.0, 100.0, 0.0);
    res.horizontal = clampDouble(horizontal, -100.0, 100.0, 0.0);
    res.rotate = clampDouble(rotate, -45.0, 45.0, 0.0);
    res.aspect = clampDouble(aspect, -100.0, 100.0, 0.0);
    res.scale = clampDouble(scale, -100.0, 100.0, 0.0);
    res.offsetX = clampDouble(offsetX, -100.0, 100.0, 0.0);
    res.offsetY = clampDouble(offsetY, -100.0, 100.0, 0.0);
    QVector<CameraRawGeometryGuide> kept;
    for (const auto &g : guides) {
        if (std::hypot(g.endX - g.startX, g.endY - g.startY) > 0.01) {
            kept.push_back(g);
        }
    }
    res.guides = kept;
    return res;
}

QImage CameraRawGeometrySettings::apply(const QImage &image) const {
    const CameraRawGeometrySettings settings = normalized();
    if (!settings.adjusts()) return image;
    const int width = image.width();
    const int height = image.height();
    if (width <= 0 || height <= 0) return image;

    double effVertical = settings.vertical;
    double effHorizontal = settings.horizontal;
    double effRotate = settings.rotate;

    if (settings.upright == CameraRawUprightMode::Guided && !settings.guides.isEmpty()) {
        const auto &first = settings.guides.first();
        const double dx = first.endX - first.startX;
        const double dy = first.endY - first.startY;
        const double len = std::hypot(dx, dy);
        if (len > 1e-4) {
            double angle = std::atan2(dy, dx) * 180.0 / M_PI;
            double rot = -angle;
            if (rot > 45.0) rot -= 90.0;
            else if (rot < -45.0) rot += 90.0;
            effRotate += rot;
        }
        if (settings.guides.size() > 1) {
            const auto &second = settings.guides[1];
            const double sx = second.endX - second.startX;
            const double sy = second.endY - second.startY;
            if (std::hypot(sx, sy) > 1e-4) {
                const double a2 = std::atan2(sy, sx) * 180.0 / M_PI;
                if (std::abs(a2) > 45.0) effVertical += (a2 > 0 ? 25.0 : -25.0);
                else effHorizontal += (a2 > 0 ? 25.0 : -25.0);
            }
        }
    }

    const double w = double(width);
    const double h = double(height);
    const double strength = (settings.projection == CameraRawProjection::Perspective) ? 1.0 : 0.55;
    const double v = (effVertical / 100.0) * w * 0.18 * strength;
    const double hz = (effHorizontal / 100.0) * h * 0.18 * strength;
    const double aspectScale = 1.0 + settings.aspect / 200.0;
    const double zoom = 1.0 + settings.scale / 100.0;
    const double shiftX = (settings.offsetX / 100.0) * w * 0.15;
    const double shiftY = (settings.offsetY / 100.0) * h * 0.15;

    // Source polygon (0,0), (w,0), (w,h), (0,h)
    QPolygonF srcPoly;
    srcPoly << QPointF(0, 0) << QPointF(w, 0) << QPointF(w, h) << QPointF(0, h);

    // Target quad
    QPointF topLeft(-v + shiftX, 0 - shiftY);
    QPointF topRight(w + v + shiftX, 0 - shiftY);
    QPointF bottomRight(w + hz + shiftX, h + shiftY);
    QPointF bottomLeft(-hz + shiftX, h + shiftY);

    const QPointF center(w / 2.0 + shiftX, h / 2.0 + shiftY);
    const double radians = effRotate * M_PI / 180.0;
    const double cosR = std::cos(radians), sinR = std::sin(radians);

    auto rotatePoint = [&](const QPointF &pt) -> QPointF {
        const double dx = pt.x() - center.x(), dy = pt.y() - center.y();
        return QPointF(center.x() + dx * cosR - dy * sinR, center.y() + dx * sinR + dy * cosR);
    };

    topLeft = rotatePoint(topLeft);
    topRight = rotatePoint(topRight);
    bottomRight = rotatePoint(bottomRight);
    bottomLeft = rotatePoint(bottomLeft);

    if (aspectScale != 1.0) {
        auto scalePoint = [&](const QPointF &pt) -> QPointF {
            return QPointF(center.x() + (pt.x() - center.x()) * aspectScale,
                           center.y() + (pt.y() - center.y()) / aspectScale);
        };
        topLeft = scalePoint(topLeft);
        topRight = scalePoint(topRight);
        bottomRight = scalePoint(bottomRight);
        bottomLeft = scalePoint(bottomLeft);
    }

    if (zoom != 1.0) {
        auto zoomPoint = [&](const QPointF &pt) -> QPointF {
            return QPointF(center.x() + (pt.x() - center.x()) * zoom,
                           center.y() + (pt.y() - center.y()) * zoom);
        };
        topLeft = zoomPoint(topLeft);
        topRight = zoomPoint(topRight);
        bottomRight = zoomPoint(bottomRight);
        bottomLeft = zoomPoint(bottomLeft);
    }

    QPolygonF dstPoly;
    dstPoly << topLeft << topRight << bottomRight << bottomLeft;

    QTransform trans;
    if (!QTransform::quadToQuad(srcPoly, dstPoly, trans)) {
        return image;
    }

    QImage warped(width, height, QImage::Format_ARGB32_Premultiplied);
    warped.fill(Qt::transparent);
    {
        QPainter p(&warped);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.setTransform(trans);
        p.drawImage(0, 0, image);
    }

    if (!settings.constrainCrop) {
        return warped;
    }

    // Find non-transparent bounds
    int minX = width, minY = height, maxX = -1, maxY = -1;
    for (int y = 0; y < height; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(warped.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            if (qAlpha(line[x]) > 0) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    if (minX > maxX || minY > maxY) return warped;
    const int cropW = maxX - minX + 1;
    const int cropH = maxY - minY + 1;
    if (cropW < 1 || cropH < 1 || (cropW == width && cropH == height)) return warped;

    const QImage cropped = warped.copy(minX, minY, cropW, cropH);
    QImage fitted(width, height, QImage::Format_ARGB32_Premultiplied);
    fitted.fill(Qt::transparent);
    {
        QPainter p(&fitted);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const double fitScale = std::min(w / cropW, h / cropH);
        const double dw = cropW * fitScale;
        const double dh = cropH * fitScale;
        p.drawImage(QRectF((w - dw) / 2.0, (h - dh) / 2.0, dw, dh), cropped);
    }
    return fitted;
}

bool CameraRawCalibrationSettings::adjusts() const {
    return shadowTint != 0.0 || redHue != 0.0 || redSaturation != 0.0 ||
           greenHue != 0.0 || greenSaturation != 0.0 || blueHue != 0.0 || blueSaturation != 0.0;
}

CameraRawCalibrationSettings CameraRawCalibrationSettings::normalized() const {
    CameraRawCalibrationSettings res = *this;
    res.shadowTint = clampDouble(shadowTint, -100.0, 100.0, 0.0);
    res.redHue = clampDouble(redHue, -100.0, 100.0, 0.0);
    res.redSaturation = clampDouble(redSaturation, -100.0, 100.0, 0.0);
    res.greenHue = clampDouble(greenHue, -100.0, 100.0, 0.0);
    res.greenSaturation = clampDouble(greenSaturation, -100.0, 100.0, 0.0);
    res.blueHue = clampDouble(blueHue, -100.0, 100.0, 0.0);
    res.blueSaturation = clampDouble(blueSaturation, -100.0, 100.0, 0.0);
    return res;
}

bool CameraRawSettings::adjustsLight() const {
    return exposure != 0.0 || contrast != 0.0 || highlights != 0.0 ||
           shadows != 0.0 || whites != 0.0 || blacks != 0.0;
}

bool CameraRawSettings::adjustsColor() const {
    return temperature != 0.0 || tint != 0.0 || vibrance != 0.0 || saturation != 0.0;
}

bool CameraRawSettings::adjustsEffects() const {
    return texture != 0.0 || clarity != 0.0 || dehaze != 0.0 || glow != 0.0 ||
           vignetteAmount != 0.0 || grainAmount != 0.0;
}

bool CameraRawSettings::isIdentity() const {
    return !adjustsLight() && !adjustsColor() && !adjustsEffects() &&
           !adjustsCurve() && !adjustsMixer() && !adjustsGrading() &&
           !adjustsDetail() && !adjustsOptics() && !adjustsGeometry() && !adjustsCalibration();
}

bool CameraRawSettings::isValid() const {
    if (!std::isfinite(exposure) || exposure < -5.0 || exposure > 5.0) return false;
    const double tones[] = {
        contrast, highlights, shadows, whites, blacks, temperature, tint, vibrance, saturation,
        texture, clarity, dehaze, glowRange, glowSpread, glowWarmth, vignetteAmount, vignetteRoundness
    };
    for (double val : tones) {
        if (!std::isfinite(val) || val < -100.0 || val > 100.0) return false;
    }
    const double units[] = {
        glow, vignetteMidpoint, vignetteFeather, vignetteHighlights, grainAmount, grainSize, grainRoughness
    };
    for (double val : units) {
        if (!std::isfinite(val) || val < 0.0 || val > 100.0) return false;
    }
    return true;
}

CameraRawSettings CameraRawSettings::normalized() const {
    CameraRawSettings res = *this;
    res.exposure = clampDouble(exposure, -5.0, 5.0, 0.0);
    res.contrast = clampDouble(contrast, -100.0, 100.0, 0.0);
    res.highlights = clampDouble(highlights, -100.0, 100.0, 0.0);
    res.shadows = clampDouble(shadows, -100.0, 100.0, 0.0);
    res.whites = clampDouble(whites, -100.0, 100.0, 0.0);
    res.blacks = clampDouble(blacks, -100.0, 100.0, 0.0);
    res.temperature = clampDouble(temperature, -100.0, 100.0, 0.0);
    res.tint = clampDouble(tint, -100.0, 100.0, 0.0);
    res.vibrance = clampDouble(vibrance, -100.0, 100.0, 0.0);
    res.saturation = clampDouble(saturation, -100.0, 100.0, 0.0);
    res.texture = clampDouble(texture, -100.0, 100.0, 0.0);
    res.clarity = clampDouble(clarity, -100.0, 100.0, 0.0);
    res.dehaze = clampDouble(dehaze, -100.0, 100.0, 0.0);
    res.glow = clampDouble(glow, 0.0, 100.0, 0.0);
    res.glowRange = clampDouble(glowRange, -100.0, 100.0, 0.0);
    res.glowSpread = clampDouble(glowSpread, -100.0, 100.0, 0.0);
    res.glowWarmth = clampDouble(glowWarmth, -100.0, 100.0, 0.0);
    res.vignetteAmount = clampDouble(vignetteAmount, -100.0, 100.0, 0.0);
    res.vignetteMidpoint = clampDouble(vignetteMidpoint, 0.0, 100.0, 50.0);
    res.vignetteRoundness = clampDouble(vignetteRoundness, -100.0, 100.0, 0.0);
    res.vignetteFeather = clampDouble(vignetteFeather, 0.0, 100.0, 50.0);
    res.vignetteHighlights = clampDouble(vignetteHighlights, 0.0, 100.0, 0.0);
    res.grainAmount = clampDouble(grainAmount, 0.0, 100.0, 0.0);
    res.grainSize = clampDouble(grainSize, 0.0, 100.0, 25.0);
    res.grainRoughness = clampDouble(grainRoughness, 0.0, 100.0, 50.0);
    res.curve = curve.normalized();
    res.mixer = mixer.normalized();
    res.grading = grading.normalized();
    res.detail = detail.normalized();
    res.optics = optics.normalized();
    res.geometry = geometry.normalized();
    res.calibration = calibration.normalized();
    return res;
}

CameraRawSettings CameraRawSettings::applying(bool showsLight, bool showsColor, bool showsEffects,
                                             bool showsCurve, bool showsMixer,
                                             bool showsGrading, bool showsDetail,
                                             bool showsOptics, bool showsGeometry,
                                             bool showsCalibration) const {
    CameraRawSettings res = *this;
    if (!showsLight) {
        res.exposure = res.contrast = res.highlights = res.shadows = res.whites = res.blacks = 0.0;
    }
    if (!showsColor) {
        res.temperature = res.tint = res.vibrance = res.saturation = 0.0;
    }
    if (!showsEffects) {
        res.texture = res.clarity = res.dehaze = res.glow = res.vignetteAmount = res.grainAmount = 0.0;
    }
    if (!showsCurve) res.curve = CameraRawCurveSettings();
    if (!showsMixer) res.mixer = CameraRawMixerSettings();
    if (!showsGrading) res.grading = CameraRawGradingSettings();
    if (!showsDetail) res.detail = CameraRawDetailSettings();
    if (!showsOptics) res.optics = CameraRawOpticsSettings();
    if (!showsGeometry) res.geometry = CameraRawGeometrySettings();
    if (!showsCalibration) res.calibration = CameraRawCalibrationSettings();
    return res;
}

std::tuple<double, double, double> CameraRawSettings::gains() const {
    const double warm = temperature / 100.0;
    const double magenta = tint / 100.0;
    return {
        1.0 + 0.35 * warm + 0.15 * magenta,
        1.0 - 0.30 * magenta,
        1.0 - 0.35 * warm + 0.15 * magenta
    };
}

std::optional<std::pair<double, double>> CameraRawSettings::neutralize(double linearRed, double green, double blue) {
    if (linearRed <= 1e-4 || green <= 1e-4 || blue <= 1e-4) return std::nullopt;
    const double a1 = 0.35 * linearRed;
    const double b1 = 0.15 * linearRed + 0.30 * green;
    const double c1 = green - linearRed;
    const double a2 = -0.35 * blue;
    const double b2 = 0.15 * blue + 0.30 * green;
    const double c2 = green - blue;
    const double det = a1 * b2 - a2 * b1;
    if (std::abs(det) <= 1e-8) return std::nullopt;
    const double warm = (c1 * b2 - c2 * b1) / det;
    const double magenta = (a1 * c2 - a2 * c1) / det;
    if (!std::isfinite(warm) || !std::isfinite(magenta)) return std::nullopt;
    return std::make_pair(warm * 100.0, magenta * 100.0);
}

std::optional<std::pair<double, double>> CameraRawSettings::neutralizeStraight(double straightRed, double green, double blue) {
    return neutralize(srgbDecode(straightRed), srgbDecode(green), srgbDecode(blue));
}

std::optional<std::pair<double, double>> CameraRawSettings::autoBalance(const QImage &image) {
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) return std::nullopt;
    const QImage converted = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    double redSum = 0.0, greenSum = 0.0, blueSum = 0.0, count = 0.0;
    for (int y = 0; y < converted.height(); ++y) {
        const QRgb *scan = reinterpret_cast<const QRgb *>(converted.constScanLine(y));
        for (int x = 0; x < converted.width(); ++x) {
            const QRgb pixel = scan[x];
            const double alpha = qAlpha(pixel);
            if (alpha == 0.0) continue;
            redSum += srgbDecode(std::min(1.0, qRed(pixel) / alpha));
            greenSum += srgbDecode(std::min(1.0, qGreen(pixel) / alpha));
            blueSum += srgbDecode(std::min(1.0, qBlue(pixel) / alpha));
            count += 1.0;
        }
    }
    if (count <= 0.0) return std::nullopt;
    return neutralize(redSum / count, greenSum / count, blueSum / count);
}

double CameraRawScope::peak() const {
    double maxVal = 0.0;
    for (int i = 0; i < binCount; ++i) {
        maxVal = std::max({maxVal, red[size_t(i)], green[size_t(i)], blue[size_t(i)]});
    }
    return maxVal;
}

std::optional<CameraRawScope> CameraRawScope::make(const QImage &image) {
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) return std::nullopt;
    CameraRawScope scope;
    const QImage conv = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);

    for (int y = 0; y < conv.height(); ++y) {
        const quint8 *p = conv.constScanLine(y);
        for (int x = 0; x < conv.width(); ++x, p += 4) {
            const double alpha = p[3];
            if (alpha == 0.0) continue;
            const double r = std::min(1.0, p[0] / alpha);
            const double g = std::min(1.0, p[1] / alpha);
            const double b = std::min(1.0, p[2] / alpha);

            const int rBin = std::clamp(int(r * 255.0 + 0.5), 0, binCount - 1);
            const int gBin = std::clamp(int(g * 255.0 + 0.5), 0, binCount - 1);
            const int bBin = std::clamp(int(b * 255.0 + 0.5), 0, binCount - 1);
            scope.red[size_t(rBin)] += 1.0;
            scope.green[size_t(gBin)] += 1.0;
            scope.blue[size_t(bBin)] += 1.0;

            const double maxCh = std::max({r, g, b});
            const double minCh = std::min({r, g, b});
            const double chroma = maxCh - minCh;
            if (chroma > 1e-4 && maxCh > 1e-4) {
                double hue = 0.0;
                if (maxCh == r) hue = (g - b) / chroma;
                else if (maxCh == g) hue = 2.0 + (b - r) / chroma;
                else hue = 4.0 + (r - g) / chroma;
                hue /= 6.0;
                if (hue < 0.0) hue += 1.0;

                const double angle = hue * 2.0 * M_PI;
                const double saturation = chroma / maxCh;
                const double plotX = 0.5 + std::cos(angle) * saturation * 0.48;
                const double plotY = 0.5 + std::sin(angle) * saturation * 0.48;
                const int col = std::clamp(int(plotX * scopeSide), 0, scopeSide - 1);
                const int row = std::clamp(int(plotY * scopeSide), 0, scopeSide - 1);
                scope.vectorscope[size_t(row * scopeSide + col)] += alpha / 255.0;
            }
        }
    }
    return scope;
}

QImage CameraRawScope::overlay(const QImage &image, bool shadows, bool highlights) {
    if (!shadows && !highlights) return image;
    QImage result = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    adjust_camera_raw_clip_overlay(result.bits(), result.width(), result.height(), result.bytesPerLine(),
                                   shadows ? 1 : 0, highlights ? 1 : 0);
    return result;
}

} // namespace compositor
