#include "io/PSDText.h"

#include "rendering/TextFaces.h"
#include "rendering/TextLayout.h"

#include <QFontMetricsF>

#include <cmath>
#include <cstring>
#include <memory>
#include <map>
#include <vector>

namespace compositor {
namespace {

// ---- The text engine dictionary: a small PostScript-like subset (<< >>, arrays, names, numbers, strings) ----------
struct Engine {
    enum Kind { None, Number, Bool, String, Dict, Array } kind = None;
    double number = 0.0;
    bool flag = false;
    QString string;
    std::map<QString, Engine> dict;
    std::vector<Engine> array;
};

const Engine *walk(const Engine *value, std::initializer_list<const char *> keys)
{
    const Engine *current = value;
    for (const char *key : keys) {
        if (!current || current->kind != Engine::Dict) return nullptr;
        const auto it = current->dict.find(QString::fromLatin1(key));
        if (it == current->dict.end()) return nullptr;
        current = &it->second;
    }
    return current;
}
std::optional<double> numberOf(const Engine *value) { if (value && value->kind == Engine::Number) return value->number; return std::nullopt; }
std::optional<bool> boolOf(const Engine *value) { if (value && value->kind == Engine::Bool) return value->flag; return std::nullopt; }
std::optional<QString> stringOf(const Engine *value) { if (value && value->kind == Engine::String) return value->string; return std::nullopt; }
const std::vector<Engine> &arrayOf(const Engine *value)
{
    static const std::vector<Engine> none;
    return value && value->kind == Engine::Array ? value->array : none;
}

class EngineCursor {
public:
    explicit EngineCursor(const QByteArray &data, int start = 0) : bytes_(data), index_(start) {}

    std::optional<Engine> parseValue()
    {
        skipWhitespace();
        if (index_ >= bytes_.size()) return std::nullopt;
        const char byte = bytes_[index_];
        if (byte == '<') {
            if (peekAt(1) == '<') return parseDictionary();
            return parseHex();
        }
        if (byte == '[') return parseArray();
        if (byte == '(') return parseString();
        if (byte == '/') {
            ++index_;
            Engine e; e.kind = Engine::String; e.string = readToken(); return e;
        }
        if (byte == '-' || byte == '+' || byte == '.' || (byte >= '0' && byte <= '9')) {
            const auto number = parseNumber();
            if (!number) return std::nullopt;
            Engine e; e.kind = Engine::Number; e.number = *number; return e;
        }
        if (takeWord("true")) { Engine e; e.kind = Engine::Bool; e.flag = true; return e; }
        if (takeWord("false")) { Engine e; e.kind = Engine::Bool; e.flag = false; return e; }
        if (takeWord("null")) { Engine e; e.kind = Engine::String; return e; }
        return std::nullopt;
    }

    std::optional<Engine> parseDictionary()
    {
        if (!take("<<")) return std::nullopt;
        Engine out; out.kind = Engine::Dict;
        while (true) {
            skipWhitespace();
            if (index_ >= bytes_.size() || bytes_[index_] == '>') break;
            if (bytes_[index_] != '/') return std::nullopt;
            ++index_;
            const QString key = readToken();
            auto value = parseValue();
            if (!value) return std::nullopt;
            out.dict[key] = std::move(*value);
        }
        if (!take(">>")) return std::nullopt;
        return out;
    }

    std::optional<Engine> parseArray()
    {
        if (!take("[")) return std::nullopt;
        Engine out; out.kind = Engine::Array;
        while (true) {
            skipWhitespace();
            if (index_ >= bytes_.size() || bytes_[index_] == ']') break;
            auto value = parseValue();
            if (!value) return std::nullopt;
            out.array.push_back(std::move(*value));
        }
        if (!take("]")) return std::nullopt;
        return out;
    }

