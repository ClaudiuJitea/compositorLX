#pragma once

#include "core/DocumentLimits.h"

#include <QImage>
#include <QJsonObject>
#include <QPainter>
#include <QPointF>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QUuid>
#include <QVector>

#include <cmath>
#include <optional>

namespace compositor {

enum class Sampling { Nearest, Smooth, HighQuality };

enum class BlendMode {
    Normal,
    // Darkening
    Darken,
    Multiply,
    ColorBurn,
    LinearBurn,
    // Lightening
    Lighten,
    Screen,
    ColorDodge,
    LinearDodge, // "Linear Dodge (Add)"
    // Contrast
    Overlay,
    SoftLight,
    HardLight,
    VividLight,
    LinearLight,
    PinLight,
    HardMix,
    // Inversion / Comparative
    Difference,
    Exclusion,
    Subtract,
    Divide,
    // Component
    Hue,
    Saturation,
    Color,
    Luminosity
};

struct CanvasGuide {
    enum class Axis { Horizontal, Vertical };

    QUuid id;
    Axis axis = Axis::Horizontal;
    double position = 0.0; // document pixels

    [[nodiscard]] bool isValid() const {
        return !id.isNull() && std::isfinite(position) && std::abs(position) <= 1000000.0;
    }
    bool operator==(const CanvasGuide &) const = default;

    [[nodiscard]] CanvasGuide offset(double x, double y) const {
        CanvasGuide g = *this;
        g.position += (axis == Axis::Vertical ? x : y);
        return g;
    }

    [[nodiscard]] CanvasGuide scaled(double x, double y) const {
        CanvasGuide g = *this;
        g.position *= (axis == Axis::Vertical ? x : y);
        return g;
    }

    [[nodiscard]] CanvasGuide mirrored(bool horizontally, double center) const {
        CanvasGuide g = *this;
        if ((horizontally && axis == Axis::Vertical) || (!horizontally && axis == Axis::Horizontal)) {
            g.position = 2.0 * center - g.position;
        }
        return g;
    }
};

// Non-printing layout grid: a major line every `spacing` px, split into `subdivisions` (64 px and eight, every 8 px,
// until changed in View > Grid Settings...). Mac 1c819d0.
struct LayoutGrid {
    static constexpr int minSpacing = 2, maxSpacing = 4096, minSubdivisions = 1, maxSubdivisions = 64;
    int spacing = 64;
    int subdivisions = 8;

    LayoutGrid() = default;
    LayoutGrid(int spacingValue, int subdivisionsValue)
        : spacing(std::clamp(spacingValue, minSpacing, maxSpacing)),
          subdivisions(std::clamp(subdivisionsValue, minSubdivisions, std::min(maxSubdivisions, std::clamp(spacingValue, minSpacing, maxSpacing)))) {}

    [[nodiscard]] double step() const { return double(spacing) / double(subdivisions); }

    // Every grid line along a document edge, including subdivisions, in whole pixels. Counted from the origin rather
    // than added up, so an uneven step doesn't drift off the majors.
    [[nodiscard]] QVector<double> lines(double length) const {
        if (length < 0.0) return {0.0};
        QVector<double> result;
        const int count = int(std::floor(length / step() + 0.001));
        result.reserve(count + 1);
        for (int i = 0; i <= count; ++i) result.push_back(std::round(i * step()));
        return result;
    }

    [[nodiscard]] bool isMajor(double value) const {
        return std::abs(std::fmod(std::round(value), double(spacing))) < 0.001;
    }
    bool operator==(const LayoutGrid &) const = default;
};

// How the layout grid is drawn (View > Grid Settings...), after Photoshop's Guides, Grid & Slices settings. Lines are
// drawn at the chosen opacity; subdivisions are dotted and fainter still.
struct GridAppearance {
    enum class Preset { LightGray, LightBlue, LightRed, Green, MediumBlue, Yellow, Magenta, Cyan, Black, Custom };
    enum class Style { Lines, DashedLines, Dots };
    static constexpr int minOpacity = 1, maxOpacity = 100;

    Preset preset = Preset::LightGray;
    // Used while `preset` is Custom; kept when another preset is chosen, so switching back finds it.
    QColor customColor = QColor::fromRgbF(0.7, 0.7, 0.7);
    Style style = Style::Lines;
    int opacity = 45;   // percent, of the major lines

