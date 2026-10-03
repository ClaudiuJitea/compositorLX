// Behaviour ported from macOS (see MAC_BASELINE): Smudge, Liquify, Blur and Clone Stamp.
// ad9ad7c Smudge without ghost copies; 7164dd4 Liquify keeps pixels sharp; 504129d Blur Radius apart from Strength;
// 095da2f big canvases; d3170ab Blur and Clone Stamp paint at the layer's own resolution.
#include "core/Document.h"
#include "core/EditorSession.h"

#include <QtTest>
#include <cmath>

using namespace compositor;

class TestPaintTools : public QObject
{
    Q_OBJECT
    static QImage solid(int w, int h, QColor c) { QImage i(w, h, QImage::Format_RGBA8888_Premultiplied); i.fill(c); return i; }
    static int channel(const QImage &image, int x, int y, int c = 0) { return image.constScanLine(y)[x * 4 + c]; }

    // A photo stand-in: smooth colour with some fine detail, so resampling shows as drift.
    static QImage photo(int w, int h)
    {
        QImage image(w, h, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < h; ++y) {
            uchar *row = image.scanLine(y);
            for (int x = 0; x < w; ++x) {
                const int base = 128 + int(90 * std::sin(x * .21) * std::cos(y * .17)) + ((x * 7 + y * 13) % 5) * 4;
                row[x * 4] = uchar(std::clamp(base, 0, 255)); row[x * 4 + 1] = uchar(std::clamp(255 - base, 0, 255));
                row[x * 4 + 2] = uchar((x * 255) / w); row[x * 4 + 3] = 255;
            }
        }
        return image;
    }

    static double meanDifference(const QImage &a, const QImage &b)
    {
        qint64 total = 0;
        for (int y = 0; y < a.height(); ++y) for (int x = 0; x < a.width() * 4; ++x) total += std::abs(int(a.constScanLine(y)[x]) - int(b.constScanLine(y)[x]));
        return double(total) / (double(a.width()) * a.height() * 4);
    }

    // A hard vertical edge, blurred by one stroke along it: how many columns it spreads over on row 50.
    static int edgeSpread(double radius, double diameter, double strength)
    {
        EditorSession session; session.createDocument(200, 100);
        QImage edge = solid(200, 100, Qt::black);
        for (int y = 0; y < 100; ++y) for (int x = 100; x < 200; ++x) std::fill_n(edge.scanLine(y) + x * 4, 4, uchar(255));
        if (!session.insertImage(edge, QStringLiteral("Edge"))) return -1;
        if (!session.beginBlurStroke(QPointF(100, 20), diameter, 1, strength, radius)) return -1;
        session.continueBrushStroke(QPointF(100, 80));
        session.endBrushStroke();
        const QImage result = session.activeLayer()->image;
        int count = 0;
        for (int x = 0; x < 200; ++x) { const int v = channel(result, x, 50); if (v > 10 && v < 245) ++count; }
        return count;
    }

private slots:
    // ad9ad7c: a bright dot smudged sideways leaves one fading trail, not a row of ghost copies.
    void smudgeLeavesOneFadingTrail();
    // 7164dd4: a Liquify stroke pushed across and back leaves the layer much as it was (mac: 0.49 levels vs 2.34).
    void liquifyStaysSharp();
    // 504129d: Blur's Radius sets how far it softens, whatever the brush size and strength.
    void blurRadiusIsIndependentOfStrength();
    // d3170ab: on a layer scaled to 5%, Blur and Clone Stamp work on the layer's own pixels.
    void blurPaintsAtLayerResolution();
    void cloneCopiesLayerPixels();
    void blurSoftensOnlyWhatTheBrushReaches();
    // 095da2f: the largest brush on a big canvas finishes.
    void largestBrushFinishes_data();
    void largestBrushFinishes();
};