    std::optional<double> parseNumber()
    {
        const int start = index_;
        const auto digit = [this] { return index_ < bytes_.size() && bytes_[index_] >= '0' && bytes_[index_] <= '9'; };
        if (index_ < bytes_.size() && (bytes_[index_] == '+' || bytes_[index_] == '-')) ++index_;
        while (digit()) ++index_;
        if (index_ < bytes_.size() && bytes_[index_] == '.') { ++index_; while (digit()) ++index_; }
        if (index_ < bytes_.size() && (bytes_[index_] == 'e' || bytes_[index_] == 'E')) {
            ++index_;
            if (index_ < bytes_.size() && (bytes_[index_] == '+' || bytes_[index_] == '-')) ++index_;
            while (digit()) ++index_;
        }
        if (index_ <= start) return std::nullopt;
        bool ok = false;
        const double value = bytes_.mid(start, index_ - start).toDouble(&ok);
        if (!ok) return std::nullopt;
        return value;
    }

    std::optional<Engine> parseString()
    {
        if (!take("(")) return std::nullopt;
        QByteArray raw;
        while (index_ < bytes_.size()) {
            const char byte = bytes_[index_++];
            if (byte == ')') break;
            if (byte == '\\') {
                if (index_ >= bytes_.size()) return std::nullopt;
                const char escaped = bytes_[index_++];
                if (escaped == 'n') raw.append('\n');
                else if (escaped == 'r') raw.append('\r');
                else if (escaped == 't') raw.append('\t');
                else if (escaped >= '0' && escaped <= '7') {
                    int value = escaped - '0';
                    for (int i = 0; i < 2; ++i) {
                        if (index_ >= bytes_.size() || bytes_[index_] < '0' || bytes_[index_] > '7') break;
                        value = value * 8 + (bytes_[index_++] - '0');
                    }
                    raw.append(char(value & 0xFF));
                } else if (escaped != '\n' && escaped != '\r') raw.append(escaped);
            } else raw.append(byte);
        }
        Engine e; e.kind = Engine::String; e.string = decodeEngine(raw); return e;
    }

    std::optional<Engine> parseHex()
    {
        if (!take("<")) return std::nullopt;
        QByteArray nibbles;
        while (index_ < bytes_.size() && bytes_[index_] != '>') {
            const char byte = bytes_[index_++];
            int nibble = -1;
            if (byte >= '0' && byte <= '9') nibble = byte - '0';
            else if (byte >= 'a' && byte <= 'f') nibble = byte - 'a' + 10;
            else if (byte >= 'A' && byte <= 'F') nibble = byte - 'A' + 10;
            if (nibble >= 0) nibbles.append(char(nibble));
        }
        if (!take(">")) return std::nullopt;
        QByteArray raw;
        for (int i = 0; i + 1 < nibbles.size(); i += 2) raw.append(char((nibbles[i] << 4) | nibbles[i + 1]));
        Engine e; e.kind = Engine::String; e.string = decodeEngine(raw); return e;
    }

private:
    static QString decodeEngine(const QByteArray &raw)
    {
        if (raw.size() >= 2 && quint8(raw[0]) == 0xFE && quint8(raw[1]) == 0xFF) {
            QString out;
            for (int i = 2; i + 1 < raw.size(); i += 2) out.append(QChar(ushort((quint8(raw[i]) << 8) | quint8(raw[i + 1]))));
            return out;
        }
        return QString::fromLatin1(raw);
    }
    static bool isDelimiter(char byte)
    {
        return quint8(byte) <= 0x20 || byte == '/' || byte == '<' || byte == '>' || byte == '[' || byte == ']' || byte == '(' || byte == ')';
    }
    QString readToken()
    {
        const int start = index_;
        while (index_ < bytes_.size() && !isDelimiter(bytes_[index_])) ++index_;
        return QString::fromLatin1(bytes_.mid(start, index_ - start));
    }
    bool takeWord(const char *word)
    {
        const QByteArray w(word);
        if (bytes_.mid(index_, w.size()) != w) return false;
        const int after = index_ + w.size();
        if (after < bytes_.size() && !isDelimiter(bytes_[after])) return false;
        index_ = after;
        return true;
    }
    bool take(const char *token)
    {
        const QByteArray t(token);
        if (bytes_.mid(index_, t.size()) != t) return false;
        index_ += t.size();
        return true;
    }
    void skipWhitespace()
    {
        while (index_ < bytes_.size() && (quint8(bytes_[index_]) <= 0x20 || bytes_[index_] == '%')) {
            if (bytes_[index_] == '%') {
                while (index_ < bytes_.size() && bytes_[index_] != '\n' && bytes_[index_] != '\r') ++index_;
            } else ++index_;
        }
    }
    char peekAt(int ahead) const { return index_ + ahead < bytes_.size() ? bytes_[index_ + ahead] : '\0'; }

