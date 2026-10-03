// Behaviour ported from macOS 1.3.x-1.4.5 (see MAC_BASELINE). Expected values come from the mac tests where one exists.
#include "core/CameraRaw.h"
#include "core/Document.h"
#include "io/PSDReader.h"
#include "core/EditorSession.h"
#include "rendering/LayerRenderer.h"
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
    // 5a8f6ce / 6dc2219 / 8ab32c9: Soft Light matches Photoshop; every mode applies to adjustments and clipping stacks
    void softLightMatchesPhotoshopFormula();
    void adjustmentInBlendModeIsNotNormal();
    void clippingStackBaseKeepsItsBlendMode();
    // 8c0417b: PSDAdjustmentTests
    void psdLevelsGammaIsInHundredths();
    void psdHueSaturationReadsMasterAndEachRange();
    void psdMaskPatchSitsWhereItIsOnTheCanvas();
    void paintRefusalsExplainThemselves();
    // c459f88 / d6e3e93 / 8b1369a: ResizeSnapTests and marquee/selection snapping
    void aSideHandleSnapsItsEdge();
    void aProportionalCornerSnapsItsNearerEdgeAndKeepsTheRatio();
    void aTurnedLayerDoesntSnap();
    void marqueePointsAndSelectionMovesSnap();
    // 1318f1e: GroupTests.ungroup*
    void ungroupRestoresChildrenAtTheFoldersSpotAndUndoes();
    void ungroupPreservesClippingBetweenTwoOfAFoldersOwnChildren();
    void ungroupingReleasesClippingThatNoLongerMakesSense();
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

static Layer fullLayer(const QString &name, QColor color, int size = 4)
{
    Layer layer;
    layer.id = QUuid::createUuid();
    layer.name = name;
    layer.transform.size = QSizeF(size, size);
    layer.image = QImage(size, size, QImage::Format_RGBA8888_Premultiplied);
    layer.image.fill(color);
    return layer;
}
static Document blankDocument(int size = 4)
{
    Document doc;
    doc.id = QUuid::createUuid();
    doc.canvasSize = QSize(size, size);
    doc.resolution = 72.0;
    return doc;
}

void TestSyncRendering::softLightMatchesPhotoshopFormula()
{
    Document doc = blankDocument();
    Layer top = fullLayer(QStringLiteral("Top"), QColor(204, 204, 204));
    top.blendMode = BlendMode::SoftLight;
    doc.layers = {fullLayer(QStringLiteral("Bottom"), QColor(153, 153, 153)), top};
    // cb 0.6, cs 0.8: D = sqrt(0.6); result = 0.6 + 0.6 * (D - 0.6) = 0.7048 -> 180
    QVERIFY(std::abs(LayerRenderer::flattened(doc).pixelColor(0, 0).red() - 180) <= 1);
    doc.layers[1].image.fill(QColor(51, 51, 51));   // cs 0.2: 0.6 - 0.6 * 0.6 * 0.4 = 0.456 -> 116
    QVERIFY(std::abs(LayerRenderer::flattened(doc).pixelColor(0, 0).red() - 116) <= 1);
    doc.layers[1].image.fill(QColor(204, 204, 204, 0));   // transparent source leaves the backdrop untouched
    QCOMPARE(LayerRenderer::flattened(doc).pixelColor(0, 0).red(), 153);
}

void TestSyncRendering::adjustmentInBlendModeIsNotNormal()
{
    Document doc = blankDocument();
    Layer adjustment;
    adjustment.id = QUuid::createUuid();
    adjustment.name = QStringLiteral("Invert");
    adjustment.transform.size = QSizeF(4, 4);
    adjustment.adjustment = QJsonObject{{QStringLiteral("kind"), QStringLiteral("Invert")}};
    adjustment.blendMode = BlendMode::LinearDodge;
    doc.formatVersion = 9;
    doc.layers = {fullLayer(QStringLiteral("Bottom"), QColor(102, 102, 102)), adjustment};
    // Inverted 102 -> 153; Linear Dodge adds it to the backdrop: 0.4 + 0.6 = 1. Normal would leave 153.
    QCOMPARE(LayerRenderer::flattened(doc).pixelColor(0, 0).red(), 255);
}

