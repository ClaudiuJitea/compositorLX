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
#include <QTextBlock>
#include <QTextFragment>
#include <algorithm>
#include <functional>
#include <optional>

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

Q_SIGNALS:
    // The look of the letters changed (a color or face applied, or a preview taken back).
    void lookChanged();

public:
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

    // ---- Per-letter color and face -------------------------------------------------------------------------
    // The editor's document is the working copy of the text's runs: every letter carries its own foreground
    // brush and font family, typed letters inherit from the letter before, and `collectStyle` reads it back.
    QColor fallbackColor = Qt::black;
    QString fallbackFace;

    // Shows `style`: its text with each letter in its own color and face. Runs that do not fit the text the
    // document ends up holding (a paragraph break it normalised) are dropped.
    void loadStyle(const TextStyle &style)
    {
        fallbackColor = QColor::fromRgbF(style.red, style.green, style.blue);
        fallbackFace = style.fontName;
        setPlainText(style.content);
        QTextCursor all(document()); all.select(QTextCursor::Document);
        all.mergeCharFormat(textRunFormat(fallbackColor, fallbackFace));
        applyTextRuns(*document(), style);
        setCurrentCharFormat(textRunFormat(fallbackColor, fallbackFace));
    }

    struct Letters { QVector<QColor> colors; QVector<QString> faces; };

    [[nodiscard]] Letters letters() const
    {
        Letters out;
        const int count = std::max(0, document()->characterCount() - 1);
        out.colors.fill(fallbackColor, count);
        out.faces.fill(fallbackFace, count);
        for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
            for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                const QTextCharFormat format = fragment.charFormat();
                const QColor color = format.hasProperty(QTextFormat::ForegroundBrush) ? format.foreground().color() : fallbackColor;
                const QString face = textFaceOf(format, fallbackFace);
                for (int i = 0; i < fragment.length(); ++i) {
                    const int index = fragment.position() + i;
                    if (index < count) { out.colors[index] = color; out.faces[index] = face; }
                }
            }
            // A paragraph break is a letter too: it has the block's look, or the letter before it when it has none.
            const int separator = block.position() + block.length() - 1;
            if (block.next().isValid() && separator < count) {
                const QTextCharFormat format = block.charFormat();
                const bool hasColor = format.hasProperty(QTextFormat::ForegroundBrush);
                const bool hasFace = format.hasProperty(QTextFormat::FontFamilies) || format.hasProperty(QTextFormat::FontFamily);
                if (hasColor) out.colors[separator] = format.foreground().color();
                else if (separator > 0) out.colors[separator] = out.colors[separator - 1];
                if (hasFace) out.faces[separator] = textFaceOf(format, fallbackFace);
                else if (separator > 0) out.faces[separator] = out.faces[separator - 1];
            }
        }
        return out;
    }

    // The style being edited: `base` (size, alignment, spacing...) with the text and the runs the letters carry.
    // Colors and faces that came from `base` keep their exact values, so an untouched text saves unchanged.
    [[nodiscard]] TextStyle collectStyle(TextStyle base) const
    {
        const QVector<TextStyle::Rgb> known = [&] {
            QVector<TextStyle::Rgb> list{{base.red, base.green, base.blue}};
            if (base.colorRuns) for (const TextColorRun &run : *base.colorRuns) list.append({run.red, run.green, run.blue});
            return list;
        }();
        const auto rgbOf = [&known](const QColor &color) {
            for (const TextStyle::Rgb &candidate : known) if (QColor::fromRgbF(candidate.r, candidate.g, candidate.b) == color) return candidate;
            return TextStyle::Rgb{color.redF(), color.greenF(), color.blueF()};
        };
        const QString oldFace = base.fontName;
        base.content = toPlainText();
        const Letters found = letters();
        if (base.content.isEmpty() || found.colors.size() != base.content.size()) {
            base.colorRuns.reset(); base.fontRuns.reset();
            return base;
        }
        QVector<TextStyle::Rgb> colors; colors.reserve(found.colors.size());
        for (const QColor &color : found.colors) colors.append(rgbOf(color));
        const TextStyle::Rgb original{base.red, base.green, base.blue};
        const TextStyle::Rgb chosen = colors.contains(original) ? original : colors.first();
        base.red = chosen.r; base.green = chosen.g; base.blue = chosen.b;
        base.setUnitColors(colors);
        base.fontName = found.faces.contains(oldFace) ? oldFace : found.faces.first();
        base.setUnitFonts(found.faces);
        return base;
    }

    // The color of the first selected letter, or of the letter before the caret.
    [[nodiscard]] QColor colorAtSelection() const
    {
        const Letters all = letters();
        if (all.colors.isEmpty()) return currentCharFormat().hasProperty(QTextFormat::ForegroundBrush) ? currentCharFormat().foreground().color() : fallbackColor;
        const QTextCursor cursor = textCursor();
        const int index = cursor.hasSelection() ? cursor.selectionStart() : std::max(0, cursor.position() - 1);
        return all.colors.value(std::min(index, int(all.colors.size()) - 1));
    }

    // The one face under the selection (or before the caret), empty when the selection mixes faces.
    [[nodiscard]] QString faceAtSelection() const
    {
        const Letters all = letters();
        if (all.faces.isEmpty()) return textFaceOf(currentCharFormat(), fallbackFace);
        const QTextCursor cursor = textCursor();
        if (!cursor.hasSelection()) return all.faces.value(std::min(std::max(0, cursor.position() - 1), int(all.faces.size()) - 1));
        const int end = std::min<int>(cursor.selectionEnd(), all.faces.size());
        const QString face = all.faces.value(cursor.selectionStart());
        for (int i = cursor.selectionStart() + 1; i < end; ++i) if (all.faces[i] != face) return {};
        return face;
    }

    // Recolors the selected letters, or all the text when nothing is selected; typing continues in that color.
    void applyColor(const QColor &color)
    {
        QTextCharFormat format; format.setForeground(color);
        applyLetterFormat(format);
    }
    void applyFace(const QString &face)
    {
        if (face.isEmpty()) return;
        QTextCharFormat format = textRunFormat(Qt::black, face);
        format.clearProperty(QTextFormat::ForegroundBrush);
        applyLetterFormat(format);
    }

    // A provisional change (the color picker's working color, the face under the menu's pointer) that is taken
    // back by popping its undo step. Each call replaces the previous preview.
    void previewLetterFormat(const std::function<void()> &apply)
    {
        endPreview(false);
        const int before = document()->availableUndoSteps();
        apply();
        previewPushed_ = document()->availableUndoSteps() > before;
        previewSteps_ = document()->availableUndoSteps();
    }
    void endPreview(bool keep)
    {
        if (!previewPushed_) return;
        previewPushed_ = false;
        if (keep || document()->availableUndoSteps() != previewSteps_) return;   // edited since: leave it
        const QTextCursor selection = textCursor();
        document()->undo();
        setTextCursor(selection);
        Q_EMIT lookChanged();
    }
    [[nodiscard]] bool hasPreview() const { return previewPushed_; }

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
        // Spacing lives in the default font, so it never touches the letters' own formats or the undo history.
        QFont spaced = document()->defaultFont();
        spaced.setLetterSpacing(QFont::AbsoluteSpacing, tracking * zoom);
        document()->setDefaultFont(spaced);
        QTextCursor cursor(document());
        cursor.select(QTextCursor::Document);

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
    void applyLetterFormat(const QTextCharFormat &format)
    {
        const QTextCursor selection = textCursor();
        QTextCursor target = selection;
        if (!target.hasSelection()) target.select(QTextCursor::Document);
        target.beginEditBlock();
        target.mergeCharFormat(format);
        target.endEditBlock();
        setTextCursor(selection);
        if (!selection.hasSelection()) mergeCurrentCharFormat(format);
        Q_EMIT lookChanged();
    }

    bool previewPushed_ = false;
    int previewSteps_ = 0;
    QLabel *grip_ = nullptr;
    bool resizing_ = false;
    bool wasResized_ = false;
    bool finishing_ = false;
    QPointF resizeStart_;
    QSize originalSize_;
};

} // namespace compositor