    QByteArray bytes_;
    int index_ = 0;
};

std::optional<Engine> engineValue(const QByteArray &data)
{
    {
        EngineCursor cursor(data, 0);
        if (auto dict = cursor.parseDictionary()) return dict;
    }
    const int start = data.indexOf("<<");
    if (start <= 0) return std::nullopt;
    EngineCursor cursor(data, start);
    return cursor.parseDictionary();
}

// ---- Descriptors ---------------------------------------------------------------------------------------------------
struct Descriptor;
struct DescriptorValue {
    enum Kind { Text, Number, Enumeration, Data, Dict, List } kind = Number;
    QString text;
    double number = 0.0;
    QByteArray data;
    std::shared_ptr<Descriptor> dict;
};
struct Descriptor {
    std::map<QString, DescriptorValue> items;
    std::optional<QString> string(const QString &key) const
    {
        const auto it = items.find(key);
        if (it != items.end() && it->second.kind == DescriptorValue::Text) return it->second.text;
        return std::nullopt;
    }
    std::optional<QString> enumeration(const QString &key) const
    {
        const auto it = items.find(key);
        if (it != items.end() && it->second.kind == DescriptorValue::Enumeration) return it->second.text;
        return std::nullopt;
    }
    std::optional<QByteArray> data(const QString &key) const
    {
        const auto it = items.find(key);
        if (it != items.end() && it->second.kind == DescriptorValue::Data) return it->second.data;
        return std::nullopt;
    }
    std::optional<QRectF> rect(const QString &key) const
    {
        const auto it = items.find(key);
        if (it == items.end() || it->second.kind != DescriptorValue::Dict || !it->second.dict) return std::nullopt;
        const auto side = [&](const QString &name) -> std::optional<double> {
            auto found = it->second.dict->items.find(name);
            if (found == it->second.dict->items.end()) found = it->second.dict->items.find(name.trimmed());
            if (found != it->second.dict->items.end() && found->second.kind == DescriptorValue::Number) return found->second.number;
            return std::nullopt;
        };
        const auto left = side(QStringLiteral("Left")), top = side(QStringLiteral("Top ")), right = side(QStringLiteral("Rght")), bottom = side(QStringLiteral("Btom"));
        if (!left || !top || !right || !bottom) return std::nullopt;
        for (const double v : {*left, *top, *right, *bottom}) if (!std::isfinite(v)) return std::nullopt;
        return QRectF(*left, *top, *right - *left, *bottom - *top);
    }
};

// Descriptor walker (class and keys are length-prefixed, or 4 bytes when the length is 0).
class Reader {
public:
    explicit Reader(const QByteArray &data) : data_(data) {}
    int remaining() const { return data_.size() - offset_; }

    std::shared_ptr<Descriptor> descriptor(bool versioned)
    {
        if (versioned) { const auto v = u32(); if (!v || *v != 16) return nullptr; }
        if (!unicode() || !identifier()) return nullptr;
        const auto count = u32();
        if (!count || *count > 10000) return nullptr;
        auto out = std::make_shared<Descriptor>();
        for (quint32 i = 0; i < *count; ++i) {
            const auto key = identifier();
            if (!key) return nullptr;
            const auto type = fourCC();
            if (!type) return nullptr;
            auto value = this->value(*type);
            if (!value) return nullptr;
            out->items[*key] = std::move(*value);
        }
        return out;
    }