void TestSyncRendering::clippingStackBaseKeepsItsBlendMode()
{
    Document doc = blankDocument();
    Layer base = fullLayer(QStringLiteral("Base"), QColor(204, 204, 204));
    base.blendMode = BlendMode::LinearDodge;
    Layer child = fullLayer(QStringLiteral("Child"), QColor(0, 0, 0, 0));
    child.maskSourceId = base.id;
    doc.layers = {fullLayer(QStringLiteral("Bottom"), QColor(102, 102, 102)), base, child};
    QCOMPARE(LayerRenderer::flattened(doc).pixelColor(0, 0).red(), 255);   // not Normal's 204
}

static QByteArray shorts(const QVector<int> &values)
{
    QByteArray data;
    for (const int value : values) { const quint16 bits = quint16(qint16(value)); data.append(char(bits >> 8)); data.append(char(bits & 0xFF)); }
    return data;
}

void TestSyncRendering::psdLevelsGammaIsInHundredths()
{
    // RGB: input 2-254, gamma 1.00 (stored as 100); red, green and blue untouched; padded to Photoshop's 29 records.
    QVector<int> values{2, 2, 254, 0, 255, 100};
    for (int i = 0; i < 3; ++i) values += {0, 255, 0, 255, 100};
    QByteArray data = shorts(values);
    data.append(QByteArray(292 - data.size(), '\0'));
    const auto json = PSDReader::levelsAdjustment(data);
    QVERIFY(json.has_value());
    const QJsonArray ranges = json->value(QStringLiteral("levels")).toObject().value(QStringLiteral("ranges")).toArray();
    QCOMPARE(ranges.size(), 4);
    const QJsonObject rgb = ranges.at(0).toObject();
    QCOMPARE(rgb.value(QStringLiteral("black")).toDouble(), 2.0);
    QCOMPARE(rgb.value(QStringLiteral("white")).toDouble(), 254.0);
    for (const QJsonValue &range : ranges) QCOMPARE(range.toObject().value(QStringLiteral("gamma")).toDouble(), 1.0);
}

void TestSyncRendering::psdHueSaturationReadsMasterAndEachRange()
{
    // Version 2, Colorize off; Colorize values (ignored), Master +5/+4/0; Reds' band and -30 saturation, +10 light.
    QByteArray data = shorts({2}) + QByteArray(2, '\0') + shorts({23, 25, 0, 5, 4, 0, 315, 345, 15, 45, 0, -30, 10});
    data += shorts(QVector<int>(7 * 5, 0));
    auto hsv = [](const QJsonObject &json) { return json.value(QStringLiteral("hsvSettings")).toObject(); };
    auto adjustment = [&](const QJsonObject &json, const QString &name) {
        const QJsonArray a = hsv(json).value(QStringLiteral("adjustments")).toArray();
        for (int i = 0; i + 1 < a.size(); i += 2) if (a[i].toString() == name) return a[i + 1].toObject();
        return QJsonObject();
    };
    auto json = PSDReader::hueSaturationAdjustment(data);
    QVERIFY(json.has_value());
    QVERIFY(!hsv(*json).value(QStringLiteral("colorize")).toBool());
    QCOMPARE(adjustment(*json, QStringLiteral("Master")).value(QStringLiteral("hue")).toDouble(), 5.0);
    QCOMPARE(adjustment(*json, QStringLiteral("Master")).value(QStringLiteral("saturation")).toDouble(), 4.0);
    QCOMPARE(adjustment(*json, QStringLiteral("Reds")).value(QStringLiteral("saturation")).toDouble(), -30.0);
    QCOMPARE(adjustment(*json, QStringLiteral("Reds")).value(QStringLiteral("lightness")).toDouble(), 10.0);
    const QJsonArray bands = hsv(*json).value(QStringLiteral("bands")).toArray();
    const QJsonObject reds = bands.at(3).toObject();   // [name, object] pairs: Master, Reds -> index 3
    QCOMPARE(reds.value(QStringLiteral("falloffStart")).toDouble(), 315.0);
    QCOMPARE(reds.value(QStringLiteral("rangeStart")).toDouble(), 345.0);
    QCOMPARE(reds.value(QStringLiteral("rangeEnd")).toDouble(), 15.0);
    QCOMPARE(reds.value(QStringLiteral("falloffEnd")).toDouble(), 45.0);

    data[2] = 1;   // Colorize on: its own values apply.
    json = PSDReader::hueSaturationAdjustment(data);
    QVERIFY(json && hsv(*json).value(QStringLiteral("colorize")).toBool());
    QCOMPARE(adjustment(*json, QStringLiteral("Master")).value(QStringLiteral("hue")).toDouble(), 23.0);
    QCOMPARE(adjustment(*json, QStringLiteral("Master")).value(QStringLiteral("saturation")).toDouble(), 25.0);
}

