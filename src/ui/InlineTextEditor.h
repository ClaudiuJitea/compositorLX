#pragma once

#include "CanvasWidget.h"
#include "ShortcutManager.h"
#include "rendering/TextLayout.h"

#include <QTextEdit>
#include <QLabel>
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QPaintEvent>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <algorithm>
#include <functional>

namespace compositor {

class InlineTextEditor final : public QTextEdit {
    Q_OBJECT
public:
    explicit InlineTextEditor(QWidget *parent = nullptr) : QTextEdit(parent)
    {
        setObjectName(QStringLiteral("inlineTextEditor"));
        setAcceptRichText(false);
        setFrameShape(QFrame::NoFrame);
        setAutoFillBackground(false);
        viewport()->setAutoFillBackground(false);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        if (parent) {
            parent->installEventFilter(this);
        }
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *grip = new QLabel(QStringLiteral("⌟"), this);
        grip->setObjectName(QStringLiteral("inlineTextGrip"));
        grip->setAlignment(Qt::AlignCenter);
        grip->setAttribute(Qt::WA_TransparentForMouseEvents);
        grip_ = grip;
    }

    std::function<void(bool)> finished;
    std::function<void(double)> trackingAdjusted;
    std::function<void(double)> leadingAdjusted;

    QRectF canvasBox;
    bool areaText = true;
    double tracking = 0.0;
    double leading = 0.0;

    void syncCanvasGeometry()
    {
        if (canvasBox.isEmpty() || finishing_) return;
        auto *canvas = dynamic_cast<CanvasWidget *>(parentWidget());
        const qreal zoom = canvas ? canvas->zoom() : 1.0;
        const QRect geometry = canvas ? canvas->widgetRectForDocumentRect(canvasBox).toAlignedRect() : canvasBox.toAlignedRect();
        const QMargins margins(qRound(4 * zoom), qRound(3 * zoom), qRound(4 * zoom), qRound(3 * zoom));
        if (viewportMargins() != margins) setViewportMargins(margins);
        setGeometry(geometry);
        const int wrapWidth = std::max(1, qRound((canvasBox.width() - 8) * zoom));
        if (areaText && lineWrapColumnOrWidth() != wrapWidth) setLineWrapColumnOrWidth(wrapWidth);
    }

    void growPointText()
    {
        if (areaText || wasResized_ || finishing_) return;
        auto *canvas = dynamic_cast<CanvasWidget *>(parentWidget());
        const qreal zoom = canvas ? canvas->zoom() : 1.0;
        const QSizeF content = document()->size();
        canvasBox.setSize(QSizeF(std::max(40.0, document()->idealWidth() / zoom + 8),
                                 std::max(20.0, content.height() / zoom + 6)));
        syncCanvasGeometry();
    }

    [[nodiscard]] bool wasResized() const { return wasResized_; }

    void finish(bool commit)
    {
        if (finishing_) return;
        finishing_ = true;
        // A replacement editor may be created before deferred deletion runs.
        setObjectName(QString());
        hide();
        if (parentWidget()) parentWidget()->setFocus(Qt::OtherFocusReason);
        if (finished) finished(commit);
        deleteLater();
    }

    void adjustTracking(double delta)
    {
        tracking = std::clamp(tracking + delta, -100.0, 1000.0);
        if (trackingAdjusted) trackingAdjusted(delta);
        applySpacing();
    }

    void adjustLeading(double delta)
    {
        leading = std::max(0.0, leading + delta);
        if (leadingAdjusted) leadingAdjusted(delta);
        applySpacing();
    }

