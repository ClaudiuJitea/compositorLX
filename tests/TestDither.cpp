// Ports CompositorTests/DitherTests.swift (mac f3cd4e9, 71635e2, d0aae29, 09ab78a, 2309a85, 9c99853, cd2f998).
#include "core/EditorSession.h"
#include "rendering/Dither.h"

#include <QtTest>
#include <cmath>
#include <limits>
#include <numeric>

using namespace compositor;

namespace {

QImage solid(int w, int h, QColor c)
{
    QImage image(w, h, QImage::Format_RGBA8888_Premultiplied);
    image.fill(c);
    return image;
}

int red(const QImage &image, int x, int y) { return image.constScanLine(y)[x * 4]; }

// One column of a Scanlines render of a flat gray, as brightness per row.
QList<int> scanlines(int gray, int spacing, double glow = 0)
{
    DitherSettings settings;
    settings.style = DitherStyle::Scanlines;
    settings.lineSpacing = spacing;
    settings.glow = glow;
    const QImage result = settings.apply(solid(16, 32, QColor(gray, gray, gray)));
    QList<int> column;
    for (int y = 0; y < 32; ++y) column << red(result, 5, y);
    return column;
}

QImage render(const QImage &image, const std::function<void(DitherSettings &)> &adjust)
{
    DitherSettings settings;
    settings.style = DitherStyle::Scanlines;
    settings.glow = 0;
    adjust(settings);
    return settings.apply(image);
}

QImage patterned()
{
    QImage image(48, 40, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(x, y, QColor((x * 5) % 256, (y * 6) % 256, ((x + y) * 3) % 256));
    return image;
}

}

class TestDither : public QObject
{
    Q_OBJECT
private slots:
    void scanlinesAreLinesOfLightThatBloomWithBrightness();
    void glowLightsBetweenTheLines();
    void dotsBreakTheLinesIntoBeads();
    void wobbleMovesLinesSideways();
    void everyStyleRunsAndKeepsSize();
    void blackWhiteOutputIsBinary();
    void applyIsDeterministic();
    void transparentPixelsStayTransparent();
    void normalizationClamps();
    void selectionClipsTheEffect();
    void undoRestoresExactPixels();
};

void TestDither::scanlinesAreLinesOfLightThatBloomWithBrightness()
{
    const QList<int> white = scanlines(255, 8), gray = scanlines(89, 8), black = scanlines(0, 8);
    for (int band = 0; band < 4; ++band) {
        const QList<int> rows = white.mid(band * 8, 8);
        QVERIFY2(rows[3] == 255 && rows[4] == 255, "a white line is lit through its middle");
        QVERIFY2(*std::min_element(rows.begin(), rows.end()) < 40, "with dark screen between lines");
    }
    const auto sum = [](const QList<int> &v) { return std::accumulate(v.begin(), v.end(), 0); };
    QVERIFY2(sum(gray) * 2 < sum(white), "a gray line is thinner and dimmer than a white one");
    for (int v : black) QCOMPARE(v, 0);
}

void TestDither::glowLightsBetweenTheLines()
{
    const QList<int> plain = scanlines(255, 8), glowing = scanlines(255, 8, 100);
    QVERIFY2(glowing[0] > plain[0] + 40, qPrintable(QStringLiteral("%1 without glow, %2 with").arg(plain[0]).arg(glowing[0])));
}

void TestDither::dotsBreakTheLinesIntoBeads()
{
    const QImage white = solid(64, 16, Qt::white);
    const QImage solidLines = render(white, [](DitherSettings &s) { s.lineSpacing = 8; });
    const QImage beads = render(white, [](DitherSettings &s) { s.lineSpacing = 8; s.dots = 100; });
    for (int x = 0; x < 64; ++x) QCOMPARE(red(solidLines, x, 4), 255);
    QVERIFY(red(beads, 3, 4) == 255 && red(beads, 4, 4) == 255 && red(beads, 0, 4) < 60 && red(beads, 8, 4) < 60);
}

void TestDither::wobbleMovesLinesSideways()
{
    QImage edge = solid(64, 64, Qt::black);
    for (int y = 0; y < 64; ++y)
        for (int x = 32; x < 64; ++x) edge.setPixelColor(x, y, Qt::white);
    const auto edges = [&](double wobble) {
        const QImage r = render(edge, [wobble](DitherSettings &s) { s.lineSpacing = 8; s.wobble = wobble; });
        QSet<int> found;
        for (int line = 0; line < 8; ++line) {
            int first = -1;
            for (int x = 0; x < 64 && first < 0; ++x) if (red(r, x, line * 8 + 4) > 128) first = x;
            found.insert(first);
        }
        return found;
    };
    QCOMPARE(edges(0), QSet<int>{32});
    QVERIFY(edges(12).size() >= 3);
}

void TestDither::everyStyleRunsAndKeepsSize()
{
    const QImage source = patterned();
    for (const auto &group : DitherInfo::groups())
        for (DitherStyle style : group) {
            DitherSettings s;
            s.style = style;
            for (DitherColors colors : {DitherColors::BlackWhite, DitherColors::TwoColors, DitherColors::Original}) {
                s.colors = colors;
                s.dark = {0.1, 0.2, 0.3};
                s.light = {0.9, 0.8, 0.7};
                const QImage out = s.apply(source);
                QVERIFY2(!out.isNull() && out.size() == source.size(), qPrintable(DitherInfo::styleName(style)));
            }
        }
    DitherSettings dot;
    dot.pixelSize = 4;
    dot.pixelShape = DitherPixelShape::Dot;
    QCOMPARE(dot.apply(source).size(), source.size());
}

void TestDither::blackWhiteOutputIsBinary()
{
    DitherSettings s;   // Atkinson, 1-bit
    s.pixelSize = 1;
    const QImage out = s.apply(patterned());
    for (int y = 0; y < out.height(); ++y)
        for (int x = 0; x < out.width(); ++x) {
            const int v = red(out, x, y);
            QVERIFY(v == 0 || v == 255);
        }
    // Chunky pixels: every 2x2 block is uniform.
    DitherSettings chunky;
    const QImage big = chunky.apply(patterned());
    for (int y = 0; y < big.height(); y += 2)
        for (int x = 0; x < big.width(); x += 2) QCOMPARE(red(big, x + 1, y + 1), red(big, x, y));
}

void TestDither::applyIsDeterministic()
{
    DitherSettings s;
    for (DitherStyle style : {DitherStyle::Atkinson, DitherStyle::Dots, DitherStyle::Ascii, DitherStyle::Scanlines}) {
        s.style = style;
        s.wobble = 5;
        s.dots = 30;
        QCOMPARE(s.apply(patterned()), s.apply(patterned()));
    }
}

void TestDither::transparentPixelsStayTransparent()
{
    QImage image = patterned();
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < image.width(); ++x) image.setPixel(x, y, 0);
    DitherSettings s;
    s.pixelSize = 1;
    const QImage out = s.apply(image);
    for (int x = 0; x < out.width(); ++x) QCOMPARE(int(out.constScanLine(5)[x * 4 + 3]), 0);
    QCOMPARE(int(out.constScanLine(20)[3]), 255);
}

