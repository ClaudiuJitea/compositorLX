#include <QtTest>
#include <QImage>
#include <QPainter>
#include <algorithm>
#include <cmath>

#include "core/CameraRaw.h"
#include "core/EditorSession.h"
#include "io/RawImporter.h"
#include "rendering/RasterOperations.h"
#include "ui/CameraRawDialog.h"
#include "ui/RawDevelopDialog.h"

#include <QTemporaryFile>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>

using namespace compositor;


class TestCameraRaw : public QObject {
    Q_OBJECT

private:
    static QImage makeImage(int w, int h, int r, int g, int b, int a = 255) {
        QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
        img.fill(QColor(r, g, b, a));
        return img;
    }

    static QImage makeGray(int w = 4, int h = 4, int a = 255) {
        return makeImage(w, h, 128, 128, 128, a);
    }

    static int chroma(QRgb pixel) {
        const int r = qRed(pixel);
        const int g = qGreen(pixel);
        const int b = qBlue(pixel);
        return std::max({r, g, b}) - std::min({r, g, b});
    }

    static QImage makeStep() {
        QImage img(24, 4, QImage::Format_ARGB32_Premultiplied);
        QPainter p(&img);
        p.fillRect(0, 0, 12, 4, QColor(40, 40, 40));
        p.fillRect(12, 0, 12, 4, QColor(200, 200, 200));
        return img;
    }

    static QImage makeChecker(int width = 24, int height = 24) {
        QImage img(width, height, QImage::Format_ARGB32_Premultiplied);
        QPainter p(&img);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                bool light = (((x / 2) + (y / 2)) % 2 == 0);
                p.fillRect(x, y, 1, 1, light ? QColor(230, 230, 230) : QColor(25, 25, 25));
            }
        }
        return img;
    }

private slots:
    void testDefaultsLeavePixelsAndAlphaAlone();
    void testExposureAddsOneStopAndContrastPivotsAroundMidGray();
    void testTonalSlidersMoveTheEndTheyName();
    void testTemperatureWarmsAndTintMovesTowardMagenta();
    void testVibranceFavorsDullColorsAndProtectsSkinWhileSaturationDoesNot();
    void testEyedropperAndAutoNeutralizeAWarmPixel();
    void testAutoWhiteBalanceNeutralizesAfterTheScan();
    void testHiddenGroupIsLeftOutAndOkIsOneUndoOrNone();
    void testTextureAndClaritySharpenAnEdgeAndLeaveAFlatField();
    void testDehazeDeepensOrLiftsAndKeepsAlpha();
    void testGlowIsIdleAtZeroAndHalationFringeIsRedderThanDiffusion();
    void testVignetteDarkensCornersAndHighlightsOnlyWhileDarkening();
    void testGrainIsStableAndTheEffectsEyeDropsTheWholeGroup();
    void testHistogramFollowsTheGradeAndClippingPaintStaysOffTheResult();
    void testCurveMixerAndGradingChangeOnlyTheirOwnTones();
    void testDetailSharpeningNoiseAndMaskingPreview();
    void testOpticsDistortionDefringeAndDetailEye();
    void testGeometryWarpAndCalibrationPrimaries();
    void testGuidedUprightFollowsADrawnLineAndLeavesAnUnguidedPicture();
    void testRawImporterMatchesAndExtensions();
    void testRawImporterFixtureLibRawDimensionsAndAsShot();
    void testRawImporterFixtureDevelopmentAndPreviewSizing();
    void testRawImporterLimitsAndFailureBehavior();
    void testCameraRawDialogPreviewRollbackRevisionLifecycle();
    void testRawDevelopDialogDebounceSerializationAndCancellationLifecycle();
};


void TestCameraRaw::testDefaultsLeavePixelsAndAlphaAlone()
{
    const QImage input = makeGray(4, 4, 128);
    const QImage output = RasterOperations::cameraRaw(input, CameraRawSettings());
    QCOMPARE(output.pixel(0, 0), input.pixel(0, 0));

    CameraRawSettings broken;
    broken.exposure = std::numeric_limits<double>::quiet_NaN();
    broken.temperature = 400.0;
    QVERIFY(!broken.isValid());
    const CameraRawSettings norm = broken.normalized();
    QCOMPARE(norm.exposure, 0.0);
    QCOMPARE(norm.temperature, 100.0);
}

void TestCameraRaw::testExposureAddsOneStopAndContrastPivotsAroundMidGray()
{
    const QImage input = makeGray();
    CameraRawSettings settings;
    settings.exposure = 1.0;
    const QImage output = RasterOperations::cameraRaw(input, settings);
    const QRgb px = output.pixel(0, 0);
    QVERIFY(std::abs(qRed(px) - 176) <= 2);
    QCOMPARE(qRed(px), qGreen(px));
    QCOMPARE(qGreen(px), qBlue(px));

    const QImage translucent = makeGray(4, 4, 128);
    const QImage transOut = RasterOperations::cameraRaw(translucent, settings);
    QCOMPARE(qAlpha(transOut.pixel(0, 0)), qAlpha(translucent.pixel(0, 0)));

    QImage pair(2, 1, QImage::Format_ARGB32_Premultiplied);
    pair.setPixelColor(0, 0, QColor(64, 64, 64));
    pair.setPixelColor(1, 0, QColor(192, 192, 192));

    settings = CameraRawSettings();
    settings.contrast = 100.0;
    QImage pushed = RasterOperations::cameraRaw(pair, settings);
    QVERIFY(qRed(pushed.pixel(0, 0)) < 10);
    QVERIFY(qRed(pushed.pixel(1, 0)) > 250);

    settings.contrast = -100.0;
    QImage flat = RasterOperations::cameraRaw(pair, settings);
    QVERIFY(std::abs(qRed(flat.pixel(0, 0)) - 128) <= 2);
    QVERIFY(std::abs(qRed(flat.pixel(1, 0)) - 128) <= 2);
}

