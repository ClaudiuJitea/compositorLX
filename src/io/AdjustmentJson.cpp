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
    // The filter layers' own objects, each key in place with its filter's default (mac encodes every member).
    if (result.value(QStringLiteral("filterLayerSettings")).isObject()) {
        QJsonObject f = result.value(QStringLiteral("filterLayerSettings")).toObject();
        const auto completeIn = [&](const QString &key, const QJsonObject &defaults) {
            if (f.contains(key)) f.insert(key, filled(f.value(key).toObject(), defaults));
        };
        completeIn(QStringLiteral("vignette"), {{QStringLiteral("amount"), 35.0}, {QStringLiteral("color"), color(0, 0, 0)},
            {QStringLiteral("midpoint"), 50.0}, {QStringLiteral("roundness"), 100.0}, {QStringLiteral("feather"), 60.0}, {QStringLiteral("highlights"), 25.0}});
        if (f.contains(QStringLiteral("vignette"))) {
            QJsonObject v = f.value(QStringLiteral("vignette")).toObject();
            v.insert(QStringLiteral("color"), filled(v.value(QStringLiteral("color")).toObject(), color(0, 0, 0)));
            f.insert(QStringLiteral("vignette"), v);
        }
        completeIn(QStringLiteral("tonal"), {{QStringLiteral("amount"), 50.0}, {QStringLiteral("radius"), 16.0}, {QStringLiteral("shadows"), 40.0},
            {QStringLiteral("midtones"), 60.0}, {QStringLiteral("highlights"), 30.0}});
        completeIn(QStringLiteral("sharpen"), {{QStringLiteral("amount"), 100.0}, {QStringLiteral("radius"), 1.0}});
        completeIn(QStringLiteral("reduceNoise"), {{QStringLiteral("strength"), 50.0}, {QStringLiteral("details"), 50.0}, {QStringLiteral("color"), 50.0}});
        completeIn(QStringLiteral("lens"), lensSettingsJson(LensEffectSettings()));
        result.insert(QStringLiteral("filterLayerSettings"), f);
    }
    return result;
}

QJsonObject lensSettingsJson(const LensEffectSettings &settings)
{
    return {{QStringLiteral("effect"), LensEffectSettings::effectName(settings.effect)}, {QStringLiteral("threshold"), settings.threshold},
            {QStringLiteral("flareAmount"), settings.flareAmount}, {QStringLiteral("flareLength"), settings.flareLength},
            {QStringLiteral("flareBlue"), settings.flareBlue}, {QStringLiteral("starAmount"), settings.starAmount},
            {QStringLiteral("starLength"), settings.starLength}, {QStringLiteral("starPoints"), settings.starPoints},
            {QStringLiteral("starAngle"), settings.starAngle}, {QStringLiteral("mistStrength"), settings.mistStrength},
            {QStringLiteral("mistSize"), settings.mistSize}, {QStringLiteral("prismAmount"), settings.prismAmount},
            {QStringLiteral("prismCopies"), settings.prismCopies}, {QStringLiteral("prismOffset"), settings.prismOffset},
            {QStringLiteral("prismAngle"), settings.prismAngle}, {QStringLiteral("prismBothWays"), settings.prismBothWays},
            {QStringLiteral("prismBlur"), settings.prismBlur}, {QStringLiteral("prismFringe"), settings.prismFringe},
            {QStringLiteral("bokehRadius"), settings.bokehRadius}, {QStringLiteral("bokehBrightness"), settings.bokehBrightness},
            {QStringLiteral("bokehRing"), settings.bokehRing}, {QStringLiteral("bokehShape"), LensEffectSettings::shapeName(settings.bokehShape)}};
}