    [[nodiscard]] static QString presetName(Preset p) {
        static const char *names[] = {"Light Gray", "Light Blue", "Light Red", "Green", "Medium Blue", "Yellow", "Magenta", "Cyan", "Black", "Custom"};
        return QString::fromLatin1(names[int(p)]);
    }
    [[nodiscard]] static QString styleName(Style st) {
        static const char *names[] = {"Lines", "Dashed Lines", "Dots"};
        return QString::fromLatin1(names[int(st)]);
    }
    [[nodiscard]] QColor color() const {
        switch (preset) {
        case Preset::LightGray: return QColor::fromRgbF(0.7, 0.7, 0.7);
        case Preset::LightBlue: return QColor::fromRgbF(0.29, 0.78, 1.0);
        case Preset::LightRed: return QColor::fromRgbF(1.0, 0.4, 0.4);
        case Preset::Green: return QColor::fromRgbF(0.25, 0.8, 0.25);
        case Preset::MediumBlue: return QColor::fromRgbF(0.2, 0.4, 1.0);
        case Preset::Yellow: return QColor::fromRgbF(1.0, 1.0, 0.0);
        case Preset::Magenta: return QColor::fromRgbF(1.0, 0.0, 1.0);
        case Preset::Cyan: return QColor::fromRgbF(0.0, 1.0, 1.0);
        case Preset::Black: return QColor::fromRgbF(0.0, 0.0, 0.0);
        case Preset::Custom: return customColor;
        }
        return customColor;
    }
    // On and off lengths in screen points; empty for a solid line.
    [[nodiscard]] QVector<qreal> dashes() const {
        switch (style) {
        case Style::Lines: return {};
        case Style::DashedLines: return {4.0, 3.0};
        case Style::Dots: return {1.0, 2.0};
        }
        return {};
    }
    [[nodiscard]] double majorAlpha() const { return double(std::clamp(opacity, minOpacity, maxOpacity)) / 100.0; }
    // Subdivisions at a little over half the majors' opacity: 28% beside the default 45%.
    [[nodiscard]] double subdivisionAlpha() const { return majorAlpha() * 28.0 / 45.0; }
    bool operator==(const GridAppearance &) const = default;
};

enum class LayerEffectKind {
    Stroke,
    DropShadow,
    ColorOverlay,
    InnerShadow,
    OuterGlow,
    InnerGlow
};

QString layerEffectKindToString(LayerEffectKind kind);
std::optional<LayerEffectKind> layerEffectKindFromString(const QString &str);

struct StrokeEffect {
    std::optional<bool> enabled;
    double size = 4.0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double opacity = 1.0;
    bool inside = false;

    [[nodiscard]] bool isEnabled() const { return enabled.value_or(true); }
    [[nodiscard]] bool isValid() const {
        return std::isfinite(size) && size >= 0.0 && size <= 500.0
            && std::isfinite(opacity) && opacity >= 0.0 && opacity <= 1.0
            && std::isfinite(red) && red >= 0.0 && red <= 1.0
            && std::isfinite(green) && green >= 0.0 && green <= 1.0
            && std::isfinite(blue) && blue >= 0.0 && blue <= 1.0;
    }
    bool operator==(const StrokeEffect &) const = default;
};

struct ShadowEffect {
    std::optional<bool> enabled;
    double angle = 90.0;
    double distance = 20.0;
    double blur = 20.0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double opacity = 0.5;

    [[nodiscard]] bool isEnabled() const { return enabled.value_or(true); }
    [[nodiscard]] QPointF offset() const {
        const double radians = angle * M_PI / 180.0;
        return QPointF(-std::cos(radians) * distance, std::sin(radians) * distance);
    }
    [[nodiscard]] bool isValid() const {
        return std::isfinite(angle) && angle >= -360.0 && angle <= 360.0
            && std::isfinite(distance) && distance >= 0.0 && distance <= 5000.0
            && std::isfinite(blur) && blur >= 0.0 && blur <= 500.0
            && std::isfinite(opacity) && opacity >= 0.0 && opacity <= 1.0
            && std::isfinite(red) && red >= 0.0 && red <= 1.0
            && std::isfinite(green) && green >= 0.0 && green <= 1.0
            && std::isfinite(blue) && blue >= 0.0 && blue <= 1.0;
    }
    bool operator==(const ShadowEffect &) const = default;
};

struct ColorOverlayEffect {
    std::optional<bool> enabled;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double opacity = 1.0;

