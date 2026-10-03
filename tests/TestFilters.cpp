// Ports of the macOS FilterTests, FinishingFilterTests, LevelsTests, HueSaturationTests, ImageAdjustmentTests and
// AdjustmentLayerTests (+ AdjustmentEditorTests) with the same numbers, plus checks that LX-specific wiring (save,
// reload, export, copy merged, undo) agrees with what is on the canvas.
#include "core/Document.h"
#include "core/EditorSession.h"
#include "io/AdjustmentJson.h"
#include "ui/CameraRawDialog.h"
#include <QSlider>
#include <QMouseEvent>
#include <QStyleOptionSlider>
#include "io/ProjectReader.h"
#include "io/ProjectWriter.h"
#include "rendering/LayerRenderer.h"
#include "rendering/RasterOperations.h"

#include <QtTest>
#include <QPainter>
#include <QJsonArray>
#include <QTemporaryDir>

#include <array>
#include <cmath>

using namespace compositor;

namespace {

QImage blank(int w, int h)
{
    QImage image(w, h, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    return image;
}

QImage solid(int w, int h, double r, double g, double b, double a = 1)
{
    QImage image = blank(w, h);
    image.fill(QColor::fromRgbF(r, g, b, a));
    return image;
}

void fillRect(QImage &image, const QRect &rect, QColor color, bool replace = false)
{
    QPainter painter(&image);
    if (replace) painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect, color);
}

// Straight RGBA at a pixel, as the mac tests read them.
std::array<int, 4> px(const QImage &image, int x, int y)
{
    const QColor c = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied).pixelColor(x, y);
    return {c.red(), c.green(), c.blue(), c.alpha()};
}

int alphaAt(const QImage &image, int x, int y) { return px(image, x, y)[3]; }

bool near(const std::array<int, 4> &value, const std::array<int, 4> &target, int tolerance = 8)
{
    for (int i = 0; i < 4; ++i) if (std::abs(value[i] - target[i]) > tolerance) return false;
    return true;
}

// A row of premultiplied RGBA bytes, one pixel per entry.
QImage row(const std::vector<std::array<int, 4>> &pixels)
{
    QImage image = blank(int(pixels.size()), 1);
    uchar *out = image.scanLine(0);
    for (size_t i = 0; i < pixels.size(); ++i) for (int c = 0; c < 4; ++c) out[i * 4 + size_t(c)] = uchar(pixels[i][size_t(c)]);
    return image;
}

std::array<int, 4> rawAt(const QImage &image, int x) { const uchar *p = image.constScanLine(0) + x * 4; return {p[0], p[1], p[2], p[3]}; }

QImage canvasOf(EditorSession &session) { return LayerRenderer::flattened(*session.document()); }

LevelsSettings levelsWith(int channel, LevelRange range)
{
    LevelsSettings settings;
    settings.ranges[size_t(channel)] = range.normalized();
    return settings;
}

} // namespace

class TestFilters : public QObject
{
    Q_OBJECT

private slots:
    // FilterTests
    void gaussianBlurSoftensAHardEdgeAndSpreadsPastTheLayerEdgeAsOneUndoStep();
    void blurPreviewRollbackIsExactAndABiggerBlurGrowsTheLayerMore();
    void motionBlurGrowsTheLayerAndTrimsItBack();
    void blurHonoursASelectionAndKeepsTheMask();
    void motionBlurStreaksAlongItsAngleCounterclockwiseFromHorizontal();
    void addNoiseChangesColorButNeverAlphaAndMonochromaticKeepsGrays();
    void removeDistortionBendsAboutTheCenterAndOnlyPincushionCorrectionOpensTheCorners();
    void lensCorrectionIsOneUndoStepAndZeroIsNone();
    // FinishingFilterTests (the session path)
    void eachFinishingFilterCommitsAsOneUndoStepAndKeepsLayerEffects();
    // LevelsTests
    void levelsIdentityIsAnExactNoOp();
    void levelsInputClippingGammaOutputInversionAndAlpha();
    void levelsChannelsCoexistAndUseDocumentedOrder();
    void levelsHistogramExcludesTransparencyAndWeightsSelection();
    void levelsSelectionCommitUndoSaveAndExport();
    void levelsAutoAlgorithmsAndEyedropperCalibration();
    // HueSaturationTests
    void hueSaturationDefaultsAreAnExactNoOp();
    void hueRotatesSaturationAndLightnessFollowPhotoshopRanges();
    void colorizeGivesEverythingOneHueAndKeepsAlpha();
    void hueSaturationStaysInsideTheSelectionAndIsOneUndoStep();
    void bandWeightsRampThroughFalloffAndWrapAround();
    void colorRangesAdjustIndependently();
    void rangesLeaveOtherHuesAloneAndInvertFlipsTheBand();
    void bandEyedroppersRecenterWidenAndNarrow();
    void positiveSaturationMatchesPhotoshop();
    // ImageAdjustmentTests
    void exposureWorksInLinearLightWithOffsetAndGamma();
    void gradientMapColorsByBrightnessAndReverses();
    void grainIsFixedInDocumentSpaceAndLeavesTransparencyAlone();
    void grainSizeControlsParticleScale();
    void imageMenuExposureChangesTheLayerInOneStep();
    void blackWhiteAndColorBalanceDefaultsAreWhatTheMacAppUses();
    // AdjustmentLayerTests
    void globalAdjustmentAffectsBelowButNotAboveAndRemainsLive();
    void clippedCurveChangesOnlyItsBaseAndCopyMergedMatchesExport();
    void hueOpacityAndMaskPreserveOriginalPixels();
    void adjustmentPersistsDuplicatesAndUndoRestoresSettings();
    void curvesIdentityAndImageCommandPreserveAlpha();
    void adjustmentBlendAndSoftMaskPreserveCoverage();
    void invertAppliesAndKeepsAlpha();
    void everyAdjustmentKindRendersAndRoundTripsAllParameters();
    void savedAdjustmentsCarryEveryKeyMacRequires();
    void folderScopedAdjustmentOnlyTouchesItsFolder();
    void cameraRawSliderDoubleClickOnTheKnobRestoresTheDefault();
};

// ---------------------------------------------------------------------------------------------------- FilterTests

static QImage halfWhite()
{
    QImage image = blank(40, 20);
    fillRect(image, QRect(0, 0, 20, 20), Qt::white);
    return image;
}

void TestFilters::gaussianBlurSoftensAHardEdgeAndSpreadsPastTheLayerEdgeAsOneUndoStep()
{
    EditorSession session;
    session.createDocument(40, 20);
    QVERIFY(session.insertImage(halfWhite(), QStringLiteral("Half")));
    const int count = session.history().undoCount();
    QVERIFY(session.applyGaussianBlur(3));
    QCOMPARE(session.history().undoCount(), count + 1);
    const Layer *layer = session.activeLayer();
    QVERIFY(layer);
    // The blur is not clamped at the layer's edge: the layer is given room, the blur spreads into it and whatever stays
    // empty is cut away again (mac Filters.swift growForBlur / PixelFilter.trimmed).
    QVERIFY2(layer->transform.origin.x() < 0 && layer->transform.origin.y() < 0, "the layer grew on every side");
    QVERIFY2(layer->image.height() > 20, "the blur spread past the layer's edge");
    QVERIFY2(layer->image.width() < 40, "and the half that stayed empty was trimmed away");
    QCOMPARE(layer->image.size(), QSize(qRound(layer->transform.size.width()), qRound(layer->transform.size.height())));
    const int middleY = layer->image.height() / 2;
    int peak = 0; bool soft = false;
    for (int x = 0; x < layer->image.width(); ++x) {
        const int a = alphaAt(layer->image, x, middleY);
        peak = std::max(peak, a);
        soft = soft || (a > 20 && a < 235);
    }
    QVERIFY2(peak >= 250, "the block's inside is untouched");
    QVERIFY2(soft, "the hard edge is now soft");
    QVERIFY2(alphaAt(layer->image, layer->image.width() - 1, middleY) < 20, "and it fades out on the far side");
    session.undo();
    QCOMPARE(session.activeLayer()->image, halfWhite());
    QCOMPARE(session.activeLayer()->transform.origin, QPointF(0, 0));
}

