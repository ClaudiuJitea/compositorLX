#include "NumericScrub.h"
#include <QApplication>
#include <QColorDialog>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <cmath>
#include <algorithm>

namespace compositor {

ScrubLabel::ScrubLabel(const QString &text, QWidget *parent)
    : QLabel(text, parent)
{
    init();
}

ScrubLabel::ScrubLabel(const QString &text, QDoubleSpinBox *target, double sensitivity,
                       std::optional<double> step, QWidget *parent)
    : QLabel(text, parent)
{
    init();
    bindDoubleSpinBox(target, sensitivity, step);
}

ScrubLabel::ScrubLabel(const QString &text, QSpinBox *target, double sensitivity,
                       std::optional<double> step, QWidget *parent)
    : QLabel(text, parent)
{
    init();
    bindSpinBox(target, sensitivity, step);
}

ScrubLabel::ScrubLabel(const QString &text, QSlider *target, double sensitivity,
                       std::optional<double> step, QWidget *parent)
    : QLabel(text, parent)
{
    init();
    bindSlider(target, sensitivity, step);
}

ScrubLabel::~ScrubLabel()
{
    if (linkedWidget_) {
        linkedWidget_->removeEventFilter(this);
    }
}

void ScrubLabel::init()
{
    setProperty("scrubbable", true);
    setCursor(Qt::SizeHorCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
}

void ScrubLabel::bindDoubleSpinBox(QDoubleSpinBox *spinBox, double sensitivity,
                                   std::optional<double> step)
{
    if (linkedWidget_) {
        linkedWidget_->removeEventFilter(this);
    }
    linkedWidget_ = spinBox;
    spinBox_ = spinBox;
    intSpinBox_ = nullptr;
    slider_ = nullptr;
    getter_ = nullptr;
    setter_ = nullptr;
    sensitivity_ = sensitivity;
    step_ = step;
    if (spinBox) {
        minimum_ = spinBox->minimum();
        maximum_ = spinBox->maximum();
        value_ = spinBox->value();
        spinBox->installEventFilter(this);
        setVisible(spinBox->isVisible());
    }
}

void ScrubLabel::bindSpinBox(QSpinBox *spinBox, double sensitivity,
                             std::optional<double> step)
{
    if (linkedWidget_) {
        linkedWidget_->removeEventFilter(this);
    }
    linkedWidget_ = spinBox;
    intSpinBox_ = spinBox;
    spinBox_ = nullptr;
    slider_ = nullptr;
    getter_ = nullptr;
    setter_ = nullptr;
    sensitivity_ = sensitivity;
    step_ = step;
    if (spinBox) {
        minimum_ = static_cast<double>(spinBox->minimum());
        maximum_ = static_cast<double>(spinBox->maximum());
        value_ = static_cast<double>(spinBox->value());
        spinBox->installEventFilter(this);
        setVisible(spinBox->isVisible());
    }
}

void ScrubLabel::bindSlider(QSlider *slider, double sensitivity,
                            std::optional<double> step)
{
    if (linkedWidget_) {
        linkedWidget_->removeEventFilter(this);
    }
    linkedWidget_ = slider;
    slider_ = slider;
    spinBox_ = nullptr;
    intSpinBox_ = nullptr;
    getter_ = nullptr;
    setter_ = nullptr;
    sensitivity_ = sensitivity;
    step_ = step;
    if (slider) {
        minimum_ = static_cast<double>(slider->minimum());
        maximum_ = static_cast<double>(slider->maximum());
        value_ = static_cast<double>(slider->value());
        slider->installEventFilter(this);
        setVisible(slider->isVisible());
    }
}

void ScrubLabel::setCallbacks(std::function<double()> getter, std::function<void(double)> setter,
                              double minimum, double maximum, double sensitivity,
                              std::optional<double> step)
{
    if (linkedWidget_) {
        linkedWidget_->removeEventFilter(this);
        linkedWidget_ = nullptr;
    }
    spinBox_ = nullptr;
    intSpinBox_ = nullptr;
    slider_ = nullptr;
    getter_ = std::move(getter);
    setter_ = std::move(setter);
    minimum_ = minimum;
    maximum_ = maximum;
    sensitivity_ = sensitivity;
    step_ = step;
    if (getter_) {
        value_ = getter_();
    }
}

double ScrubLabel::currentValue() const
{
    if (spinBox_) {
        return spinBox_->value();
    }
    if (intSpinBox_) {
        return static_cast<double>(intSpinBox_->value());
    }
    if (slider_) {
        return static_cast<double>(slider_->value());
    }
    if (getter_) {
        return getter_();
    }
    return value_;
}

void ScrubLabel::applyValue(double proposed)
{
    if (step_.has_value() && *step_ > 0.0) {
        proposed = std::round(proposed / *step_) * *step_;
    }
    if (spinBox_) {
        minimum_ = spinBox_->minimum();
        maximum_ = spinBox_->maximum();
    } else if (intSpinBox_) {
        minimum_ = static_cast<double>(intSpinBox_->minimum());
        maximum_ = static_cast<double>(intSpinBox_->maximum());
    } else if (slider_) {
        minimum_ = static_cast<double>(slider_->minimum());
        maximum_ = static_cast<double>(slider_->maximum());
    }
    proposed = std::clamp(proposed, minimum_, maximum_);
    value_ = proposed;

    if (spinBox_) {
        spinBox_->setValue(value_);
    } else if (intSpinBox_) {
        intSpinBox_->setValue(static_cast<int>(std::round(value_)));
    } else if (slider_) {
        slider_->setValue(static_cast<int>(std::round(value_)));
    } else if (setter_) {
        setter_(value_);
    }
    emit scrubValueChanged(value_);
}

void ScrubLabel::updateCursor(bool hovering)
{
    isHovering_ = hovering;
    setCursor(Qt::SizeHorCursor);
}

void ScrubLabel::enterEvent(QEnterEvent *event)
{
    updateCursor(true);
    QLabel::enterEvent(event);
}

void ScrubLabel::leaveEvent(QEvent *event)
{
    updateCursor(false);
    QLabel::leaveEvent(event);
}

void ScrubLabel::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        pressed_ = true;
        dragging_ = false;
        pressPoint_ = event->position();
        startValue_ = currentValue();
        value_ = startValue_;
        setFocus();
        event->accept();
        return;
    }
    QLabel::mousePressEvent(event);
}