void TestCameraRaw::testTonalSlidersMoveTheEndTheyName()
{
    QImage brightAndMid(2, 1, QImage::Format_ARGB32_Premultiplied);
    brightAndMid.setPixelColor(0, 0, QColor(230, 230, 230));
    brightAndMid.setPixelColor(1, 0, QColor(128, 128, 128));

    CameraRawSettings settings;
    settings.highlights = -100.0;
    QImage recovered = RasterOperations::cameraRaw(brightAndMid, settings);
    QVERIFY(qRed(recovered.pixel(0, 0)) < 200);
    QVERIFY(std::abs(qRed(recovered.pixel(1, 0)) - 128) <= 2);

    settings = CameraRawSettings();
    settings.whites = 100.0;
    QImage clipped = RasterOperations::cameraRaw(brightAndMid, settings);
    QCOMPARE(qRed(clipped.pixel(0, 0)), 255);
    QVERIFY(std::abs(qRed(clipped.pixel(1, 0)) - 128) <= 2);

    QImage viz = RasterOperations::cameraRaw(brightAndMid, settings, CameraRawClipping::Highlights);
    QCOMPARE(viz.pixelColor(0, 0), QColor(255, 255, 255, 255));
    QCOMPARE(viz.pixelColor(1, 0), QColor(0, 0, 0, 255));

    QImage darkAndMid(2, 1, QImage::Format_ARGB32_Premultiplied);
    darkAndMid.setPixelColor(0, 0, QColor(20, 20, 20));
    darkAndMid.setPixelColor(1, 0, QColor(128, 128, 128));

    settings = CameraRawSettings();
    settings.shadows = 100.0;
    QImage opened = RasterOperations::cameraRaw(darkAndMid, settings);
    QVERIFY(qRed(opened.pixel(0, 0)) > 50);
    QVERIFY(std::abs(qRed(opened.pixel(1, 0)) - 128) <= 2);

    settings = CameraRawSettings();
    settings.blacks = -100.0;
    QImage crushed = RasterOperations::cameraRaw(darkAndMid, settings);
    QVERIFY(qRed(crushed.pixel(0, 0)) < 20);
    QVERIFY(std::abs(qRed(crushed.pixel(1, 0)) - 128) <= 2);

    QImage shadowViz = RasterOperations::cameraRaw(darkAndMid, settings, CameraRawClipping::Shadows);
    QCOMPARE(shadowViz.pixelColor(0, 0), QColor(0, 0, 0, 255));
    QCOMPARE(shadowViz.pixelColor(1, 0), QColor(255, 255, 255, 255));
}

void TestCameraRaw::testTemperatureWarmsAndTintMovesTowardMagenta()
{
    const QImage input = makeGray();
    CameraRawSettings settings;
    settings.temperature = 100.0;
    QImage warm = RasterOperations::cameraRaw(input, settings);
    QRgb warmPx = warm.pixel(0, 0);
    QVERIFY(qRed(warmPx) > 128);
    QVERIFY(qBlue(warmPx) < 128);
    QVERIFY(qRed(warmPx) > qBlue(warmPx));

    settings = CameraRawSettings();
    settings.tint = 100.0;
    QImage magenta = RasterOperations::cameraRaw(input, settings);
    QRgb magPx = magenta.pixel(0, 0);
    QVERIFY(qGreen(magPx) < 128);
    QVERIFY(qGreen(magPx) < qRed(magPx));
    QVERIFY(qGreen(magPx) < qBlue(magPx));
}

void TestCameraRaw::testVibranceFavorsDullColorsAndProtectsSkinWhileSaturationDoesNot()
{
    const QImage dullGreen = makeImage(4, 4, 77, 153, 77);
    const QImage saturatedGreen = makeImage(4, 4, 20, 200, 20);
    const QImage skin = makeImage(4, 4, 153, 115, 77);

    CameraRawSettings settings;
    settings.vibrance = 100.0;
    const int dullBefore = chroma(dullGreen.pixel(0, 0));
    const int saturatedBefore = chroma(saturatedGreen.pixel(0, 0));
    const int skinBefore = chroma(skin.pixel(0, 0));

    const int dullDelta = chroma(RasterOperations::cameraRaw(dullGreen, settings).pixel(0, 0)) - dullBefore;
    const int saturatedDelta = chroma(RasterOperations::cameraRaw(saturatedGreen, settings).pixel(0, 0)) - saturatedBefore;
    const int skinDelta = chroma(RasterOperations::cameraRaw(skin, settings).pixel(0, 0)) - skinBefore;

    QVERIFY(dullDelta > saturatedDelta + 10);
    QCOMPARE(dullBefore, skinBefore);
    QVERIFY(dullDelta > skinDelta + 10);

    const QImage red = makeImage(4, 4, 160, 120, 120);
    const QImage blue = makeImage(4, 4, 100, 100, 140);
    settings = CameraRawSettings();
    settings.saturation = 100.0;
    const int redBefore = chroma(red.pixel(0, 0));
    const int blueBefore = chroma(blue.pixel(0, 0));
    const int redAfter = chroma(RasterOperations::cameraRaw(red, settings).pixel(0, 0));
    const int blueAfter = chroma(RasterOperations::cameraRaw(blue, settings).pixel(0, 0));
    const double redRatio = double(redAfter) / double(redBefore);
    const double blueRatio = double(blueAfter) / double(blueBefore);
    QVERIFY(std::abs(redRatio - 2.0) < 0.15);
    QVERIFY(std::abs(blueRatio - 2.0) < 0.15);
}