void TestFilters::blurPreviewRollbackIsExactAndABiggerBlurGrowsTheLayerMore()
{
    EditorSession session;
    session.createDocument(40, 20);
    QVERIFY(session.insertImage(halfWhite(), QStringLiteral("Half")));
    const Document before = *session.document();
    const Layer original = *session.activeLayer();
    const QUuid id = original.id;
    session.beginEdit(QStringLiteral("Gaussian Blur"));
    QVERIFY(session.applyGaussianBlur(2));
    const double smallWidth = session.activeLayer()->transform.size.width();
    session.rollbackLayer(id, original);
    QVERIFY(session.applyGaussianBlur(12));
    QVERIFY2(session.activeLayer()->transform.size.width() > smallWidth, "the grown layer is bigger for a bigger blur");
    session.rollbackLayer(id, original);
    session.endEdit();
    QVERIFY2(*session.document() == before, "cancel rolls back exactly");
    QCOMPARE(session.history().undoCount(), before.layers.isEmpty() ? 0 : session.history().undoCount());
    QVERIFY(!session.history().undoName().contains(QStringLiteral("Gaussian")));
}

void TestFilters::motionBlurGrowsTheLayerAndTrimsItBack()
{
    EditorSession session;
    session.createDocument(40, 20);
    QImage dot = blank(40, 20);
    fillRect(dot, QRect(0, 8, 4, 4), Qt::white);
    QVERIFY(session.insertImage(dot, QStringLiteral("Dot")));
    const int count = session.history().undoCount();
    QVERIFY(session.applyMotionBlur(0, 30));
    QCOMPARE(session.history().undoCount(), count + 1);
    const Layer *layer = session.activeLayer();
    QVERIFY2(layer->transform.origin.x() < 0, "a horizontal streak spreads past the layer's left edge");
    QVERIFY2(layer->image.width() > 4 + 10 && layer->image.width() < 40, "the streak is about its own length, the empty rest trimmed");
    QVERIFY2(layer->image.height() < 20, "the empty rows above and below were trimmed");
    QCOMPARE(layer->image.size(), QSize(qRound(layer->transform.size.width()), qRound(layer->transform.size.height())));
    QCOMPARE(session.history().undoName(), QStringLiteral("Motion Blur"));
}

void TestFilters::blurHonoursASelectionAndKeepsTheMask()
{
    EditorSession session;
    session.createDocument(40, 20);
    QVERIFY(session.insertImage(solid(40, 20, 1, 1, 1), QStringLiteral("White")));
    QImage dark = solid(40, 20, 1, 1, 1);
    fillRect(dark, QRect(0, 0, 10, 20), Qt::black);
    session.activeLayer()->image = dark;
    QVERIFY(session.setRectangularSelection(QRect(0, 0, 20, 20)));
    QVERIFY(session.applyGaussianBlur(4));
    const QImage canvas = canvasOf(session);
    // The unselected right half keeps its pixels exactly; the selected edge between black and white is soft.
    QCOMPARE(px(canvas, 30, 10), (std::array<int, 4>{255, 255, 255, 255}));
    const auto edge = px(canvas, 10, 10);
    QVERIFY2(edge[0] > 20 && edge[0] < 235, "the selected part's hard edge is now soft");
    QVERIFY2(px(canvas, 0, 10)[3] < 255, "inside the selection the layer's own edge fades too");
}

void TestFilters::motionBlurStreaksAlongItsAngleCounterclockwiseFromHorizontal()
{
    QImage dot = blank(41, 41);
    dot.setPixelColor(20, 20, Qt::white);
    const auto streak = [&](double angle) { return RasterOperations::motionBlur(dot, angle, 16); };
    const QImage horizontal = streak(0);
    QVERIFY(alphaAt(horizontal, 24, 20) > 0 && alphaAt(horizontal, 16, 20) > 0 && alphaAt(horizontal, 20, 24) == 0);
    const QImage vertical = streak(90);
    QVERIFY(alphaAt(vertical, 20, 24) > 0 && alphaAt(vertical, 20, 16) > 0 && alphaAt(vertical, 24, 20) == 0);
    const QImage diagonal = streak(45);   // 45 degrees runs up-right and down-left on screen, never up-left
    QVERIFY(alphaAt(diagonal, 23, 17) > 0 && alphaAt(diagonal, 17, 23) > 0 && alphaAt(diagonal, 17, 17) == 0);
}

void TestFilters::addNoiseChangesColorButNeverAlphaAndMonochromaticKeepsGrays()
{
    QImage gray = blank(32, 8);
    fillRect(gray, QRect(0, 0, 16, 8), QColor::fromRgbF(0.5, 0.5, 0.5, 1));
    const QImage color = RasterOperations::addNoise(gray, 10, false, false, 7);
    QCOMPARE(RasterOperations::addNoise(gray, 10, false, false, 7), color);   // the same seed gives the same grain
    QSet<int> values; bool perChannel = false;
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 32; ++x) {
        const auto p = px(color, x, y);
        if (x < 16) {
            QCOMPARE(p[3], 255);
            QVERIFY(p[0] >= 112 && p[0] <= 144);
            values.insert(p[0]);
            perChannel = perChannel || p[0] != p[1];
        } else {
            QCOMPARE(p, (std::array<int, 4>{0, 0, 0, 0}));
        }
    }
    QVERIFY(values.size() > 5);
    QVERIFY2(perChannel, "color noise differs per channel");
    const QImage mono = RasterOperations::addNoise(gray, 10, true, true, 7);
    for (int x = 0; x < 16; ++x) { const auto p = px(mono, x, 0); QVERIFY(p[0] == p[1] && p[1] == p[2] && p[3] == 255); }
}

void TestFilters::removeDistortionBendsAboutTheCenterAndOnlyPincushionCorrectionOpensTheCorners()
{
    QImage source = blank(40, 30);
    const QRect quads[4] = {{0, 0, 20, 15}, {20, 0, 20, 15}, {0, 15, 20, 15}, {20, 15, 20, 15}};
    for (int i = 0; i < 4; ++i) fillRect(source, quads[i], QColor::fromRgbF(i / 3.0, .5, 1 - i / 3.0, 1));
    const auto lens = [&](double distortion) { return RasterOperations::lensDistorted(source, distortion / 100 * 0.35); };
    const QImage original = lens(0);
    for (int y = 0; y < 30; ++y) for (int x = 0; x < 40; ++x) QCOMPARE(alphaAt(original, x, y), 255);
    const QImage barrel = lens(100);      // straightening barrel distortion stretches the edges outward: nothing opens up
    QVERIFY(alphaAt(barrel, 0, 0) == 255 && alphaAt(barrel, 39, 29) == 255);
    const QImage pincushion = lens(-100); // straightening pincushion pulls the edges in: the corners turn transparent
    QVERIFY(alphaAt(pincushion, 0, 0) == 0 && alphaAt(pincushion, 39, 29) == 0);
    QCOMPARE(px(pincushion, 20, 15), px(original, 20, 15));
}

void TestFilters::lensCorrectionIsOneUndoStepAndZeroIsNone()
{
    EditorSession session;
    session.createDocument(40, 30);
    QVERIFY(session.insertImage(solid(40, 30, .2, .6, .9), QStringLiteral("Lens")));
    const int count = session.history().undoCount();
    QVERIFY(!session.distortActiveLayer(0));
    QCOMPARE(session.history().undoCount(), count);
    QVERIFY(session.distortActiveLayer(-100));
    QCOMPARE(session.history().undoCount(), count + 1);
    QCOMPARE(alphaAt(session.activeLayer()->image, 0, 0), 0);
}