void ScrubLabel::mouseMoveEvent(QMouseEvent *event)
{
    if (pressed_) {
        const double dx = event->position().x() - pressPoint_.x();
        const double dy = event->position().y() - pressPoint_.y();
        if (!dragging_) {
            // Drag threshold: 1px (matching macOS DragGesture(minimumDistance: 1))
            if (std::abs(dx) >= 1.0 || std::hypot(dx, dy) >= 1.0) {
                dragging_ = true;
                emit dragStarted();
                emit dragStatusChanged(true);
                if (onStart_) {
                    onStart_();
                }
            }
        }
        if (dragging_) {
            const double delta = dx * sensitivity_;
            const double proposed = startValue_ + delta;
            applyValue(proposed);
            event->accept();
            return;
        }
    }
    QLabel::mouseMoveEvent(event);
}

void ScrubLabel::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && pressed_) {
        pressed_ = false;
        if (dragging_) {
            dragging_ = false;
            emit dragEnded();
            emit dragStatusChanged(false);
            if (onEnd_) {
                onEnd_();
            }
        }
        updateCursor(isHovering_);
        event->accept();
        return;
    }
    QLabel::mouseReleaseEvent(event);
}

void ScrubLabel::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && dragging_) {
        cancelDrag();
        event->accept();
        return;
    }
    QLabel::keyPressEvent(event);
}

void ScrubLabel::cancelDrag()
{
    if (!dragging_) {
        pressed_ = false;
        return;
    }
    dragging_ = false;
    pressed_ = false;
    applyValue(startValue_);
    emit dragCancelled();
    emit dragEnded();
    emit dragStatusChanged(false);
    if (onCancel_) {
        onCancel_();
    } else if (onEnd_) {
        onEnd_();
    }
    updateCursor(isHovering_);
}

bool ScrubLabel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == linkedWidget_) {
        if (event->type() == QEvent::Show) {
            this->show();
        } else if (event->type() == QEvent::Hide) {
            this->hide();
        }
    }
    return QLabel::eventFilter(watched, event);
}