LensEffectSettings lensSettingsFromJson(const QJsonObject &object)
{
    LensEffectSettings s;
    const auto read = [&](const char *key, double &into) { if (object.value(QLatin1String(key)).isDouble()) into = object.value(QLatin1String(key)).toDouble(); };
    for (LensEffect effect : LensEffectSettings::effects)
        if (object.value(QStringLiteral("effect")).toString() == LensEffectSettings::effectName(effect)) s.effect = effect;
    for (BokehShape shape : LensEffectSettings::shapes)
        if (object.value(QStringLiteral("bokehShape")).toString() == LensEffectSettings::shapeName(shape)) s.bokehShape = shape;
    read("threshold", s.threshold); read("flareAmount", s.flareAmount); read("flareLength", s.flareLength); read("flareBlue", s.flareBlue);
    read("starAmount", s.starAmount); read("starLength", s.starLength); read("starAngle", s.starAngle);
    if (object.value(QStringLiteral("starPoints")).isDouble()) s.starPoints = object.value(QStringLiteral("starPoints")).toInt(6);
    read("mistStrength", s.mistStrength); read("mistSize", s.mistSize); read("prismAmount", s.prismAmount); read("prismCopies", s.prismCopies);
    read("prismOffset", s.prismOffset); read("prismAngle", s.prismAngle); read("prismBlur", s.prismBlur); read("prismFringe", s.prismFringe);
    if (object.value(QStringLiteral("prismBothWays")).isBool()) s.prismBothWays = object.value(QStringLiteral("prismBothWays")).toBool();
    read("bokehRadius", s.bokehRadius); read("bokehBrightness", s.bokehBrightness); read("bokehRing", s.bokehRing);
    return s;
}