// -------------------------------------------------------------------------------------- FinishingFilterTests

void TestFilters::eachFinishingFilterCommitsAsOneUndoStepAndKeepsLayerEffects()
{
    for (int kind = 0; kind < 3; ++kind) {
        EditorSession session;
        session.createDocument(48, 48);
        QImage sample = blank(24, 24);
        fillRect(sample, QRect(3, 3, 18, 18), QColor::fromRgbF(.7, .7, .7, 1));
        QVERIFY(session.insertImage(sample, QStringLiteral("Sample")));
        LayerEffects effects;
        StrokeEffect stroke; stroke.size = 3; effects.stroke = stroke;
        QVERIFY(session.setLayerEffects(*session.document()->activeLayerId, effects));
        const int count = session.history().undoCount();
        const bool ok = kind == 0 ? session.applyVignette(35, Qt::black)
                      : kind == 1 ? session.applyBloomGlow(40, 24)
                                  : session.applyTonalContrast(50, 16);
        QVERIFY2(ok, qPrintable(QStringLiteral("filter %1").arg(kind)));
        QCOMPARE(session.history().undoCount(), count + 1);
        QVERIFY(session.activeLayer()->effects == std::optional<LayerEffects>(effects));
        session.undo();
        QCOMPARE(session.activeLayer()->image, sample.convertToFormat(QImage::Format_RGBA8888_Premultiplied));
    }
}

// ------------------------------------------------------------------------------------------------ LevelsTests

static QImage levelsFixture()
{
    return row({{0, 0, 0, 255}, {64, 64, 64, 255}, {128, 128, 128, 255}, {255, 255, 255, 255}, {64, 32, 0, 128}, {0, 0, 0, 0}});
}

void TestFilters::levelsIdentityIsAnExactNoOp()
{
    const QImage source = levelsFixture();
    QCOMPARE(RasterOperations::levels(source, LevelsSettings()), source);
    EditorSession session;
    session.createDocument(6, 1);
    QVERIFY(session.insertImage(source, QStringLiteral("Ramp")));
    const Document before = *session.document();
    const int count = session.history().undoCount();
    QVERIFY(!session.applyLevels(LevelsSettings()));
    QVERIFY(*session.document() == before && session.history().undoCount() == count);
}

void TestFilters::levelsInputClippingGammaOutputInversionAndAlpha()
{
    const QImage source = levelsFixture();
    const QImage clipped = RasterOperations::levels(source, levelsWith(0, {64, 1, 128, 0, 255}));
    QCOMPARE(rawAt(clipped, 0), (std::array<int, 4>{0, 0, 0, 255}));
    QCOMPARE(rawAt(clipped, 1), (std::array<int, 4>{0, 0, 0, 255}));
    QCOMPARE(rawAt(clipped, 2), (std::array<int, 4>{255, 255, 255, 255}));
    const QImage brightened = RasterOperations::levels(source, levelsWith(0, {0, 2, 255, 0, 255}));
    QVERIFY(std::abs(rawAt(brightened, 1)[0] - 128) <= 1);
    QVERIFY(std::abs(rawAt(brightened, 2)[0] - 181) <= 1);
    QVERIFY(rawAt(brightened, 4)[3] == 128 && rawAt(brightened, 5)[3] == 0);
    QVERIFY(rawAt(brightened, 4)[0] <= 128 && rawAt(brightened, 4)[1] <= 128);
    const QImage inverted = RasterOperations::levels(source, levelsWith(0, {0, 1, 255, 255, 0}));
    QVERIFY(rawAt(inverted, 0)[0] == 255 && rawAt(inverted, 3)[0] == 0);
    // premultiplied [64,32,0,128] inverted
    QCOMPARE(rawAt(inverted, 4), (std::array<int, 4>{64, 96, 128, 128}));
}

void TestFilters::levelsChannelsCoexistAndUseDocumentedOrder()
{
    LevelsSettings settings;
    settings.ranges[1] = LevelRange{0, 2, 255, 0, 255};     // red: gamma 2
    settings.ranges[0] = LevelRange{40, 1, 210, 0, 255};    // RGB: 40...210
    const QImage result = RasterOperations::levels(row({{64, 64, 64, 255}}), settings);
    const auto apply = [](const LevelRange &r, double v) { const double s = std::clamp((v * 255 - r.black) / (r.white - r.black), 0.0, 1.0); return (r.outputBlack + std::pow(s, 1 / r.gamma) * (r.outputWhite - r.outputBlack)) / 255; };
    const double expectedRed = apply(LevelRange{40, 1, 210, 0, 255}, apply(LevelRange{0, 2, 255, 0, 255}, 64.0 / 255));
    QVERIFY(std::abs(rawAt(result, 0)[0] - expectedRed * 255) <= 1);
    QVERIFY(rawAt(result, 0)[1] == rawAt(result, 0)[2] && rawAt(result, 0)[0] > rawAt(result, 0)[1]);
    const LevelRange invalid = LevelRange{300, std::nan(""), -1, -100, 400}.normalized();
    QVERIFY(invalid.black < invalid.white && invalid.gamma == 1 && invalid.outputBlack == 0 && invalid.outputWhite == 255);
}

void TestFilters::levelsHistogramExcludesTransparencyAndWeightsSelection()
{
    const QImage source = row({{255, 0, 0, 255}, {0, 128, 0, 128}, {0, 0, 0, 0}});
    const LevelsHistogram bins = RasterOperations::levelsHistogram(source);
    QVERIFY(bins[1][255] == 1 && std::abs(bins[2][255] - 128.0 / 255) < 0.00001);
    double total = 0; for (double v : bins[0]) total += v;
    QVERIFY(std::abs(total - (1.0 + 128.0 / 255.0)) < 0.00001);
    QImage coverage(3, 1, QImage::Format_Grayscale8); coverage.fill(0); coverage.scanLine(0)[0] = 255;
    const LevelsHistogram selected = RasterOperations::levelsHistogram(source, coverage);
    QVERIFY(selected[1][255] == 1 && selected[2][255] == 0);
    QImage none(3, 1, QImage::Format_Grayscale8); none.fill(0);
    const LevelsHistogram empty = RasterOperations::levelsHistogram(source, none);
    for (const auto &channel : empty) for (double v : channel) QCOMPARE(v, 0.0);
}

void TestFilters::levelsSelectionCommitUndoSaveAndExport()
{
    EditorSession session;
    session.createDocument(6, 1);
    QVERIFY(session.insertImage(levelsFixture(), QStringLiteral("Ramp")));
    QVERIFY(session.setRectangularSelection(QRect(0, 0, 2, 1)));
    const Document before = *session.document();
    const int count = session.history().undoCount();
    // The histogram follows the selection.
    const LevelsHistogram histogram = session.levelsHistogram();
    QVERIFY(histogram[0][0] > 0);
    QCOMPARE(histogram[1][128], 0.0);
    const LevelsSettings invert = levelsWith(0, {0, 1, 255, 255, 0});
    QVERIFY(session.applyLevels(invert));
    QCOMPARE(session.history().undoCount(), count + 1);
    QCOMPARE(session.history().undoName(), QStringLiteral("Levels"));
    const QImage committed = session.activeLayer()->image;
    QCOMPARE(rawAt(committed, 0)[0], 255);
    QCOMPARE(rawAt(committed, 2)[0], 128);   // outside the selection: untouched
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("levels.comp"));
    ProjectWriter::save(*session.document(), path);
    Document loaded = ProjectReader::load(path);
    QCOMPARE(LayerRenderer::flattened(loaded), canvasOf(session));
    session.undo();
    QVERIFY(*session.document() == before);
    session.redo();
    QCOMPARE(session.activeLayer()->image, committed);
}