    [[nodiscard]] bool isEnabled() const { return enabled.value_or(true); }
    [[nodiscard]] bool isValid() const {
        return std::isfinite(opacity) && opacity >= 0.0 && opacity <= 1.0
            && std::isfinite(red) && red >= 0.0 && red <= 1.0
            && std::isfinite(green) && green >= 0.0 && green <= 1.0
            && std::isfinite(blue) && blue >= 0.0 && blue <= 1.0;
    }
    bool operator==(const ColorOverlayEffect &) const = default;
};

struct InnerShadowEffect {
    std::optional<bool> enabled;
    double angle = 90.0;
    double distance = 10.0;
    double blur = 10.0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double opacity = 0.5;

    [[nodiscard]] bool isEnabled() const { return enabled.value_or(true); }
    [[nodiscard]] QPointF offset() const {
        const double radians = angle * M_PI / 180.0;
        return QPointF(-std::cos(radians) * distance, std::sin(radians) * distance);
    }
    [[nodiscard]] bool isValid() const {
        return std::isfinite(angle) && angle >= -360.0 && angle <= 360.0
            && std::isfinite(distance) && distance >= 0.0 && distance <= 5000.0
            && std::isfinite(blur) && blur >= 0.0 && blur <= 500.0
            && std::isfinite(opacity) && opacity >= 0.0 && opacity <= 1.0
            && std::isfinite(red) && red >= 0.0 && red <= 1.0
            && std::isfinite(green) && green >= 0.0 && green <= 1.0
            && std::isfinite(blue) && blue >= 0.0 && blue <= 1.0;
    }
    bool operator==(const InnerShadowEffect &) const = default;
};

struct OuterGlowEffect {
    std::optional<bool> enabled;
    double size = 20.0;
    double red = 1.0;
    double green = 1.0;
    double blue = 1.0;
    double opacity = 0.75;

    [[nodiscard]] bool isEnabled() const { return enabled.value_or(true); }
    [[nodiscard]] bool isValid() const {
        return std::isfinite(size) && size >= 0.0 && size <= 500.0
            && std::isfinite(opacity) && opacity >= 0.0 && opacity <= 1.0
            && std::isfinite(red) && red >= 0.0 && red <= 1.0
            && std::isfinite(green) && green >= 0.0 && green <= 1.0
            && std::isfinite(blue) && blue >= 0.0 && blue <= 1.0;
    }
    bool operator==(const OuterGlowEffect &) const = default;
};

struct InnerGlowEffect {
    std::optional<bool> enabled;
    double size = 10.0;
    double red = 1.0;
    double green = 1.0;
    double blue = 1.0;
    double opacity = 0.75;

    [[nodiscard]] bool isEnabled() const { return enabled.value_or(true); }
    [[nodiscard]] bool isValid() const {
        return std::isfinite(size) && size >= 0.0 && size <= 500.0
            && std::isfinite(opacity) && opacity >= 0.0 && opacity <= 1.0
            && std::isfinite(red) && red >= 0.0 && red <= 1.0
            && std::isfinite(green) && green >= 0.0 && green <= 1.0
            && std::isfinite(blue) && blue >= 0.0 && blue <= 1.0;
    }
    bool operator==(const InnerGlowEffect &) const = default;
};

struct LayerEffects {
    std::optional<StrokeEffect> stroke;
    std::optional<ShadowEffect> shadow;
    std::optional<ColorOverlayEffect> colorOverlay;
    std::optional<InnerShadowEffect> innerShadow;
    std::optional<OuterGlowEffect> outerGlow;
    std::optional<InnerGlowEffect> innerGlow;