void TestSyncRendering::psdMaskPatchSitsWhereItIsOnTheCanvas()
{
    // A 2x2 white patch at (3, 1) on a 6x4 canvas, black everywhere else, on a layer that covers the canvas: the patch
    // must land at (3, 1), not stretch over the whole layer.
    QImage patch(2, 2, QImage::Format_Grayscale8);
    patch.fill(255);
    const QImage mask = PSDReader::maskOnLayerGrid(patch, QRectF(3, 1, 2, 2), 0, QRectF(0, 0, 6, 4), QSize(6, 4));
    QCOMPARE(mask.size(), QSize(6, 4));
    QStringList rows;
    for (int y = 0; y < 4; ++y) { QString row; for (int x = 0; x < 6; ++x) row += qGray(mask.pixel(x, y)) > 127 ? '#' : '.'; rows << row; }
    QCOMPARE(rows, (QStringList{QStringLiteral("......"), QStringLiteral("...##."), QStringLiteral("...##."), QStringLiteral("......")}));
    // A patch that already covers the layer's grid is returned as it is.
    QImage full(6, 4, QImage::Format_Grayscale8);
    full.fill(77);
    QCOMPARE(PSDReader::maskOnLayerGrid(full, QRectF(0, 0, 6, 4), 255, QRectF(0, 0, 6, 4), QSize(6, 4)), full);
    // A scaled-down layer: its grid is larger than its placement, so the patch is scaled into the grid.
    const QImage scaled = PSDReader::maskOnLayerGrid(patch, QRectF(2, 0, 2, 2), 0, QRectF(0, 0, 4, 2), QSize(8, 4));
    QCOMPARE(scaled.size(), QSize(8, 4));
    QCOMPARE(qGray(scaled.pixel(5, 1)), 255);
    QCOMPARE(qGray(scaled.pixel(1, 1)), 0);
}

void TestSyncRendering::paintRefusalsExplainThemselves()
{
    EditorSession session;
    session.createDocument(20, 10, true);
    QVERIFY(session.paintRefusal().isEmpty());
    QImage empty(20, 10, QImage::Format_Grayscale8);
    empty.fill(0);
    session.document()->selection = empty;          // an empty selection draws no ants and stops every brush
    QVERIFY(session.paintRefusal().contains(QStringLiteral("Deselect")));
    session.deselect();
    session.document()->selection.reset();
    session.activeLayer()->visible = false;
    QVERIFY(session.paintRefusal().contains(QStringLiteral("hidden")));
    session.activeLayer()->visible = true;
    QVERIFY(session.paintRefusal().isEmpty());
}

static QVector<QUuid> ids(const EditorSession &session)
{
    QVector<QUuid> out;
    for (const Layer &l : session.document()->layers) out << l.id;
    return out;
}
static const Layer &layerById(const EditorSession &session, const QUuid &id)
{
    for (const Layer &l : session.document()->layers) if (l.id == id) return l;
    static Layer none; return none;
}

