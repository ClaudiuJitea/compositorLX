#include "core/CameraRawTables.h"

#include <QFile>
#include <QtEndian>
#include <algorithm>
#include <cstring>

static void initCameraRawResources()
{
    Q_INIT_RESOURCE(camera_raw);
}

namespace compositor::CameraRawTables
{
namespace
{
constexpr int entries = size * size * size * 3, count = 763;
const QByteArray &tables()
{
    static const QByteArray data = [] {
        initCameraRawResources();
        QFile file(QStringLiteral(":/camera-raw/CameraRawTables.bin"));
        if (!file.open(QIODevice::ReadOnly))
            qFatal("Camera Raw tables are missing");
        const QByteArray bytes = file.readAll();
        if (bytes.size() < 14 || bytes.first(4) != "CRT4" ||
            qFromLittleEndian<quint16>(bytes.constData() + 4) != size ||
            qFromLittleEndian<quint16>(bytes.constData() + 6) != count)
            qFatal("Invalid Camera Raw tables");
        const int step = qFromLittleEndian<quint16>(bytes.constData() + 8);
        const QByteArray planes = qUncompress(bytes.sliced(10));
        if (step <= 0 || planes.size() != count * entries)
            qFatal("Corrupt Camera Raw tables");
        QByteArray out(count * entries, Qt::Uninitialized);
        constexpr int cells = size * size * size;
        for (int table = 0; table < count; ++table)
            for (int c = 0; c < 3; ++c) {
                uint8_t running = 0;
                for (int cell = 0; cell < cells; ++cell) {
                    running += uint8_t(planes[(table * 3 + c) * cells + cell]);
                    const int axis = c == 0   ? cell / (size * size)
                                     : c == 1 ? (cell / size) % size
                                              : cell % size;
                    out[table * entries + cell * 3 + c] = char(
                        std::clamp(qRound(axis * 255.0 / (size - 1)) + int(int8_t(running)) * step, 0, 255));
                }
            }
        return out;
    }();
    return data;
}
const uint8_t *table(int index)
{
    return reinterpret_cast<const uint8_t *>(tables().constData()) + index * entries;
}
std::pair<int, double> between(double value, const std::vector<double> &points)
{
    value = std::clamp(value, points.front(), points.back());
    int i = 0;
    while (i < int(points.size()) - 2 && value > points[i + 1])
        ++i;
    return {i, (value - points[i]) / (points[i + 1] - points[i])};
}
std::pair<int, double> between(double value, double low, double step, int n)
{
    const double position = std::clamp((value - low) / step, 0.0, double(n - 1));
    const int i = std::min(n - 2, int(position));
    return {i, position - i};
}
CameraRawStage blend(const std::vector<std::pair<const uint8_t *, double>> &parts)
{
    CameraRawStage stage{};
    for (size_t i = 0; i < std::min(size_t(8), parts.size()); ++i) {
        stage.table[i] = parts[i].first;
        stage.weight[i] = float(parts[i].second);
    }
    return stage;
}
CameraRawStage stage(int start, double value, double low, double step, int n)
{
    const auto [i, t] = between(value, low, step, n);
    return blend({{table(start + i), 1 - t}, {table(start + i + 1), t}});
}
CameraRawStage fifty(int start, double value)
{
    return stage(start, value, -100, 50, 5);
}
CameraRawStage adaptive(int start, double value, double brightness)
{
    const auto [row, down] = between(brightness, {53, 80, 104, 130, 160, 190, 224});
    const auto [column, across] = between(value, {-100, -50, -25, 0, 25, 50, 100});
    const int base = start + row * 7 + column;
    return blend({{table(base), (1 - down) * (1 - across)},
                  {table(base + 1), (1 - down) * across},
                  {table(base + 7), down * (1 - across)},
                  {table(base + 8), down * across}});
}
} // namespace
const uint8_t *exposureTables()
{
    return table(81);
}
CameraRawStage whiteBalance(double temperature, double tint)
{
    const auto [row, down] = between(temperature, -100, 25, 9);
    const auto [column, across] = between(tint, -100, 25, 9);
    return blend({{table(row * 9 + column), (1 - down) * (1 - across)},
                  {table(row * 9 + column + 1), (1 - down) * across},
                  {table((row + 1) * 9 + column), down * (1 - across)},
                  {table((row + 1) * 9 + column + 1), down * across}});
}
Brightness brightness(const QImage &image)
{
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    double out[3]{};
    camera_raw_statistics(rgba.constBits(), rgba.width(), rgba.height(), rgba.bytesPerLine(), out);
    return {out[0] * 255, out[1] * 255, out[2] * 255};
}
std::vector<CameraRawStage> stages(const CameraRawSettings &s, const Brightness &b, Part part)
{
    std::vector<CameraRawStage> out;
    const auto &c = s.calibration;
    int i = 0;
    for (double value :
         {c.redHue, c.redSaturation, c.greenHue, c.greenSaturation, c.blueHue, c.blueSaturation}) {
        if (value != 0)
            out.push_back(fifty(405 + i * 5, value));
        ++i;
    }
    if (s.exposure != 0)
        out.push_back(stage(81, s.exposure, -5, .5, 21));
    if (s.temperature != 0 || s.tint != 0)
        out.push_back(whiteBalance(s.temperature, s.tint));
    if (s.contrast != 0)
        out.push_back(adaptive(138, s.contrast, b.brightest));
    if (s.highlights != 0)
        out.push_back(adaptive(187, s.highlights, b.logBrightest));
    const size_t before = out.size();
    if (s.whites != 0)
        out.push_back(stage(102, s.whites, -100, 25, 9));
    if (s.blacks != 0)
        out.push_back(stage(111, s.blacks, -100, 25, 9));
    if (s.saturation != 0)
        out.push_back(stage(120, s.saturation, -100, 25, 9));
    if (s.vibrance != 0)
        out.push_back(stage(129, s.vibrance, -100, 25, 9));
    if (s.curve.hasMeasuredDividers()) {
        i = 0;
        for (double value : {s.curve.shadows, s.curve.darks, s.curve.lights, s.curve.highlights}) {
            if (value != 0)
                out.push_back(fifty(435 + i * 5, value));
            ++i;
        }
    }
    if (part == Part::BeforeShadows)
        out.resize(before);
    if (part == Part::AfterShadows)
        out.erase(out.begin(), out.begin() + before);
    return out;
}
std::vector<CameraRawStage> mixerStages(const CameraRawMixerSettings &m)
{
    std::vector<CameraRawStage> out;
    int kind = 0;
    for (const auto &values : {m.luminance, m.saturation, m.hue}) {
        for (int color = 0; color < 8; ++color)
            if (values[color] != 0)
                out.push_back(fifty(285 + (kind * 8 + color) * 5, values[color]));
        ++kind;
    }
    return out;
}
std::vector<CameraRawStage> gradingStages(const CameraRawGradingSettings &g)
{
    std::vector<CameraRawStage> out;
    const CameraRawGradeWheel wheels[] = {g.shadows, g.midtones, g.highlights, g.global};
    for (int i = 0; i < 4; ++i)
        if (wheels[i].luminance != 0)
            out.push_back(fifty(743 + i * 5, wheels[i].luminance));
    const double balance = g.balance / 100;
    const double shares[] = {std::max(0.0, balance < 0 ? 1 - 1.2 * balance : 1 - 1.4 * balance), 1,
                             std::max(0.0, balance < 0 ? 1 + 1.4 * balance : 1 + 2 * balance), 1};
    const auto [blendLow, blendUp] = between(g.blending, {0, 50, 100});
    for (int index : {3, 1, 2, 0}) {
        const auto &w = wheels[index];
        if (w.saturation * shares[index] <= 0)
            continue;
        const int wheel = index == 1 ? 2 : index == 2 ? 1 : index;
        const bool blends = index == 0 || index == 2;
        const double position = std::fmod(w.hue, 360) / 30;
        const int first = int(position) % 12, second = (first + 1) % 12;
        const double around = position - int(position);
        const auto [level, up] = between(w.saturation * shares[index], {0, 25, 50, 100});
        std::vector<std::pair<const uint8_t *, double>> parts;
        const std::vector<std::pair<int, double>> blendings =
            blends ? std::vector<std::pair<int, double>>{{blendLow, 1 - blendUp}, {blendLow + 1, blendUp}}
                   : std::vector<std::pair<int, double>>{{1, 1}};
        for (const auto &[blendIndex, blendWeight] : blendings) {
            const int base = 455 + (blendIndex == 0 ? 0 : blendIndex == 1 ? 72 : 216) + wheel * 36;
            for (const auto &[hue, hueWeight] :
                 std::vector<std::pair<int, double>>{{first, 1 - around}, {second, around}})
                for (const auto &[sat, satWeight] :
                     std::vector<std::pair<int, double>>{{level, 1 - up}, {level + 1, up}})
                    parts.emplace_back(sat == 0 ? nullptr : table(base + hue * 3 + sat - 1),
                                       blendWeight * hueWeight * satWeight);
        }
        out.push_back(blend(parts));
    }
    return out;
}
std::vector<float> compose(const std::vector<CameraRawStage> &stages, bool identity)
{
    if (stages.empty() && !identity)
        return {};
    std::vector<float> out(grid * grid * grid * 3);
    camera_raw_compose(out.data(), grid, stages.data(), int(stages.size()), size);
    return out;
}
std::optional<std::pair<double, double>> neutralize(double red, double green, double blue)
{
    if (std::min({red, green, blue}) <= 1e-4)
        return std::nullopt;
    const auto cast = [=](double temperature, double tint) {
        const auto s = whiteBalance(temperature, tint);
        double rgb[] = {red, green, blue};
        camera_raw_stage_color(&s, size, rgb);
        return *std::max_element(rgb, rgb + 3) - *std::min_element(rgb, rgb + 3);
    };
    double temperature = 0, tint = 0, best = cast(0, 0);
    for (const auto &[step, reach] : std::vector<std::pair<double, double>>{{10, 100}, {2, 10}, {.5, 2}}) {
        const double centerT = temperature, centerTint = tint;
        for (double t = centerT - reach; t <= centerT + reach; t += step)
            for (double m = centerTint - reach; m <= centerTint + reach; m += step) {
                if (std::abs(t) > 100 || std::abs(m) > 100)
                    continue;
                const double found = cast(t, m);
                if (found < best) {
                    best = found;
                    temperature = t;
                    tint = m;
                }
            }
    }
    return std::pair{temperature, tint};
}
void applyShadows(QImage &image, double amount, double scale)
{
    if (amount == 0)
        return;
    static const QByteArray bytes = [] {
        initCameraRawResources();
        QFile file(QStringLiteral(":/camera-raw/CameraRawShadows.bin"));
        if (!file.open(QIODevice::ReadOnly))
            qFatal("Camera Raw Shadows tables are missing");
        return file.readAll();
    }();
    const auto i16 = [](int i) { return int(qFromLittleEndian<quint16>(bytes.constData() + i)); };
    const auto f32 = [](int i) {
        quint32 bits = qFromLittleEndian<quint32>(bytes.constData() + i);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    };
    const int amounts = i16(6), xCount = i16(8), kCount = i16(10), terms = i16(12),
              per = xCount * kCount * terms;
    if (bytes.first(4) != "CRS2" || i16(4) != 2 || terms != 3 ||
        bytes.size() != 34 + amounts * 4 + 2 * amounts * per * 4)
        qFatal("Invalid Shadows tables");
    std::vector<double> values{0};
    for (int i = 0; i < amounts; ++i)
        values.push_back(f32(34 + i * 4));
    std::sort(values.begin(), values.end());
    const auto [index, t] = between(amount, values);
    const int set = std::max(image.width(), image.height()) / (scale > 0 ? scale : 1) >= f32(14) ? 1 : 0;
    const auto coefficient = [&](double value, int i) {
        if (value == 0)
            return 0.0f;
        int a = 0;
        while (a < amounts && f32(34 + a * 4) != value)
            ++a;
        return f32(34 + amounts * 4 + ((set * amounts + a) * per + i) * 4);
    };
    std::vector<float> coefficients(per);
    for (int i = 0; i < per; ++i)
        coefficients[i] =
            float(coefficient(values[index], i) * (1 - t) + coefficient(values[index + 1], i) * t);
    adjust_camera_raw_shadows(image.bits(), image.width(), image.height(), image.bytesPerLine(),
                              coefficients.data(), xCount, f32(18), f32(22), kCount, f32(26), f32(30));
}
} // namespace compositor::CameraRawTables