NumericScrubFilter::NumericScrubFilter(QLabel *label, QSpinBox *spinBox, double sensitivity,
                                       std::optional<double> step, QObject *parent)
    : QObject(parent ? parent : label)
    , label_(label)
    , intSpinBox_(spinBox)
    , sensitivity_(sensitivity)
    , step_(step)
{
    if (spinBox) {
        minimum_ = spinBox->minimum();
        maximum_ = spinBox->maximum();
        value_ = spinBox->value();
    }
    if (label_) {
        label_->setProperty("scrubbable", true);
        label_->setCursor(Qt::SizeHorCursor);
        label_->setFocusPolicy(Qt::StrongFocus);
        label_->installEventFilter(this);
    }
}

NumericScrubFilter::NumericScrubFilter(QLabel *label, QDoubleSpinBox *spinBox, double sensitivity,
                                       std::optional<double> step, QObject *parent)
    : QObject(parent ? parent : label)
    , label_(label)
    , spinBox_(spinBox)
    , sensitivity_(sensitivity)
    , step_(step)
{
    if (spinBox) {
        minimum_ = spinBox->minimum();
        maximum_ = spinBox->maximum();
        value_ = spinBox->value();
    }
    if (label_) {
        label_->setProperty("scrubbable", true);
        label_->setCursor(Qt::SizeHorCursor);
        label_->setFocusPolicy(Qt::StrongFocus);
        label_->installEventFilter(this);
    }
}

NumericScrubFilter::NumericScrubFilter(QLabel *label, QSlider *slider, double sensitivity,
                                       std::optional<double> step, QObject *parent)
    : QObject(parent ? parent : label)
    , label_(label)
    , slider_(slider)
    , sensitivity_(sensitivity)
    , step_(step)
{
    if (slider) {
        minimum_ = slider->minimum();
        maximum_ = slider->maximum();
        value_ = slider->value();
    }
    if (label_) {
        label_->setProperty("scrubbable", true);
        label_->setCursor(Qt::SizeHorCursor);
        label_->setFocusPolicy(Qt::StrongFocus);
        label_->installEventFilter(this);
    }
}

NumericScrubFilter::~NumericScrubFilter()
{
    if (label_) {
        label_->removeEventFilter(this);
    }
}

double NumericScrubFilter::currentValue() const
{
    if (spinBox_) return spinBox_->value();
    if (intSpinBox_) return static_cast<double>(intSpinBox_->value());
    if (slider_) return static_cast<double>(slider_->value());
    return value_;
}

void NumericScrubFilter::applyValue(double proposed)
{
    if (step_.has_value() && *step_ > 0.0) {
        proposed = std::round(proposed / *step_) * *step_;
    }
    if (spinBox_) {
        minimum_ = spinBox_->minimum();
        maximum_ = spinBox_->maximum();
    } else if (intSpinBox_) {
        minimum_ = static_cast<double>(intSpinBox_->minimum());
        maximum_ = static_cast<double>(intSpinBox_->maximum());
    } else if (slider_) {
        minimum_ = static_cast<double>(slider_->minimum());
        maximum_ = static_cast<double>(slider_->maximum());
    }
    proposed = std::clamp(proposed, minimum_, maximum_);
    value_ = proposed;

    if (spinBox_) {
        spinBox_->setValue(value_);
    } else if (intSpinBox_) {
        intSpinBox_->setValue(static_cast<int>(std::round(value_)));
    } else if (slider_) {
        slider_->setValue(static_cast<int>(std::round(value_)));
    }
    emit scrubValueChanged(value_);
}

void NumericScrubFilter::cancelDrag()
{
    if (!dragging_) {
        pressed_ = false;
        return;
    }
    dragging_ = false;
    pressed_ = false;
    applyValue(startValue_);
    emit dragCancelled();
    emit dragEnded();
    emit dragStatusChanged(false);
}

