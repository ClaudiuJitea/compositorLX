#pragma once

#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QStyleOptionSlider>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace compositor {

// The slim slider of macOS AppSlider.swift (SliderGeometry, SliderTrackView): a thin rounded track, the accent filling it
// to the knob's center, and a light round knob with a soft shadow that grows a little while it's held. A slider given
// colors (SliderTrack::apply, the "trackGradient" property) paints them along the whole track instead of the fill.
namespace SliderLook {
// mac SliderGeometry.plainTrack and colorTrack: a color track is a little thicker, so its colors read.
inline constexpr double plainTrack = 3;
inline constexpr double colorTrack = 4;
inline constexpr int knob = 14;
// Room for the knob to grow while held, and for its shadow: across the slider, and at each end of the track.
inline constexpr int thickness = 22;
inline constexpr int inset = 3;

// How far the fill reaches: to the knob's center, except over the last few points at each end, where it ramps quickly
// to empty and to full, so the knob shows none at the minimum and all at the maximum (mac SliderGeometry.fillWidth).
inline double fillLength(double fraction, double length, double knobLength = knob)
{
    const double half = knobLength / 2, travel = std::max(0.0, length - knobLength), position = fraction * travel;
    const double ramp = std::min(6.0, travel / 2);
    if (ramp <= 0) return fraction * length;
    if (position < ramp) return position / ramp * (ramp + half);
    const double remaining = travel - position;
    if (remaining < ramp) return length - remaining / ramp * (ramp + half);
    return position + half;
}
} // namespace SliderLook

/// Also: clicking a slider's track puts the knob under the pointer and carries on dragging from there, rather than
/// paging towards it (the behaviour mac SliderSnap gives every slider).
class SliderJumpStyle final : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    int styleHint(StyleHint hint, const QStyleOption *option = nullptr, const QWidget *widget = nullptr,
                  QStyleHintReturn *returnData = nullptr) const override
    {
        if (hint == SH_Slider_AbsoluteSetButtons) return Qt::LeftButton;
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }

    int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr, const QWidget *widget = nullptr) const override
    {
        switch (metric) {
        case PM_SliderThickness: return SliderLook::thickness;
        case PM_SliderControlThickness:
        case PM_SliderLength: return SliderLook::knob;
        case PM_SliderTickmarkOffset: return 0;
        default: return QProxyStyle::pixelMetric(metric, option, widget);
        }
    }

    QRect subControlRect(ComplexControl control, const QStyleOptionComplex *option, SubControl sub,
                         const QWidget *widget = nullptr) const override
    {
        const auto *slider = qstyleoption_cast<const QStyleOptionSlider *>(option);
        if (control != CC_Slider || !slider) return QProxyStyle::subControlRect(control, option, sub, widget);
        const bool horizontal = slider->orientation == Qt::Horizontal;
        const QRect r = horizontal ? slider->rect.adjusted(SliderLook::inset, 0, -SliderLook::inset, 0)
                                   : slider->rect.adjusted(0, SliderLook::inset, 0, -SliderLook::inset);
        if (sub == SC_SliderGroove) return r;
        if (sub == SC_SliderHandle) {
            // The knob's center travels from half a knob in from one end to half a knob in from the other.
            const int span = (horizontal ? r.width() : r.height()) - SliderLook::knob;
            const int position = sliderPositionFromValue(slider->minimum, slider->maximum, slider->sliderPosition,
                                                         std::max(0, span), slider->upsideDown);
            return horizontal ? QRect(r.left() + position, r.center().y() - SliderLook::knob / 2 + 1, SliderLook::knob, SliderLook::knob)
                              : QRect(r.center().x() - SliderLook::knob / 2 + 1, r.top() + position, SliderLook::knob, SliderLook::knob);
        }
        return QProxyStyle::subControlRect(control, option, sub, widget);
    }

    void drawComplexControl(ComplexControl control, const QStyleOptionComplex *option, QPainter *painter,
                            const QWidget *widget = nullptr) const override
    {
        const auto *slider = qstyleoption_cast<const QStyleOptionSlider *>(option);
        if (control != CC_Slider || !slider) { QProxyStyle::drawComplexControl(control, option, painter, widget); return; }
        drawSlider(*slider, painter, widget);
    }

    QSize sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &size, const QWidget *widget) const override
    {
        if (type == CT_Slider) {
            const auto *slider = qstyleoption_cast<const QStyleOptionSlider *>(option);
            const bool horizontal = !slider || slider->orientation == Qt::Horizontal;
            return horizontal ? QSize(std::max(size.width(), 40), SliderLook::thickness) : QSize(SliderLook::thickness, std::max(size.height(), 40));
        }
        return QProxyStyle::sizeFromContents(type, option, size, widget);
    }