    [[nodiscard]] bool isEmpty() const {
        return !stroke && !shadow && !colorOverlay && !innerShadow && !outerGlow && !innerGlow;
    }
    [[nodiscard]] bool isValid() const {
        return (!stroke || stroke->isValid())
            && (!shadow || shadow->isValid())
            && (!colorOverlay || colorOverlay->isValid())
            && (!innerShadow || innerShadow->isValid())
            && (!outerGlow || outerGlow->isValid())
            && (!innerGlow || innerGlow->isValid());
    }
    [[nodiscard]] bool contains(LayerEffectKind kind) const {
        switch (kind) {
        case LayerEffectKind::Stroke: return stroke.has_value();
        case LayerEffectKind::DropShadow: return shadow.has_value();
        case LayerEffectKind::ColorOverlay: return colorOverlay.has_value();
        case LayerEffectKind::InnerShadow: return innerShadow.has_value();
        case LayerEffectKind::OuterGlow: return outerGlow.has_value();
        case LayerEffectKind::InnerGlow: return innerGlow.has_value();
        }
        return false;
    }
    [[nodiscard]] bool isEnabled(LayerEffectKind kind) const {
        switch (kind) {
        case LayerEffectKind::Stroke: return stroke && stroke->isEnabled();
        case LayerEffectKind::DropShadow: return shadow && shadow->isEnabled();
        case LayerEffectKind::ColorOverlay: return colorOverlay && colorOverlay->isEnabled();
        case LayerEffectKind::InnerShadow: return innerShadow && innerShadow->isEnabled();
        case LayerEffectKind::OuterGlow: return outerGlow && outerGlow->isEnabled();
        case LayerEffectKind::InnerGlow: return innerGlow && innerGlow->isEnabled();
        }
        return false;
    }
    void setEnabled(LayerEffectKind kind, bool enabled) {
        switch (kind) {
        case LayerEffectKind::Stroke: if (stroke) stroke->enabled = enabled; break;
        case LayerEffectKind::DropShadow: if (shadow) shadow->enabled = enabled; break;
        case LayerEffectKind::ColorOverlay: if (colorOverlay) colorOverlay->enabled = enabled; break;
        case LayerEffectKind::InnerShadow: if (innerShadow) innerShadow->enabled = enabled; break;
        case LayerEffectKind::OuterGlow: if (outerGlow) outerGlow->enabled = enabled; break;
        case LayerEffectKind::InnerGlow: if (innerGlow) innerGlow->enabled = enabled; break;
        }
    }
    void remove(LayerEffectKind kind) {
        switch (kind) {
        case LayerEffectKind::Stroke: stroke.reset(); break;
        case LayerEffectKind::DropShadow: shadow.reset(); break;
        case LayerEffectKind::ColorOverlay: colorOverlay.reset(); break;
        case LayerEffectKind::InnerShadow: innerShadow.reset(); break;
        case LayerEffectKind::OuterGlow: outerGlow.reset(); break;
        case LayerEffectKind::InnerGlow: innerGlow.reset(); break;
        }
    }
    [[nodiscard]] QVector<LayerEffectKind> kinds() const {
        QVector<LayerEffectKind> list;
        if (stroke) list.append(LayerEffectKind::Stroke);
        if (shadow) list.append(LayerEffectKind::DropShadow);
        if (colorOverlay) list.append(LayerEffectKind::ColorOverlay);
        if (innerShadow) list.append(LayerEffectKind::InnerShadow);
        if (outerGlow) list.append(LayerEffectKind::OuterGlow);
        if (innerGlow) list.append(LayerEffectKind::InnerGlow);
        return list;
    }
    [[nodiscard]] LayerEffects visible() const {
        LayerEffects v;
        if (stroke && stroke->isEnabled()) v.stroke = stroke;
        if (shadow && shadow->isEnabled()) v.shadow = shadow;
        if (colorOverlay && colorOverlay->isEnabled()) v.colorOverlay = colorOverlay;
        if (innerShadow && innerShadow->isEnabled()) v.innerShadow = innerShadow;
        if (outerGlow && outerGlow->isEnabled()) v.outerGlow = outerGlow;
        if (innerGlow && innerGlow->isEnabled()) v.innerGlow = innerGlow;
        return v;
    }
    [[nodiscard]] std::optional<QColor> color(LayerEffectKind kind) const {
        switch (kind) {
        case LayerEffectKind::Stroke:
            if (stroke) return QColor::fromRgbF(stroke->red, stroke->green, stroke->blue);
            break;
        case LayerEffectKind::DropShadow:
            if (shadow) return QColor::fromRgbF(shadow->red, shadow->green, shadow->blue);
            break;
        case LayerEffectKind::ColorOverlay:
            if (colorOverlay) return QColor::fromRgbF(colorOverlay->red, colorOverlay->green, colorOverlay->blue);
            break;
        case LayerEffectKind::InnerShadow:
            if (innerShadow) return QColor::fromRgbF(innerShadow->red, innerShadow->green, innerShadow->blue);
            break;
        case LayerEffectKind::OuterGlow:
            if (outerGlow) return QColor::fromRgbF(outerGlow->red, outerGlow->green, outerGlow->blue);
            break;
        case LayerEffectKind::InnerGlow:
            if (innerGlow) return QColor::fromRgbF(innerGlow->red, innerGlow->green, innerGlow->blue);
            break;
        }
        return std::nullopt;
    }
    void setColor(LayerEffectKind kind, const QColor &c) {
        const double r = c.redF(), g = c.greenF(), b = c.blueF();
        switch (kind) {
        case LayerEffectKind::Stroke:
            if (stroke) { stroke->red = r; stroke->green = g; stroke->blue = b; }
            break;
        case LayerEffectKind::DropShadow:
            if (shadow) { shadow->red = r; shadow->green = g; shadow->blue = b; }
            break;
        case LayerEffectKind::ColorOverlay:
            if (colorOverlay) { colorOverlay->red = r; colorOverlay->green = g; colorOverlay->blue = b; }
            break;
        case LayerEffectKind::InnerShadow:
            if (innerShadow) { innerShadow->red = r; innerShadow->green = g; innerShadow->blue = b; }
            break;
        case LayerEffectKind::OuterGlow:
            if (outerGlow) { outerGlow->red = r; outerGlow->green = g; outerGlow->blue = b; }
            break;
        case LayerEffectKind::InnerGlow:
            if (innerGlow) { innerGlow->red = r; innerGlow->green = g; innerGlow->blue = b; }
            break;
        }
    }
    bool operator==(const LayerEffects &) const = default;
};

enum class TextAlignment { Left, Center, Right };

// Letters painted in another color than the text's own (format 10+). `location`/`length` count UTF-16 units of
// the content, which is what QString indexes by.
struct TextColorRun {
    int location = 0;
    int length = 0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    bool operator==(const TextColorRun &) const = default;
};

// Letters set in another face than the text's own (format 11+).
struct TextFontRun {
    int location = 0;
    int length = 0;
    QString fontName;
    bool operator==(const TextFontRun &) const = default;
};

struct TextStyle {
    QString content = QStringLiteral("Text");
    QString fontName = QStringLiteral("Helvetica");
    double fontSize = 72.0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    TextAlignment alignment = TextAlignment::Left;
    double tracking = 0.0;
    double leading = 0.0;
    std::optional<QSizeF> boxSize;
    // Sorted, non-overlapping, never empty when present. Nullopt when the whole text is one color / one face.
    std::optional<QVector<TextColorRun>> colorRuns;
    std::optional<QVector<TextFontRun>> fontRuns;

