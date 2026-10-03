#pragma once

#include <QProxyStyle>

namespace compositor {

/// Clicking a slider's track puts the knob under the pointer and carries on dragging from there, rather than paging
/// towards it (the behaviour mac SliderSnap gives every slider).
class SliderJumpStyle final : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    int styleHint(StyleHint hint, const QStyleOption *option = nullptr, const QWidget *widget = nullptr,
                  QStyleHintReturn *returnData = nullptr) const override
    {
        if (hint == SH_Slider_AbsoluteSetButtons) return Qt::LeftButton;
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
};

} // namespace compositor