private:
    static QColor knobColor(const QPalette &palette, bool hovered)
    {
        // mac Color(white: 0.92) on its dark scheme; a white knob on a light one, both a touch brighter under the pointer.
        const bool dark = palette.color(QPalette::Window).lightnessF() < .5;
        return dark ? (hovered ? QColor(247, 247, 247) : QColor(235, 235, 235)) : QColor(255, 255, 255);
    }

    void drawSlider(const QStyleOptionSlider &slider, QPainter *painter, const QWidget *widget) const
    {
        const QPalette &palette = slider.palette;
        const bool enabled = slider.state & State_Enabled;
        const bool horizontal = slider.orientation == Qt::Horizontal;
        const bool dark = palette.color(QPalette::Window).lightnessF() < .5;
        const QVariantList colors = widget ? widget->property("trackGradient").toList() : QVariantList();
        const QRect bounds = subControlRect(CC_Slider, &slider, SC_SliderGroove, widget);
        const double length = horizontal ? bounds.width() : bounds.height();
        const double thickness = colors.size() >= 2 ? SliderLook::colorTrack : SliderLook::plainTrack;
        const QRectF handle = subControlRect(CC_Slider, &slider, SC_SliderHandle, widget);
        const QRectF track = horizontal ? QRectF(bounds.left(), handle.center().y() - thickness / 2, length, thickness)
                                        : QRectF(handle.center().x() - thickness / 2, bounds.top(), thickness, length);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        // mac: the whole slider at 45% while disabled.
        if (!enabled) painter->setOpacity(.45);
        painter->setPen(Qt::NoPen);
        const double radius = thickness / 2;
        if (colors.size() >= 2) {
            QLinearGradient gradient(track.topLeft(), horizontal ? track.topRight() : track.bottomLeft());
            for (int i = 0; i < colors.size(); ++i) gradient.setColorAt(double(i) / (colors.size() - 1), colors[i].value<QColor>());
            painter->setBrush(gradient);
            painter->drawRoundedRect(track, radius, radius);
        } else {
            // mac Color.white.opacity(0.14) on dark; its light counterpart a soft gray.
            painter->setBrush(dark ? QColor(255, 255, 255, 36) : QColor(0, 0, 0, 38));
            painter->drawRoundedRect(track, radius, radius);
            const int span = std::max(1, slider.maximum - slider.minimum);
            double fraction = double(slider.sliderPosition - slider.minimum) / span;
            if (slider.upsideDown) fraction = 1 - fraction;
            if (!horizontal) fraction = 1 - fraction;   // A vertical slider fills from the bottom.
            const double fill = SliderLook::fillLength(std::clamp(fraction, 0.0, 1.0), length);
            if (fill > 0) {
                painter->setBrush(palette.color(QPalette::Highlight));
                const QRectF filled = horizontal ? QRectF(track.left(), track.top(), fill, thickness)
                                                 : QRectF(track.left(), track.bottom() - fill, thickness, fill);
                painter->drawRoundedRect(filled, radius, radius);
            }
        }

        if (slider.subControls & SC_SliderHandle) {
            const bool pressed = (slider.activeSubControls & SC_SliderHandle) && (slider.state & State_Sunken);
            const bool hovered = enabled && (slider.activeSubControls & SC_SliderHandle) && (slider.state & State_MouseOver);
            // Held, it grows a little (mac scaleEffect 1.25 while grabbed).
            const double side = SliderLook::knob * (pressed ? 1.2 : 1.0);
            const QRectF knob(handle.center() - QPointF(side / 2, side / 2), QSizeF(side, side));
            // A soft shadow, not an outline, keeps it apart from a light track (mac shadow: black 35%, radius 1, y 0.5).
            painter->setBrush(QColor(0, 0, 0, dark ? 70 : 45));
            painter->drawEllipse(knob.adjusted(-.75, -.25, .75, 1.25));
            painter->setBrush(QColor(0, 0, 0, dark ? 40 : 25));
            painter->drawEllipse(knob.adjusted(-1.5, -.5, 1.5, 2));
            painter->setBrush(knobColor(palette, hovered));
            painter->drawEllipse(knob);
            if (!dark) {
                painter->setPen(QPen(QColor(0, 0, 0, 40), 1));
                painter->setBrush(Qt::NoBrush);
                painter->drawEllipse(knob.adjusted(.5, .5, -.5, -.5));
            }
            if (slider.state & State_HasFocus && slider.state & State_KeyboardFocusChange) {
                painter->setPen(QPen(palette.color(QPalette::Highlight), 1.5));
                painter->setBrush(Qt::NoBrush);
                painter->drawEllipse(knob.adjusted(-2, -2, 2, 2));
            }
        }
        painter->restore();
    }
};

} // namespace compositor