void TestSyncRendering::ungroupRestoresChildrenAtTheFoldersSpotAndUndoes()
{
    EditorSession session;
    session.createDocument(100, 100);
    session.addBlankLayer();
    const QUuid below = *session.document()->activeLayerId;
    QVERIFY2(!session.canUngroupLayers(), "a plain layer has nothing to unwrap");
    session.addGroup();
    const QUuid group = *session.document()->activeLayerId;
    session.addBlankLayer();
    const QUuid childA = *session.document()->activeLayerId;
    session.addBlankLayer();
    const QUuid childB = *session.document()->activeLayerId;
    session.selectLayer(std::nullopt);
    session.addBlankLayer();
    const QUuid above = *session.document()->activeLayerId;
    QCOMPARE(layerById(session, childA).parentId, std::optional<QUuid>(group));
    QCOMPARE(layerById(session, childB).parentId, std::optional<QUuid>(group));

    session.selectLayer(group);
    QVERIFY(session.canUngroupLayers());
    const int undoCount = session.history().undoCount();
    session.ungroupLayers();
    const auto order = ids(session);
    QVERIFY2(!order.contains(group), "the folder itself goes");
    QVERIFY(!layerById(session, childA).parentId && !layerById(session, childB).parentId);
    QVERIFY(order.indexOf(below) < order.indexOf(childA));
    QVERIFY(order.indexOf(childA) < order.indexOf(childB));
    QVERIFY(order.indexOf(childB) < order.indexOf(above));
    QCOMPARE(session.selectedLayerIds(), (QSet<QUuid>{childA, childB}));
    QCOMPARE(session.history().undoCount(), undoCount + 1);
    QCOMPARE(session.history().undoName(), QStringLiteral("Ungroup Layers"));

    session.undo();
    QCOMPARE(layerById(session, childA).parentId, std::optional<QUuid>(group));
    QVERIFY(layerById(session, group).group);
    session.redo();
    QCOMPARE(session.document()->layers.size(), 4);
}

void TestSyncRendering::ungroupPreservesClippingBetweenTwoOfAFoldersOwnChildren()
{
    EditorSession session;
    session.createDocument(100, 100);
    session.addBlankLayer();
    const QUuid base = *session.document()->activeLayerId;
    session.addBlankLayer();
    const QUuid clipped = *session.document()->activeLayerId;
    session.document()->layers[session.document()->layers.size() - 1].maskSourceId = base;
    session.selectLayers({base, clipped}, base);
    session.groupSelectedLayers();
    session.ungroupLayers();
    QCOMPARE(layerById(session, clipped).maskSourceId, std::optional<QUuid>(base));
}

void TestSyncRendering::ungroupingReleasesClippingThatNoLongerMakesSense()
{
    EditorSession session;
    session.createDocument(100, 100);
    session.addBlankLayer();
    const QUuid outsideBase = *session.document()->activeLayerId;
    session.addBlankLayer();
    const QUuid between = *session.document()->activeLayerId;
    session.addBlankLayer();
    const QUuid childSource = *session.document()->activeLayerId;
    session.document()->layers[session.document()->layers.size() - 1].maskSourceId = outsideBase;
    session.selectLayer(childSource);
    session.groupSelectedLayers();
    const QUuid group = *session.document()->activeLayerId;
    QCOMPARE(layerById(session, childSource).parentId, std::optional<QUuid>(group));
    session.ungroupLayers();
    QCOMPARE(ids(session), (QVector<QUuid>{outsideBase, between, childSource}));
    QVERIFY(!layerById(session, childSource).maskSourceId);   // no longer next to its base, so the clip goes
}

