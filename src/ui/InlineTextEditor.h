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
        viewport()->setMouseTracking(true);
        setMouseTracking(true);
        // The box's outline, its eight handles and the overflow marker are drawn over the whole editor, margins included.
        auto *overlay = new BoxOverlay(this);
        overlay->setObjectName(QStringLiteral("inlineTextGrip"));
        overlay->setAttribute(Qt::WA_TransparentForMouseEvents);
        overlay->paintBox = [this](QPainter &painter) { paintBox(painter); };
        grip_ = overlay;
        connect(this, &QTextEdit::textChanged, overlay, [overlay] { overlay->update(); });
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
    double fontSize = 48.0;   // in layer pixels; a leading of 0 is Auto, 120% of it

    void syncCanvasGeometry()
    {
        if (canvasBox.isEmpty() || finishing_) return;
        auto *canvas = dynamic_cast<CanvasWidget *>(parentWidget());
        const qreal zoom = canvas ? canvas->zoom() : 1.0;
        const QRect geometry = canvas ? canvas->widgetRectForDocumentRect(canvasBox).toAlignedRect() : canvasBox.toAlignedRect();
        // The letters sit kTextPadding inside the box, as in the layer raster; a fixed line height puts the first baseline
        // a little off where the document would, which the top margin makes up.
        const double lineHeight = document()->firstBlock().blockFormat().lineHeight();
        const double shift = lineHeight > 0 ? baselineShift(*document(), lineHeight) : 0.0;
        const QMargins margins(qRound(kTextPadding * zoom), std::max(0, qRound(kTextPadding * zoom + shift)),
                               qRound(kTextPadding * zoom), qRound(kTextPadding * zoom));
        if (viewportMargins() != margins) setViewportMargins(margins);
        setGeometry(geometry);
        const int wrapWidth = std::max(1, qRound((canvasBox.width() - 2 * kTextPadding) * zoom));
        if (areaText && lineWrapColumnOrWidth() != wrapWidth) setLineWrapColumnOrWidth(wrapWidth);
    }

    void growPointText()
    {
        if (areaText || wasResized_ || finishing_) return;
        auto *canvas = dynamic_cast<CanvasWidget *>(parentWidget());
        const qreal zoom = canvas ? canvas->zoom() : 1.0;
        const double lineHeight = std::max(1.0, document()->firstBlock().blockFormat().lineHeight());
        // The text measures in the document's (zoomed) pixels; the padding is in layer pixels.
        const double width = (document()->idealWidth() + std::max(1, document()->defaultFont().pixelSize()) * 0.1) / zoom + 2 * kTextPadding;
        const double height = std::max(document()->size().height() / zoom, lineHeight / zoom) + 2 * kTextPadding;
        canvasBox.setSize(QSizeF(std::max(16.0, std::ceil(width)), std::max(16.0, std::ceil(height))));
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

    // The edge or corner a point (in this widget's coordinates) is on, in handle order top-left, top, top-right, right,
    // bottom-right, bottom, bottom-left, left: a band along each edge, as macOS's box has, rather than only the squares.
    // -1 anywhere else, which is the text.
    [[nodiscard]] int handleAt(const QPoint &point) const
    {
        const int reach = std::max(2, std::min(10, std::min(width(), height()) / 3));
        if (point.x() < -reach || point.x() > width() + reach || point.y() < -reach || point.y() > height() + reach) return -1;
        const bool left = point.x() <= reach, right = point.x() >= width() - reach;
        const bool top = point.y() <= reach, bottom = point.y() >= height() - reach;
        if (left && top) return 0;
        if (right && top) return 2;
        if (right && bottom) return 4;
        if (left && bottom) return 6;
        if (top) return 1;
        if (right) return 3;
        if (bottom) return 5;
        if (left) return 7;
        return -1;
    }


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

    // Up closes the lines up, down opens them out, counting from whatever Auto works out to (as macOS does).
    void adjustLeading(double delta)
    {
        const double current = textLineHeight(fontSize, leading);
        leading = delta < 0 ? std::max(1.0, current + delta) : std::min(5000.0, current + delta);
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
        blockFormat.setLineHeight(textLineHeight(fontSize, leading) * zoom, QTextBlockFormat::FixedHeight);
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

    static Qt::CursorShape cursorForHandle(int handle)
    {
        switch (handle) {
        case 0: case 4: return Qt::SizeFDiagCursor;
        case 2: case 6: return Qt::SizeBDiagCursor;
        case 1: case 5: return Qt::SizeVerCursor;
        case 3: case 7: return Qt::SizeHorCursor;
        default: return Qt::IBeamCursor;
        }
    }

    // The margins around the text belong to this widget, not to its viewport, and a scroll area drops mouse events there:
    // the box's edges are in them, so they are handled here.
    bool event(QEvent *e) override
    {
        switch (e->type()) {
        case QEvent::MouseButtonPress: mousePressEvent(static_cast<QMouseEvent *>(e)); return true;
        case QEvent::MouseMove: mouseMoveEvent(static_cast<QMouseEvent *>(e)); return true;
        case QEvent::MouseButtonRelease: mouseReleaseEvent(static_cast<QMouseEvent *>(e)); return true;
        default: return QTextEdit::event(e);
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const int handle = event->button() == Qt::LeftButton ? handleAt(mapFromGlobal(event->globalPosition().toPoint())) : -1;
        if (handle >= 0) {
            resizeHandle_ = handle;
            resizeStart_ = event->globalPosition();
            originalGeometry_ = geometry();
            event->accept();
            return;
        }
        QTextEdit::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (resizeHandle_ >= 0) {
            const QPointF delta = event->globalPosition() - resizeStart_;
            auto *canvas = dynamic_cast<CanvasWidget *>(parentWidget());
            const qreal zoom = canvas ? canvas->zoom() : 1.0;
            const int minimum = std::max(1, qRound(16 * zoom));
            QRect rect = originalGeometry_;
            const int dx = qRound(delta.x()), dy = qRound(delta.y());
            const int h = resizeHandle_;
            if (h == 0 || h == 6 || h == 7) rect.setLeft(std::min(rect.left() + dx, rect.right() - minimum + 1));
            if (h == 2 || h == 3 || h == 4) rect.setRight(std::max(rect.right() + dx, rect.left() + minimum - 1));
            if (h == 0 || h == 1 || h == 2) rect.setTop(std::min(rect.top() + dy, rect.bottom() - minimum + 1));
            if (h == 4 || h == 5 || h == 6) rect.setBottom(std::max(rect.bottom() + dy, rect.top() + minimum - 1));
            // A box holds its text and wraps it rather than scaling it, whatever it was before.
            wasResized_ = true;
            areaText = true;
            if (canvas) canvasBox = canvas->documentRectForWidgetRect(QRectF(rect));
            else canvasBox = QRectF(rect);
            setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            setLineWrapMode(QTextEdit::FixedPixelWidth);
            syncCanvasGeometry();
            event->accept();
            return;
        }
        const int handle = handleAt(mapFromGlobal(event->globalPosition().toPoint()));
        setCursor(cursorForHandle(handle));
        viewport()->setCursor(cursorForHandle(handle));
        QTextEdit::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (resizeHandle_ >= 0) {
            resizeHandle_ = -1;
            setCursor(Qt::IBeamCursor);
            viewport()->setCursor(Qt::IBeamCursor);
            event->accept();
            return;
        }
        QTextEdit::mouseReleaseEvent(event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QTextEdit::resizeEvent(event);
        if (grip_) grip_->setGeometry(rect());
    }

private:
    // A transparent layer over the editor that draws the box: see paintBox.
    class BoxOverlay final : public QWidget {
    public:
        explicit BoxOverlay(QWidget *parent) : QWidget(parent) {}
        std::function<void(QPainter &)> paintBox;
    protected:
        void paintEvent(QPaintEvent *) override { QPainter painter(this); if (paintBox) paintBox(painter); }
    };

    // Text that does not fit the box is marked by a plus in the bottom-right handle, as in Photoshop.
    [[nodiscard]] bool overflows() const
    {
        if (!areaText) return false;
        auto *canvas = dynamic_cast<CanvasWidget *>(parentWidget());
        const qreal zoom = canvas ? canvas->zoom() : 1.0;
        const double room = (canvasBox.height() - 2 * kTextPadding) * zoom;
        return document()->size().height() > room + 0.5;
    }

    void paintBox(QPainter &painter)
    {
        const QColor accent(94, 167, 242);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(accent, 1));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
        const int size = 6;
        const QPoint corners[8] = {rect().topLeft(), QPoint(width() / 2, 0), rect().topRight(), QPoint(width() - 1, height() / 2),
                                   rect().bottomRight(), QPoint(width() / 2, height() - 1), rect().bottomLeft(), QPoint(0, height() / 2)};
        for (const QPoint &center : corners) {
            const QRect handle(center.x() - size / 2, center.y() - size / 2, size, size);
            painter.fillRect(handle, Qt::white);
            painter.setPen(QPen(accent, 1));
            painter.drawRect(handle);
        }
        if (overflows()) {
            const QPoint c = corners[4];
            painter.setPen(QPen(Qt::black, 1));
            painter.drawLine(c.x() - 2, c.y(), c.x() + 2, c.y());
            painter.drawLine(c.x(), c.y() - 2, c.x(), c.y() + 2);
        }
    }

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
    QWidget *grip_ = nullptr;
    int resizeHandle_ = -1;
    bool wasResized_ = false;
    bool finishing_ = false;
    QPointF resizeStart_;
    QRect originalGeometry_;
};

} // namespace compositor