    struct Rgb {
        double r, g, b;
        bool operator==(const Rgb &) const = default;
    };

    // Swift's Character.isNewline: LF, VT, FF, CR, NEL, LS, PS (mac rejects a face name containing any of them).
    [[nodiscard]] static bool hasNewline(const QString &text) {
        for (const QChar c : text) {
            const ushort u = c.unicode();
            if (u == 0x0A || u == 0x0B || u == 0x0C || u == 0x0D || u == 0x85 || u == 0x2028 || u == 0x2029) return true;
        }
        return false;
    }
    [[nodiscard]] bool colorRunsAreValid() const {
        if (!colorRuns) return true;
        if (colorRuns->isEmpty()) return false;
        qint64 end = 0;
        for (const TextColorRun &run : *colorRuns) {
            if (run.location < end || run.length <= 0) return false;
            for (const double c : {run.red, run.green, run.blue}) if (!std::isfinite(c) || c < 0.0 || c > 1.0) return false;
            end = qint64(run.location) + run.length;
        }
        return end <= content.size();
    }
    [[nodiscard]] bool fontRunsAreValid() const {
        if (!fontRuns) return true;
        if (fontRuns->isEmpty()) return false;
        qint64 end = 0;
        for (const TextFontRun &run : *fontRuns) {
            if (run.location < end || run.length <= 0) return false;
            if (run.fontName.isEmpty() || run.fontName.size() > 200 || hasNewline(run.fontName)) return false;
            end = qint64(run.location) + run.length;
        }
        return end <= content.size();
    }
    // The lowest manifest version able to carry this style.
    [[nodiscard]] int requiredFormatVersion() const { return fontRuns ? 11 : colorRuns ? 10 : 1; }