    std::optional<DescriptorValue> value(const QString &type)
    {
        DescriptorValue out;
        if (type == QLatin1String("doub")) {
            const auto n = f64(); if (!n) return std::nullopt;
            out.kind = DescriptorValue::Number; out.number = *n; return out;
        }
        if (type == QLatin1String("UntF")) {
            if (!fourCC()) return std::nullopt;
            const auto n = f64(); if (!n) return std::nullopt;
            out.kind = DescriptorValue::Number; out.number = *n; return out;
        }
        if (type == QLatin1String("long")) {
            const auto n = u32(); if (!n) return std::nullopt;
            out.kind = DescriptorValue::Number; out.number = double(qint32(*n)); return out;
        }
        if (type == QLatin1String("comp")) {
            const auto raw = bytes(8); if (!raw) return std::nullopt;
            quint64 bits = 0; for (const char c : *raw) bits = (bits << 8) | quint8(c);
            out.kind = DescriptorValue::Number; out.number = double(qint64(bits)); return out;
        }
        if (type == QLatin1String("bool")) {
            if (!u8()) return std::nullopt;
            out.kind = DescriptorValue::Number; out.number = 0; return out;
        }
        if (type == QLatin1String("TEXT")) {
            const auto t = unicode(); if (!t) return std::nullopt;
            out.kind = DescriptorValue::Text; out.text = *t; return out;
        }
        if (type == QLatin1String("enum")) {
            if (!identifier()) return std::nullopt;
            const auto name = identifier(); if (!name) return std::nullopt;
            out.kind = DescriptorValue::Enumeration; out.text = *name; return out;
        }
        if (type == QLatin1String("tdta")) {
            const auto length = u32();
            if (!length || *length > 8000000) return std::nullopt;
            const auto raw = bytes(int(*length)); if (!raw) return std::nullopt;
            out.kind = DescriptorValue::Data; out.data = *raw; return out;
        }
        if (type == QLatin1String("Objc") || type == QLatin1String("GlbO")) {
            auto nested = descriptor(false); if (!nested) return std::nullopt;
            out.kind = DescriptorValue::Dict; out.dict = nested; return out;
        }
        if (type == QLatin1String("VlLs")) {
            const auto count = u32();
            if (!count || *count > 10000) return std::nullopt;
            for (quint32 i = 0; i < *count; ++i) {
                const auto itemType = fourCC(); if (!itemType) return std::nullopt;
                if (!value(*itemType)) return std::nullopt;
            }
            out.kind = DescriptorValue::List; return out;
        }
        if (type == QLatin1String("alis")) {
            const auto length = u32();
            if (!length || *length > 8000000 || !bytes(int(*length))) return std::nullopt;
            return out;
        }
        if (type == QLatin1String("obj ")) return reference() ? std::optional<DescriptorValue>(out) : std::nullopt;
        if (type == QLatin1String("type") || type == QLatin1String("GlbC")) {
            if (!unicode() || !identifier()) return std::nullopt;
            return out;
        }
        return std::nullopt;
    }

    // Skips a descriptor reference so a later EngineData item can still be read.
    bool reference()
    {
        const auto count = u32();
        if (!count || *count > 10000) return false;
        for (quint32 i = 0; i < *count; ++i) {
            const auto form = fourCC();
            if (!form) return false;
            if (*form == QLatin1String("prop")) { if (!unicode() || !identifier() || !identifier()) return false; }
            else if (*form == QLatin1String("Clss")) { if (!unicode() || !identifier()) return false; }
            else if (*form == QLatin1String("Enmr")) { if (!unicode() || !identifier() || !identifier() || !identifier()) return false; }
            else if (*form == QLatin1String("rele")) { if (!unicode() || !identifier() || !u32()) return false; }
            else if (*form == QLatin1String("Idnt") || *form == QLatin1String("indx")) { if (!u32()) return false; }
            else if (*form == QLatin1String("name")) { if (!unicode()) return false; }
            else return false;
        }
        return true;
    }