void TestCameraRaw::testEyedropperAndAutoNeutralizeAWarmPixel()
{
    const double straightRed = 160.0 / 255.0;
    const double straightGreen = 140.0 / 255.0;
    const double straightBlue = 120.0 / 255.0;
    const QImage warm = makeImage(8, 8, 160, 140, 120);
    const int before = chroma(warm.pixel(0, 0));

    auto solved = CameraRawSettings::neutralizeStraight(straightRed, straightGreen, straightBlue);
    QVERIFY(solved.has_value());
    CameraRawSettings settings;
    settings.temperature = solved->first;
    settings.tint = solved->second;
    const QImage neutral = RasterOperations::cameraRaw(warm, settings);
    QVERIFY(chroma(neutral.pixel(0, 0)) < before / 2);

    auto autoSol = CameraRawSettings::autoBalance(warm);
    QVERIFY(autoSol.has_value());
    settings.temperature = autoSol->first;
    settings.tint = autoSol->second;
    const QImage averaged = RasterOperations::cameraRaw(warm, settings);
    QVERIFY(chroma(averaged.pixel(0, 0)) < before / 2);

    // Transparent pixel balance should return nullopt
    const QImage clear = makeImage(8, 8, 255, 0, 0, 0);
    QVERIFY(!CameraRawSettings::autoBalance(clear).has_value());
}

void TestCameraRaw::testAutoWhiteBalanceNeutralizesAfterTheScan()
{
    const QImage warm = makeImage(8, 8, 160, 140, 120);
    const int before = chroma(warm.pixel(0, 0));

    auto solved = CameraRawSettings::autoBalance(warm);
    QVERIFY(solved.has_value());
    CameraRawSettings settings;
    settings.whiteBalance = CameraRawWhiteBalance::Auto;
    settings.temperature = solved->first;
    settings.tint = solved->second;
    const QImage result = RasterOperations::cameraRaw(warm, settings);
    QVERIFY(chroma(result.pixel(0, 0)) < before / 2);
}

void TestCameraRaw::testHiddenGroupIsLeftOutAndOkIsOneUndoOrNone()
{
    const QImage input = makeGray(8, 8);
    CameraRawSettings settings;
    settings.exposure = 1.0;
    settings.temperature = 40.0;
    const CameraRawSettings hiddenLight = settings.applying(false, true);
    const QImage colorOnly = RasterOperations::cameraRaw(input, hiddenLight);
    QVERIFY(colorOnly != input);
    QCOMPARE(hiddenLight.exposure, 0.0);
    QCOMPARE(hiddenLight.temperature, 40.0);

    const CameraRawSettings neither = settings.applying(false, false);
    QVERIFY(neither.isIdentity());
    QCOMPARE(RasterOperations::cameraRaw(input, neither), input);
}

void TestCameraRaw::testTextureAndClaritySharpenAnEdgeAndLeaveAFlatField()
{
    const QImage flat = makeGray();
    CameraRawSettings settings;
    settings.texture = 100.0;
    settings.clarity = 100.0;
    const QImage flatResult = RasterOperations::cameraRaw(flat, settings);
    QCOMPARE(flatResult, flat);

    const QImage edge = makeStep();
    const int origFar = qRed(edge.pixel(0, 0));
    const int origNear = qRed(edge.pixel(8, 0));
    const int origEdge = qRed(edge.pixel(11, 0));

    settings = CameraRawSettings();
    settings.texture = 100.0;
    const QImage textured = RasterOperations::cameraRaw(edge, settings);
    QCOMPARE(qRed(textured.pixel(0, 0)), origFar);
    QCOMPARE(qRed(textured.pixel(8, 0)), origNear);
    QVERIFY(qRed(textured.pixel(11, 0)) != origEdge);

    settings = CameraRawSettings();
    settings.clarity = 100.0;
    const QImage clarified = RasterOperations::cameraRaw(edge, settings);
    QCOMPARE(qRed(clarified.pixel(0, 0)), origFar);
    QVERIFY(qRed(clarified.pixel(8, 0)) != origNear);

    settings.clarity = -100.0;
    const QImage softened = RasterOperations::cameraRaw(edge, settings);
    const int hardGap = std::abs(qRed(edge.pixel(12, 0)) - qRed(edge.pixel(11, 0)));
    const int softGap = std::abs(qRed(softened.pixel(12, 0)) - qRed(softened.pixel(11, 0)));
    QVERIFY(softGap < hardGap);
}

void TestCameraRaw::testDehazeDeepensOrLiftsAndKeepsAlpha()
{
    const QImage dark = makeImage(4, 4, 30, 30, 30);
    const QImage pale = makeImage(4, 4, 180, 150, 150);

    CameraRawSettings settings;
    settings.dehaze = 100.0;
    const QImage deepened = RasterOperations::cameraRaw(dark, settings);
    QVERIFY(qRed(deepened.pixel(0, 0)) < 30);

    const int paleBefore = chroma(pale.pixel(0, 0));
    const QImage paleAfter = RasterOperations::cameraRaw(pale, settings);
    QVERIFY(chroma(paleAfter.pixel(0, 0)) > paleBefore);

    settings.dehaze = -100.0;
    const QImage lifted = RasterOperations::cameraRaw(dark, settings);
    QVERIFY(qRed(lifted.pixel(0, 0)) > 30);

    const QImage faded = RasterOperations::cameraRaw(pale, settings);
    QVERIFY(chroma(faded.pixel(0, 0)) < paleBefore);

    const QImage translucent = makeImage(4, 4, 30, 30, 30, 128);
    settings.dehaze = 100.0;
    const QImage outTrans = RasterOperations::cameraRaw(translucent, settings);
    QCOMPARE(qAlpha(outTrans.pixel(0, 0)), 128);
}