void TestDither::normalizationClamps()
{
    DitherSettings s;
    s.pixelSize = 99.4; s.cellSize = 1; s.textSize = 500; s.lineSpacing = 2.6; s.glow = 400; s.dots = -5;
    s.wobble = 100; s.angle = -200; s.levels = 1; s.diffusion = 150; s.density = -300; s.contrast = 300;
    s.dark = {-1, 0.5, 2}; s.light = {std::nan(""), 1, 1};
    s.characters = QStringLiteral("ab\ncd") + QString(100, QLatin1Char('x'));
    const DitherSettings n = s.normalized();
    QCOMPARE(n.pixelSize, 32.0); QCOMPARE(n.cellSize, 4.0); QCOMPARE(n.textSize, 64.0); QCOMPARE(n.lineSpacing, 3.0);
    QCOMPARE(n.glow, 100.0); QCOMPARE(n.dots, 0.0); QCOMPARE(n.wobble, 64.0); QCOMPARE(n.angle, -90.0);
    QCOMPARE(n.levels, 2.0); QCOMPARE(n.diffusion, 100.0); QCOMPARE(n.density, -100.0); QCOMPARE(n.contrast, 100.0);
    QCOMPARE(n.dark.red, 0.0); QCOMPARE(n.dark.blue, 1.0); QCOMPARE(n.light.red, 0.0);
    QVERIFY(!n.characters.contains(QLatin1Char('\n')));
    QCOMPARE(n.characters.size(), 64);
    QVERIFY(n.characters.startsWith(QStringLiteral("abcd")));
    DitherSettings nan; nan.pixelSize = std::numeric_limits<double>::quiet_NaN();
    QCOMPARE(nan.normalized().pixelSize, 2.0);
    QCOMPARE(DitherSettings().normalized(), DitherSettings());
    QCOMPARE(n.normalized(), n);
    DitherSettings rounded; rounded.pixelSize = 2.4; rounded.cellSize = 8.6;
    QCOMPARE(rounded.normalized().pixelSize, 2.0); QCOMPARE(rounded.normalized().cellSize, 9.0);
}

void TestDither::selectionClipsTheEffect()
{
    EditorSession session;
    session.createDocument(40, 40, true);
    session.document()->layers[0].image = patterned().scaled(40, 40);
    const QImage before = session.activeLayer()->image;
    session.setRectangularSelection(QRect(10, 10, 12, 12));
    DitherSettings s;
    s.pixelSize = 1;
    QVERIFY(session.applyDither(s));
    const QImage after = session.activeLayer()->image;
    QVERIFY(after != before);
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 40; ++x) {
            if (QRect(10, 10, 12, 12).contains(x, y)) continue;
            QCOMPARE(after.pixel(x, y), before.pixel(x, y));
        }
    bool changed = false;
    for (int y = 10; y < 22; ++y) for (int x = 10; x < 22; ++x) changed |= after.pixel(x, y) != before.pixel(x, y);
    QVERIFY(changed);
}

void TestDither::undoRestoresExactPixels()
{
    EditorSession session;
    session.createDocument(40, 40, true);
    session.document()->layers[0].image = patterned().scaled(40, 40);
    const QImage before = session.activeLayer()->image;
    DitherSettings s;
    QVERIFY(session.applyDither(s));
    QCOMPARE(session.history().undoName(), QStringLiteral("Dither"));
    QVERIFY(session.activeLayer()->image != before);
    session.undo();
    QCOMPARE(session.activeLayer()->image, before);
}

QTEST_MAIN(TestDither)
#include "TestDither.moc"