void TestPaintTools::smudgeLeavesOneFadingTrail()
{
    EditorSession session; session.createDocument(300, 100);
    QImage image = solid(300, 100, QColor(26, 26, 26));
    for (int y = 0; y < 100; ++y) for (int x = 0; x < 300; ++x) if (std::hypot(x + .5 - 60, y + .5 - 50) <= 8) std::fill_n(image.scanLine(y) + x * 4, 4, uchar(255));
    QVERIFY(session.insertImage(image, QStringLiteral("Dot")));
    QVERIFY(session.beginWarpStroke(QPointF(60, 50), 1, 40, .5, .6));
    for (double x = 63; x <= 240; x += 3) session.continueBrushStroke(QPointF(x, 50));
    QVERIFY(session.endBrushStroke());
    const QImage result = session.activeLayer()->image;
    QVector<int> row;
    for (int x = 70; x < 240; ++x) row.push_back(channel(result, x, 50));
    int peaks = 0;
    for (int i = 2; i < row.size() - 2; ++i) if (row[i] > row[i - 2] + 2 && row[i] > row[i + 2] + 2) ++peaks;
    QCOMPARE(peaks, 0);
    QVERIFY2(row.first() > row.last() + 20, qPrintable(QStringLiteral("trail %1 to %2").arg(row.first()).arg(row.last())));
}

void TestPaintTools::liquifyStaysSharp()
{
    EditorSession session; session.createDocument(300, 200);
    const QImage image = photo(300, 200);
    QVERIFY(session.insertImage(image, QStringLiteral("Photo")));
    const QImage untouched = session.activeLayer()->image;
    QVERIFY(session.beginWarpStroke(QPointF(60, 100), 0, 60, .3, .7));
    for (double x = 64; x <= 200; x += 4) session.continueBrushStroke(QPointF(x, 100));
    for (double x = 196; x >= 60; x -= 4) session.continueBrushStroke(QPointF(x, 100));
    QVERIFY(session.endBrushStroke());
    const double drift = meanDifference(session.activeLayer()->image, untouched);
    qInfo() << "liquify push-and-back drift (mean levels):" << drift;
    QVERIFY2(drift < 1.0, qPrintable(QStringLiteral("drift %1").arg(drift)));
}

void TestPaintTools::blurRadiusIsIndependentOfStrength()
{
    const int narrow = edgeSpread(2, 60, 1), wide = edgeSpread(12, 60, 1);
    QVERIFY(narrow > 0);
    QVERIFY2(wide > narrow * 2, qPrintable(QStringLiteral("%1 against %2").arg(narrow).arg(wide)));
    QCOMPARE(edgeSpread(12, 80, 1), wide);
}

void TestPaintTools::blurPaintsAtLayerResolution()
{
    EditorSession session; session.createDocument(100, 100);
    QImage edge = solid(1000, 1000, Qt::black);
    for (int y = 0; y < 1000; ++y) for (int x = 500; x < 1000; ++x) std::fill_n(edge.scanLine(y) + x * 4, 4, uchar(255));
    QVERIFY(session.insertImage(edge, QStringLiteral("Edge"), QPointF(25, 25)));
    Layer *layer = session.activeLayer();
    layer->transform.size = QSizeF(50, 50);   // 5%: twenty layer pixels to a canvas pixel
    layer->transform.origin = QPointF(0, 0);
    const QPointF centre = layer->transform.center();
    QVERIFY(session.beginBlurStroke(centre - QPointF(0, 15), 30, 1, 1, 2));
    session.continueBrushStroke(centre + QPointF(0, 15));
    QVERIFY(session.endBrushStroke());
    const QImage result = session.activeLayer()->image;
    QCOMPARE(result.size(), QSize(1000, 1000));
    int spread = 0;
    for (int x = 0; x < 1000; ++x) { const int v = channel(result, x, 500); if (v > 10 && v < 245) ++spread; }
    // A 2 unit radius is 40 layer pixels of softening: the edge spreads over far more than a canvas-sized blur would.
    QVERIFY2(spread > 80, qPrintable(QStringLiteral("spread %1").arg(spread)));
    QVERIFY2(spread < 400, qPrintable(QStringLiteral("spread %1").arg(spread)));
    QCOMPARE(channel(result, 20, 500), 0);
    QCOMPARE(channel(result, 980, 500), 255);
}

