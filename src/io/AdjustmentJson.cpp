#include "io/AdjustmentJson.h"

#include <QJsonArray>

#include <cmath>

namespace compositor {
namespace {

QJsonObject identityLevelRange()
{
    return {{QStringLiteral("black"), 0.0}, {QStringLiteral("gamma"), 1.0}, {QStringLiteral("white"), 255.0},
            {QStringLiteral("outputBlack"), 0.0}, {QStringLiteral("outputWhite"), 255.0}};
}

QJsonObject identityLevels()
{
    QJsonArray ranges;
    for (int i = 0; i < 4; ++i) ranges.append(identityLevelRange());
    return {{QStringLiteral("channel"), QStringLiteral("RGB")}, {QStringLiteral("ranges"), ranges}};
}

QJsonArray identityCurve()
{
    return {QJsonObject{{QStringLiteral("x"), 0.0}, {QStringLiteral("y"), 0.0}},
            QJsonObject{{QStringLiteral("x"), 255.0}, {QStringLiteral("y"), 255.0}}};
}

QJsonObject identityCurves()
{
    QJsonArray channels;
    for (int i = 0; i < 4; ++i) channels.append(identityCurve());
    return {{QStringLiteral("channel"), QStringLiteral("RGB")}, {QStringLiteral("channels"), channels}};
}

// `object` with every key of `defaults` it lacks.
QJsonObject filled(QJsonObject object, const QJsonObject &defaults)
{
    for (auto it = defaults.begin(); it != defaults.end(); ++it)
        if (!object.contains(it.key())) object.insert(it.key(), it.value());
    return object;
}

QJsonObject color(double r, double g, double b)
{
    return {{QStringLiteral("red"), r}, {QStringLiteral("green"), g}, {QStringLiteral("blue"), b}};
}

bool finite(const QJsonValue &value) { return value.isDouble() && std::isfinite(value.toDouble()); }

} // namespace

QJsonObject completedAdjustmentJson(const QJsonObject &adjustment)
{
    if (adjustment.isEmpty()) return adjustment;
    QJsonObject result = adjustment;
    for (const char *key : {"hue", "saturation", "lightness"})
        if (!result.contains(QLatin1String(key))) result.insert(QLatin1String(key), 0.0);
    if (!result.contains(QStringLiteral("colorize"))) result.insert(QStringLiteral("colorize"), false);

    // Levels: four ranges, RGB then red, green, blue; each range needs all five numbers.
    QJsonObject levels = filled(result.value(QStringLiteral("levels")).toObject(), identityLevels());
    QJsonArray ranges = levels.value(QStringLiteral("ranges")).toArray();
    while (ranges.size() < 4) ranges.append(identityLevelRange());
    for (int i = 0; i < ranges.size(); ++i) ranges[i] = filled(ranges.at(i).toObject(), identityLevelRange());
    levels.insert(QStringLiteral("ranges"), ranges);
    result.insert(QStringLiteral("levels"), levels);

    QJsonObject curves = filled(result.value(QStringLiteral("curves")).toObject(), identityCurves());
    QJsonArray channels = curves.value(QStringLiteral("channels")).toArray();
    while (channels.size() < 4) channels.append(identityCurve());
    curves.insert(QStringLiteral("channels"), channels);
    result.insert(QStringLiteral("curves"), curves);

    const auto complete = [&](const QString &key, const QJsonObject &defaults) {
        if (result.contains(key)) result.insert(key, filled(result.value(key).toObject(), defaults));
    };
    complete(QStringLiteral("exposureSettings"), {{QStringLiteral("exposure"), 0.0}, {QStringLiteral("offset"), 0.0}, {QStringLiteral("gamma"), 1.0}});
    complete(QStringLiteral("grainSettings"), {{QStringLiteral("amount"), 25.0}, {QStringLiteral("size"), 1.5},
                                               {QStringLiteral("roughness"), 50.0}, {QStringLiteral("seed"), 0}});
    complete(QStringLiteral("blackWhiteSettings"), {{QStringLiteral("reds"), 40.0}, {QStringLiteral("yellows"), 60.0},
        {QStringLiteral("greens"), 40.0}, {QStringLiteral("cyans"), 60.0}, {QStringLiteral("blues"), 20.0},
        {QStringLiteral("magentas"), 80.0}, {QStringLiteral("tint"), false}, {QStringLiteral("tintHue"), 40.0},
        {QStringLiteral("tintSaturation"), 20.0}});
    complete(QStringLiteral("colorBalanceSettings"), {{QStringLiteral("shadowCyanRed"), 0.0}, {QStringLiteral("shadowMagentaGreen"), 0.0},
        {QStringLiteral("shadowYellowBlue"), 0.0}, {QStringLiteral("midCyanRed"), 0.0}, {QStringLiteral("midMagentaGreen"), 0.0},
        {QStringLiteral("midYellowBlue"), 0.0}, {QStringLiteral("highlightCyanRed"), 0.0}, {QStringLiteral("highlightMagentaGreen"), 0.0},
        {QStringLiteral("highlightYellowBlue"), 0.0}, {QStringLiteral("preserveLuminosity"), true}});
    if (result.contains(QStringLiteral("gradientMapSettings"))) {
        QJsonObject map = filled(result.value(QStringLiteral("gradientMapSettings")).toObject(),
            {{QStringLiteral("shadows"), color(0, 0, 0)}, {QStringLiteral("highlights"), color(1, 1, 1)}, {QStringLiteral("reversed"), false}});
        for (const QString &end : {QStringLiteral("shadows"), QStringLiteral("highlights")})
            map.insert(end, filled(map.value(end).toObject(), color(end == QStringLiteral("shadows") ? 0 : 1, end == QStringLiteral("shadows") ? 0 : 1, end == QStringLiteral("shadows") ? 0 : 1)));
        result.insert(QStringLiteral("gradientMapSettings"), map);
    }
    return result;
}

QString adjustmentJsonError(const QJsonObject &adjustment)
{
    const QJsonObject value = completedAdjustmentJson(adjustment);
    const auto number = [&](const QJsonObject &object, const char *key) { return object.value(QLatin1String(key)).toDouble(); };
    const auto inRange = [](double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; };

    const QJsonArray ranges = value.value(QStringLiteral("levels")).toObject().value(QStringLiteral("ranges")).toArray();
    if (ranges.size() != 4) return QStringLiteral("Levels needs four channel ranges");
    for (const QJsonValue &entry : ranges) {
        const QJsonObject r = entry.toObject();
        const double black = number(r, "black"), white = number(r, "white"), gamma = number(r, "gamma");
        if (!inRange(black, 0, 254) || !inRange(white, black + 1, 255) || !inRange(gamma, 0.1, 9.99)
            || !inRange(number(r, "outputBlack"), 0, 255) || !inRange(number(r, "outputWhite"), 0, 255))
            return QStringLiteral("Levels range out of bounds");
    }
    const QJsonArray channels = value.value(QStringLiteral("curves")).toObject().value(QStringLiteral("channels")).toArray();
    if (channels.size() != 4) return QStringLiteral("Curves needs four channels");
    for (const QJsonValue &channel : channels) {
        const QJsonArray points = channel.toArray();
        if (points.size() < 2 || points.size() > 32) return QStringLiteral("a curve needs 2 to 32 points");
        double previous = -1;
        for (int i = 0; i < points.size(); ++i) {
            const QJsonObject p = points.at(i).toObject();
            const double x = number(p, "x"), y = number(p, "y");
            if (!finite(p.value(QStringLiteral("x"))) || !finite(p.value(QStringLiteral("y"))) || !inRange(x, 0, 255) || !inRange(y, 0, 255)
                || x <= previous || (i == 0 && x != 0) || (i == points.size() - 1 && x != 255))
                return QStringLiteral("curve points out of order or bounds");
            previous = x;
        }
    }
    if (const QJsonObject e = value.value(QStringLiteral("exposureSettings")).toObject(); !e.isEmpty()
        && (!inRange(number(e, "exposure"), -20, 20) || !inRange(number(e, "offset"), -0.5, 0.5) || !inRange(number(e, "gamma"), 0.01, 9.99)))
        return QStringLiteral("Exposure out of range");
    if (const QJsonObject g = value.value(QStringLiteral("grainSettings")).toObject(); !g.isEmpty()
        && (!inRange(number(g, "amount"), 0, 100) || !inRange(number(g, "size"), 0.5, 20) || !inRange(number(g, "roughness"), 0, 100)))
        return QStringLiteral("Grain out of range");
    if (const QJsonObject m = value.value(QStringLiteral("gradientMapSettings")).toObject(); !m.isEmpty())
        for (const QString &end : {QStringLiteral("shadows"), QStringLiteral("highlights")}) {
            const QJsonObject c = m.value(end).toObject();
            for (const char *channel : {"red", "green", "blue"})
                if (!inRange(number(c, channel), 0, 1)) return QStringLiteral("Gradient Map color out of range");
        }
    return {};
}

} // namespace compositor