    std::optional<QString> unicode()
    {
        const auto count = u32();
        if (!count || *count > 1000000) return std::nullopt;
        const auto raw = bytes(int(*count) * 2);
        if (!raw) return std::nullopt;
        QString out;
        for (int i = 0; i + 1 < raw->size(); i += 2) out.append(QChar(ushort((quint8((*raw)[i]) << 8) | quint8((*raw)[i + 1]))));
        return out;
    }
    std::optional<QString> identifier()
    {
        const auto length = u32();
        if (!length) return std::nullopt;
        if (*length == 0) return fourCC();
        if (*length > 10000) return std::nullopt;
        const auto raw = bytes(int(*length));
        if (!raw) return std::nullopt;
        return QString::fromLatin1(*raw);
    }
    std::optional<QString> fourCC()
    {
        const auto raw = bytes(4);
        if (!raw) return std::nullopt;
        return QString::fromLatin1(*raw);
    }
    std::optional<QByteArray> bytes(int count)
    {
        if (count < 0 || offset_ + count > data_.size()) return std::nullopt;
        const QByteArray slice = data_.mid(offset_, count);
        offset_ += count;
        return slice;
    }
    std::optional<quint8> u8()
    {
        if (offset_ >= data_.size()) return std::nullopt;
        return quint8(data_[offset_++]);
    }
    std::optional<quint16> u16()
    {
        const auto raw = bytes(2);
        if (!raw) return std::nullopt;
        return quint16((quint8((*raw)[0]) << 8) | quint8((*raw)[1]));
    }
    std::optional<quint32> u32()
    {
        const auto raw = bytes(4);
        if (!raw) return std::nullopt;
        quint32 v = 0; for (const char c : *raw) v = (v << 8) | quint8(c);
        return v;
    }
    std::optional<double> f64()
    {
        const auto raw = bytes(8);
        if (!raw) return std::nullopt;
        quint64 bits = 0; for (const char c : *raw) bits = (bits << 8) | quint8(c);
        double out; static_assert(sizeof(out) == sizeof(bits));
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }

private:
    QByteArray data_;
    int offset_ = 0;
};

// ---- Placement -----------------------------------------------------------------------------------------------------
struct Placement {
    double pixelScale = 1.0;
    double rotation = 0.0;
    bool flipY = false;
    double tx = 0.0, ty = 0.0;
    double exx = 0, exy = 0, eyx = 0, eyy = 0;
    QPointF map(const QPointF &p) const { return QPointF(exx * p.x() + exy * p.y() + tx, eyx * p.x() + eyy * p.y() + ty); }
};

// Uniform scale, rotation and an optional vertical flip. Shear and uneven scale return nothing. Engine sizes are
// already in text-space units that the matrix maps into document pixels; document PPI is print metadata and must not
// multiply that product again.
std::optional<Placement> placement(double xx, double xy, double yx, double yy, double tx, double ty)
{
    const double scaleX = std::hypot(xx, yx);
    if (scaleX <= 1e-6) return std::nullopt;
    const double cosR = xx / scaleX, sinR = yx / scaleX;
    const double localX = cosR * xy + sinR * yy;
    const double localY = -sinR * xy + cosR * yy;
    const double scaleY = std::abs(localY);
    if (scaleY <= 1e-6) return std::nullopt;
    const double largest = std::max(scaleX, scaleY);
    if (std::abs(localX) > 0.02 * largest || std::abs(scaleX - scaleY) > 0.02 * largest) return std::nullopt;
    if (!std::isfinite(scaleX) || scaleX <= 0) return std::nullopt;
    const double ySign = localY < 0 ? -1.0 : 1.0;
    Placement out;
    out.pixelScale = scaleX;
    out.rotation = std::atan2(sinR, cosR) * 180.0 / M_PI;
    out.flipY = localY < 0;
    out.tx = tx; out.ty = ty;
    out.exx = cosR * scaleX; out.eyx = sinR * scaleX;
    out.exy = -sinR * scaleX * ySign; out.eyy = cosR * scaleX * ySign;
    return out;
}


struct Rgb3 { double r, g, b; };

Rgb3 colorOf(const std::vector<double> &values)
{
    const auto unit = [](double v) { return v > 1 ? std::clamp(v, 0.0, 255.0) / 255.0 : std::clamp(v, 0.0, 1.0); };
    if (values.size() >= 4) return {unit(values[1]), unit(values[2]), unit(values[3])};
    if (values.size() == 3) return {unit(values[0]), unit(values[1]), unit(values[2])};
    if (!values.empty()) { const double g = unit(values[0]); return {g, g, g}; }
    return {0, 0, 0};
}

std::vector<double> colorValues(const Engine *data)
{
    std::vector<double> out;
    for (const Engine &v : arrayOf(walk(data, {"FillColor", "Values"}))) if (const auto n = numberOf(&v)) out.push_back(*n);
    return out;
}

struct Signature {
    double font = 0, size = 0, tracking = 0, leading = 0, horizontalScale = 1, verticalScale = 1;
    bool autoLeading = true, bold = false, italic = false;
    double red = 0, green = 0, blue = 0;
    bool operator==(const Signature &) const = default;
};

Signature signatureOf(const Engine &run)
{
    const Engine *data = walk(&run, {"StyleSheet", "StyleSheetData"});
    if (!data) data = &run;
    Signature sign;
    sign.font = numberOf(walk(data, {"Font"})).value_or(0);
    sign.size = numberOf(walk(data, {"FontSize"})).value_or(0);
    sign.tracking = numberOf(walk(data, {"Tracking"})).value_or(0);
    sign.autoLeading = boolOf(walk(data, {"AutoLeading"})).value_or(true);
    sign.leading = numberOf(walk(data, {"Leading"})).value_or(0);
    sign.horizontalScale = numberOf(walk(data, {"HorizontalScale"})).value_or(1);
    sign.verticalScale = numberOf(walk(data, {"VerticalScale"})).value_or(1);
    sign.bold = boolOf(walk(data, {"FauxBold"})).value_or(false);
    sign.italic = boolOf(walk(data, {"FauxItalic"})).value_or(false);
    const Rgb3 rgb = colorOf(colorValues(data));
    sign.red = rgb.r; sign.green = rgb.g; sign.blue = rgb.b;
    return sign;
}

void applyStyle(TextStyle &style, const Engine &engine, double pixelScale, QStringList &notes)
{
    const std::vector<Engine> &runs = arrayOf(walk(&engine, {"EngineDict", "StyleRun", "RunArray"}));
    const Engine *first = runs.empty() ? &engine : &runs.front();
    const Engine *sheet = walk(first, {"StyleSheet", "StyleSheetData"});
    if (!sheet) sheet = walk(&engine, {"EngineDict", "StyleRun", "RunArray"});
    const Engine *data = sheet ? sheet : first;
    const double points = numberOf(walk(data, {"FontSize"})).value_or(12);
    if (!std::isfinite(points) || points <= 0) return;
    style.fontSize = std::clamp(points * pixelScale, 1.0, 2000.0);
    const std::vector<Engine> &fonts = arrayOf(walk(&engine, {"ResourceDict", "FontSet"}));
    const int index = int(std::lround(numberOf(walk(data, {"Font"})).value_or(0)));
    if (index >= 0 && index < int(fonts.size())) {
        const auto name = stringOf(walk(&fonts[index], {"Name"}));
        if (name && !name->isEmpty()) style.fontName = *name;
    }
    const std::vector<double> values = colorValues(data);
    if (!values.empty()) {
        const Rgb3 rgb = colorOf(values);
        style.red = rgb.r; style.green = rgb.g; style.blue = rgb.b;
    }
    const double tracking = numberOf(walk(data, {"Tracking"})).value_or(0);
    if (std::isfinite(tracking)) style.tracking = std::clamp(tracking * style.fontSize / 1000.0, -100.0, 1000.0);
    const bool autoLeading = boolOf(walk(data, {"AutoLeading"})).value_or(true);
    if (!autoLeading) {
        const auto leading = numberOf(walk(data, {"Leading"}));
        if (leading && std::isfinite(*leading) && *leading > 0) style.leading = std::clamp(*leading * pixelScale, 0.0, 5000.0);
    }
    if (boolOf(walk(data, {"FauxBold"})).value_or(false) || boolOf(walk(data, {"FauxItalic"})).value_or(false)) notes.append(PSDText::fauxNote());
    if (runs.size() > 1) {
        const Signature head = signatureOf(runs.front());
        for (size_t i = 1; i < runs.size(); ++i) {
            if (!(signatureOf(runs[i]) == head)) { notes.append(PSDText::firstStyleNote()); break; }
        }
    }
    const std::vector<Engine> &paragraphs = arrayOf(walk(&engine, {"EngineDict", "ParagraphRun", "RunArray"}));
    const Engine *paragraph = paragraphs.empty() ? &engine : &paragraphs.front();
    const double justification = numberOf(walk(paragraph, {"ParagraphSheet", "Properties", "Justification"})).value_or(0);
    switch (int(std::lround(justification))) {
    case 1: style.alignment = TextAlignment::Right; break;
    case 2: style.alignment = TextAlignment::Center; break;
    case 0: style.alignment = TextAlignment::Left; break;
    default:
        style.alignment = TextAlignment::Left;
        notes.append(PSDText::justifyNote());
    }
}

std::optional<QString> cleaned(std::optional<QString> text)
{
    if (!text) return std::nullopt;
    QString out = *text;
    while (!out.isEmpty() && (out.front() == QChar(0xFEFF) || out.front() == QChar(0))) out.remove(0, 1);
    while (!out.isEmpty() && out.back() == QChar(0)) out.chop(1);
    out.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return out;
}

LayerTransform layerTransform(const QSizeF &image, const QPointF &imageAnchor, const QPointF &documentAnchor, double rotation, bool flipY)
{
    // Flip, then clockwise rotation about the center (the layer transform's own order).
    QPointF local(imageAnchor.x() - image.width() / 2, imageAnchor.y() - image.height() / 2);
    if (flipY) local.setY(-local.y());
    const double radians = rotation * M_PI / 180.0;
    const QPointF rotated(local.x() * std::cos(radians) - local.y() * std::sin(radians),
                          local.x() * std::sin(radians) + local.y() * std::cos(radians));
    const QPointF center(documentAnchor.x() - rotated.x(), documentAnchor.y() - rotated.y());
    LayerTransform out;
    out.origin = QPointF(center.x() - image.width() / 2, center.y() - image.height() / 2);
    out.size = image;
    out.rotation = rotation;
    out.flipY = flipY;
    return out;
}

} // namespace

namespace PSDText {

QString rasterizedNote() { return QStringLiteral("Editable Photoshop text becomes pixels and can’t be retyped."); }
QString firstStyleNote() { return QStringLiteral("Only the first text style was kept."); }
QString warpNote() { return QStringLiteral("The Photoshop text warp was omitted."); }
QString fauxNote() { return QStringLiteral("Faux bold or faux italic was omitted."); }
QString justifyNote() { return QStringLiteral("Full justification was imported as left alignment."); }

QString missingFontNote(const QString &name)
{
    if (resolveTextFace(name).installed) return {};
    return QStringLiteral("The font “%1” isn’t installed, so the text was drawn with the system font.").arg(name);
}

std::optional<PSDTextSource> parse(const QMap<QString, QByteArray> &extra)
{
    QByteArray data = extra.value(QStringLiteral("TySh"));
    if (data.isEmpty()) data = extra.value(QStringLiteral("tySh"));
    if (data.isEmpty() || data.size() > 8000000) return std::nullopt;
    Reader reader(data);
    const auto version = reader.u16();
    if (!version || *version != 1) return std::nullopt;
    double m[6];
    for (double &value : m) {
        const auto v = reader.f64();
        if (!v || !std::isfinite(*v)) return std::nullopt;
        value = *v;
    }
    const auto textVersion = reader.u16();
    if (!textVersion || *textVersion != 50) return std::nullopt;
    const auto text = reader.descriptor(true);
    if (!text) return std::nullopt;
    if (const auto orientation = text->enumeration(QStringLiteral("Ornt")); orientation && *orientation == QLatin1String("Vrtc")) return std::nullopt;
    const auto placed = placement(m[0], m[1], m[2], m[3], m[4], m[5]);
    if (!placed) return std::nullopt;

    QStringList notes;
    if (reader.remaining() >= 2) {
        const auto warpVersion = reader.u16();
        if (warpVersion && *warpVersion == 1) {
            if (const auto warp = reader.descriptor(true)) {
                const auto style = warp->enumeration(QStringLiteral("warpStyle"));
                if (style && *style != QLatin1String("warpNone") && *style != QLatin1String("none")) notes.append(warpNote());
            }
        }
    }

    std::optional<Engine> engine;
    if (const auto raw = text->data(QStringLiteral("EngineData"))) engine = engineValue(*raw);
    std::optional<QString> content = cleaned(text->string(QStringLiteral("Txt ")));
    if (!content) content = cleaned(text->string(QStringLiteral("Txt")));
    if (!content && engine) content = cleaned(stringOf(walk(&*engine, {"EngineDict", "Editor", "Text"})));
    if (!content || content->isEmpty() || content->size() > 100000) return std::nullopt;

    TextStyle style;
    style.content = *content;
    if (engine) applyStyle(style, *engine, placed->pixelScale, notes);
    else style.fontSize = std::min(2000.0, std::max(1.0, 12 * placed->pixelScale));
    if (!std::isfinite(style.fontSize) || style.fontSize <= 0) return std::nullopt;

    QPointF anchor(placed->tx, placed->ty);
    bool anchorIsFrame = false;
    const auto bounds = text->rect(QStringLiteral("bounds"));
    const auto glyphs = text->rect(QStringLiteral("boundingBox"));
    if (bounds && glyphs && bounds->width() > glyphs->width() + 4 && bounds->height() > glyphs->height() + 4
        && bounds->width() > 1 && bounds->height() > 1) {
        TextStyle boxed = style;
        boxed.boxSize = QSizeF(bounds->width() * placed->pixelScale + kTextPadding * 2, bounds->height() * placed->pixelScale + kTextPadding * 2);
        // A paragraph frame the model cannot store is dropped entirely: importing as point text would lose the wrap
        // without saying so. Vertical text already falls back the same way.
        if (!boxed.isValid()) return std::nullopt;
        style = boxed;
        anchor = placed->map(QPointF(bounds->left(), bounds->top()));
        anchorIsFrame = true;
    }
    if (!style.isValid()) return std::nullopt;
    PSDTextSource source;
    source.style = style;
    source.notes = notes;
    source.documentAnchor = anchor;
    source.rotation = placed->rotation;
    source.flipY = placed->flipY;
    source.anchorIsFrame = anchorIsFrame;
    return source;
}

std::optional<PSDTextRendered> render(const PSDTextSource &source)
{
    const QImage image = renderStyledText(source.style);
    if (image.isNull()) return std::nullopt;
    const QSizeF size = image.size();
    QPointF anchor(kTextPadding, kTextPadding);
    if (!source.anchorIsFrame) {
        // First baseline: the line's height less the face's descent, below the padding.
        const double descent = QFontMetricsF(textStyleFont(source.style)).descent();
        const double baseline = kTextPadding + textLineHeight(source.style) - descent;
        const double x = source.style.alignment == TextAlignment::Left ? kTextPadding
                       : source.style.alignment == TextAlignment::Center ? size.width() / 2 : size.width() - kTextPadding;
        anchor = QPointF(x, baseline);
    }
    PSDTextRendered out;
    out.image = image;
    out.transform = layerTransform(size, anchor, source.documentAnchor, source.rotation, source.flipY);
    if (!out.transform.isValid()) return std::nullopt;
    return out;
}

} // namespace PSDText
} // namespace compositor