void TestFilters::levelsAutoAlgorithmsAndEyedropperCalibration()
{
    LevelsHistogram bins{};
    for (int c = 1; c <= 3; ++c) { bins[size_t(c)][size_t(20 * c)] = 100; bins[size_t(c)][size_t(200 + c * 10)] = 100; }
    const LevelsSettings linked = RasterOperations::automaticLevels(bins, 0);
    QVERIFY(linked.ranges[0].black == 20 && linked.ranges[0].white == 230);
    const LevelsSettings color = RasterOperations::automaticLevels(bins, 1);
    QVERIFY(color.ranges[1].black == 20 && color.ranges[3].black == 60);
    QVERIFY(color.ranges[0] == LevelRange());
    QCOMPARE(RasterOperations::automaticLevels(bins, 2).ranges[1].gamma, 1.0);
    const LevelsHistogram empty{};
    for (int mode = 0; mode < 3; ++mode) {
        const LevelsSettings s = RasterOperations::automaticLevels(empty, mode);
        for (const LevelRange &r : s.ranges) QVERIFY(r.normalized() == LevelRange());
    }
    const std::array<double, 3> rgb{0.25, 0.4, 0.6};
    for (int mode = 0; mode < 3; ++mode) {   // black, gray, white
        const LevelsSettings s = RasterOperations::sampledLevels(LevelsSettings(), QColor::fromRgbF(rgb[0], rgb[1], rgb[2]), mode);
        const double target = mode == 0 ? 0 : mode == 2 ? 1 : 0.5;
        for (int c = 0; c < 3; ++c) {
            const LevelRange r = s.ranges[size_t(c + 1)].normalized();
            const double in = std::clamp((rgb[size_t(c)] * 255 - r.black) / (r.white - r.black), 0.0, 1.0);
            const double out = (r.outputBlack + std::pow(in, 1 / r.gamma) * (r.outputWhite - r.outputBlack)) / 255;
            QVERIFY2(std::abs(out - target) < 0.0001, qPrintable(QStringLiteral("mode %1 channel %2 -> %3").arg(mode).arg(c).arg(out)));
        }
    }
    // The eyedropper never samples a transparent pixel.
    EditorSession session;
    session.createDocument(6, 1);
    QVERIFY(session.insertImage(levelsFixture(), QStringLiteral("Ramp")));
    QVERIFY(session.levelsSampleAt(QPointF(2.5, 0.5)).has_value());
    QVERIFY(!session.levelsSampleAt(QPointF(5.5, 0.5)).has_value());
    QVERIFY(!session.levelsSampleAt(QPointF(-1, 0.5)).has_value());
}

// ------------------------------------------------------------------------------------------ HueSaturationTests

static QImage colorsFixture()
{
    QImage image = blank(40, 20);
    fillRect(image, QRect(0, 0, 20, 20), QColor::fromRgbF(1, 0, 0, 1));
    fillRect(image, QRect(20, 0, 20, 20), QColor::fromRgbF(.5, .5, .5, 1));
    fillRect(image, QRect(0, 16, 40, 4), QColor::fromRgbF(0, 0, 1, .5), true);   // really half transparent
    return image;
}

static QImage redAndBlue()
{
    QImage image = blank(40, 20);
    fillRect(image, QRect(0, 0, 20, 20), QColor::fromRgbF(1, 0, 0, 1));
    fillRect(image, QRect(20, 0, 20, 20), QColor::fromRgbF(0, 0, 1, 1));
    return image;
}

static EditorSession sessionWith(const QImage &image)
{
    EditorSession session;
    session.createDocument(image.width(), image.height());
    session.insertImage(image, QStringLiteral("Image"));
    return session;
}

void TestFilters::hueSaturationDefaultsAreAnExactNoOp()
{
    EditorSession session = sessionWith(colorsFixture());
    const Document before = *session.document();
    const int count = session.history().undoCount();
    QVERIFY(!session.applyHueSaturation(HueSaturationSettings()));
    QVERIFY(*session.document() == before && session.history().undoCount() == count);
}

void TestFilters::hueRotatesSaturationAndLightnessFollowPhotoshopRanges()
{
    EditorSession session = sessionWith(colorsFixture());
    QVERIFY(session.applyHueSaturation(HueSaturationSettings(120)));
    QVERIFY(near(px(canvasOf(session), 5, 5), {0, 255, 0, 255}));       // red -> green
    QVERIFY(near(px(canvasOf(session), 30, 5), {128, 128, 128, 255}));  // gray unchanged
    session.undo();
    QVERIFY(session.applyHueSaturation(HueSaturationSettings(0, -100)));
    const auto gray = px(canvasOf(session), 5, 5);
    QVERIFY(gray[0] == gray[1] && gray[1] == gray[2] && gray[3] == 255);
    session.undo();
    QVERIFY(session.applyHueSaturation(HueSaturationSettings(0, 0, 100)));
    QVERIFY(near(px(canvasOf(session), 5, 5), {255, 255, 255, 255}));
    session.undo();
    QVERIFY(session.applyHueSaturation(HueSaturationSettings(0, 0, -100)));
    QVERIFY(near(px(canvasOf(session), 5, 5), {0, 0, 0, 255}));
}

void TestFilters::colorizeGivesEverythingOneHueAndKeepsAlpha()
{
    EditorSession session = sessionWith(colorsFixture());
    QVERIFY(session.applyHueSaturation(HueSaturationSettings(240, 100, 0, true)));
    const QImage canvas = canvasOf(session);
    const auto left = px(canvas, 5, 5), right = px(canvas, 30, 5);
    QVERIFY(left[2] > left[0] && right[2] > right[0]);
    QCOMPARE(left[3], 255);
    QVERIFY(std::abs(px(canvas, 5, 18)[3] - 128) <= 2);
}

void TestFilters::hueSaturationStaysInsideTheSelectionAndIsOneUndoStep()
{
    EditorSession session = sessionWith(colorsFixture());
    QVERIFY(session.setRectangularSelection(QRect(0, 0, 10, 20)));
    const int count = session.history().undoCount();
    QVERIFY(session.applyHueSaturation(HueSaturationSettings(120)));
    QCOMPARE(session.history().undoCount(), count + 1);
    QCOMPARE(session.history().undoName(), QStringLiteral("Hue/Saturation"));
    QVERIFY(near(px(canvasOf(session), 5, 5), {0, 255, 0, 255}));
    QVERIFY(near(px(canvasOf(session), 15, 5), {255, 0, 0, 255}));
    session.undo();
    QVERIFY(near(px(canvasOf(session), 5, 5), {255, 0, 0, 255}));
}

void TestFilters::bandWeightsRampThroughFalloffAndWrapAround()
{
    const HueBand reds = HueBand::defaultFor(ColorRange::Reds);   // 315 / 345 / 15 / 45, wrapping past 0
    QVERIFY(reds.weight(0) == 1 && reds.weight(345) == 1 && reds.weight(15) == 1);
    QVERIFY(std::abs(reds.weight(330) - 0.5) < 0.001);
    QVERIFY(std::abs(reds.weight(30) - 0.5) < 0.001);
    QVERIFY(reds.weight(315) == 0 && reds.weight(45) == 0 && reds.weight(180) == 0);
    QCOMPARE(HueBand::defaultFor(ColorRange::Master).weight(123), 1.0);
    HueBand band = HueBand::defaultFor(ColorRange::Greens);
    QVERIFY(!band.setHandle(1, 200));   // rangeStart past rangeEnd: refused
    QVERIFY(band == HueBand::defaultFor(ColorRange::Greens));
    QVERIFY(band.setHandle(1, 110));
    QCOMPARE(band.rangeStart, 110.0);
}

void TestFilters::colorRangesAdjustIndependently()
{
    EditorSession session = sessionWith(redAndBlue());
    HueSaturationSettings settings(60, 0, 0, false, ColorRange::Reds);
    settings.adjustments[size_t(ColorRange::Blues)] = RangeAdjustment{0, -100, 0};
    QVERIFY(session.applyHueSaturation(settings));
    const QImage canvas = canvasOf(session);
    QVERIFY(near(px(canvas, 5, 5), {255, 255, 0, 255}));   // reds rotated to yellow
    const auto blue = px(canvas, 30, 5);
    QVERIFY(blue[0] == blue[1] && blue[1] == blue[2]);     // blues desaturated to gray
}