void TestCameraRaw::testGlowIsIdleAtZeroAndHalationFringeIsRedderThanDiffusion()
{
    QImage spot(21, 21, QImage::Format_ARGB32_Premultiplied);
    spot.fill(Qt::black);
    QPainter p(&spot);
    p.fillRect(8, 8, 5, 5, Qt::white);

    CameraRawSettings settings;
    settings.glowWarmth = 100.0;
    settings.glowRange = 100.0;
    const QImage idle = RasterOperations::cameraRaw(spot, settings);
    QCOMPARE(idle, spot);

    settings.glow = 100.0;
    settings.glowWarmth = 100.0;
    settings.glowStyle = CameraRawGlowStyle::Diffusion;
    const QImage diffusion = RasterOperations::cameraRaw(spot, settings);

    settings.glowStyle = CameraRawGlowStyle::Halation;
    const QImage halation = RasterOperations::cameraRaw(spot, settings);

    const int fringeX = 15, fringeY = 10;
    QVERIFY(qRed(diffusion.pixel(fringeX, fringeY)) > qRed(diffusion.pixel(0, 0)));
    QCOMPARE(halation.pixelColor(0, 0), QColor(0, 0, 0, 255));

    const int diffRedExcess = qRed(diffusion.pixel(fringeX, fringeY)) - qGreen(diffusion.pixel(fringeX, fringeY));
    const int halRedExcess = qRed(halation.pixel(fringeX, fringeY)) - qGreen(halation.pixel(fringeX, fringeY));
    QVERIFY(halRedExcess > diffRedExcess);
}

void TestCameraRaw::testVignetteDarkensCornersAndHighlightsOnlyWhileDarkening()
{
    const QImage grayField = makeGray(9, 9);
    CameraRawSettings settings;
    settings.vignetteAmount = -100.0;
    const QImage darkened = RasterOperations::cameraRaw(grayField, settings);
    const int center = qRed(darkened.pixel(4, 4));
    const int corner = qRed(darkened.pixel(0, 0));
    QVERIFY(std::abs(center - 128) <= 2);
    QVERIFY(corner < center - 40);

    const QImage white = makeImage(9, 9, 255, 255, 255);
    settings.vignetteHighlights = 100.0;
    settings.vignetteStyle = CameraRawVignetteStyle::HighlightPriority;
    const int protectedVal = qRed(RasterOperations::cameraRaw(white, settings).pixel(0, 0));

    settings.vignetteHighlights = 0.0;
    const int exposed = qRed(RasterOperations::cameraRaw(white, settings).pixel(0, 0));
    QVERIFY(protectedVal > exposed + 40);

    settings.vignetteHighlights = 100.0;
    settings.vignetteStyle = CameraRawVignetteStyle::PaintOverlay;
    const int painted = qRed(RasterOperations::cameraRaw(white, settings).pixel(0, 0));
    QVERIFY(painted < protectedVal);

    settings = CameraRawSettings();
    settings.vignetteAmount = 100.0;
    settings.vignetteHighlights = 0.0;
    const QImage plain = RasterOperations::cameraRaw(grayField, settings);
    settings.vignetteHighlights = 100.0;
    const QImage withHighlights = RasterOperations::cameraRaw(grayField, settings);
    QCOMPARE(plain, withHighlights);
}

void TestCameraRaw::testGrainIsStableAndTheEffectsEyeDropsTheWholeGroup()
{
    const QImage field = makeGray(16, 16);
    CameraRawSettings settings;
    settings.grainSize = 40.0;
    settings.grainRoughness = 80.0;
    const QImage silent = RasterOperations::cameraRaw(field, settings, CameraRawClipping::None, 1.0, 4);
    QCOMPARE(silent, field);

    settings.grainAmount = 70.0;
    const QImage first = RasterOperations::cameraRaw(field, settings, CameraRawClipping::None, 1.0, 4);
    const QImage second = RasterOperations::cameraRaw(field, settings, CameraRawClipping::None, 1.0, 4);
    QCOMPARE(first, second);
    QVERIFY(first != field);
    QCOMPARE(qRed(first.pixel(0, 0)), qGreen(first.pixel(0, 0)));
    QCOMPARE(qGreen(first.pixel(0, 0)), qBlue(first.pixel(0, 0)));

    const QImage clear = makeImage(4, 4, 128, 128, 128, 0);
    const QImage clearOut = RasterOperations::cameraRaw(clear, settings, CameraRawClipping::None, 1.0, 4);
    QCOMPARE(qAlpha(clearOut.pixel(0, 0)), 0);

    const QImage edge = makeStep();
    settings = CameraRawSettings();
    settings.texture = 100.0;
    settings.grainAmount = 50.0;
    const CameraRawSettings hidden = settings.applying(true, true, false);
    QVERIFY(hidden.isIdentity());
    const QImage hiddenPixels = RasterOperations::cameraRaw(edge, hidden, CameraRawClipping::None, 1.0, 2);
    QCOMPARE(hiddenPixels, edge);
}