bool NumericScrubFilter::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == label_) {
        switch (event->type()) {
        case QEvent::Enter:
            label_->setCursor(Qt::SizeHorCursor);
            break;
        case QEvent::Leave:
            if (!pressed_) {
                label_->setCursor(Qt::ArrowCursor);
            }
            break;
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                pressed_ = true;
                dragging_ = false;
                pressPoint_ = me->position();
                startValue_ = currentValue();
                value_ = startValue_;
                label_->setFocus();
                return true;
            }
            break;
        }
        case QEvent::MouseMove: {
            if (pressed_) {
                auto *me = static_cast<QMouseEvent *>(event);
                const double dx = me->position().x() - pressPoint_.x();
                const double dy = me->position().y() - pressPoint_.y();
                if (!dragging_) {
                    if (std::abs(dx) >= 1.0 || std::hypot(dx, dy) >= 1.0) {
                        dragging_ = true;
                        emit dragStarted();
                        emit dragStatusChanged(true);
                    }
                }
                if (dragging_) {
                    const double delta = dx * sensitivity_;
                    const double proposed = startValue_ + delta;
                    applyValue(proposed);
                    return true;
                }
            }
            break;
        }
        case QEvent::MouseButtonRelease: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton && pressed_) {
                pressed_ = false;
                if (dragging_) {
                    dragging_ = false;
                    emit dragEnded();
                    emit dragStatusChanged(false);
                }
                return true;
            }
            break;
        }
        case QEvent::KeyPress: {
            auto *ke = static_cast<QKeyEvent *>(event);
            if (ke->key() == Qt::Key_Escape && dragging_) {
                cancelDrag();
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

NumericScrubFilter *attachScrubToLabel(QLabel *label, QSpinBox *spinBox, double sensitivity,
                                       std::optional<double> step)
{
    if (!label || !spinBox) return nullptr;
    return new NumericScrubFilter(label, spinBox, sensitivity, step, label);
}

NumericScrubFilter *attachScrubToLabel(QLabel *label, QDoubleSpinBox *spinBox, double sensitivity,
                                       std::optional<double> step)
{
    if (!label || !spinBox) return nullptr;
    return new NumericScrubFilter(label, spinBox, sensitivity, step, label);
}

NumericScrubFilter *attachScrubToLabel(QLabel *label, QSlider *slider, double sensitivity,
                                       std::optional<double> step)
{
    if (!label || !slider) return nullptr;
    return new NumericScrubFilter(label, slider, sensitivity, step, label);
}

void attachColorDialogScrubbing(QColorDialog *picker)
{
    if (!picker) return;
    const auto labels = picker->findChildren<QLabel *>();
    for (QLabel *label : labels) {
        if (auto *spin = qobject_cast<QSpinBox *>(label->buddy())) {
            QString cleanText = label->text();
            cleanText.remove(QLatin1Char('&'));
            if (spin->maximum() == 255 && spin->minimum() == 0) {
                if (cleanText.contains(QStringLiteral("Red"), Qt::CaseInsensitive)) {
                    label->setObjectName(QStringLiteral("colorRedLabel"));
                    spin->setObjectName(QStringLiteral("colorRedSpin"));
                    attachScrubToLabel(label, spin, 1.0, 1.0);
                } else if (cleanText.contains(QStringLiteral("Green"), Qt::CaseInsensitive)) {
                    label->setObjectName(QStringLiteral("colorGreenLabel"));
                    spin->setObjectName(QStringLiteral("colorGreenSpin"));
                    attachScrubToLabel(label, spin, 1.0, 1.0);
                } else if (cleanText.contains(QStringLiteral("Blue"), Qt::CaseInsensitive)) {
                    label->setObjectName(QStringLiteral("colorBlueLabel"));
                    spin->setObjectName(QStringLiteral("colorBlueSpin"));
                    attachScrubToLabel(label, spin, 1.0, 1.0);
                } else if (cleanText.contains(QStringLiteral("Sat"), Qt::CaseInsensitive)) {
                    label->setObjectName(QStringLiteral("colorSatLabel"));
                    spin->setObjectName(QStringLiteral("colorSatSpin"));
                    attachScrubToLabel(label, spin, 1.0, 1.0);
                } else if (cleanText.contains(QStringLiteral("Val"), Qt::CaseInsensitive)) {
                    label->setObjectName(QStringLiteral("colorValLabel"));
                    spin->setObjectName(QStringLiteral("colorValSpin"));
                    attachScrubToLabel(label, spin, 1.0, 1.0);
                }
            } else if (spin->maximum() == 359 && spin->minimum() == 0) {
                if (cleanText.contains(QStringLiteral("Hue"), Qt::CaseInsensitive)) {
                    label->setObjectName(QStringLiteral("colorHueLabel"));
                    spin->setObjectName(QStringLiteral("colorHueSpin"));
                    attachScrubToLabel(label, spin, 1.0, 1.0);
                }
            }
        }
    }
}

} // namespace compositor