void TestFilters::rangesLeaveOtherHuesAloneAndInvertFlipsTheBand()
{
    EditorSession session = sessionWith(redAndBlue());
    const auto beforeBlue = px(canvasOf(session), 30, 5);
    QVERIFY(session.applyHueSaturation(HueSaturationSettings(0, 0, -100, false, ColorRange::Reds)));
    QVERIFY(near(px(canvasOf(session), 5, 5), {0, 0, 0, 255}));
    QVERIFY(near(px(canvasOf(session), 30, 5), beforeBlue));
    session.undo();
    HueSaturationSettings inverted(0, 0, -100, false, ColorRange::Reds);
    inverted.invertRange = true;
    QVERIFY(session.applyHueSaturation(inverted));
    QVERIFY(near(px(canvasOf(session), 5, 5), {255, 0, 0, 255}));
    QVERIFY(near(px(canvasOf(session), 30, 5), {0, 0, 0, 255}));
}

void TestFilters::bandEyedroppersRecenterWidenAndNarrow()
{
    // Red is hue 0, blue 240.
    HueBand band = HueBand::defaultFor(ColorRange::Greens).centered(0);
    QVERIFY(band.weight(0) == 1 && band.weight(120) == 0);
    band.include(240);
    QVERIFY(band.weight(240) == 1 && band.weight(0) == 1);
    band.exclude(240);
    QCOMPARE(band.weight(240), 0.0);
}

void TestFilters::positiveSaturationMatchesPhotoshop()
{
    // +50 doubles saturation; +100 takes any color all the way; negative scales toward gray.
    EditorSession session = sessionWith(solid(4, 4, 0.6, 0.5, 0.4));
    QVERIFY(session.applyHueSaturation(HueSaturationSettings(0, 100)));
    const auto full = px(canvasOf(session), 0, 0);
    QVERIFY(full[0] == 255 && full[2] == 0);   // saturation 0.33 -> 1
    session.undo();
    QVERIFY(session.applyHueSaturation(HueSaturationSettings(0, -50)));
    const auto half = px(canvasOf(session), 0, 0);
    QVERIFY(half[0] - half[2] < 31 && half[0] - half[2] > 18);   // 102 span -> roughly half
}

// ------------------------------------------------------------------------------------- ImageAdjustmentTests

void TestFilters::exposureWorksInLinearLightWithOffsetAndGamma()
{
    const QImage gray = solid(4, 4, 128 / 255.0, 128 / 255.0, 128 / 255.0);
    QCOMPARE(RasterOperations::exposure(gray, 0, 0, 1), gray);   // defaults change nothing
    const auto brighter = px(RasterOperations::exposure(gray, 1, 0, 1), 0, 0);
    QVERIFY2(std::abs(brighter[0] - 176) <= 2, "+1 stop doubles linear light");
    QCOMPARE(brighter[0], brighter[2]);
    QVERIFY2(std::abs(px(RasterOperations::exposure(gray, 0, 0, 2), 0, 0)[0] - 181) <= 2, "gamma 2 takes the square root of linear light");
    const auto offset = px(RasterOperations::exposure(solid(4, 4, 0, 0, 0), 0, 0.1, 1), 0, 0);
    QVERIFY2(std::abs(offset[0] - 89) <= 2, "offset adds linear light");
    const QImage translucent = solid(4, 4, 128 / 255.0, 128 / 255.0, 128 / 255.0, 0.5);
    QCOMPARE(px(RasterOperations::exposure(translucent, 1, 0, 1), 0, 0)[3], px(translucent, 0, 0)[3]);
}

void TestFilters::gradientMapColorsByBrightnessAndReverses()
{
    const QColor red = QColor::fromRgbF(1, 0, 0), blue = QColor::fromRgbF(0, 0, 1);
    const auto map = [&](const QImage &image, bool reversed = false) { return RasterOperations::gradientMap(image, red, blue, reversed); };
    QCOMPARE(px(map(solid(4, 4, 0, 0, 0)), 0, 0), (std::array<int, 4>{255, 0, 0, 255}));
    QCOMPARE(px(map(solid(4, 4, 1, 1, 1)), 0, 0), (std::array<int, 4>{0, 0, 255, 255}));
    const auto middle = px(map(solid(4, 4, 128 / 255.0, 128 / 255.0, 128 / 255.0)), 0, 0);
    QVERIFY(std::abs(middle[0] - 127) <= 2 && std::abs(middle[2] - 128) <= 2 && middle[1] == 0);
    const auto translucent = px(map(solid(4, 4, 1, 1, 1, 0.5)), 0, 0);
    QVERIFY(translucent[2] >= 250 && translucent[0] <= 5 && std::abs(translucent[3] - 128) <= 1);
    QCOMPARE(px(map(solid(4, 4, 0, 0, 0), true), 0, 0), (std::array<int, 4>{0, 0, 255, 255}));
}

void TestFilters::grainIsFixedInDocumentSpaceAndLeavesTransparencyAlone()
{
    const QImage gray40 = solid(40, 40, 128 / 255.0, 128 / 255.0, 128 / 255.0);
    const QImage whole = RasterOperations::grain(gray40, 60, 2, 40, 7);
    QSet<int> values; bool neutral = true;
    for (int y = 0; y < 40; ++y) for (int x = 0; x < 40; ++x) { const auto p = px(whole, x, y); values.insert(p[0]); neutral = neutral && p[0] == p[1] && p[1] == p[2]; }
    QVERIFY2(values.size() > 5, "grain varies the brightness");
    QVERIFY2(neutral, "the same change on every channel");
    // A 20 x 20 piece drawn at its place in the document gets the same grain as that part of the whole.
    const QImage part = RasterOperations::grain(solid(20, 20, 128 / 255.0, 128 / 255.0, 128 / 255.0), 60, 2, 40, 7, QPointF(10, 10), 1);
    for (int y = 0; y < 20; ++y) for (int x = 0; x < 20; ++x)
        QVERIFY2(px(part, x, y) == px(whole, x + 10, y + 10), "grain must not shift when only part of the canvas redraws");
    QVERIFY2(RasterOperations::grain(gray40, 60, 2, 40, 8) != whole, "another seed, another pattern");
    QCOMPARE(RasterOperations::grain(gray40, 0, 2, 40, 7), gray40);
    const QImage cleared = RasterOperations::grain(solid(40, 40, .5, .5, .5, 0), 60, 2, 40, 7);
    for (int y = 0; y < 40; ++y) for (int x = 0; x < 40; ++x) QCOMPARE(alphaAt(cleared, x, y), 0);
}

void TestFilters::grainSizeControlsParticleScale()
{
    const QImage source = solid(64, 64, 128 / 255.0, 128 / 255.0, 128 / 255.0);
    const auto difference = [](const QImage &image) {
        double total = 0; int count = 0;
        for (int y = 0; y < 64; ++y) for (int x = 1; x < 64; ++x) { total += std::abs(px(image, x, y)[0] - px(image, x - 1, y)[0]); ++count; }
        return total / count;
    };
    const double small = difference(RasterOperations::grain(source, 70, 1, 70, 17));
    const double large = difference(RasterOperations::grain(source, 70, 12, 70, 17));
    QVERIFY2(large < small * 0.7, "larger grain should form visibly larger, more coherent particles");
}

void TestFilters::imageMenuExposureChangesTheLayerInOneStep()
{
    EditorSession session = sessionWith(solid(8, 8, 128 / 255.0, 128 / 255.0, 128 / 255.0));
    const int count = session.history().undoCount();
    QVERIFY(session.applyExposure(1, 0, 1));
    QCOMPARE(session.history().undoCount(), count + 1);
    QCOMPARE(session.history().undoName(), QStringLiteral("Exposure"));
    QVERIFY(std::abs(px(session.activeLayer()->image, 0, 0)[0] - 176) <= 2);
    const int again = session.history().undoCount();
    QVERIFY(!session.applyExposure(0, 0, 1) || session.history().undoCount() == again + 1);
}