void TestCameraRaw::testHistogramFollowsTheGradeAndClippingPaintStaysOffTheResult()
{
    const QImage black = makeImage(4, 4, 0, 0, 0);
    const QImage white = makeImage(4, 4, 255, 255, 255);
    auto blackScope = CameraRawScope::make(black);
    auto whiteScope = CameraRawScope::make(white);
    QVERIFY(blackScope.has_value());
    QVERIFY(whiteScope.has_value());

    const auto peakIdx = [](const std::array<double, 256> &bins) -> int {
        return std::max_element(bins.begin(), bins.end()) - bins.begin();
    };
    QCOMPARE(peakIdx(blackScope->red), 0);
    QCOMPARE(peakIdx(whiteScope->red), 255);

    CameraRawSettings settings;
    settings.exposure = 1.0;
    auto shifted = CameraRawScope::make(RasterOperations::cameraRaw(makeGray(), settings));
    QVERIFY(shifted.has_value());
    QVERIFY(peakIdx(shifted->red) > 128);

    const QImage shadowed = CameraRawScope::overlay(black, true, false);
    QVERIFY(qBlue(shadowed.pixel(0, 0)) > qRed(shadowed.pixel(0, 0)));

    const QImage highlighted = CameraRawScope::overlay(white, false, true);
    QVERIFY(qRed(highlighted.pixel(0, 0)) > qBlue(highlighted.pixel(0, 0)));

    const QImage untouched = CameraRawScope::overlay(black, false, false);
    QCOMPARE(untouched, black);

    const QImage red = makeImage(4, 4, 255, 0, 0);
    auto redScope = CameraRawScope::make(red);
    QVERIFY(redScope.has_value());
    const int hottest = std::max_element(redScope->vectorscope.begin(), redScope->vectorscope.end()) - redScope->vectorscope.begin();
    QVERIFY(hottest % CameraRawScope::scopeSide > CameraRawScope::scopeSide / 2);
}

void TestCameraRaw::testCurveMixerAndGradingChangeOnlyTheirOwnTones()
{
    const QImage dark = makeImage(4, 4, 31, 31, 31);
    const QImage light = makeImage(4, 4, 158, 158, 158);

    CameraRawSettings settings;
    settings.curve.shadows = 100.0;
    const int darkGain = qRed(RasterOperations::cameraRaw(dark, settings).pixel(0, 0)) - 31;
    const int lightGain = qRed(RasterOperations::cameraRaw(light, settings).pixel(0, 0)) - 158;
    QVERIFY(darkGain > lightGain + 8);

    settings = CameraRawSettings();
    settings.curve.rgb = CameraRawCurveSettings::strongContrast;
    const QImage midDark = makeImage(4, 4, 64, 64, 64);
    const int contrasted = qRed(RasterOperations::cameraRaw(midDark, settings).pixel(0, 0));
    QVERIFY(contrasted < 55);

    settings = CameraRawSettings();
    settings.mixer.hue[0] = 100.0;
    const QImage pureRed = makeImage(4, 4, 255, 0, 0);
    const QImage shifted = RasterOperations::cameraRaw(pureRed, settings);
    QVERIFY(qGreen(shifted.pixel(0, 0)) > qBlue(shifted.pixel(0, 0)));

    settings = CameraRawSettings();
    settings.grading.shadows.saturation = 100.0;
    const QImage gradedDark = RasterOperations::cameraRaw(dark, settings);
    const QImage white = makeImage(4, 4, 255, 255, 255);
    const QImage gradedLight = RasterOperations::cameraRaw(white, settings);
    QVERIFY(qRed(gradedDark.pixel(0, 0)) > qGreen(gradedDark.pixel(0, 0)) + 5);
    QVERIFY(std::abs(qRed(gradedLight.pixel(0, 0)) - qGreen(gradedLight.pixel(0, 0))) <= 2);

    settings.grading.balance = 100.0;
    const QImage balanced = RasterOperations::cameraRaw(dark, settings);
    QVERIFY(qRed(balanced.pixel(0, 0)) - qGreen(balanced.pixel(0, 0)) <
            qRed(gradedDark.pixel(0, 0)) - qGreen(gradedDark.pixel(0, 0)));

    settings = CameraRawSettings();
    settings.curve.shadows = 100.0;
    const CameraRawSettings hidden = settings.applying(true, true, true, false);
    QCOMPARE(RasterOperations::cameraRaw(dark, hidden), dark);
}

void TestCameraRaw::testDetailSharpeningNoiseAndMaskingPreview()
{
    const QImage edge = makeStep();
    CameraRawSettings settings;
    settings.detail.sharpenAmount = 150.0;
    settings.detail.sharpenRadius = 50.0;
    const QImage sharpened = RasterOperations::cameraRaw(edge, settings);
    QVERIFY(sharpened != edge);

    settings = CameraRawSettings();
    settings.detail.noiseLuminance = 80.0;
    const QImage flat = makeGray(8, 8);
    const QImage smoothed = RasterOperations::cameraRaw(flat, settings);
    QCOMPARE(smoothed, flat);

    settings.detail.sharpenMasking = 50.0;
    const QImage mask = RasterOperations::cameraRaw(edge, settings, CameraRawClipping::None, 1.0, 0, -1, true);
    for (int y = 0; y < mask.height(); ++y) {
        for (int x = 0; x < mask.width(); ++x) {
            QRgb p = mask.pixel(x, y);
            QCOMPARE(qRed(p), qGreen(p));
            QCOMPARE(qGreen(p), qBlue(p));
        }
    }
}

void TestCameraRaw::testOpticsDistortionDefringeAndDetailEye()
{
    const QImage stepped = makeChecker();
    CameraRawSettings settings;
    settings.optics.distortion = 100.0;
    const QImage warped = RasterOperations::cameraRaw(stepped, settings);
    QVERIFY(warped != stepped);

    const QImage purple = makeImage(4, 4, 204, 51, 230);
    const int purpleBefore = chroma(purple.pixel(0, 0));
    settings = CameraRawSettings();
    settings.optics.purpleAmount = 100.0;
    settings.optics.purpleHueLow = 250.0;
    settings.optics.purpleHueHigh = 320.0;
    const QImage defringed = RasterOperations::cameraRaw(purple, settings);
    QVERIFY(chroma(defringed.pixel(0, 0)) < purpleBefore);

    settings = CameraRawSettings();
    settings.optics.removeChromaticAberration = true;
    QVERIFY(settings.optics.adjusts());

    settings = CameraRawSettings();
    settings.detail.sharpenAmount = 40.0;
    const CameraRawSettings hidden = settings.applying(true, true, true, true, true, true, false);
    const QImage edge = makeStep();
    QCOMPARE(RasterOperations::cameraRaw(edge, hidden), edge);
}

