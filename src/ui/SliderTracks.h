#pragma once

// The colored slider tracks of macOS CameraRawSlider.swift (CameraRawSliderTrack), shared by the Camera Raw dialog and
// the Color Balance / Hue-Saturation panels.
#include <QColor>
#include <QSlider>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <cmath>

namespace compositor {
namespace SliderTrack {

inline QColor hsv(double degrees, double saturation, double brightness)
{
    double turns = degrees / 360.0; turns -= std::floor(turns);
    return QColor::fromHsvF(turns, saturation, brightness);
}
inline QList<QColor> temperature() { return {QColor::fromRgbF(.22, .46, .95), QColor::fromRgbF(.98, .82, .18)}; }
inline QList<QColor> tint() { return {QColor::fromRgbF(.28, .70, .34), QColor::fromRgbF(.70, .40, .64)}; }
inline QList<QColor> chroma() { return {QColor::fromRgbF(.62, .62, .64), QColor::fromRgbF(.86, .18, .20)}; }
inline QList<QColor> hue(double degrees) { return {hsv(degrees - 50, .85, .9), hsv(degrees + 50, .85, .9)}; }
inline QList<QColor> saturation(double degrees) { return {QColor::fromRgbF(.55, .55, .56), hsv(degrees, .9, .9)}; }
inline QList<QColor> luminance(double degrees) { return {hsv(degrees, .55, .18), hsv(degrees, .35, .95)}; }
inline QList<QColor> spectrum(double degrees)
{
    QList<QColor> colors;
    for (int offset = -180; offset <= 180; offset += 30) colors.push_back(hsv(degrees + offset, .85, .9));
    return colors;
}
inline QList<QColor> cyanRed() { return {QColor::fromRgbF(.10, .72, .80), QColor::fromRgbF(.86, .18, .20)}; }
inline QList<QColor> magentaGreen() { return {QColor::fromRgbF(.80, .22, .70), QColor::fromRgbF(.24, .70, .30)}; }
inline QList<QColor> yellowBlue() { return {QColor::fromRgbF(.95, .82, .18), QColor::fromRgbF(.22, .40, .92)}; }

// The track's colors, painted along the whole groove by the editor's slider style (SliderJumpStyle), 4 px thick as macOS's
// color tracks are.
inline QVariantList gradient(const QList<QColor> &colors)
{
    QVariantList list;
    for (const QColor &color : colors) list.push_back(color);
    return list;
}
inline void apply(QSlider *slider, const QList<QColor> &colors)
{
    if (!slider || colors.isEmpty()) return;
    slider->setProperty("trackGradient", gradient(colors));
    slider->setProperty("trackColors", int(colors.size()));
    slider->update();
}

} // namespace SliderTrack
} // namespace compositor