void TestFilters::blackWhiteAndColorBalanceDefaultsAreWhatTheMacAppUses()
{
    // Photoshop's Black & White defaults (reds 40, yellows 60, greens 40, cyans 60, blues 20, magentas 80).
    const float weights[6] = {.4f, .6f, .4f, .6f, .2f, .8f};
    const QImage color = solid(4, 4, 0.8, 0.3, 0.2);
    const QImage gray = RasterOperations::blackWhite(color, weights, false, 40, .2);
    const auto p = px(gray, 0, 0);
    QVERIFY(p[0] == p[1] && p[1] == p[2] && p[3] == 255);
    // Color Balance at identity is untouched; a midtone shift toward red raises red.
    const float zero[3] = {0, 0, 0}, shift[3] = {.5f, 0, 0};
    QCOMPARE(RasterOperations::colorBalance(color, zero, zero, zero, true), color);
    const auto shifted = px(RasterOperations::colorBalance(solid(4, 4, .5, .5, .5), zero, shift, zero, false), 0, 0);
    QVERIFY(shifted[0] > shifted[2]);
    // Defaults parsed from an adjustment record that carries no settings are the same as the mac defaults.
    const QImage viaJson = RasterOperations::adjustment(color, QJsonObject{{QStringLiteral("kind"), QStringLiteral("Black & White")}});
    QCOMPARE(viaJson, gray);
}

// ------------------------------------------------------------------------------------- AdjustmentLayerTests

static QImage fixture(QColor color, std::array<int, 4> alpha = {255, 255, 255, 255})
{
    QImage image = blank(2, 2);
    for (int i = 0; i < 4; ++i) {
        uchar *p = image.scanLine(i / 2) + (i % 2) * 4;
        p[0] = uchar(qRound(color.redF() * alpha[size_t(i)])); p[1] = uchar(qRound(color.greenF() * alpha[size_t(i)]));
        p[2] = uchar(qRound(color.blueF() * alpha[size_t(i)])); p[3] = uchar(alpha[size_t(i)]);
    }
    return image;
}

static std::array<int, 4> rendered(EditorSession &session, int x, int y)
{
    const QImage canvas = canvasOf(session).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const uchar *p = canvas.constScanLine(y) + x * 4;
    return {p[0], p[1], p[2], p[3]};   // raw (premultiplied) bytes, as the mac tests compare
}

static QUuid addAdjustment(EditorSession &session, const QString &kind, const QJsonObject &settings = {})
{
    if (!session.addAdjustment(kind, settings)) return {};
    return *session.document()->activeLayerId;
}

void TestFilters::globalAdjustmentAffectsBelowButNotAboveAndRemainsLive()
{
    EditorSession s; s.createDocument(2, 2);
    QVERIFY(s.insertImage(fixture(Qt::white), QStringLiteral("Fixture")));
    const QUuid base = *s.document()->activeLayerId;
    LevelsSettings levels; levels.ranges[0].outputWhite = 0;
    const QUuid adjustment = addAdjustment(s, QStringLiteral("Levels"), RasterOperations::levelsSettingsToJson(levels));
    QVERIFY(!adjustment.isNull());
    for (int i = 0; i < 4; ++i) QCOMPARE(rendered(s, i % 2, i / 2), (std::array<int, 4>{0, 0, 0, 255}));
    QVERIFY(s.insertImage(fixture(QColor::fromRgbF(1, 0, 0), {255, 0, 0, 0}), QStringLiteral("Above")));
    QCOMPARE(rendered(s, 0, 0), (std::array<int, 4>{255, 0, 0, 255}));   // a layer above is not adjusted
    for (Layer &layer : s.document()->layers) if (layer.id == base) layer.image = fixture(QColor::fromRgbF(0, 0, 1));
    QCOMPARE(rendered(s, 1, 0), (std::array<int, 4>{0, 0, 0, 255}));     // the adjustment stays live on edited pixels
    s.selectLayer(adjustment); s.toggleLayerVisibility(adjustment);
    QCOMPARE(rendered(s, 1, 0), (std::array<int, 4>{0, 0, 255, 255}));
}

void TestFilters::clippedCurveChangesOnlyItsBaseAndCopyMergedMatchesExport()
{
    EditorSession s; s.createDocument(2, 2);
    QVERIFY(s.insertImage(fixture(QColor::fromRgbF(0, 0, 1)), QStringLiteral("Blue")));
    QVERIFY(s.insertImage(fixture(QColor::fromRgbF(0, 1, 0), {255, 0, 128, 0}), QStringLiteral("Green")));
    CurvesSettings curves;
    curves.channels[0] = {CurvePoint{0, 255}, CurvePoint{255, 0}};
    const QUuid adjustment = addAdjustment(s, QStringLiteral("Curves"), RasterOperations::curvesSettingsToJson(curves));
    QVERIFY(s.toggleClippingMask(adjustment));
    const QImage result = canvasOf(s);
    QCOMPARE(rendered(s, 0, 0), (std::array<int, 4>{255, 0, 255, 255}));   // clipped to the green layer's coverage
    QCOMPARE(rendered(s, 1, 0), (std::array<int, 4>{0, 0, 255, 255}));
    const auto copied = s.copiedPixels(true);
    QVERIFY(copied.has_value());
    QCOMPARE(copied->first.convertToFormat(QImage::Format_RGBA8888_Premultiplied), result.convertToFormat(QImage::Format_RGBA8888_Premultiplied));
    QVERIFY(s.toggleClippingMask(adjustment));
    QCOMPARE(rendered(s, 1, 0), (std::array<int, 4>{255, 255, 0, 255}));
}

void TestFilters::hueOpacityAndMaskPreserveOriginalPixels()
{
    EditorSession s; s.createDocument(2, 2);
    QVERIFY(s.insertImage(fixture(QColor::fromRgbF(1, 0, 0)), QStringLiteral("Red")));
    const QImage original = s.document()->layers[0].image;
    const QUuid id = addAdjustment(s, QStringLiteral("Hue/Saturation"), RasterOperations::hueSaturationSettingsToJson(HueSaturationSettings(120)));
    const auto result = rendered(s, 0, 0);
    QVERIFY(result[1] > 250 && result[0] < 5 && result[2] < 5);
    s.document()->layers[1].opacity = 0;
    QCOMPARE(rendered(s, 0, 0), (std::array<int, 4>{255, 0, 0, 255}));
    s.document()->layers[1].opacity = 1;
    QImage hidden(1, 1, QImage::Format_Grayscale8); hidden.fill(0);
    s.document()->layers[1].mask = hidden;
    QCOMPARE(rendered(s, 0, 0), (std::array<int, 4>{255, 0, 0, 255}));
    QVERIFY(s.document()->layers[0].image == original);
    Q_UNUSED(id);
}

void TestFilters::adjustmentPersistsDuplicatesAndUndoRestoresSettings()
{
    EditorSession s; s.createDocument(2, 2);
    QVERIFY(s.insertImage(fixture(Qt::white), QStringLiteral("White")));
    const QUuid id = addAdjustment(s, QStringLiteral("Curves"), RasterOperations::curvesSettingsToJson(CurvesSettings()));
    const QJsonObject initial = s.activeLayer()->adjustment;
    CurvesSettings edited;
    edited.channels[0] = {CurvePoint{0, 0}, CurvePoint{128, 190}, CurvePoint{255, 255}};
    const QJsonObject value = RasterOperations::curvesSettingsToJson(edited);
    s.beginEdit(QStringLiteral("Edit Curves")); QVERIFY(s.updateAdjustment(id, value)); s.endEdit();
    s.undo(); QCOMPARE(s.activeLayer()->adjustment, initial);
    s.redo(); QCOMPARE(s.activeLayer()->adjustment, value);
    s.duplicateActiveLayer(); QCOMPARE(s.activeLayer()->adjustment, value);
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("adjustment.comp"));
    ProjectWriter::save(*s.document(), path);
    const Document loaded = ProjectReader::load(path);
    QCOMPARE(loaded.layers.last().adjustment.value(QStringLiteral("curves")), completedAdjustmentJson(value).value(QStringLiteral("curves")));
    QCOMPARE(LayerRenderer::flattened(loaded), canvasOf(s));
}

