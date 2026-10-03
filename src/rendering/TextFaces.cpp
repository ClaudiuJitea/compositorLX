#include "rendering/TextFaces.h"

#include <QFontDatabase>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace compositor {
namespace {

QString normalized(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) if (c.isLetterOrNumber()) out.append(c.toLower());
    return out;
}

struct FaceIndex {
    QHash<QString, QString> families;   // normalized family -> family
    QHash<QString, TextFace> byName;    // normalized "<family><style>" and "<family>" -> face
};

QMutex &indexMutex() { static QMutex mutex; return mutex; }
FaceIndex *&indexPointer() { static FaceIndex *index = nullptr; return index; }

const FaceIndex &faceIndex()
{
    FaceIndex *&index = indexPointer();
    if (index) return *index;
    index = new FaceIndex;
    for (const QString &family : QFontDatabase::families()) {
        index->families.insert(normalized(family), family);
        TextFace plain; plain.family = family; plain.installed = true;
        index->byName.insert(normalized(family), plain);
        for (const QString &style : QFontDatabase::styles(family)) {
            TextFace face;
            face.family = family; face.styleName = style; face.installed = true;
            face.weight = QFontDatabase::weight(family, style);
            face.italic = QFontDatabase::italic(family, style);
            const QString key = normalized(family) + normalized(style);
            if (!index->byName.contains(key)) index->byName.insert(key, face);
        }
    }
    return *index;
}

struct StyleTraits { int weight = QFont::Normal; bool italic = false; };

StyleTraits traitsOf(QString style)
{
    style = style.toLower();
    StyleTraits traits;
    traits.italic = style.contains(QLatin1String("italic")) || style.contains(QLatin1String("oblique")) || style.contains(QLatin1String("ital"));
    struct Token { const char *text; int weight; };
    static const Token tokens[] = {
        {"extrabold", QFont::ExtraBold}, {"ultrabold", QFont::ExtraBold}, {"semibold", QFont::DemiBold}, {"demibold", QFont::DemiBold},
        {"demi", QFont::DemiBold}, {"extralight", QFont::ExtraLight}, {"ultralight", QFont::ExtraLight}, {"black", QFont::Black},
        {"heavy", QFont::Black}, {"bold", QFont::Bold}, {"medium", QFont::Medium}, {"light", QFont::Light}, {"thin", QFont::Thin},
    };
    for (const Token &token : tokens) {
        if (style.contains(QLatin1String(token.text))) { traits.weight = token.weight; break; }
    }
    return traits;
}

QString stripPostScriptSuffix(QString name)
{
    for (const char *suffix : {"psmt", "mt", "ps"}) {
        if (name.endsWith(QLatin1String(suffix)) && name.size() > int(strlen(suffix))) {
            name.chop(int(strlen(suffix)));
            break;
        }
    }
    return name;
}

QString splitCamelCase(const QString &name)
{
    QString out;
    for (int i = 0; i < name.size(); ++i) {
        if (i > 0 && name[i].isUpper() && (name[i - 1].isLower() || name[i - 1].isDigit())) out.append(QLatin1Char(' '));
        out.append(name[i]);
    }
    return out;
}

} // namespace

void resetTextFaceCache()
{
    QMutexLocker lock(&indexMutex());
    delete indexPointer();
    indexPointer() = nullptr;
}

TextFace resolveTextFace(const QString &rawName)
{
    const QString name = rawName.trimmed();
    QMutexLocker lock(&indexMutex());
    const FaceIndex &index = faceIndex();
    const QString key = normalized(name);
    if (const auto it = index.byName.constFind(key); it != index.byName.constEnd()) return it.value();

    // "Family-Style" (PostScript) with whatever Adobe/Apple suffixes: Arial-BoldMT, TimesNewRomanPS-ItalicMT.
    const int dash = name.lastIndexOf(QLatin1Char('-'));
    const QString familyPart = dash > 0 ? name.left(dash) : name;
    const QString stylePart = dash > 0 ? stripPostScriptSuffix(normalized(name.mid(dash + 1))) : QString();
    const StyleTraits traits = traitsOf(stylePart);

    QString familyKey = normalized(familyPart);
    QString family = index.families.value(familyKey);
    if (family.isEmpty()) family = index.families.value(stripPostScriptSuffix(familyKey));
    TextFace face;
    face.weight = traits.weight;
    face.italic = traits.italic;
    if (!family.isEmpty()) {
        face.family = family;
        face.installed = true;
        // Prefer the installed style that is exactly this weight and slant, so synthesized bold is not used.
        int bestDistance = 1000;
        for (const QString &style : QFontDatabase::styles(family)) {
            if (QFontDatabase::italic(family, style) != traits.italic) continue;
            const int distance = std::abs(QFontDatabase::weight(family, style) - traits.weight);
            if (distance < bestDistance) { bestDistance = distance; face.styleName = style; }
        }
        if (bestDistance > 150) face.styleName.clear();
        return face;
    }
    // Names like "AvenirNext" are written with the camel-case run together; a single word (Helvetica) stays as it is.
    QString pretty = familyPart.trimmed();
    for (const char *suffix : {"PSMT", "MT", "PS"}) {
        if (pretty.endsWith(QLatin1String(suffix)) && pretty.size() > int(strlen(suffix))) { pretty.chop(int(strlen(suffix))); break; }
    }
    face.family = splitCamelCase(pretty);
    face.installed = false;
    return face;
}

QFont textFontForFace(const QString &faceName, double pixelSize)
{
    const TextFace face = resolveTextFace(faceName);
    QFont font(face.family);
    font.setWeight(static_cast<QFont::Weight>(face.weight));
    font.setItalic(face.italic);
    if (!face.styleName.isEmpty()) font.setStyleName(face.styleName);
    font.setPixelSize(std::max(1, int(std::lround(pixelSize))));
    return font;
}

QString composeTextFace(const QString &faceName, bool bold, bool italic)
{
    const TextFace current = resolveTextFace(faceName);
    const int target = bold ? int(QFont::Bold) : (current.weight >= QFont::DemiBold ? int(QFont::Normal) : current.weight);
    if (current.installed) {
        QString best;
        int bestDistance = 1000;
        for (const QString &style : QFontDatabase::styles(current.family)) {
            if (QFontDatabase::italic(current.family, style) != italic) continue;
            const int distance = std::abs(QFontDatabase::weight(current.family, style) - target);
            if (distance < bestDistance) { bestDistance = distance; best = style; }
        }
        if (!best.isEmpty() && bestDistance <= 150) {
            if (!italic && target == int(QFont::Normal) && QFontDatabase::weight(current.family, best) == int(QFont::Normal)) return current.family;
            QString family = current.family;
            QString style = best;
            family.remove(QLatin1Char(' ')); style.remove(QLatin1Char(' '));
            return family + QLatin1Char('-') + style;
        }
    }
    // Not installed (or the family has no such style): the conventional PostScript spelling.
    QString base = faceName.trimmed();
    if (const int dash = base.lastIndexOf(QLatin1Char('-')); dash > 0) base = base.left(dash);
    const QString lowered = normalized(base);
    const bool oblique = lowered.startsWith(QLatin1String("helvetica")) || lowered.startsWith(QLatin1String("courier"));
    if (!bold && !italic) return base;
    if (bold && italic) return base + (oblique ? QStringLiteral("-BoldOblique") : QStringLiteral("-BoldItalic"));
    if (bold) return base + QStringLiteral("-Bold");
    return base + (oblique ? QStringLiteral("-Oblique") : QStringLiteral("-Italic"));
}

} // namespace compositor