// A 300x200 canvas: the layer being resized, 100x100 at (10,10), and another whose left edge is at x 150.
static QUuid resizeFixture(EditorSession &session)
{
    session.createDocument(300, 200);
    QImage image(20, 20, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::red);
    session.insertImage(image, QStringLiteral("Other"));
    session.activeLayer()->transform = LayerTransform{QPointF(150, 150), QSizeF(40, 40)};
    session.insertImage(image, QStringLiteral("Resized"));
    session.activeLayer()->transform = LayerTransform{QPointF(10, 10), QSizeF(100, 100)};
    return session.activeLayer()->id;
}
static LayerTransform snappedResize(EditorSession &session, const QUuid &layer, QPoint sign, QPointF from, QPointF to, bool lock)
{
    const LayerTransform start = session.activeLayer()->transform;
    const QPointF snapped = session.snappedResizePoint(to, start, from, sign, false, lock, {layer}, 5.0);
    LayerTransform result = resizedByHandle(start, sign, from, snapped, false, lock);
    result.origin = QPointF(qRound(result.origin.x()), qRound(result.origin.y()));
    result.size = QSizeF(qRound(result.size.width()), qRound(result.size.height()));
    return result;
}

void TestSyncRendering::aSideHandleSnapsItsEdge()
{
    EditorSession session;
    const QUuid layer = resizeFixture(session);
    // The right edge dragged to 147, three pixels short of the other layer's left edge.
    LayerTransform r = snappedResize(session, layer, QPoint(1, 0), QPointF(110, 60), QPointF(147, 60), false);
    QCOMPARE(r.origin.x() + r.size.width(), 150.0);
    QCOMPARE(r.size.height(), 100.0);   // only the dragged edge moves
    r = snappedResize(session, layer, QPoint(1, 0), QPointF(110, 60), QPointF(130, 60), false);
    QCOMPARE(r.origin.x() + r.size.width(), 130.0);   // out of reach, it doesn't
}

void TestSyncRendering::aProportionalCornerSnapsItsNearerEdgeAndKeepsTheRatio()
{
    EditorSession session;
    const QUuid layer = resizeFixture(session);
    // Bottom right dragged toward (146, 148): the bottom edge is nearer 150 than the right edge is.
    const LayerTransform r = snappedResize(session, layer, QPoint(1, 1), QPointF(110, 110), QPointF(146, 148), true);
    QCOMPARE(r.origin.y() + r.size.height(), 150.0);
    QVERIFY2(std::abs(r.size.width() - r.size.height()) <= 1, "still square");
}

void TestSyncRendering::aTurnedLayerDoesntSnap()
{
    EditorSession session;
    const QUuid layer = resizeFixture(session);
    session.activeLayer()->transform.rotation = 20;
    const LayerTransform start = session.activeLayer()->transform;
    const QPointF point(147, 60);
    QCOMPARE(session.snappedResizePoint(point, start, QPointF(110, 60), QPoint(1, 0), false, false, {layer}, 5.0), point);
}

void TestSyncRendering::marqueePointsAndSelectionMovesSnap()
{
    EditorSession session;
    resizeFixture(session);
    std::optional<double> gx, gy;
    // Near the other layer's left edge (150) and the canvas top (0): both axes snap on their own.
    QPointF p = session.snappedPoint(QPointF(147, 3), 5.0, &gx, &gy);
    QCOMPARE(p.x(), 150.0);
    QCOMPARE(p.y(), 0.0);
    QVERIFY(gx && gy);
    p = session.snappedPoint(QPointF(120, 80), 5.0, &gx, &gy);   // out of reach: untouched
    QCOMPARE(p, QPointF(120, 80));
    QVERIFY(!gx && !gy);
    // A 20x20 selection at (60, 60) dragged right by 67: its right edge (147) is 3 short of 150... its middle/edges snap.
    const QPointF moved = session.snappedSelectionOffset(QRectF(60, 60, 20, 20), QPointF(67, 0), 5.0);
    QCOMPARE((60 + moved.x() + 20), 150.0);
    // An axis locked by Shift doesn't snap.
    const QPointF locked = session.snappedSelectionOffset(QRectF(60, 60, 20, 20), QPointF(67, 0), 5.0, false, true);
    QCOMPARE(locked.x(), 67.0);
    session.setSnapEnabled(false);
    QCOMPARE(session.snappedPoint(QPointF(147, 3), 5.0), QPointF(147, 3));
}

QTEST_MAIN(TestSyncRendering)
#include "TestSyncRendering.moc"