    [[nodiscard]] Rgb colorAt(int index) const {
        if (colorRuns) for (const TextColorRun &run : *colorRuns)
            if (run.location <= index && index < run.location + run.length) return {run.red, run.green, run.blue};
        return {red, green, blue};
    }
    [[nodiscard]] QString fontNameAt(int index) const {
        if (fontRuns) for (const TextFontRun &run : *fontRuns)
            if (run.location <= index && index < run.location + run.length) return run.fontName;
        return fontName;
    }
    // The one face covering [start, start+length), or an empty string when the range is empty or mixed.
    [[nodiscard]] QString uniformFontName(int start, int length) const {
        const int count = content.size();
        start = std::clamp(start, 0, count);
        const int end = std::clamp(start + length, start, count);
        if (end <= start) return {};
        const QString face = fontNameAt(start);
        for (int i = start + 1; i < end; ++i) if (fontNameAt(i) != face) return {};
        return face;
    }

    // Paints a range. An empty range, or one covering the whole text, recolors all of it.
    void setColor(Rgb color, int start, int length) {
        const int count = content.size();
        start = std::clamp(start, 0, count);
        const int end = std::clamp(start + length, start, count);
        if (start == end || (start == 0 && end == count)) { red = color.r; green = color.g; blue = color.b; colorRuns.reset(); return; }
        QVector<Rgb> units = unitColors();
        for (int i = start; i < end; ++i) units[i] = color;
        setUnitColors(units);
    }
    void setFont(const QString &name, int start, int length) {
        if (name.isEmpty() || name.size() > 200 || hasNewline(name)) return;
        const int count = content.size();
        start = std::clamp(start, 0, count);
        const int end = std::clamp(start + length, start, count);
        if (start == end || (start == 0 && end == count)) { fontName = name; fontRuns.reset(); return; }
        QVector<QString> units = unitFonts();
        for (int i = start; i < end; ++i) units[i] = name;
        setUnitFonts(units);
    }
    // Keeps each letter's color and face when [start, start+length) is replaced by `newLength` units, which
    // inherit from the letter before (or the first letter replaced). Call before `content` changes.
    void replaceCharacters(int start, int length, int newLength) {
        const int count = content.size();
        start = std::clamp(start, 0, count);
        const int end = std::clamp(start + length, start, count);
        newLength = std::max(0, newLength);
        if (colorRuns) {
            QVector<Rgb> units = unitColors();
            const Rgb inherited = start > 0 ? units[start - 1] : (end > start ? units[start] : (units.isEmpty() ? Rgb{red, green, blue} : units.first()));
            units.remove(start, end - start);
            units.insert(start, newLength, inherited);
            setUnitColors(units);
        }
        if (fontRuns) {
            QVector<QString> units = unitFonts();
            const QString inherited = start > 0 ? units[start - 1] : (end > start ? units[start] : (units.isEmpty() ? fontName : units.first()));
            units.remove(start, end - start);
            units.insert(start, newLength, inherited);
            setUnitFonts(units);
        }
    }

