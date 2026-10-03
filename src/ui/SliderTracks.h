#pragma once

// The colored slider tracks of macOS CameraRawSlider.swift (CameraRawSliderTrack) as Qt style sheets, shared by the
// Camera Raw dialog and the Color Balance / Hue-Saturation panels.
#include <QColor>
#include <QSlider>
#include <QString>
#include <QStringList>
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

inline QString styleSheet(const QList<QColor> &colors)
{
    QString stops;
    for (int i = 0; i < colors.size(); ++i)
        stops += QStringLiteral("stop:%1 %2%3").arg(colors.size() > 1 ? double(i) / (colors.size() - 1) : 0.0, 0, 'f', 4).arg(colors[i].name(), i + 1 < colors.size() ? QStringLiteral(", ") : QString());
    return QStringLiteral("QSlider::groove:horizontal{height:6px;border-radius:3px;background:qlineargradient(x1:0,y1:0,x2:1,y2:0,%1);}"
                          "QSlider::handle:horizontal{width:12px;margin:-4px 0;border-radius:6px;background:#eeeeee;border:1px solid #444444;}").arg(stops);
}
inline void apply(QSlider *slider, const QList<QColor> &colors)
{
    if (slider && !colors.isEmpty()) { slider->setStyleSheet(styleSheet(colors)); slider->setProperty("trackColors", int(colors.size())); }
}

} // namespace SliderTrack
} // namespace compositor