QString adjustmentJsonError(const QJsonObject &adjustment)
{
    if (adjustment.contains("filterLayerSettings")) {
        if (!adjustment.value("filterLayerSettings").isObject()) return QStringLiteral("Invalid filter layer settings");
        const auto f = adjustment.value("filterLayerSettings").toObject();
        struct Range { const char *key; double lo, hi; };
        for (const auto &r : {Range{"progressiveFrom", 0, 250}, Range{"progressiveTo", 0, 250},
            Range{"bloomAmount", 0, 100}, Range{"bloomRadius", 1, 150}, Range{"rgbSplitDistance", 0, 200},
            Range{"rgbSplitBlur", 0, 400}, Range{"rgbSplitAngle", -180, 180}, Range{"rgbSplitReach", 1, 1000000}}) {
            const auto v = f.value(QLatin1String(r.key));
            if (!v.isUndefined() && (!finite(v) || v.toDouble() < r.lo || v.toDouble() > r.hi)) return QStringLiteral("Filter layer parameter out of range: %1").arg(QLatin1String(r.key));
        }
        for (const char *key : {"progressiveStart", "progressiveEnd", "rgbSplitCenter"}) if (f.contains(QLatin1String(key))) {
            const auto p = f.value(QLatin1String(key)).toArray();
            if (p.size() != 2 || !finite(p[0]) || !finite(p[1]) || std::abs(p[0].toDouble()) > 1000000 || std::abs(p[1].toDouble()) > 1000000)
                return QStringLiteral("Invalid filter layer point");
        }
        for (const char *key : {"progressiveRadial", "progressiveInverted", "rgbSplitRadial"})
            if (f.contains(QLatin1String(key)) && !f.value(QLatin1String(key)).isBool()) return QStringLiteral("Invalid filter layer toggle");
        // The objects of versions 18, 19 and 21 (mac VignetteLayerSettings, TonalLayerSettings, SharpenLayerSettings,
        // ReduceNoiseLayerSettings): synthesized Codable, so every key is required, each within its filter's range.
        // Lens Effects reads each setting it finds and defaults the rest (LensEffectSettings.init(from:)).
        const auto object = [&](const char *name, std::initializer_list<Range> ranges) -> QString {
            if (!f.contains(QLatin1String(name))) return {};
            if (!f.value(QLatin1String(name)).isObject()) return QStringLiteral("Invalid %1 settings").arg(QLatin1String(name));
            const QJsonObject o = f.value(QLatin1String(name)).toObject();
            for (const Range &r : ranges) {
                const QJsonValue v = o.value(QLatin1String(r.key));
                if (!finite(v) || v.toDouble() < r.lo || v.toDouble() > r.hi)
                    return QStringLiteral("Filter layer parameter out of range: %1.%2").arg(QLatin1String(name), QLatin1String(r.key));
            }
            return {};
        };
        for (const QString &problem : {
                 object("vignette", {{"amount", 0, 100}, {"midpoint", 0, 100}, {"roundness", -100, 100}, {"feather", 0, 100}, {"highlights", 0, 100}}),
                 object("tonal", {{"amount", 0, 100}, {"radius", 1, 100}, {"shadows", -100, 100}, {"midtones", -100, 100}, {"highlights", -100, 100}}),
                 object("sharpen", {{"amount", 0, 300}, {"radius", 0.1, 20}}),
                 object("reduceNoise", {{"strength", 0, 100}, {"details", 0, 100}, {"color", 0, 100}})})
            if (!problem.isEmpty()) return problem;
        if (f.contains(QStringLiteral("vignette"))) {
            const QJsonObject c = f.value(QStringLiteral("vignette")).toObject().value(QStringLiteral("color")).toObject();
            for (const char *channel : {"red", "green", "blue"}) {
                const QJsonValue v = c.value(QLatin1String(channel));
                if (!finite(v) || v.toDouble() < 0 || v.toDouble() > 1) return QStringLiteral("Vignette color out of range");
            }
        }
        if (f.contains(QStringLiteral("lens"))) {
            if (!f.value(QStringLiteral("lens")).isObject()) return QStringLiteral("Invalid lens settings");
            const QJsonObject lens = f.value(QStringLiteral("lens")).toObject();
            for (const Range &r : {Range{"threshold", 50, 100}, Range{"flareAmount", 0, 100}, Range{"flareLength", 1, 100},
                     Range{"flareBlue", 0, 100}, Range{"starAmount", 0, 100}, Range{"starLength", 2, 40}, Range{"starAngle", -180, 180},
                     Range{"mistStrength", 0, 100}, Range{"mistSize", 0, 100}, Range{"prismAmount", 0, 100}, Range{"prismCopies", 1, 8},
                     Range{"prismOffset", 0, 50}, Range{"prismAngle", -180, 180}, Range{"prismBlur", 0, 100}, Range{"prismFringe", 0, 100},
                     Range{"bokehRadius", 0.5, 6}, Range{"bokehBrightness", 0, 100}, Range{"bokehRing", 0, 100}}) {
                const QJsonValue v = lens.value(QLatin1String(r.key));
                if (!v.isUndefined() && (!finite(v) || v.toDouble() < r.lo || v.toDouble() > r.hi))
                    return QStringLiteral("Lens Effects parameter out of range: %1").arg(QLatin1String(r.key));
            }
            if (lens.contains(QStringLiteral("prismCopies")) && std::round(lens.value(QStringLiteral("prismCopies")).toDouble()) != lens.value(QStringLiteral("prismCopies")).toDouble())
                return QStringLiteral("Lens Effects prismCopies must be whole");
            if (lens.contains(QStringLiteral("starPoints"))) {
                const QJsonValue v = lens.value(QStringLiteral("starPoints"));
                const double points = v.toDouble(-1);
                if (!v.isDouble() || (points != 4 && points != 6 && points != 8 && points != 12 && points != 14))
                    return QStringLiteral("Lens Effects starPoints must be 4, 6, 8, 12 or 14");
            }
            if (lens.contains(QStringLiteral("prismBothWays")) && !lens.value(QStringLiteral("prismBothWays")).isBool())
                return QStringLiteral("Invalid Lens Effects toggle");
            static const QStringList effects{QStringLiteral("Anamorphic Flare"), QStringLiteral("Starburst"), QStringLiteral("Mist"),
                                             QStringLiteral("Prism"), QStringLiteral("Bokeh")};
            static const QStringList shapes{QStringLiteral("Round"), QStringLiteral("Hexagon"), QStringLiteral("Oval"), QStringLiteral("Cat's Eye")};
            if (lens.contains(QStringLiteral("effect")) && !effects.contains(lens.value(QStringLiteral("effect")).toString()))
                return QStringLiteral("Unknown Lens Effects effect");
            if (lens.contains(QStringLiteral("bokehShape")) && !shapes.contains(lens.value(QStringLiteral("bokehShape")).toString()))
                return QStringLiteral("Unknown Lens Effects bokeh shape");
        }
    }
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