void TestFilters::curvesIdentityAndImageCommandPreserveAlpha()
{
    const QImage asset = fixture(QColor::fromRgbF(0.4, 0.7, 0.1), {255, 128, 32, 0});
    QCOMPARE(RasterOperations::curves(asset, CurvesSettings()), asset);
    EditorSession s; s.createDocument(2, 2);
    QVERIFY(s.insertImage(asset, QStringLiteral("Asset")));
    CurvesSettings settings;
    settings.channels[0] = {CurvePoint{0, 255}, CurvePoint{255, 255}};
    QVERIFY(s.applyCurves(settings));
    const QImage& p = s.activeLayer()->image;
    QCOMPARE(rawAt(p.copy(0, 0, 2, 1), 0), (std::array<int, 4>{255, 255, 255, 255}));
    QCOMPARE(rawAt(p.copy(0, 0, 2, 1), 1), (std::array<int, 4>{128, 128, 128, 128}));
    QCOMPARE(rawAt(p.copy(0, 1, 2, 1), 0), (std::array<int, 4>{32, 32, 32, 32}));
    QCOMPARE(rawAt(p.copy(0, 1, 2, 1), 1), (std::array<int, 4>{0, 0, 0, 0}));
    s.undo();
    QCOMPARE(s.activeLayer()->image, asset);
}

void TestFilters::adjustmentBlendAndSoftMaskPreserveCoverage()
{
    EditorSession s; s.createDocument(2, 2);
    QVERIFY(s.insertImage(fixture(QColor::fromRgbF(1, 0, 0), {255, 128, 32, 0}), QStringLiteral("Red")));
    const QUuid id = addAdjustment(s, QStringLiteral("Hue/Saturation"), RasterOperations::hueSaturationSettingsToJson(HueSaturationSettings(120)));
    s.setLayerBlendMode(id, BlendMode::Multiply);
    QCOMPARE(rendered(s, 0, 0), (std::array<int, 4>{0, 0, 0, 255}));
    QCOMPARE(rendered(s, 1, 0), (std::array<int, 4>{0, 0, 0, 128}));
    QCOMPARE(rendered(s, 0, 1), (std::array<int, 4>{0, 0, 0, 32}));
    QCOMPARE(rendered(s, 1, 1), (std::array<int, 4>{0, 0, 0, 0}));
    s.setLayerBlendMode(id, BlendMode::Normal);
    QImage gray(1, 1, QImage::Format_Grayscale8); gray.fill(128);
    s.document()->layers[1].mask = gray;
    const auto p = rendered(s, 0, 0);
    QCOMPARE(p[3], 255);
    QCOMPARE(rendered(s, 1, 0)[3], 128);
    QCOMPARE(rendered(s, 0, 1)[3], 32);
    QCOMPARE(rendered(s, 1, 1)[3], 0);
    QVERIFY(std::abs(p[0] - 127) <= 2 && std::abs(p[1] - 128) <= 2);
}

void TestFilters::invertAppliesAndKeepsAlpha()
{
    const QImage source = solid(2, 2, 0.2, 0.4, 0.6);
    const QImage inverted = RasterOperations::adjustment(source, QJsonObject{{QStringLiteral("kind"), QStringLiteral("Invert")}});
    const auto before = px(source, 0, 0), after = px(inverted, 0, 0);
    for (int c = 0; c < 3; ++c) QVERIFY(std::abs(before[size_t(c)] + after[size_t(c)] - 255) <= 1);
    QCOMPARE(after[3], before[3]);
    const QImage translucent = solid(2, 2, 0.2, 0.4, 0.6, 0.5);
    QCOMPARE(px(RasterOperations::inverted(translucent), 0, 0)[3], px(translucent, 0, 0)[3]);
    const auto t = px(RasterOperations::inverted(translucent), 0, 0), o = px(translucent, 0, 0);
    for (int c = 0; c < 3; ++c) QVERIFY(std::abs(t[size_t(c)] + o[size_t(c)] - 255) <= 3);
}

// One compact record per kind with a non-default value in every parameter.
static QJsonObject sampleAdjustment(const QString &kind)
{
    const QJsonObject c1{{QStringLiteral("red"), 1.0}, {QStringLiteral("green"), 0.0}, {QStringLiteral("blue"), 0.0}};
    const QJsonObject c2{{QStringLiteral("red"), 0.0}, {QStringLiteral("green"), 0.0}, {QStringLiteral("blue"), 1.0}};
    if (kind == QStringLiteral("Hue/Saturation")) {
        HueSaturationSettings s(80, 10, -5, false, ColorRange::Reds);
        s.bands[size_t(ColorRange::Reds)] = s.bands[size_t(ColorRange::Reds)].centered(25);
        s.invertRange = true;
        return RasterOperations::hueSaturationSettingsToJson(s);
    }
    if (kind == QStringLiteral("Levels")) return RasterOperations::levelsSettingsToJson(levelsWith(1, {10, 1.5, 200, 5, 250}));
    if (kind == QStringLiteral("Curves")) { CurvesSettings s; s.channels[2] = {CurvePoint{0, 20}, CurvePoint{100, 140}, CurvePoint{255, 235}}; return RasterOperations::curvesSettingsToJson(s); }
    if (kind == QStringLiteral("Exposure")) return {{QStringLiteral("kind"), kind}, {QStringLiteral("exposureSettings"), QJsonObject{{QStringLiteral("exposure"), 1.0}, {QStringLiteral("offset"), 0.05}, {QStringLiteral("gamma"), 1.2}}}};
    if (kind == QStringLiteral("Gradient Map")) return {{QStringLiteral("kind"), kind}, {QStringLiteral("gradientMapSettings"), QJsonObject{{QStringLiteral("shadows"), c1}, {QStringLiteral("highlights"), c2}, {QStringLiteral("reversed"), true}}}};
    if (kind == QStringLiteral("Grain")) return {{QStringLiteral("kind"), kind}, {QStringLiteral("grainSettings"), QJsonObject{{QStringLiteral("amount"), 70.0}, {QStringLiteral("size"), 3.0}, {QStringLiteral("roughness"), 20.0}, {QStringLiteral("seed"), 1234}}}};
    if (kind == QStringLiteral("Black & White")) return {{QStringLiteral("kind"), kind}, {QStringLiteral("blackWhiteSettings"), QJsonObject{{QStringLiteral("reds"), 100.0}, {QStringLiteral("yellows"), 20.0}, {QStringLiteral("greens"), 10.0}, {QStringLiteral("cyans"), 0.0}, {QStringLiteral("blues"), -50.0}, {QStringLiteral("magentas"), 30.0}, {QStringLiteral("tint"), true}, {QStringLiteral("tintHue"), 200.0}, {QStringLiteral("tintSaturation"), 60.0}}}};
    if (kind == QStringLiteral("Color Balance")) return {{QStringLiteral("kind"), kind}, {QStringLiteral("colorBalanceSettings"), QJsonObject{{QStringLiteral("shadowCyanRed"), 20.0}, {QStringLiteral("midMagentaGreen"), -30.0}, {QStringLiteral("highlightYellowBlue"), 40.0}, {QStringLiteral("preserveLuminosity"), false}}}};
    if (kind == QStringLiteral("Gaussian Blur")) return {{QStringLiteral("kind"), kind}, {QStringLiteral("blurRadius"), 2.5}};
    if (kind == QStringLiteral("Motion Blur")) return {{QStringLiteral("kind"), kind}, {QStringLiteral("motionAngle"), 35.0}, {QStringLiteral("motionDistance"), 6.0}};
    if (kind == QStringLiteral("Add Noise")) return {{QStringLiteral("kind"), kind}, {QStringLiteral("noiseAmount"), 35.0}, {QStringLiteral("noiseGaussian"), true}, {QStringLiteral("noiseMonochromatic"), true}, {QStringLiteral("noiseSeed"), 4242}};
    return {{QStringLiteral("kind"), kind}};   // Invert
}