    [[nodiscard]] QVector<Rgb> unitColors() const {
        QVector<Rgb> units(content.size(), Rgb{red, green, blue});
        if (colorRuns) for (const TextColorRun &run : *colorRuns)
            for (int i = std::max(0, run.location); i < std::min<int>(units.size(), run.location + run.length); ++i) units[i] = {run.red, run.green, run.blue};
        return units;
    }
    void setUnitColors(const QVector<Rgb> &units) {
        const Rgb base{red, green, blue};
        QVector<TextColorRun> runs;
        for (int i = 0; i < units.size(); ++i) {
            if (units[i] == base) continue;
            if (!runs.isEmpty() && runs.last().location + runs.last().length == i
                && Rgb{runs.last().red, runs.last().green, runs.last().blue} == units[i]) ++runs.last().length;
            else runs.append({i, 1, units[i].r, units[i].g, units[i].b});
        }
        if (runs.isEmpty()) colorRuns.reset(); else colorRuns = runs;
    }
    [[nodiscard]] QVector<QString> unitFonts() const {
        QVector<QString> units(content.size(), fontName);
        if (fontRuns) for (const TextFontRun &run : *fontRuns)
            for (int i = std::max(0, run.location); i < std::min<int>(units.size(), run.location + run.length); ++i) units[i] = run.fontName;
        return units;
    }
    void setUnitFonts(const QVector<QString> &units) {
        if (!units.isEmpty() && std::all_of(units.cbegin(), units.cend(), [&](const QString &u) { return u == units.first(); })) {
            fontName = units.first(); fontRuns.reset(); return;
        }
        QVector<TextFontRun> runs;
        for (int i = 0; i < units.size(); ++i) {
            if (units[i] == fontName) continue;
            if (!runs.isEmpty() && runs.last().location + runs.last().length == i && runs.last().fontName == units[i]) ++runs.last().length;
            else runs.append({i, 1, units[i]});
        }
        if (runs.isEmpty()) fontRuns.reset(); else fontRuns = runs;
    }

    [[nodiscard]] bool isValid() const {
        if (!colorRunsAreValid() || !fontRunsAreValid()) return false;
        if (content.size() > 100000) return false;
        if (!std::isfinite(fontSize) || fontSize < 1.0 || fontSize > 2000.0) return false;
        if (!std::isfinite(red) || red < 0.0 || red > 1.0) return false;
        if (!std::isfinite(green) || green < 0.0 || green > 1.0) return false;
        if (!std::isfinite(blue) || blue < 0.0 || blue > 1.0) return false;
        if (!std::isfinite(tracking) || tracking < -100.0 || tracking > 1000.0) return false;
        if (!std::isfinite(leading) || leading < 0.0 || leading > 5000.0) return false;
        if (boxSize) {
            const double w = boxSize->width();
            const double h = boxSize->height();
            if (!std::isfinite(w) || !std::isfinite(h) || w < 16.0 || w > 30000.0 || h < 16.0 || h > 30000.0) return false;
            if (w * h > 100000000.0) return false;
        }
        return true;
    }
    bool operator==(const TextStyle &) const = default;
};

enum class ShapeKind { Rectangle, Ellipse, Line };

struct LayerShapeStyle {
    ShapeKind kind = ShapeKind::Rectangle;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double cornerRadius = 0.0;
    std::optional<double> lineWidth;
    std::optional<QPointF> start;
    std::optional<QPointF> end;

    [[nodiscard]] bool isValid() const {
        if (!std::isfinite(red) || red < 0.0 || red > 1.0) return false;
        if (!std::isfinite(green) || green < 0.0 || green > 1.0) return false;
        if (!std::isfinite(blue) || blue < 0.0 || blue > 1.0) return false;
        if (!std::isfinite(cornerRadius) || cornerRadius < 0.0) return false;
        if (kind == ShapeKind::Line) {
            if (!lineWidth || !std::isfinite(*lineWidth) || *lineWidth < 0.0 || *lineWidth > 500.0) return false;
            if (!start || !std::isfinite(start->x()) || !std::isfinite(start->y())) return false;
            if (!end || !std::isfinite(end->x()) || !std::isfinite(end->y())) return false;
        }
        return true;
    }
    bool operator==(const LayerShapeStyle &) const = default;
};

enum class AdjustmentKind {
    HueSaturation,
    Levels,
    Curves,
    Exposure,
    GradientMap,
    Grain,
    Invert,
    BlackWhite,
    ColorBalance,
    // Version 9 additions
    GaussianBlur,
    MotionBlur,
    AddNoise
};

struct LayerTransform {
    QPointF origin;
    QSizeF size;
    double rotation = 0.0;
    bool flipX = false;
    bool flipY = false;
    Sampling sampling = Sampling::HighQuality;

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] QPointF center() const;
    [[nodiscard]] LayerTransform following(const LayerTransform &oldPlacement, const LayerTransform &newPlacement) const;
    [[nodiscard]] LayerTransform placing(const QTransform &unitSquareMap) const;
    [[nodiscard]] QTransform unitToDocument() const;
    [[nodiscard]] double scalePercent(const QSizeF &pixelSize) const { return size.width() / std::max(1.0, pixelSize.width()) * 100.0; }
    [[nodiscard]] LayerTransform scaled(double percent, const QSizeF &pixelSize) const;
    [[nodiscard]] LayerTransform rounded() const;
    [[nodiscard]] LayerTransform mirrored(bool horizontally, double axis) const;
    [[nodiscard]] bool samePlacement(const LayerTransform &other) const;
    bool operator==(const LayerTransform &) const = default;
};