void TestCameraRaw::testGeometryWarpAndCalibrationPrimaries()
{
    const QImage grid = makeChecker(12, 12);
    CameraRawSettings settings;
    settings.geometry.vertical = 40.0;
    const QImage warped = settings.geometry.apply(grid);
    QVERIFY(warped != grid);

    settings = CameraRawSettings();
    settings.calibration.redHue = 80.0;
    const QImage red = makeImage(4, 4, 255, 0, 0);
    const QImage calibrated = RasterOperations::cameraRaw(red, settings);
    QVERIFY(calibrated.pixel(0, 0) != red.pixel(0, 0));

    const CameraRawSettings hidden = settings.applying(true, true, true, true, true, true, true, true, true, false);
    const QImage hiddenOut = RasterOperations::cameraRaw(red, hidden);
    QCOMPARE(hiddenOut.pixel(0, 0), red.pixel(0, 0));
}

void TestCameraRaw::testGuidedUprightFollowsADrawnLineAndLeavesAnUnguidedPicture()
{
    const QImage cool = makeImage(16, 16, 51, 115, 204);
    const QImage warm = makeImage(16, 16, 217, 64, 38);

    CameraRawSettings settings;
    settings.geometry.upright = CameraRawUprightMode::Guided;
    QCOMPARE(RasterOperations::cameraRaw(cool, settings), cool);
    QCOMPARE(RasterOperations::cameraRaw(warm, settings), warm);

    settings.geometry.guides = {{0.1, 0.15, 0.9, 0.8}};
    const QImage coolGuided = RasterOperations::cameraRaw(cool, settings);
    const QImage warmGuided = RasterOperations::cameraRaw(warm, settings);
    QVERIFY(coolGuided != cool);
    QVERIFY(warmGuided != warm);
    QVERIFY(coolGuided != warmGuided);
}

void TestCameraRaw::testRawImporterMatchesAndExtensions()
{
    QVERIFY(RawImporter::matches(QStringLiteral("photo.CR2")));
    QVERIFY(RawImporter::matches(QStringLiteral("photo.dng")));
    QVERIFY(RawImporter::matches(QStringLiteral("photo.NEF")));
    QVERIFY(RawImporter::matches(QStringLiteral("photo.ARW")));
    QVERIFY(RawImporter::matches(QStringLiteral("photo.orf")));
    QVERIFY(RawImporter::matches(QStringLiteral("photo.rw2")));
    QVERIFY(!RawImporter::matches(QStringLiteral("photo.png")));
    QVERIFY(!RawImporter::matches(QStringLiteral("photo.jpg")));
    QVERIFY(!RawImporter::matches(QStringLiteral("photo.psd")));

    RawDevelopSettings s;
    QVERIFY(s.isAsShot());
    s.exposure = 1.5f;
    QVERIFY(!s.isAsShot());
    s.reset();
    QVERIFY(s.isAsShot());
    QCOMPARE(s.exposure, 0.0f);
}

void TestCameraRaw::testRawImporterFixtureLibRawDimensionsAndAsShot()
{
    const QString fixturePath = QStringLiteral(FIXTURES_DIR) + QStringLiteral("/raw/sample.kdc");
    QVERIFY2(QFile::exists(fixturePath), qPrintable(fixturePath));
    QVERIFY(RawImporter::matches(fixturePath));

    RawImporter::RawDimensions dims;
    QVERIFY(RawImporter::probeDimensions(fixturePath, dims));
    QCOMPARE(dims.visibleWidth, 848);
    QCOMPARE(dims.visibleHeight, 976);
    QCOMPARE(dims.outputWidth, 1301);
    QCOMPARE(dims.outputHeight, 976);
    QVERIFY(std::abs(dims.pixelAspect - 1.53459) < 0.01);

    int width = 0, height = 0;
    QVERIFY(RawImporter::pixelSize(fixturePath, width, height));
    QVERIFY(width > 0);
    QVERIFY(height > 0);
    QCOMPARE(width, 1301);
    QCOMPARE(height, 976);

    const auto asShot = RawImporter::asShot(fixturePath);
    QVERIFY(asShot.has_value());
    QVERIFY(asShot->isAsShot());
    QVERIFY(asShot->temperature >= 2000.0f && asShot->temperature <= 12000.0f);
    QCOMPARE(asShot->asShotTemperature, asShot->temperature);
    QCOMPARE(asShot->tint, 0.0f);
    QCOMPARE(asShot->exposure, 0.0f);
    QCOMPARE(asShot->boost, 1.0f);
}