    void applySpacing()
    {
        auto *canvas = dynamic_cast<CanvasWidget *>(parentWidget());
        const qreal zoom = canvas ? canvas->zoom() : 1.0;
        QTextCursor cursor(document());
        cursor.select(QTextCursor::Document);

        QTextCharFormat charFormat;
        charFormat.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        charFormat.setFontLetterSpacing(tracking * zoom);
        cursor.mergeCharFormat(charFormat);

        QTextBlockFormat blockFormat;
        if (leading > 0.0) {
            blockFormat.setLineHeight(leading * zoom, QTextBlockFormat::MinimumHeight);
        }
        cursor.mergeBlockFormat(blockFormat);

        growPointText();
        syncCanvasGeometry();
        if (viewport()) viewport()->update();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget() && (event->type() == QEvent::Resize || event->type() == QEvent::Paint)) {
            syncCanvasGeometry();
        }
        return QTextEdit::eventFilter(watched, event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        const auto translated = ShortcutManager::instance().translateTextKeyEvent(event);
        if (translated.suppressed) {
            event->accept();
            return;
        }

        const int effectiveKey = translated.key;
        const Qt::KeyboardModifiers effectiveMods = translated.modifiers;

        if (effectiveKey == Qt::Key_Escape) {
            finish(false);
            event->accept();
            return;
        }
        if ((effectiveKey == Qt::Key_Return || effectiveKey == Qt::Key_Enter) && (effectiveMods & Qt::ControlModifier)) {
            finish(true);
            event->accept();
            return;
        }

        if (effectiveMods & Qt::AltModifier) {
            const double step = (effectiveMods & Qt::ShiftModifier) ? 10.0 : 1.0;
            if (effectiveKey == Qt::Key_Left) {
                adjustTracking(-step);
                event->accept();
                return;
            }
            if (effectiveKey == Qt::Key_Right) {
                adjustTracking(step);
                event->accept();
                return;
            }
            if (effectiveKey == Qt::Key_Up) {
                adjustLeading(-step);
                event->accept();
                return;
            }
            if (effectiveKey == Qt::Key_Down) {
                adjustLeading(step);
                event->accept();
                return;
            }
        }

        QTextEdit::keyPressEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (QRect(width() - 18, height() - 18, 18, 18).contains(event->position().toPoint())) {
            resizing_ = true;
            resizeStart_ = event->globalPosition();
            originalSize_ = size();
            setCursor(Qt::SizeFDiagCursor);
            event->accept();
            return;
        }
        QTextEdit::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (resizing_) {
            const QPointF delta = event->globalPosition() - resizeStart_;
            resize(std::max(120, originalSize_.width() + qRound(delta.x())),
                   std::max(48, originalSize_.height() + qRound(delta.y())));
            event->accept();
            return;
        }
        setCursor(QRect(width() - 18, height() - 18, 18, 18).contains(event->position().toPoint())
                  ? Qt::SizeFDiagCursor : Qt::IBeamCursor);
        QTextEdit::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (resizing_) {
            resizing_ = false;
            wasResized_ = true;
            areaText = true;
            auto *canvas = dynamic_cast<CanvasWidget *>(parentWidget());
            if (canvas) {
                canvasBox.setSize(canvas->documentRectForWidgetRect(geometry()).size());
            }
            setWordWrapMode(QTextOption::WordWrap);
            setLineWrapMode(QTextEdit::FixedPixelWidth);
            syncCanvasGeometry();
            setCursor(Qt::IBeamCursor);
            event->accept();
            return;
        }
        QTextEdit::mouseReleaseEvent(event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QTextEdit::resizeEvent(event);
        if (grip_) grip_->setGeometry(width() - 18, height() - 18, 16, 16);
    }

    void paintEvent(QPaintEvent *event) override
    {
        QTextEdit::paintEvent(event);
        QPainter painter(viewport());
        painter.setPen(QPen(QColor(94, 167, 242), 1));
        painter.drawRect(viewport()->rect().adjusted(0, 0, -1, -1));
    }

private:
    QLabel *grip_ = nullptr;
    bool resizing_ = false;
    bool wasResized_ = false;
    bool finishing_ = false;
    QPointF resizeStart_;
    QSize originalSize_;
};

} // namespace compositor
