#pragma once

#include "core/Document.h"

#include <QAbstractTextDocumentLayout>
#include <QImage>
#include <QPainter>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextBlock>

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

// Document-wide layout without per-letter formatting: size, weight, spacing and alignment come from the default
// font and options, so each letter keeps its own color and face (the inline editor uses this).
inline void layoutTextDocument(QTextDocument &document, const QFont &font, int alignment, bool wrap, double leading = 0.0)
{
    document.setDocumentMargin(0);
    document.setDefaultFont(font);
    QTextOption option = document.defaultTextOption();
    option.setWrapMode(wrap ? QTextOption::WordWrap : QTextOption::NoWrap);
    option.setAlignment(alignment == 1 ? Qt::AlignHCenter : alignment == 2 ? Qt::AlignRight : Qt::AlignLeft);
    document.setDefaultTextOption(option);
    QTextCursor cursor(&document); cursor.select(QTextCursor::Document);
    QTextBlockFormat block;
    block.setAlignment(option.alignment());
    if (leading > 0.0) block.setLineHeight(leading, QTextBlockFormat::MinimumHeight);
    cursor.mergeBlockFormat(block);
}

inline QColor textRgbColor(const TextStyle::Rgb &rgb) { return QColor::fromRgbF(rgb.r, rgb.g, rgb.b); }

[[nodiscard]] inline QTextCharFormat textRunFormat(const QColor &color, const QString &face)
{
    QTextCharFormat format;
    format.setForeground(color);
    format.setFontFamilies({face});
    format.setProperty(QTextFormat::FontFamily, face);
    return format;
}

inline QString textFaceOf(const QTextCharFormat &format, const QString &fallback)
{
    if (format.hasProperty(QTextFormat::FontFamilies)) {
        const QStringList families = format.property(QTextFormat::FontFamilies).toStringList();
        if (!families.isEmpty()) return families.first();
    }
    if (format.hasProperty(QTextFormat::FontFamily)) return format.stringProperty(QTextFormat::FontFamily);
    return fallback;
}

// Gives every letter of the document its own color and face. Runs that do not fit the document's text
// (a paragraph break the document normalised, say) are ignored rather than misapplied.
inline void applyTextRuns(QTextDocument &document, const TextStyle &style)
{
    if (!style.colorRuns && !style.fontRuns) return;
    const int count = style.content.size();
    if (count == 0 || document.characterCount() - 1 != count) return;
    const QVector<TextStyle::Rgb> colors = style.unitColors();
    const QVector<QString> faces = style.unitFonts();
    QTextCursor cursor(&document);
    for (int start = 0; start < count;) {
        int end = start + 1;
        while (end < count && colors[end] == colors[start] && faces[end] == faces[start]) ++end;
        cursor.setPosition(start); cursor.setPosition(end, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(textRunFormat(textRgbColor(colors[start]), faces[start]));
        start = end;
    }
}

inline QImage drawTextDocument(QTextDocument &document, const QSize &box, bool areaText)
{
    document.setTextWidth(areaText ? std::max(1, box.width() - 8) : -1);
    const QSize size = areaText ? box : QSize(qCeil(document.idealWidth()) + 8, qCeil(document.size().height()) + 6);
    if (size.width() < 1 || size.height() < 1 || size.width() > 30000 || size.height() > 30000
        || qint64(size.width()) * size.height() > 100000000LL) return {};
    QImage image(size, QImage::Format_RGBA8888_Premultiplied); image.fill(Qt::transparent);
    QPainter painter(&image); painter.translate(4, 3);
    document.drawContents(&painter, QRectF(0, 0, size.width() - 8, size.height() - 6));
    return image;
}

inline QImage renderText(const QString &text, const QSize &box, const QFont &font, const QColor &color, int alignment, bool areaText, double tracking = 0.0, double leading = 0.0)
{
    QTextDocument document; document.setPlainText(text);
    formatTextDocument(document, font, color, alignment, areaText, tracking, leading);
    return drawTextDocument(document, box, areaText);
}

// Renders a style with each letter in its own color and face. `font` carries size, weight and slant (its family
// is the style's base face); tracking and leading come from the style.
inline QImage renderText(const TextStyle &style, const QSize &box, QFont font, int alignment, bool areaText)
{
    if (style.tracking != 0.0) font.setLetterSpacing(QFont::AbsoluteSpacing, style.tracking);
    QTextDocument document; document.setPlainText(style.content);
    formatTextDocument(document, font, QColor::fromRgbF(style.red, style.green, style.blue), alignment, areaText, style.tracking, style.leading);
    applyTextRuns(document, style);
    return drawTextDocument(document, box, areaText);
}
} // namespace compositor
