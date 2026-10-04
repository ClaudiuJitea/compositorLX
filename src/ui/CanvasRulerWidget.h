#pragma once

#include "core/Document.h"
#include "core/EditorSession.h"

#include <QWidget>

namespace compositor {

class CanvasWidget;

class CanvasRulerCornerWidget final : public QWidget {
    Q_OBJECT
public:
    explicit CanvasRulerCornerWidget(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;
};

class CanvasRulerWidget final : public QWidget {
    Q_OBJECT
public:
    CanvasRulerWidget(CanvasGuide::Axis axis, EditorSession &session, CanvasWidget *canvas, QWidget *parent = nullptr);

    [[nodiscard]] CanvasGuide::Axis axis() const { return axis_; }

    static double majorStep(double pointsPerPixel);
    static QString label(double value);

signals:
    void guideChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void applyThickness();
    CanvasGuide::Axis axis_;
    EditorSession &session_;
    CanvasWidget *canvas_ = nullptr;
    bool dragging_ = false;
};

} // namespace compositor
