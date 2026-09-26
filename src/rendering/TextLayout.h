#pragma once

#include <QAbstractTextDocumentLayout>
#include <QImage>
#include <QPainter>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>

namespace compositor {

inline void formatTextDocument(QTextDocument &document, const QFont &font, const QColor &color, int alignment, bool wrap, double tracking = 0.0, double leading = 0.0)
{
    document.setDocumentMargin(0);
    QFont styledFont = font;
    if (tracking != 0.0) {
        styledFont.setLetterSpacing(QFont::AbsoluteSpacing, tracking);
    }
    document.setDefaultFont(styledFont);
    QTextOption option = document.defaultTextOption();
    option.setWrapMode(wrap ? QTextOption::WordWrap : QTextOption::NoWrap);
    option.setAlignment(alignment == 1 ? Qt::AlignHCenter : alignment == 2 ? Qt::AlignRight : Qt::AlignLeft);
    document.setDefaultTextOption(option);
    QTextCursor cursor(&document); cursor.select(QTextCursor::Document);
    QTextCharFormat style; style.setFont(styledFont); style.setForeground(color);
    if (tracking != 0.0) {
        style.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        style.setFontLetterSpacing(tracking);
    }
    cursor.mergeCharFormat(style);
    QTextBlockFormat block;
    block.setAlignment(option.alignment());
    if (leading > 0.0) {
        block.setLineHeight(leading, QTextBlockFormat::MinimumHeight);
    }
    cursor.mergeBlockFormat(block);
}

inline QImage renderText(const QString &text, const QSize &box, const QFont &font, const QColor &color, int alignment, bool areaText, double tracking = 0.0, double leading = 0.0)
{
    QTextDocument document; document.setPlainText(text);
    formatTextDocument(document, font, color, alignment, areaText, tracking, leading);
    document.setTextWidth(areaText ? std::max(1, box.width() - 8) : -1);
    const QSize size = areaText ? box : QSize(qCeil(document.idealWidth()) + 8, qCeil(document.size().height()) + 6);
    if (size.width() < 1 || size.height() < 1 || size.width() > 30000 || size.height() > 30000
        || qint64(size.width()) * size.height() > 100000000LL) return {};
    QImage image(size, QImage::Format_RGBA8888_Premultiplied); image.fill(Qt::transparent);
    QPainter painter(&image); painter.translate(4, 3);
    document.drawContents(&painter, QRectF(0, 0, size.width() - 8, size.height() - 6));
    return image;
}
} // namespace compositor
