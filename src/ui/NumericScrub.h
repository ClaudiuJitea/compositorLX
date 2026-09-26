#pragma once

#include <QLabel>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QSlider>
#include <QPointer>
#include <optional>
#include <functional>

class QColorDialog;

namespace compositor {

class NumericScrubFilter : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isDragging READ isDragging NOTIFY dragStatusChanged)
    Q_PROPERTY(double sensitivity READ sensitivity WRITE setSensitivity)
public:
    NumericScrubFilter(QLabel *label, QSpinBox *spinBox, double sensitivity = 1.0,
                       std::optional<double> step = 1.0, QObject *parent = nullptr);
    NumericScrubFilter(QLabel *label, QDoubleSpinBox *spinBox, double sensitivity = 1.0,
                       std::optional<double> step = std::nullopt, QObject *parent = nullptr);
    NumericScrubFilter(QLabel *label, QSlider *slider, double sensitivity = 1.0,
                       std::optional<double> step = std::nullopt, QObject *parent = nullptr);
    ~NumericScrubFilter() override;

    bool eventFilter(QObject *watched, QEvent *event) override;
    void cancelDrag();
    [[nodiscard]] bool isDragging() const { return dragging_; }

    [[nodiscard]] double sensitivity() const { return sensitivity_; }
    void setSensitivity(double s) { sensitivity_ = s; }

    [[nodiscard]] std::optional<double> step() const { return step_; }
    void setStep(std::optional<double> s) { step_ = s; }

    [[nodiscard]] double currentValue() const;

signals:
    void dragStarted();
    void dragEnded();
    void dragCancelled();
    void dragStatusChanged(bool dragging);
    void scrubValueChanged(double value);

private:
    void applyValue(double proposed);

    QPointer<QLabel> label_;
    QPointer<QSpinBox> intSpinBox_;
    QPointer<QDoubleSpinBox> spinBox_;
    QPointer<QSlider> slider_;
    double sensitivity_ = 1.0;
    std::optional<double> step_ = 1.0;
    double minimum_ = 0.0;
    double maximum_ = 100.0;
    double startValue_ = 0.0;
    double value_ = 0.0;
    bool pressed_ = false;
    bool dragging_ = false;
    QPointF pressPoint_;
};

NumericScrubFilter *attachScrubToLabel(QLabel *label, QSpinBox *spinBox, double sensitivity = 1.0,
                                       std::optional<double> step = 1.0);
NumericScrubFilter *attachScrubToLabel(QLabel *label, QDoubleSpinBox *spinBox, double sensitivity = 1.0,
                                       std::optional<double> step = std::nullopt);
NumericScrubFilter *attachScrubToLabel(QLabel *label, QSlider *slider, double sensitivity = 1.0,
                                       std::optional<double> step = std::nullopt);

void attachColorDialogScrubbing(QColorDialog *picker);

class ScrubLabel : public QLabel {
    Q_OBJECT
    Q_PROPERTY(bool isDragging READ isDragging NOTIFY dragStatusChanged)
    Q_PROPERTY(double sensitivity READ sensitivity WRITE setSensitivity)
public:
    explicit ScrubLabel(const QString &text, QWidget *parent = nullptr);
    ScrubLabel(const QString &text, QDoubleSpinBox *target, double sensitivity = 1.0,
               std::optional<double> step = std::nullopt, QWidget *parent = nullptr);
    ScrubLabel(const QString &text, QSpinBox *target, double sensitivity = 1.0,
               std::optional<double> step = std::nullopt, QWidget *parent = nullptr);
    ScrubLabel(const QString &text, QSlider *target, double sensitivity = 1.0,
               std::optional<double> step = std::nullopt, QWidget *parent = nullptr);
    ~ScrubLabel() override;

    void bindDoubleSpinBox(QDoubleSpinBox *spinBox, double sensitivity = 1.0,
                           std::optional<double> step = std::nullopt);
    void bindSpinBox(QSpinBox *spinBox, double sensitivity = 1.0,
                     std::optional<double> step = std::nullopt);
    void bindSlider(QSlider *slider, double sensitivity = 1.0,
                    std::optional<double> step = std::nullopt);

    void setCallbacks(std::function<double()> getter, std::function<void(double)> setter,
                      double minimum, double maximum, double sensitivity = 1.0,
                      std::optional<double> step = std::nullopt);

    [[nodiscard]] double sensitivity() const { return sensitivity_; }
    void setSensitivity(double s) { sensitivity_ = s; }

    [[nodiscard]] std::optional<double> step() const { return step_; }
    void setStep(std::optional<double> s) { step_ = s; }

    [[nodiscard]] double minimum() const { return minimum_; }
    void setMinimum(double min) { minimum_ = min; }

    [[nodiscard]] double maximum() const { return maximum_; }
    void setMaximum(double max) { maximum_ = max; }

    [[nodiscard]] bool isDragging() const { return dragging_; }
    [[nodiscard]] double currentValue() const;

    void setOnStart(std::function<void()> cb) { onStart_ = std::move(cb); }
    void setOnEnd(std::function<void()> cb) { onEnd_ = std::move(cb); }
    void setOnCancel(std::function<void()> cb) { onCancel_ = std::move(cb); }

    void cancelDrag();

signals:
    void dragStarted();
    void dragEnded();
    void dragCancelled();
    void dragStatusChanged(bool dragging);
    void scrubValueChanged(double value);

protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void init();
    void applyValue(double val);
    void updateCursor(bool hovering);

    QPointer<QWidget> linkedWidget_;
    QPointer<QDoubleSpinBox> spinBox_;
    QPointer<QSpinBox> intSpinBox_;
    QPointer<QSlider> slider_;
    std::function<double()> getter_;
    std::function<void(double)> setter_;
    std::function<void()> onStart_;
    std::function<void()> onEnd_;
    std::function<void()> onCancel_;

    double sensitivity_ = 1.0;
    std::optional<double> step_ = std::nullopt;
    double minimum_ = -1000000.0;
    double maximum_ = 1000000.0;
    double value_ = 0.0;
    double startValue_ = 0.0;
    QPointF pressPoint_;
    bool pressed_ = false;
    bool dragging_ = false;
    bool isHovering_ = false;
};

} // namespace compositor
