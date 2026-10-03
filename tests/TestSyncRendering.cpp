// Behaviour ported from macOS 1.3.x-1.4.5 (see MAC_BASELINE). Expected values come from the mac tests where one exists.
#include "core/CameraRaw.h"
#include "core/Document.h"
#include "core/EditorSession.h"
#include "rendering/RasterOperations.h"

#include <QtTest>
#include <cmath>

using namespace compositor;

class TestSyncRendering : public QObject
{
    Q_OBJECT
    static QImage solid(int w, int h, QColor c) { QImage i(w, h, QImage::Format_RGBA8888_Premultiplied); i.fill(c); return i; }

private slots:
    // 60d9117: CameraRawTests.parametricCurveIsSmooth / parametricCurveMatchesPhotoshop / curveDeepensColorLikePhotoshop
    void parametricCurveIsSmooth();
    void parametricCurveMatchesPhotoshop();
    void curveDeepensColorLikePhotoshop();
    // fe7a83d: HueSaturationTests.positiveSaturationMatchesPhotoshop
    void positiveSaturationMatchesPhotoshop();
    // 133c34a / b3419ab
    void inverseOfEverythingDeselects();
    void maskButtonRevealsOrHidesTheSelection();
};

void TestSyncRendering::parametricCurveIsSmooth()
{
    CameraRawCurveSettings curve;
    QCOMPARE(curve.parametric(0.3), 0.3);
    const double cases[][4] = {{100, 0, 0, 0}, {0, 100, -100, 0}, {-100, 50, 100, -60}};
    for (const auto &a : cases) {
        curve.shadows = a[0]; curve.darks = a[1]; curve.lights = a[2]; curve.highlights = a[3];
        std::vector<double> samples;
        for (int i = 0; i <= 200; ++i) samples.push_back(curve.parametric(i / 200.0));
        QCOMPARE(samples.front(), 0.0);
        QCOMPARE(samples.back(), 1.0);
        for (size_t i = 1; i < samples.size(); ++i) QVERIFY2(samples[i] >= samples[i - 1] - 1e-9, "the curve keeps rising");
        auto jump = [&](int steps) {
            std::vector<double> slopes;
            double prev = curve.parametric(0);
            for (int i = 1; i <= steps; ++i) { const double v = curve.parametric(double(i) / steps); slopes.push_back((v - prev) * steps); prev = v; }
            double worst = 0;
            for (size_t i = 1; i < slopes.size(); ++i) worst = std::max(worst, std::abs(slopes[i] - slopes[i - 1]));
            return worst;
        };
        QVERIFY2(jump(400) < jump(200) * 0.7, "a corner in the curve");
    }
    curve = {};
    curve.darks = 100;
    const double before = curve.parametric(0.4);
    curve.darkSplit = 70;
    QVERIFY2(curve.parametric(0.4) > before, "widening the darks region spreads its lift");
}

void TestSyncRendering::parametricCurveMatchesPhotoshop()
{
    CameraRawCurveSettings curve;
    curve.darks = -51;
    curve.lights = 59;
    const double photoshop[][2] = {{0.093, 0.011}, {0.192, 0.089}, {0.267, 0.174}, {0.367, 0.310}, {0.491, 0.498},
                                   {0.616, 0.698}, {0.690, 0.804}, {0.765, 0.886}, {0.840, 0.947}, {0.915, 0.982}};
    for (const auto &p : photoshop)
        QVERIFY2(std::abs(curve.parametric(p[0]) - p[1]) < 0.035, qPrintable(QStringLiteral("at %1: %2, Photoshop %3").arg(p[0]).arg(curve.parametric(p[0])).arg(p[1])));
}

void TestSyncRendering::curveDeepensColorLikePhotoshop()
{
    const QImage orange = solid(1, 1, QColor::fromRgbF(0.85f, 0.35f, 0.1f));
    CameraRawSettings settings;
    settings.curve.darks = -51;
    settings.curve.lights = 59;
    QColor c = RasterOperations::cameraRaw(orange, settings).pixelColor(0, 0);
    QVERIFY2(c.red() > 230 && c.green() < 80, qPrintable(c.name()));   // red up, green down, as Photoshop's
    settings.curve.refineSaturation = -100;
    c = RasterOperations::cameraRaw(orange, settings).pixelColor(0, 0);
    QVERIFY2(double(c.green()) / c.red() > 0.35, qPrintable(c.name()));  // brightness only keeps orange orange
}

void TestSyncRendering::positiveSaturationMatchesPhotoshop()
{
    // HSL saturation 0.2 at +50 doubles to 0.4; +100 takes any color all the way; a gray stays gray.
    auto saturationAfter = [](QColor in, double amount) {
        const QImage out = RasterOperations::hueSaturation(solid(1, 1, in), HueSaturationSettings(0, amount, 0));
        return out.pixelColor(0, 0).hslSaturationF();
    };
    QColor base = QColor::fromHslF(0.1f, 0.2f, 0.5f);
    QVERIFY(std::abs(saturationAfter(base, 50) - 0.4) < 0.02);
    base = QColor::fromHslF(0.1f, 0.3f, 0.5f);
    QVERIFY(std::abs(saturationAfter(base, 62) - 0.3 / 0.38) < 0.02);
    QVERIFY(saturationAfter(QColor::fromHslF(0.1f, 0.1f, 0.5f), 100) > 0.98);
    QVERIFY(saturationAfter(QColor::fromHslF(0.1f, 0.8f, 0.5f), 50) > 0.98);
    QVERIFY(saturationAfter(QColor(128, 128, 128), 100) < 0.02);
    QVERIFY(std::abs(saturationAfter(QColor::fromHslF(0.1f, 0.6f, 0.5f), -50) - 0.3) < 0.02);
}

void TestSyncRendering::inverseOfEverythingDeselects()
{
    EditorSession session;
    session.createDocument(20, 10, true);
    session.selectAll();
    QVERIFY(session.document()->selection.has_value());
    session.invertSelection();
    QVERIFY2(!session.document()->selection.has_value(), "inverse of everything is no selection");
    session.undo();
    QVERIFY(session.document()->selection.has_value());
    session.setRectangularSelection(QRect(0, 0, 5, 5));
    session.invertSelection();
    QVERIFY(session.document()->selection.has_value());
}

void TestSyncRendering::maskButtonRevealsOrHidesTheSelection()
{
    for (const bool reveal : {true, false}) {
        EditorSession session;
        session.createDocument(40, 20, true);
        session.setRectangularSelection(QRect(20, 5, 10, 10));
        QVERIFY(session.addLayerMask(reveal));
        QCOMPARE(session.history().undoName(), reveal ? QStringLiteral("Reveal Selection") : QStringLiteral("Hide Selection"));
        const QImage &mask = session.activeLayer()->mask;
        const int inside = qGray(mask.pixel(25, 10)) & 0xff, outside = qGray(mask.pixel(2, 2)) & 0xff;
        QCOMPARE(inside, reveal ? 255 : 0);       // selected area: white (visible) when revealing
        QCOMPARE(outside, reveal ? 0 : 255);
        QVERIFY(!session.document()->selection.has_value());   // the selection is used up
    }
}

QTEST_MAIN(TestSyncRendering)
#include "TestSyncRendering.moc"
