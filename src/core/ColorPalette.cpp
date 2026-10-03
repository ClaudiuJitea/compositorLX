#include "core/ColorPalette.h"

#include <algorithm>
#include <cmath>

namespace compositor {

QColor PickerHSB::rgb() const
{
    const double wrapped = std::fmod(std::fmod(hue, 360.0) + 360.0, 360.0) / 60.0;
    const double c = brightness * saturation;
    const double x = c * (1 - std::abs(std::fmod(wrapped, 2.0) - 1));
    const double m = brightness - c;
    double r, g, b;
    switch (int(wrapped)) {
    case 0: r = c; g = x; b = 0; break;
    case 1: r = x; g = c; b = 0; break;
    case 2: r = 0; g = c; b = x; break;
    case 3: r = 0; g = x; b = c; break;
    case 4: r = x; g = 0; b = c; break;
    default: r = c; g = 0; b = x; break;
    }
    const auto clamp = [](double v) { return std::clamp(v, 0.0, 1.0); };
    return QColor::fromRgbF(float(clamp(r + m)), float(clamp(g + m)), float(clamp(b + m)));
}

void PickerHSB::setRGB(const QColor &color)
{
    const double red = color.redF(), green = color.greenF(), blue = color.blueF();
    const double high = std::max({red, green, blue}), low = std::min({red, green, blue});
    const double delta = high - low;
    brightness = high;
    if (high > 0) saturation = delta / high;
    if (delta <= 0) return;
    double h;
    if (high == red) h = (green - blue) / delta;
    else if (high == green) h = (blue - red) / delta + 2;
    else h = (red - green) / delta + 4;
    h *= 60;
    hue = h < 0 ? h + 360 : h;
}

namespace palette {

QColor quantized(const QColor &color)
{
    return QColor(color.red(), color.green(), color.blue());
}

QString hex(const QColor &color)
{
    return QStringLiteral("%1%2%3").arg(color.red(), 2, 16, QLatin1Char('0')).arg(color.green(), 2, 16, QLatin1Char('0'))
        .arg(color.blue(), 2, 16, QLatin1Char('0')).toUpper();
}

std::optional<QColor> fromHex(const QString &input)
{
    QString text = input.trimmed();
    if (text.startsWith(QLatin1Char('#'))) text.remove(0, 1);
    if (text.size() == 3) text = QStringLiteral("%1%1%2%2%3%3").arg(text[0]).arg(text[1]).arg(text[2]);
    if (text.size() != 6) return std::nullopt;
    bool ok = false;
    const uint value = text.toUInt(&ok, 16);
    // toUInt accepts a sign or 0x prefix only past six characters, but guard the digits explicitly.
    for (const QChar c : text) if (!c.isDigit() && !(c.toLower() >= QLatin1Char('a') && c.toLower() <= QLatin1Char('f'))) return std::nullopt;
    if (!ok) return std::nullopt;
    return QColor((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
}

} // namespace palette
} // namespace compositor