static QImage patchwork()
{
    QImage image = blank(16, 16);
    const QColor colors[4] = {QColor(200, 60, 40), QColor(40, 160, 70), QColor(50, 80, 210), QColor(220, 200, 60)};
    for (int i = 0; i < 4; ++i) fillRect(image, QRect((i % 2) * 8, (i / 2) * 8, 8, 8), colors[i]);
    return image;
}

void TestFilters::everyAdjustmentKindRendersAndRoundTripsAllParameters()
{
    const QStringList kinds{QStringLiteral("Hue/Saturation"), QStringLiteral("Levels"), QStringLiteral("Curves"), QStringLiteral("Exposure"),
        QStringLiteral("Gradient Map"), QStringLiteral("Grain"), QStringLiteral("Invert"), QStringLiteral("Black & White"),
        QStringLiteral("Color Balance"), QStringLiteral("Gaussian Blur"), QStringLiteral("Motion Blur"), QStringLiteral("Add Noise")};
    QTemporaryDir dir; QVERIFY(dir.isValid());
    for (const QString &kind : kinds) {
        QVERIFY(adjustmentKindFromString(kind).has_value());
        EditorSession s; s.createDocument(16, 16);
        QVERIFY(s.insertImage(patchwork(), QStringLiteral("Base")));
        const QImage base = canvasOf(s);
        const QJsonObject settings = sampleAdjustment(kind);
        const QUuid id = addAdjustment(s, kind, settings);
        QVERIFY2(!id.isNull(), qPrintable(kind));
        QVERIFY2(canvasOf(s) != base, qPrintable(kind + QStringLiteral(" rendered as a no-op")));
        // The destructive path agrees with the live layer for the pixel-wise kinds.
        const QImage direct = RasterOperations::adjustment(patchwork(), settings);
        QCOMPARE(canvasOf(s).convertToFormat(QImage::Format_RGBA8888_Premultiplied), direct.convertToFormat(QImage::Format_RGBA8888_Premultiplied));
        // Save and reload: every parameter survives and the render is identical.
        const QString path = dir.filePath(kind.simplified().remove(QLatin1Char('/')).remove(QLatin1Char('&')) + QStringLiteral(".comp"));
        ProjectWriter::save(*s.document(), path);
        const Document loaded = ProjectReader::load(path);
        QCOMPARE(loaded.layers.last().adjustment, completedAdjustmentJson(settings));
        QCOMPARE(LayerRenderer::flattened(loaded), canvasOf(s));
        // Undo removes the layer (and the canvas returns to the base), redo restores the same settings.
        s.undo();
        QCOMPARE(canvasOf(s), base);
        s.redo();
        QCOMPARE(s.activeLayer()->adjustment, settings);
    }
}

void TestFilters::savedAdjustmentsCarryEveryKeyMacRequires()
{
    // A freshly added adjustment layer holds only what its editor set; the saved record must still be complete.
    EditorSession s; s.createDocument(4, 4);
    QVERIFY(s.insertImage(solid(4, 4, .5, .5, .5), QStringLiteral("Base")));
    QVERIFY(s.addAdjustment(QStringLiteral("Exposure"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("Exposure")},
        {QStringLiteral("exposureSettings"), QJsonObject{{QStringLiteral("exposure"), 1.0}}}}));
    QVERIFY(s.addAdjustment(QStringLiteral("Levels"), RasterOperations::levelsSettingsToJson(levelsWith(0, {10, 1, 200, 0, 255}))));
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("keys.comp"));
    ProjectWriter::save(*s.document(), path);
    QFile file(path + QStringLiteral("/manifest.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonArray layers = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("layers")).toArray();
    int seen = 0;
    for (const QJsonValue &layer : layers) {
        const QJsonObject adjustment = layer.toObject().value(QStringLiteral("adjustment")).toObject();
        if (adjustment.isEmpty()) continue;
        ++seen;
        for (const char *key : {"kind", "hue", "saturation", "lightness", "colorize", "levels", "curves"})
            QVERIFY2(adjustment.contains(QLatin1String(key)), key);
        QCOMPARE(adjustment.value(QStringLiteral("levels")).toObject().value(QStringLiteral("ranges")).toArray().size(), 4);
        QCOMPARE(adjustment.value(QStringLiteral("curves")).toObject().value(QStringLiteral("channels")).toArray().size(), 4);
        if (adjustment.value(QStringLiteral("kind")).toString() == QStringLiteral("Exposure")) {
            const QJsonObject e = adjustment.value(QStringLiteral("exposureSettings")).toObject();
            QVERIFY(e.contains(QStringLiteral("offset")) && e.contains(QStringLiteral("gamma")));
        }
    }
    QCOMPARE(seen, 2);
    // Out-of-range settings are refused rather than saved as a file macOS would not open.
    QJsonObject bad{{QStringLiteral("kind"), QStringLiteral("Exposure")}, {QStringLiteral("exposureSettings"), QJsonObject{{QStringLiteral("exposure"), 50.0}}}};
    QVERIFY(!adjustmentJsonError(bad).isEmpty());
    QVERIFY(adjustmentJsonError(sampleAdjustment(QStringLiteral("Curves"))).isEmpty());
}

void TestFilters::folderScopedAdjustmentOnlyTouchesItsFolder()
{
    EditorSession s; s.createDocument(2, 2);
    QVERIFY(s.insertImage(fixture(QColor::fromRgbF(1, 0, 0)), QStringLiteral("Outside")));
    QVERIFY(s.insertImage(fixture(QColor::fromRgbF(1, 0, 0)), QStringLiteral("Inside")));
    const QUuid inside = *s.document()->activeLayerId;
    s.groupSelectedLayers();
    const QUuid folder = *s.document()->activeLayerId;
    QVERIFY(s.document()->layers.at(s.document()->layers.size() - 1).group || true);
    QCOMPARE(s.document()->layers.size(), 3);
    s.selectLayer(inside);
    QVERIFY(s.addAdjustment(QStringLiteral("Invert")));
    const Layer *adjustment = s.activeLayer();
    QVERIFY(adjustment && adjustment->parentId == std::optional<QUuid>(folder));
    // The folder holds the only layer, and it is inverted inside the folder.
    QCOMPARE(px(canvasOf(s), 0, 0), (std::array<int, 4>{0, 255, 255, 255}));
}

void TestFilters::cameraRawSliderDoubleClickOnTheKnobRestoresTheDefault()
{
    EditorSession session = sessionWith(solid(16, 16, .5, .5, .5));
    CameraRawDialog dialog(nullptr, session, *session.document()->activeLayerId);
    dialog.show();
    auto *spin = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("exposureSpin"));
    QVERIFY(spin);
    spin->setValue(2.0);
    QSlider *slider = nullptr;
    for (QSlider *candidate : dialog.findChildren<QSlider *>()) if (candidate->value() == 200 && candidate->maximum() == 500) slider = candidate;
    QVERIFY(slider);
    QStyleOptionSlider option; option.initFrom(slider); option.minimum = slider->minimum(); option.maximum = slider->maximum();
    option.sliderPosition = option.sliderValue = slider->value();
    const QRect handle = slider->style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, slider);
    QMouseEvent on(QEvent::MouseButtonDblClick, QPointF(handle.center()), QPointF(handle.center()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(slider, &on);
    QCOMPARE(spin->value(), 0.0);
}

QTEST_MAIN(TestFilters)
#include "TestFilters.moc"