void TestCameraRaw::testRawImporterFixtureDevelopmentAndPreviewSizing()
{
    const QString fixturePath = QStringLiteral(FIXTURES_DIR) + QStringLiteral("/raw/sample.kdc");
    const auto asShot = RawImporter::asShot(fixturePath);
    QVERIFY(asShot.has_value());

    // 1. Full development (limit = 0)
    const QImage full = RawImporter::develop(fixturePath, *asShot, 0);
    QVERIFY(!full.isNull());
    QCOMPARE(full.format(), QImage::Format_ARGB32_Premultiplied);
    QCOMPARE(full.width(), 1301);
    QCOMPARE(full.height(), 976);

    bool hasNonZero = false;
    for (int y = 0; y < full.height(); y += 50) {
        for (int x = 0; x < full.width(); x += 50) {
            QRgb px = full.pixel(x, y);
            if (qRed(px) > 0 || qGreen(px) > 0 || qBlue(px) > 0) {
                hasNonZero = true;
                break;
            }
        }
        if (hasNonZero) break;
    }
    QVERIFY(hasNonZero);

    // 2. Downscaled preview sizing (limit = 800)
    const QImage preview = RawImporter::develop(fixturePath, *asShot, 800);
    QVERIFY(!preview.isNull());
    QCOMPARE(preview.format(), QImage::Format_ARGB32_Premultiplied);
    const int longest = std::max(preview.width(), preview.height());
    QVERIFY(longest <= 800);
    QCOMPARE(preview.width(), 651);
    QCOMPARE(preview.height(), 488);

    // 3. Multi-parameter development: exposure shifts
    RawDevelopSettings brightSettings = *asShot;
    brightSettings.exposure = 2.0f;
    const QImage bright = RawImporter::develop(fixturePath, brightSettings, 800);
    QVERIFY(!bright.isNull());

    RawDevelopSettings darkSettings = *asShot;
    darkSettings.exposure = -2.0f;
    const QImage dark = RawImporter::develop(fixturePath, darkSettings, 800);
    QVERIFY(!dark.isNull());

    const QRgb brightPx = bright.pixel(bright.width() / 2, bright.height() / 2);
    const QRgb darkPx = dark.pixel(dark.width() / 2, dark.height() / 2);
    QVERIFY(qRed(brightPx) >= qRed(darkPx));
    QVERIFY(qGreen(brightPx) >= qGreen(darkPx));
}

void TestCameraRaw::testRawImporterLimitsAndFailureBehavior()
{
    const QString fixturePath = QStringLiteral(FIXTURES_DIR) + QStringLiteral("/raw/sample.kdc");

    // 1. Non-existent file
    const QString nonExistent = QStringLiteral("/tmp/does_not_exist_xyz_12345.cr2");
    int w = 0, h = 0;
    QVERIFY(!RawImporter::pixelSize(nonExistent, w, h));
    QVERIFY(!RawImporter::asShot(nonExistent).has_value());
    QVERIFY(RawImporter::develop(nonExistent, RawDevelopSettings{}).isNull());

    // 2. Zero-byte file
    QTemporaryFile emptyFile;
    emptyFile.setFileTemplate(QStringLiteral("empty_XXXXXX.kdc"));
    QVERIFY(emptyFile.open());
    emptyFile.close();
    QVERIFY(!RawImporter::pixelSize(emptyFile.fileName(), w, h));
    QVERIFY(!RawImporter::asShot(emptyFile.fileName()).has_value());
    QVERIFY(RawImporter::develop(emptyFile.fileName(), RawDevelopSettings{}).isNull());

    // 3. Non-RAW / corrupt file with RAW extension
    QTemporaryFile fakeRaw;
    fakeRaw.setFileTemplate(QStringLiteral("fake_XXXXXX.dng"));
    QVERIFY(fakeRaw.open());
    fakeRaw.write("This is not a real RAW file at all. Just random plain text bytes.");
    fakeRaw.close();
    QVERIFY(!RawImporter::pixelSize(fakeRaw.fileName(), w, h));
    QVERIFY(!RawImporter::asShot(fakeRaw.fileName()).has_value());
    QVERIFY(RawImporter::develop(fakeRaw.fileName(), RawDevelopSettings{}).isNull());

    // 4. Cancellation token aborts development early
    std::atomic<bool> cancelled{true};
    const QImage aborted = RawImporter::develop(fixturePath, RawDevelopSettings{}, 0, &cancelled);
    QVERIFY(aborted.isNull());

    // 5. Test 848-to-1301 width difference and budget rejection:
    // Visible sensor pixels = 848 * 976 = 827,648.
    // Demosaiced/output pixels = 1301 * 976 = 1,269,776.
    // If budget is 1,000,000 (which is > 827,648 and < 1,269,776):
    // The pre-allocation bound must reject the file before unpack/process.
    const auto asShot = RawImporter::asShot(fixturePath);
    QVERIFY(asShot.has_value());
    const QImage budgetRejected = RawImporter::develop(fixturePath, *asShot, 0, nullptr, 1000000LL);
    QVERIFY(budgetRejected.isNull());

    // With a sufficient budget (e.g. 1,500,000 > 1,269,776), development succeeds:
    const QImage budgetAccepted = RawImporter::develop(fixturePath, *asShot, 0, nullptr, 1500000LL);
    QVERIFY(!budgetAccepted.isNull());
    QCOMPARE(budgetAccepted.width(), 1301);
    QCOMPARE(budgetAccepted.height(), 976);

    // 6. Verify limits constants
    QCOMPARE(RawImporter::MaxFileBytes, 512LL * 1024LL * 1024LL);
    QCOMPARE(RawImporter::MaxDimension, 30000);
    QCOMPARE(RawImporter::MaxDecodedPixels, 100000000LL);
    QCOMPARE(RawImporter::MaxOutputPixels, 100000000LL);
}