void TestPaintTools::cloneCopiesLayerPixels()
{
    EditorSession session; session.createDocument(100, 100);
    QImage stripes = solid(1000, 1000, Qt::black);
    for (int y = 0; y < 1000; ++y) for (int x = 0; x < 1000; ++x) if (x % 2 == 0) std::fill_n(stripes.scanLine(y) + x * 4, 4, uchar(255));
    // Fine detail only on the left, so the copy shows.
    for (int y = 0; y < 1000; ++y) for (int x = 600; x < 1000; ++x) std::fill_n(stripes.scanLine(y) + x * 4, 4, uchar(0));
    QVERIFY(session.insertImage(stripes, QStringLiteral("Stripes"), QPointF(25, 25)));
    Layer *layer = session.activeLayer();
    layer->transform.size = QSizeF(50, 50);
    layer->transform.origin = QPointF(0, 0);
    // One canvas unit is twenty layer pixels. Copy from unit 10 (pixel 200) onto unit 30 (pixel 600).
    session.setCloneSource(QPointF(10, 25));
    QVERIFY(session.beginCloneStroke(QPointF(30, 25), 8, 1, 1, true, false));
    session.continueBrushStroke(QPointF(31, 25));
    QVERIFY(session.endBrushStroke());
    const QImage result = session.activeLayer()->image;
    QCOMPARE(result.size(), QSize(1000, 1000));
    // Pixel-exact copy, one-pixel stripes intact: nothing was resampled through a canvas-sized picture.
    int matches = 0, checked = 0;
    for (int x = 600; x < 640; ++x) { ++checked; if (channel(result, x, 500) == ((x - 400) % 2 == 0 ? 255 : 0)) ++matches; }
    QCOMPARE(matches, checked);
}

void TestPaintTools::blurSoftensOnlyWhatTheBrushReaches()
{
    EditorSession session; session.createDocument(400, 100);
    QImage image = solid(400, 100, Qt::black);
    for (int y = 0; y < 100; ++y) for (int x = 0; x < 400; ++x) if ((x / 2 + y / 2) % 2 == 0) std::fill_n(image.scanLine(y) + x * 4, 4, uchar(255));
    QVERIFY(session.insertImage(image, QStringLiteral("Checks")));
    const QImage before = session.activeLayer()->image;
    QVERIFY(session.beginBlurStroke(QPointF(50, 50), 30, 1, 1, 3));
    session.continueBrushStroke(QPointF(80, 50));
    QVERIFY(session.endBrushStroke());
    const QImage after = session.activeLayer()->image;
    QVERIFY(after != before);
    for (int y = 0; y < 100; ++y) for (int x = 150; x < 400; ++x) QCOMPARE(channel(after, x, y), channel(before, x, y));
}

void TestPaintTools::largestBrushFinishes_data()
{
    QTest::addColumn<int>("mode");
    QTest::newRow("liquify") << 0;
    QTest::newRow("smudge") << 1;
    QTest::newRow("blur") << 2;
}

void TestPaintTools::largestBrushFinishes()
{
    QFETCH(int, mode);
    const int side = 3000;
    EditorSession session; session.createDocument(side, side);
    QImage bands(side, side, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < side; ++y) { uchar *row = bands.scanLine(y); for (int x = 0; x < side; ++x) { const int band = x * 8 / side; row[x * 4] = uchar(band * 36); row[x * 4 + 1] = 100; row[x * 4 + 2] = uchar(255 - band * 36); row[x * 4 + 3] = 255; } }
    QVERIFY(session.insertImage(bands, QStringLiteral("Bands")));
    const QImage before = session.activeLayer()->image;
    const double y = side / 2.0;
    const QPointF start(1000, y);
    if (mode == 2) QVERIFY(session.beginBlurStroke(start, 2000, .5, 1, 20));
    else QVERIFY(session.beginWarpStroke(start, mode, 2000, .5, 1));
    for (int step = 1; step <= 4; ++step) session.continueBrushStroke(QPointF(1000 + step * 100, y + (step % 3) * 40));
    QVERIFY(session.endBrushStroke());
    QVERIFY(session.activeLayer()->image != before);
}

QTEST_MAIN(TestPaintTools)
#include "TestPaintTools.moc"