// `start` resized by dragging the handle at `sign` (-1/0/1 per axis) from `startPoint` to `point`, in document pixels:
// from the opposite handle, or from the centre with `fromCenter`; `lockRatio` keeps the aspect ratio. Only size and
// origin change. One definition for the drag itself and for snapping, which re-evaluates it at other pointer positions.
[[nodiscard]] LayerTransform resizedByHandle(const LayerTransform &start, const QPoint &sign, const QPointF &startPoint,
                                             const QPointF &point, bool fromCenter, bool lockRatio);

struct Layer {
    QUuid id;
    QString name;
    bool visible = true;
    LayerTransform transform;
    QString imageFile;
    QImage image;
    std::optional<QUuid> parentId;
    bool group = false;
    double opacity = 1.0;
    BlendMode blendMode = BlendMode::Normal;
    QString maskFile;
    QImage mask;
    bool maskEnabled = true;
    std::optional<LayerTransform> maskPlacement;
    bool maskLinked = true;
    std::optional<QUuid> maskSourceId;
    QJsonObject adjustment;
    QJsonObject shape;
    std::optional<LayerEffects> effects;
    std::optional<TextStyle> text;
    std::optional<LayerShapeStyle> shapeStyle;

    bool operator==(const Layer &) const = default;
};

struct Document {
    int formatVersion = 0;
    QUuid id;
    quint64 generation = 0;
    QSize canvasSize;
    double resolution = 72.0;
    std::optional<QUuid> activeLayerId;
    QVector<Layer> layers; // Bottom to top, matching the project manifest.
    QVector<CanvasGuide> guides;
    /// Canvas-sized grayscale coverage. Null means no selection; an all-black image is an explicit empty selection.
    std::optional<QImage> selection;
    QString projectPath;

    bool operator==(const Document &other) const {
        return formatVersion == other.formatVersion
            && id == other.id
            && canvasSize == other.canvasSize
            && resolution == other.resolution
            && activeLayerId == other.activeLayerId
            && layers == other.layers
            && guides == other.guides
            && selection == other.selection
            && projectPath == other.projectPath;
    }
};

[[nodiscard]] Sampling samplingFromString(const QString &value);
[[nodiscard]] QString samplingToString(Sampling value);

[[nodiscard]] std::optional<BlendMode> blendModeFromString(const QString &value);
[[nodiscard]] QString blendModeToString(BlendMode mode);
[[nodiscard]] QPainter::CompositionMode compositionMode(BlendMode mode);

[[nodiscard]] QString guideAxisToString(CanvasGuide::Axis axis);
[[nodiscard]] std::optional<CanvasGuide::Axis> guideAxisFromString(const QString &value);

[[nodiscard]] QString textAlignmentToString(TextAlignment align);
[[nodiscard]] std::optional<TextAlignment> textAlignmentFromString(const QString &value);

[[nodiscard]] QString shapeKindToString(ShapeKind kind);
[[nodiscard]] std::optional<ShapeKind> shapeKindFromString(const QString &value);

[[nodiscard]] QString adjustmentKindToString(AdjustmentKind kind);
[[nodiscard]] std::optional<AdjustmentKind> adjustmentKindFromString(const QString &value);
[[nodiscard]] bool isVersion9Adjustment(AdjustmentKind kind);

} // namespace compositor