void TestCameraRaw::testCameraRawDialogPreviewRollbackRevisionLifecycle()
{
    EditorSession session;
    session.createDocument(100, 100, false);
    QImage testImg = makeImage(100, 100, 100, 100, 100);
    session.insertPixelLayer(testImg, QPointF(0, 0), QStringLiteral("Layer 1"));
    const Layer *active = session.activeLayer();
    QVERIFY(active);
    const QUuid layerId = active->id;

    const quint64 initialRev = session.sessionRevision();
    const QRgb originalPx = active->image.pixel(50, 50);

    const int undoCountBeforeDialog = session.history().undoCount();

    // 1. Open CameraRawDialog: preview updates and advances revision
    {
        CameraRawDialog dialog(nullptr, session, layerId);
        QVERIFY(session.sessionRevision() > initialRev);

        const quint64 previewRev = session.sessionRevision();

        auto *exposureSpin = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("exposureSpin"));
        QVERIFY(exposureSpin);
        exposureSpin->setValue(2.0);
        QVERIFY(session.sessionRevision() > previewRev);

        auto *previewBox = dialog.findChild<QCheckBox *>(QStringLiteral("previewBox"));
        QVERIFY(previewBox);
        const quint64 beforeUncheck = session.sessionRevision();
        previewBox->setChecked(false);
        QVERIFY(session.sessionRevision() > beforeUncheck);
        QCOMPARE(session.activeLayer()->image.pixel(50, 50), originalPx);

        const quint64 beforeCheck = session.sessionRevision();
        previewBox->setChecked(true);
        QVERIFY(session.sessionRevision() > beforeCheck);

        const quint64 beforeReject = session.sessionRevision();
        dialog.reject();
        QVERIFY(session.sessionRevision() > beforeReject);
        QCOMPARE(session.activeLayer()->image.pixel(50, 50), originalPx);
        QCOMPARE(session.history().undoCount(), undoCountBeforeDialog);
    }

    // 2. Open CameraRawDialog, change settings, and accept -> commit with undo
    {
        const quint64 beforeDialog = session.sessionRevision();
        CameraRawDialog dialog(nullptr, session, layerId);
        auto *exposureSpin = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("exposureSpin"));
        QVERIFY(exposureSpin);
        exposureSpin->setValue(1.5);

        dialog.accept();
        QVERIFY(session.sessionRevision() > beforeDialog);
        QCOMPARE(session.history().undoCount(), undoCountBeforeDialog + 1);
        QCOMPARE(session.history().undoName(), QStringLiteral("Camera Raw Filter"));




        const QRgb modifiedPx = session.activeLayer()->image.pixel(50, 50);
        QVERIFY(modifiedPx != originalPx);

        session.undo();
        QCOMPARE(session.activeLayer()->image.pixel(50, 50), originalPx);

        session.redo();
        QCOMPARE(session.activeLayer()->image.pixel(50, 50), modifiedPx);
    }
}


void TestCameraRaw::testRawDevelopDialogDebounceSerializationAndCancellationLifecycle()
{
    const QString fixturePath = QStringLiteral(FIXTURES_DIR) + QStringLiteral("/raw/sample.kdc");

    // 1. Debounce and monotonic revision advancement during rapid slider dragging
    {
        RawDevelopDialog dialog(nullptr, fixturePath);
        const quint64 initialRev = dialog.previewRevision();

        auto *exposureSpin = dialog.findChild<QDoubleSpinBox *>();
        QVERIFY(exposureSpin);

        for (int i = 1; i <= 10; ++i) {
            exposureSpin->setValue(double(i) * 0.1);
        }
        QVERIFY(dialog.previewRevision() > initialRev);

        auto *previewLabel = dialog.findChild<QLabel *>();
        QVERIFY(previewLabel);

        int elapsed = 0;
        while (elapsed < 5000 && (dialog.isPreviewRunning() || dialog.isPreviewPending() || previewLabel->pixmap().isNull())) {
            QTest::qWait(50);
            elapsed += 50;
        }

        QVERIFY(!previewLabel->pixmap().isNull());

        QPushButton *resetBtn = nullptr;
        const auto buttons = dialog.findChildren<QPushButton *>();
        for (auto *b : buttons) {
            if (b->text().contains(QStringLiteral("Reset"))) {
                resetBtn = b;
                break;
            }
        }
        QVERIFY(resetBtn);
        const quint64 beforeResetRev = dialog.previewRevision();
        resetBtn->click();
        QVERIFY(dialog.previewRevision() > beforeResetRev);
        QVERIFY(dialog.settings().isAsShot());

        dialog.reject();
    }

    // 2. Safe cancellation / reject while preview is running
    {
        auto *dialog = new RawDevelopDialog(nullptr, fixturePath);
        dialog->triggerPreviewImmediate();
        dialog->reject();
        delete dialog;
    }

    // 3. Full Import workflow produces developed image
    {
        RawDevelopDialog dialog(nullptr, fixturePath);
        QPushButton *importBtn = nullptr;
        const auto buttons = dialog.findChildren<QPushButton *>();
        for (auto *b : buttons) {
            if (b->text().contains(QStringLiteral("Import"))) {
                importBtn = b;
                break;
            }
        }
        QVERIFY(importBtn);
        importBtn->click();

        int elapsed = 0;
        while (elapsed < 5000 && (dialog.isImportRunning() || dialog.developedImage().isNull())) {
            QTest::qWait(50);
            elapsed += 50;
        }

        const QImage developed = dialog.developedImage();
        QVERIFY(!developed.isNull());
        QCOMPARE(developed.width(), 1301);
        QCOMPARE(developed.height(), 976);

        EditorSession session;
        QVERIFY(session.insertImage(developed, QStringLiteral("sample"), std::nullopt));
        QVERIFY(session.hasDocument());
        QCOMPARE(session.document()->canvasSize, QSize(1301, 976));
    }

    // 4. Safe cancellation / reject while full Import is running
    {
        auto *dialog = new RawDevelopDialog(nullptr, fixturePath);
        QPushButton *importBtn = nullptr;
        const auto buttons = dialog->findChildren<QPushButton *>();
        for (auto *b : buttons) {
            if (b->text().contains(QStringLiteral("Import"))) {
                importBtn = b;
                break;
            }
        }
        QVERIFY(importBtn);
        importBtn->click();
        QVERIFY(dialog->isImportRunning());

        dialog->reject();
        delete dialog;
    }
}

QTEST_MAIN(TestCameraRaw)
#include "TestCameraRaw.moc"

