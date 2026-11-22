#include "core/EditorSession.h"
#include "io/AdjustmentJson.h"
#include "io/ImageExporter.h"
#include "io/PSDReader.h"
#include "io/PSDWriter.h"
#include "rendering/LayerEffectsRenderer.h"
#include "rendering/RgbSplit.h"
#include "io/ProjectReader.h"
#include "io/ProjectWriter.h"
#include "rendering/DeepImage.h"
#include "rendering/FilterLayers.h"
#include "rendering/LayerRenderer.h"
#include "rendering/LensEffects.h"
#include "rendering/RimLight.h"
#include "rendering/VolumetricLight.h"
#include "rendering/SubjectRemoval.h"
#include "ui/MainWindow.h"
#include "ui/ColorPickerDialog.h"
#include "core/ColorPalette.h"
#include "core/LayerDistances.h"
#include "ui/LayerListModel.h"
#include "ui/CanvasWidget.h"
#include "core/DocumentLimits.h"
#include <QtTest>
#include <QComboBox>
#include <QLineEdit>
#include <QToolButton>
#include <QListView>
#include <QCheckBox>
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMenuBar>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTabBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QLabel>
#include <QBuffer>
#include <QImageReader>
#include <QPainter>
#include <QTimer>

using namespace compositor;
// Compositor 1.5.9 – 1.7.0, each case naming the macOS commit (and test) it mirrors.
class TestUpstream170 : public QObject {
    Q_OBJECT

    static QImage solid(int w, int h, QColor color)
    {
        QImage image(w, h, QImage::Format_RGBA8888_Premultiplied);
        image.fill(color);
        return image;
    }
    // FilterLayerTests.scene: a gray picture with soft detail and one bright light.
    static QImage scene(int w = 80, int h = 60)
    {
        QImage image = solid(w, h, QColor::fromRgbF(.45f, .45f, .45f));
        QPainter p(&image);
        p.fillRect(QRect(10, 10, 20, 30), QColor::fromRgbF(.6f, .6f, .6f));
        p.fillRect(QRect(50, 25, 6, 6), Qt::white);
        return image;
    }
    static std::array<int, 4> pixel(const QImage &image, int x, int y)
    {
        const QColor c = image.convertToFormat(QImage::Format_RGBA8888).pixelColor(x, y);
        return {c.red(), c.green(), c.blue(), c.alpha()};
    }
    static QJsonObject manifestOf(const QString &url)
    {
        QFile file(url + "/manifest.json");
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
    }
    static void rewriteVersion(const QString &url, int version)
    {
        QJsonObject manifest = manifestOf(url);
        manifest.insert("version", version);
        QFile file(url + "/manifest.json");
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(manifest).toJson());
    }
    static QJsonObject filterLayer(const Layer &layer, const QString &key)
    {
        return layer.adjustment.value("filterLayerSettings").toObject().value(key).toObject();
    }
    // ReduceNoiseTests.noisy: mid gray, every pixel's brightness and color thrown a little off at random.
    static QImage noisy(bool edge = false, int size = 120)
    {
        QImage image(size, size, QImage::Format_RGBA8888_Premultiplied);
        QRandomGenerator random(7);
        for (int y = 0; y < size; ++y) {
            uchar *row = image.scanLine(y);
            for (int x = 0; x < size; ++x) {
                const int base = edge ? (x < size / 2 ? 70 : 190) : 128, shift = random.bounded(-30, 31);
                for (int c = 0; c < 3; ++c) row[x * 4 + c] = uchar(std::clamp(base + shift + random.bounded(-20, 21), 0, 255));
                row[x * 4 + 3] = 255;
            }
        }
        return image;
    }
    static double spread(const QImage &image)
    {
        const QImage i = image.convertToFormat(QImage::Format_RGBA8888);
        std::vector<double> values;
        for (int y = 0; y < i.height(); ++y)
            for (int x = 0; x < i.width(); ++x)
                for (int c = 0; c < 3; ++c) values.push_back(i.constScanLine(y)[x * 4 + c]);
        double mean = 0;
        for (double v : values) mean += v;
        mean /= values.size();
        double total = 0;
        for (double v : values) total += std::abs(v - mean);
        return total / values.size();
    }

private slots:
    void initTestCase() { GpuRenderer::setBackend(RenderBackend::Cpu); }

    // 7b5ba97 FilterLayerTests.vignetteLensEffectsAndTonalContrastLayersRenderEditAndSave: each kind adds as a layer
    // that changes the picture, keeps its settings through a save, and needs version 18 (17 refuses it).
    void vignetteLensEffectsAndTonalContrastLayersRenderEditAndSave()
    {
        for (const QString &kind : {QStringLiteral("Vignette"), QStringLiteral("Lens Effects"), QStringLiteral("Tonal Contrast")}) {
            EditorSession session;
            session.createDocument(80, 60);
            QVERIFY(session.insertImage(scene(), "Scene"));
            const QImage before = LayerRenderer::flattened(*session.document());
            QVERIFY2(session.addAdjustment(kind), qPrintable(kind));
            Layer &layer = *session.activeLayer();
            QJsonObject settings = layer.adjustment.value("filterLayerSettings").toObject();
            if (kind == "Vignette") { auto v = settings.value("vignette").toObject(); v.insert("amount", 80); settings.insert("vignette", v); }
            if (kind == "Tonal Contrast") { auto v = settings.value("tonal").toObject(); v.insert("amount", 100); settings.insert("tonal", v); }
            if (kind == "Lens Effects") { auto v = settings.value("lens").toObject(); v.insert("effect", "Mist"); v.insert("mistStrength", 90); settings.insert("lens", v); }
            QJsonObject adjustment = layer.adjustment; adjustment.insert("filterLayerSettings", settings);
            QVERIFY(session.updateAdjustment(layer.id, adjustment));
            const Layer saved = *session.activeLayer();
            const QImage after = LayerRenderer::flattened(*session.document());
            QVERIFY2(after != before, qPrintable(kind + " changes the image"));
            QVERIFY(ProjectWriter::computeTargetVersion(*session.document()) >= 18);
            QTemporaryDir dir;
            const QString url = dir.filePath("FilterLayers18.comp");
            ProjectWriter::save(*session.document(), url);
            const Document loaded = ProjectReader::load(url);
            const QJsonObject reopened = completedAdjustmentJson(loaded.layers.last().adjustment);
            QCOMPARE(reopened.value("filterLayerSettings"), completedAdjustmentJson(saved.adjustment).value("filterLayerSettings"));
            rewriteVersion(url, 17);
            QVERIFY_THROWS_EXCEPTION(ProjectError, ProjectReader::load(url));
            Document older = *session.document(); older.formatVersion = 17;
            QVERIFY_THROWS_EXCEPTION(std::exception, ProjectWriter::save(older, dir.filePath("Older.comp")));
        }
    }

    // 7b5ba97/b0f0b2b/c1c5356: each later kind's settings live in an object of their own, at their filter's defaults.
    void laterKindsStartAtTheirFiltersDefaults()
    {
        EditorSession session;
        session.createDocument(40, 30);
        const QMap<QString, QPair<QString, QJsonObject>> expected{
            {"Vignette", {"vignette", VignetteLayerSettings().json()}},
            {"Tonal Contrast", {"tonal", TonalLayerSettings().json()}},
            {"Sharpen", {"sharpen", QJsonObject{{"amount", 100}, {"radius", 1}}}},
            {"Reduce Noise", {"reduceNoise", QJsonObject{{"strength", 50}, {"details", 50}, {"color", 50}}}}};
        for (auto it = expected.begin(); it != expected.end(); ++it) {
            QVERIFY(session.addAdjustment(it.key()));
            QCOMPARE(filterLayer(*session.activeLayer(), it.value().first), it.value().second);
        }
        QVERIFY(session.addAdjustment("Lens Effects"));
        QCOMPARE(lensSettingsFromJson(filterLayer(*session.activeLayer(), "lens")), LensEffectSettings());
    }

    // 7b5ba97 ProjectStore.validate: the reader gates, and out-of-range settings refused as the mac's isValid does.
    void readerGatesTheLaterKinds()
    {
        QTemporaryDir dir;
        const auto layerWith = [&](const QJsonObject &adjustment, int version) {
            EditorSession session; session.createDocument(20, 20);
            session.addAdjustment("Invert");
            const QString url = dir.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".comp");
            ProjectWriter::save(*session.document(), url);
            QJsonObject manifest = manifestOf(url);
            QJsonArray layers = manifest.value("layers").toArray();
            QJsonObject record = layers.last().toObject();
            record.insert("adjustment", completedAdjustmentJson(adjustment));
            layers[layers.size() - 1] = record;
            manifest.insert("layers", layers);
            manifest.insert("version", version);
            QFile file(url + "/manifest.json"); file.open(QIODevice::WriteOnly | QIODevice::Truncate); file.write(QJsonDocument(manifest).toJson()); file.close();
            try { ProjectReader::load(url); return QString(); } catch (const ProjectError &e) { return e.message(); }
        };
        const auto adjustment = [](const QString &kind, const QString &key, const QJsonObject &object) {
            return QJsonObject{{"kind", kind}, {"filterLayerSettings", QJsonObject{{key, object}}}};
        };
        QVERIFY(!layerWith(adjustment("Vignette", "vignette", VignetteLayerSettings().json()), 17).isEmpty());
        QVERIFY(layerWith(adjustment("Vignette", "vignette", VignetteLayerSettings().json()), 18).isEmpty());
        QVERIFY(!layerWith(adjustment("Sharpen", "sharpen", SharpenSettings().json()), 18).isEmpty());
        QVERIFY(layerWith(adjustment("Sharpen", "sharpen", SharpenSettings().json()), 19).isEmpty());
        QVERIFY(!layerWith(adjustment("Reduce Noise", "reduceNoise", ReduceNoiseSettings().json()), 20).isEmpty());
        QVERIFY(layerWith(adjustment("Reduce Noise", "reduceNoise", ReduceNoiseSettings().json()), 21).isEmpty());
        // Prism's Fringe (20) is read at 18 as the mac reads it, ungated.
        QJsonObject lens = lensSettingsJson(LensEffectSettings());
        QVERIFY(layerWith(adjustment("Lens Effects", "lens", lens), 18).isEmpty());
        // Out of range: refused.
        QJsonObject wide = SharpenSettings().json(); wide.insert("amount", 301);
        QVERIFY(!layerWith(adjustment("Sharpen", "sharpen", wide), 22).isEmpty());
        lens.insert("starPoints", 5);
        QVERIFY(!layerWith(adjustment("Lens Effects", "lens", lens), 22).isEmpty());
        // A missing Vignette key: the mac's synthesized Codable needs every one; the writer fills them in.
        QJsonObject partial = VignetteLayerSettings().json(); partial.remove("feather");
        QCOMPARE(completedAdjustmentJson(adjustment("Vignette", "vignette", partial)).value("filterLayerSettings").toObject()
                     .value("vignette").toObject().value("feather").toDouble(), 60.0);
    }

    // 6d49a23 ProjectTests.everyReleasedFormatVersionStillOpens, with 1.7.0's range.
    void everyReleasedFormatVersionStillOpens()
    {
        QTemporaryDir dir; EditorSession session; session.createDocument(30, 20);
        QVERIFY(session.insertImage(solid(10, 10, Qt::red), "Plain"));
        const QString url = dir.filePath("Plain.comp");
        ProjectWriter::save(*session.document(), url);
        for (int version = 1; version <= 22; ++version) {
            rewriteVersion(url, version);
            try { ProjectReader::load(url); } catch (const ProjectError &e) { QFAIL(qPrintable(QStringLiteral("version %1: %2").arg(version).arg(e.message()))); }
        }
        rewriteVersion(url, 23);
        QVERIFY_THROWS_EXCEPTION(ProjectError, ProjectReader::load(url));
    }

    // 338a609 FilterLayerTests.lensEffectsLayerSizesByTheDocument: a light at the right edge of a 200-pixel piece of an
    // 800-pixel document; its one Prism copy, 10% of 800, lands 80 pixels left, not 20.
    void lensEffectsLayerSizesByTheDocument()
    {
        QImage piece = solid(200, 50, Qt::black);
        { QPainter p(&piece); p.fillRect(QRect(180, 20, 10, 10), Qt::white); }
        LensEffectSettings lens; lens.effect = LensEffect::Prism; lens.prismCopies = 1; lens.prismOffset = 10;
        const QJsonObject adjustment{{"kind", "Lens Effects"}, {"filterLayerSettings", QJsonObject{{"lens", lensSettingsJson(lens)}}}};
        const QImage result = FilterLayers::applyLayer(piece, adjustment, QRectF(0, 0, 800, 400));
        QVERIFY2(pixel(result, 105, 25)[1] > 40, "the copy lands 80 pixels left");
        QVERIFY2(pixel(result, 160, 25)[1] < 10, "not 20, as a 200-pixel picture would put it");
    }

    // b0f0b2b FilterLayerTests.sharpenBringsOutEdgesAndSaves: darker just before the edge, lighter just after, flat areas
    // as they were; a Sharpen layer needs version 19.
    void sharpenBringsOutEdgesAndSaves()
    {
        QImage edge = solid(40, 20, QColor::fromRgbF(.3f, .3f, .3f));
        { QPainter p(&edge); p.fillRect(QRect(20, 0, 20, 20), QColor::fromRgbF(.7f, .7f, .7f)); }
        const SharpenSettings settings{200, 2};
        const QImage result = FilterLayers::sharpen(edge, settings, 1, RenderBackend::Cpu);
        QVERIFY2(pixel(result, 18, 10)[0] < pixel(edge, 18, 10)[0] - 5, "darker just before the edge");
        QVERIFY2(pixel(result, 21, 10)[0] > pixel(edge, 21, 10)[0] + 5, "lighter just after it");
        QCOMPARE(pixel(result, 5, 10), pixel(edge, 5, 10));

        EditorSession session; session.createDocument(40, 20);
        QVERIFY(session.insertImage(edge, "Edge"));
        QVERIFY(session.addAdjustment("Sharpen", QJsonObject{{"filterLayerSettings", QJsonObject{{"sharpen", settings.json()}}}}));
        QCOMPARE(pixel(LayerRenderer::flattened(*session.document()), 21, 10), pixel(result, 21, 10));
        QCOMPARE(ProjectWriter::computeTargetVersion(*session.document()), 19);
        QTemporaryDir dir; const QString url = dir.filePath("Sharpen.comp");
        ProjectWriter::save(*session.document(), url);
        QCOMPARE(filterLayer(ProjectReader::load(url).layers.last(), "sharpen"), settings.json());
        rewriteVersion(url, 18);
        QVERIFY_THROWS_EXCEPTION(ProjectError, ProjectReader::load(url));
    }

    // b0f0b2b: Filter › Sharpen is one undo step, Amount 0 leaves none, and Last Filter repeats it.
    void sharpenFilterUndoAndLastFilter()
    {
        EditorSession session; session.createDocument(40, 20);
        QImage edge = solid(40, 20, QColor::fromRgbF(.3f, .3f, .3f));
        { QPainter p(&edge); p.fillRect(QRect(20, 0, 20, 20), QColor::fromRgbF(.7f, .7f, .7f)); }
        QVERIFY(session.insertImage(edge, "Edge"));
        QVERIFY(!session.applySharpen({0, 2}));
        QVERIFY(session.applySharpen({150, 1.5}));
        QCOMPARE(session.history().undoName(), QStringLiteral("Sharpen"));
        QCOMPARE(session.lastFilterName(), QStringLiteral("Sharpen"));
        const QImage once = session.activeLayer()->image;
        QVERIFY(session.applyLastFilter());
        QVERIFY(session.activeLayer()->image != once);
    }

    // c1c5356 ReduceNoiseTests: it smooths the noise, more strength smooths more, and the edge between dark and light
    // stays a step, not a ramp. 8926ea8: a Reduce Noise layer needs version 21.
    void reduceNoiseSmoothsNoiseAndKeepsEdges()
    {
        const double before = spread(noisy()), after = spread(FilterLayers::reduceNoise(noisy(), {}));
        QVERIFY2(after < before * .7, qPrintable(QStringLiteral("noise %1 after, against %2 before").arg(after).arg(before)));
        const double light = spread(FilterLayers::reduceNoise(noisy(), {20, 50, 50}));
        const double strong = spread(FilterLayers::reduceNoise(noisy(), {100, 50, 50}));
        QVERIFY(strong < light);
        const QImage kept = FilterLayers::reduceNoise(noisy(true), {100, 50, 50}).convertToFormat(QImage::Format_RGBA8888);
        const auto column = [&](int x) { double sum = 0; for (int y = 0; y < 120; ++y) sum += kept.constScanLine(y)[x * 4 + 1]; return sum / 120; };
        QVERIFY2(column(60) - column(59) > 90, qPrintable(QStringLiteral("%1 to %2").arg(column(59)).arg(column(60))));

        EditorSession session; session.createDocument(120, 120);
        QVERIFY(session.insertImage(noisy(), "Noisy"));
        QVERIFY(session.applyReduceNoise({}));
        QCOMPARE(session.history().undoName(), QStringLiteral("Reduce Noise"));
        QVERIFY(!session.applyReduceNoise({0, 50, 0}));
        QVERIFY(session.addAdjustment("Reduce Noise"));
        QCOMPARE(ProjectWriter::computeTargetVersion(*session.document()), 21);
    }

    // 6d49a23 LensEffectsTests.prismFringeSplitsTheCopysColorsOrKeepsThemCrisp.
    void prismFringeSplitsTheCopysColorsOrKeepsThemCrisp()
    {
        QImage night = solid(400, 240, QColor::fromRgbF(.05f, .06f, .1f));
        { QPainter p(&night); p.fillRect(QRect(190, 110, 20, 20), Qt::white); }
        const auto split = [&](double fringe) {
            LensEffectSettings s; s.effect = LensEffect::Prism; s.prismAngle = 180; s.prismOffset = 25; s.prismCopies = 1;
            s.prismAmount = 100; s.prismFringe = fringe;
            const QImage image = s.apply(night, RenderBackend::Cpu);
            int apart = 0;
            for (int x = 80; x <= 120; ++x) { const auto c = pixel(image, x, 120); if ((c[0] > 128) != (c[2] > 128)) ++apart; }
            return apart;
        };
        QCOMPARE(split(0), 0);
        QVERIFY(split(100) >= 4);
        LensEffectSettings wide; wide.prismFringe = 250;
        QCOMPARE(wide.normalized().prismFringe, 100.0);
    }

    // 7651033: Starburst draws every light out as far for all three colors before the spikes are spread, so a small
    // light's spike reaches as far in blue as a big one's does: the spike past the light stays near white, not red.
    void starburstSpreadsEachLengthOnceForAllColors()
    {
        QImage night = solid(400, 240, QColor::fromRgbF(.02f, .02f, .02f));
        { QPainter p(&night); p.fillRect(QRect(198, 118, 4, 4), Qt::white); }
        LensEffectSettings s; s.effect = LensEffect::Starburst; s.starAngle = 0; s.starPoints = 4; s.starAmount = 100; s.starLength = 20;
        const QImage image = s.apply(night, RenderBackend::Cpu);
        const auto along = pixel(image, 225, 120);
        QVERIFY2(along[0] > 20 && along[2] > 20, "the spike reaches 25 pixels out");
        QVERIFY2(along[0] - along[2] < 30, "red and blue reach nearly as far");
    }

    // 7b5ba97 CompositorApp Filter menu: those that can be layers, in the New Adjustment Layer menu's order, then the rest.
    void filterAndAdjustmentMenusFollowTheMac()
    {
        MainWindow window; window.show();
        QMenu *filter = nullptr, *layerMenu = nullptr;
        for (QAction *action : window.menuBar()->actions()) {
            if (action->text().remove('&') == "Filter") filter = action->menu();
            if (action->text().remove('&') == "Layer") layerMenu = action->menu();
        }
        QVERIFY(filter && layerMenu);
        QStringList items;
        for (QAction *action : filter->actions()) items << (action->isSeparator() ? QStringLiteral("—") : action->text());
        items.removeAll(QString());
        const QStringList layered{"Gaussian Blur…", "Motion Blur…", "Progressive Blur…", "Sharpen…", "Reduce Noise…", "Add Noise…",
                                  "Vignette…", "Bloom / Glow…", "RGB Split…", "Lens Effects…", "Tonal Contrast…"};
        const int first = int(items.indexOf("Gaussian Blur…"));
        QVERIFY(first > 0);
        QCOMPARE(items.mid(first, layered.size()), layered);
        QCOMPARE(items.value(first + int(layered.size())), QStringLiteral("—"));
        QVERIFY(items.indexOf("Dither…") > first && items.indexOf("Camera Raw Filter…") > items.indexOf("Lens Correction…"));
        QMenu *adjustments = nullptr;
        for (QAction *action : layerMenu->actions()) if (action->menu() && action->text() == "New Adjustment Layer") adjustments = action->menu();
        QVERIFY(adjustments);
        window.session().createDocument(40, 30); window.syncDocumentViews();
        emit adjustments->aboutToShow();
        QStringList kinds;
        for (QAction *action : adjustments->actions()) kinds << (action->isSeparator() ? QStringLiteral("—") : action->text());
        QCOMPARE(kinds, (QStringList{"Curves…", "Levels…", "Hue/Saturation…", "Black && White…", "Color Balance…", "Exposure…", "Gradient Map…",
                                     "Grain…", "Invert", "—", "Gaussian Blur…", "Motion Blur…", "Progressive Blur…", "Sharpen…", "Reduce Noise…",
                                     "Add Noise…", "Vignette…", "Bloom / Glow…", "RGB Split…", "Lens Effects…", "Tonal Contrast…"}));
    }

    // 2d44b8d: an adjustment layer's panel has no Preview checkbox; the filter's own panel keeps one. b0f0b2b: Edit
    // Adjustment on a Sharpen layer opens its panel and edits the layer's settings as one step.
    void adjustmentLayerPanelsHaveNoPreviewCheckbox()
    {
        MainWindow window; window.resize(1000, 760); window.show();
        EditorSession &session = window.session();
        session.createDocument(60, 40); QVERIFY(session.insertImage(scene(60, 40), "Scene"));
        window.syncDocumentViews();
        bool layerPreview = true, filterPreview = false;
        const auto inPanel = [&](const std::function<void(QDialog *)> &check) {
            QTimer::singleShot(0, &window, [&window, check] {
                auto *dialog = window.findChild<QDialog *>("sharpenDialog");
                if (!dialog) { QFAIL("no Sharpen panel"); return; }
                check(dialog);
            });
        };
        inPanel([&](QDialog *dialog) {
            auto *box = dialog->findChild<QCheckBox *>("filterPreview");
            filterPreview = box && box->isVisibleTo(dialog);
            dialog->reject();
        });
        window.findChild<QAction *>("cmdSharpen")->trigger();
        QVERIFY(filterPreview);
        QVERIFY(session.addAdjustment("Sharpen"));
        window.syncDocumentViews();
        inPanel([&](QDialog *dialog) {
            auto *box = dialog->findChild<QCheckBox *>("filterPreview");
            layerPreview = box && box->isVisibleTo(dialog);
            if (auto *amount = dialog->findChild<QDoubleSpinBox *>("sharpenAmount")) amount->setValue(180);
            dialog->accept();
        });
        window.findChild<QAction *>("commandEditAdjustment")->trigger();
        QVERIFY(!layerPreview);
        QCOMPARE(filterLayer(*session.activeLayer(), "sharpen").value("amount").toDouble(), 180.0);
        QCOMPARE(session.history().undoName(), QStringLiteral("Edit Sharpen"));
    }

    // 9986d12 SelectionFeatherTests.featherAddsUpEachTimeItIsApplied, 820b8fa theStatusBarsFeatherFollowsTheSelection.
    void featherAddsUpAndTheStatusBarShowsIt()
    {
        EditorSession session; session.createDocument(60, 20);
        QVERIFY(!session.selectionFeather());
        session.selectAll();
        QVERIFY2(!session.selectionFeather(), "a hard edge has no feather to show");
        QVERIFY(session.setRectangularSelection(QRect(10, 4, 40, 12)));
        for (int i = 0; i < 3; ++i) QVERIFY(session.featherSelection(2));
        QCOMPARE(session.selectionFeather(), std::optional<int>(6));
        // The hard edge feathered by the total, not a blur of a blur: as one Feather of 6 makes it.
        EditorSession once; once.createDocument(60, 20); once.setRectangularSelection(QRect(10, 4, 40, 12)); once.featherSelection(6);
        QCOMPARE(*session.document()->selection, *once.document()->selection);
        session.undo();
        QCOMPARE(session.selectionFeather(), std::optional<int>(4));
        QVERIFY(session.featherSelection(250));
        QCOMPARE(session.selectionFeather(), std::optional<int>(250));
        session.deselect();
        QVERIFY(!session.selectionFeather());
    }

    // 9986d12 SelectionTests.repeatedContractAndExpandKeepARectangleARectangle; Expand and Contract share one amount, 5.
    void contractAndExpandKeepARectangleARectangle()
    {
        EditorSession session; session.createDocument(1920, 1080);
        QCOMPARE(session.selectionModifyAmount(), 5);
        for (const QRect start : {QRect(300, 200, 900, 700), QRect(123, 87, 641, 513)}) {
            QVERIFY(session.setRectangularSelection(start));
            QRect expected = start;
            for (int step = 0; step < 3; ++step) {
                QVERIFY(session.contractSelection(5)); expected.adjust(5, 5, -5, -5);
                QCOMPARE(*session.selectionBounds(), expected);
                const QImage mask = session.document()->selection->convertToFormat(QImage::Format_Grayscale8);
                for (const QPoint corner : {expected.topLeft(), expected.topRight(), expected.bottomLeft(), expected.bottomRight()})
                    QCOMPARE(int(mask.pixelColor(corner).red()), 255);
            }
            for (int step = 0; step < 3; ++step) { QVERIFY(session.expandSelection(5)); expected.adjust(-5, -5, 5, 5); }
            QCOMPARE(*session.selectionBounds(), start);
        }
        // Contracted by more than it is: nothing selected.
        QVERIFY(session.setRectangularSelection(QRect(10, 10, 20, 20)));
        QVERIFY(session.contractSelection(15));
        QVERIFY(!session.selectionBounds() || session.selectionBounds()->isEmpty());
    }

    // 63f1013 SelectionTests.selectionSizeFollowsTheMarqueeAndStaysOnTheCanvas (the finished selection's part).
    void selectionSizeStaysOnTheCanvas()
    {
        EditorSession session; session.createDocument(100, 60);
        QVERIFY(!session.selectionSize());
        QVERIFY(session.setRectangularSelection(QRect(10, 20, 40, 15)));
        QCOMPARE(*session.selectionSize(), QSize(40, 15));
        QVERIFY(session.setPolygonSelection(QPolygonF(QRectF(70, -20, 60, 60))));
        QCOMPARE(*session.selectionSize(), QSize(30, 40));
        session.deselect();
        QVERIFY(!session.selectionSize());
    }

    // bb5c8cc hiddenEdgesKeepTheSelectionAndAnyNewOneShows, 9cd704d extrasHideTheOutlineAndGuidesTogether.
    void showExtrasHidesTheOutlineAndGuidesTogether()
    {
        EditorSession session; session.createDocument(100, 60);
        session.setShowsGuides(true);
        QVERIFY(session.setRectangularSelection(QRect(10, 10, 20, 20)));
        session.setShowsExtras(false);
        QVERIFY(!session.showsSelectionEdges() && !session.showsGuides() && session.document()->selection);
        session.setShowsExtras(true);
        QVERIFY(session.showsSelectionEdges() && session.showsGuides());
        session.setShowsGuides(false);
        session.setShowsExtras(false);
        session.setShowsExtras(true);
        QVERIFY2(session.showsSelectionEdges() && !session.showsGuides(), "Show Extras doesn't turn on guides Ctrl+; turned off");
        session.setShowsSelectionEdges(false);
        QVERIFY(session.setRectangularSelection(QRect(40, 40, 20, 10)));
        QVERIFY2(session.showsSelectionEdges(), "a new selection shows its outline");
    }

    // 4f9ed78 HistoryTests.undoStopsAtWhatStartedTheDocument.
    void undoStopsAtWhatStartedTheDocument()
    {
        EditorSession session;
        QVERIFY(session.insertImage(solid(20, 10, Qt::red), "Opened"));
        const Document opened = *session.document();
        QVERIFY(!session.canUndo());
        session.addBlankLayer();
        session.undo(); session.undo();
        QVERIFY(session.document() && *session.document() == opened);
        EditorSession canvas; canvas.createDocument(40, 40);
        QVERIFY(!canvas.canUndo() && canvas.document());
    }

    // 46af4f2 ScrollZoomTests.theWheelTurnedAwayZoomsInWithNaturalScrollingOnOrOff.
    void wheelZoomFollowsTheWheelWhicheverWayScrollingIsSet()
    {
        QVERIFY(CanvasWidget::wheelZoomFactor(10, false) > 1);
        QVERIFY(CanvasWidget::wheelZoomFactor(-10, false) < 1);
        QVERIFY(CanvasWidget::wheelZoomFactor(-10, true) > 1);
        QVERIFY(CanvasWidget::wheelZoomFactor(10, true) < 1);
        QCOMPARE(CanvasWidget::wheelZoomFactor(0, false), 1.0);
    }

    // 42eb9b2 ImageSizeTests: the scrub reaches past 100 megapixels, within the side and surface limits.
    void imageSizeScrubRespectsSideAndSurfaceLimits()
    {
        const auto range = DocumentLimits::imageSizeScrubPixelRange(4000, 6000, false);
        QVERIFY(range.first <= 20000 && range.second >= 20000);
        QCOMPARE(range.second, double(DocumentLimits::maxSide));
        QCOMPARE(DocumentLimits::imageSizeScrubPixelRange(6000, 20000, false).second, double(DocumentLimits::maxSurfacePixels) / 20000);
        const auto locked = DocumentLimits::imageSizeScrubPixelRange(8000, 6000, true);
        QVERIFY(locked.second >= 16000);
        QVERIFY(std::abs(locked.second * (locked.second * 6000 / 8000) - double(DocumentLimits::maxSurfacePixels)) < .001);
        QCOMPARE(DocumentLimits::imageSizeScrubPixelRange(8000, 1000, true).second, double(DocumentLimits::maxSide));
        const auto tall = DocumentLimits::imageSizeScrubPixelRange(1000, 8000, true);
        QCOMPARE(tall.second * 8, double(DocumentLimits::maxSide));
        QCOMPARE(tall.first, 1.0);
        QCOMPARE(DocumentLimits::imageSizeScrubPixelRange(8000, 1000, true).first, 8.0);
    }

    // 1ea5eab LayerAppearanceTests.selectionToolNumberKeysSetSelectedLayersOpacity; fc3c81a moving the Marquee mid-drag;
    // 2872eb1 letters in the Layers panel are shortcuts, not a jump to a layer's name.
    void selectionToolsTakeOpacityKeysAndSpaceMovesTheMarquee()
    {
        MainWindow window; window.resize(900, 700); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.session().createDocument(200, 150, true);
        window.syncDocumentViews();
        CanvasWidget *canvas = window.canvas();
        for (const auto tool : {CanvasWidget::Tool::Marquee, CanvasWidget::Tool::Lasso, CanvasWidget::Tool::Wand}) {
            canvas->setTool(tool);
            window.session().setSelectedLayersOpacity(1);
            QTest::keyClick(canvas, Qt::Key_3);
            QTest::qWait(700);
            QCOMPARE(window.session().activeLayer()->opacity, .3);
        }
        canvas->setTool(CanvasWidget::Tool::Marquee);
        canvas->setFocus();
        const QPointF origin = canvas->canvasRect().topLeft();
        const double zoom = canvas->zoom();
        const auto at = [&](double x, double y) { return (origin + QPointF(x, y) * zoom).toPoint(); };
        // Away from the canvas's edges and middle, which a Marquee's corners snap to.
        QTest::mousePress(canvas, Qt::LeftButton, {}, at(31, 33));
        QTest::mouseMove(canvas, at(53, 47));
        QTest::keyPress(canvas, Qt::Key_Space);
        QTest::mouseMove(canvas, at(70, 60));
        QTest::mouseMove(canvas, at(85, 70));
        QTest::keyRelease(canvas, Qt::Key_Space);
        QTest::mouseRelease(canvas, Qt::LeftButton, {}, at(85, 70));
        QVERIFY(window.session().selectionBounds());
        QCOMPARE(*window.session().selectionBounds(), QRect(46, 43, 22, 14));

        window.session().addBlankLayer(); window.session().renameLayer(*window.session().document()->activeLayerId, "Folder 1");
        window.syncDocumentViews();
        const auto before = window.session().document()->activeLayerId;
        window.layerView()->setFocus();
        QTest::keyClick(window.layerView(), Qt::Key_K);
        QCOMPARE(window.session().document()->activeLayerId, before);
    }

    // 4ae432b LayerAlignTests: a 400 × 300 canvas with a solid layer for each rect, bottom to top.
    static QVector<QUuid> boxes(EditorSession &session, const QVector<QRect> &rects)
    {
        session.createDocument(400, 300);
        QVector<QUuid> ids;
        for (const QRect &rect : rects) {
            session.insertImage(solid(rect.width(), rect.height(), Qt::red), "Box", QRectF(rect).center());
            ids.push_back(*session.document()->activeLayerId);
        }
        return ids;
    }
    static QVector<double> origins(EditorSession &session, const QVector<QUuid> &ids, bool x)
    {
        QVector<double> result;
        for (const QUuid &id : ids) for (const Layer &layer : session.document()->layers) if (layer.id == id) result << (x ? layer.transform.origin.x() : layer.transform.origin.y());
        return result;
    }
    void severalLayersAlignToEachOther()
    {
        EditorSession session; const auto ids = boxes(session, {QRect(10, 20, 40, 30), QRect(100, 60, 60, 20), QRect(200, 10, 20, 50)});
        session.selectLayers(QSet<QUuid>(ids.begin(), ids.end()));
        QVERIFY(session.alignLayers(LayerAlignment::Left));
        QCOMPARE(origins(session, ids, true), (QVector<double>{10, 10, 10}));
        QCOMPARE(session.history().undoName(), QStringLiteral("Align Left Edges"));
        QVERIFY(session.alignLayers(LayerAlignment::Bottom));
        QCOMPARE(origins(session, ids, false), (QVector<double>{50, 60, 30}));
        session.undo();
        QCOMPARE(origins(session, ids, false), (QVector<double>{20, 60, 10}));
    }
    void oneLayerAlignsToTheCanvasAndTheSelectionComesFirst()
    {
        EditorSession session; const auto ids = boxes(session, {QRect(10, 10, 101, 60)});
        session.selectLayers({ids.first()});
        QVERIFY(session.alignLayers(LayerAlignment::HorizontalCenter));
        // 400 − 101 leaves an odd 299 to share: the layer lands on a whole pixel, not between two.
        QCOMPARE(origins(session, ids, true).first(), 150.0);
        QVERIFY(session.alignLayers(LayerAlignment::VerticalCenter));
        QCOMPARE(origins(session, ids, false).first(), 120.0);
        QVERIFY(session.setRectangularSelection(QRect(300, 0, 50, 50)));
        QVERIFY(session.alignLayers(LayerAlignment::Right));
        QCOMPARE(origins(session, ids, true).first(), 249.0);
    }
    void aSelectedFolderMovesAsOne()
    {
        EditorSession session; const auto ids = boxes(session, {QRect(50, 10, 20, 20), QRect(90, 40, 20, 20), QRect(200, 100, 20, 20)});
        session.selectLayers({ids[0], ids[1]});
        session.groupSelectedLayers();
        const QUuid folder = *session.document()->activeLayerId;
        session.selectLayers({folder, ids[2]});
        QVERIFY(session.alignLayers(LayerAlignment::Left));
        QCOMPARE(origins(session, ids, true), (QVector<double>{50, 90, 50}));
    }
    void distributeSpreadsMiddlesOrGapsEvenly()
    {
        const QVector<QRectF> three{QRectF(0, 0, 10, 10), QRectF(60, 0, 30, 10), QRectF(100, 0, 10, 10)};
        const auto widths = [](const QVector<QSizeF> &o) { QVector<double> r; for (const auto &s : o) r << s.width(); return r; };
        QCOMPARE(widths(distributionOffsets(LayerDistribution::HorizontalCenters, three)), (QVector<double>{0, -20, 0}));
        QCOMPARE(widths(distributionOffsets(LayerDistribution::HorizontalSpacing, three)), (QVector<double>{0, -20, 0}));
        const QVector<QRectF> uneven{QRectF(0, 0, 10, 10), QRectF(0, 70, 10, 10), QRectF(0, 20, 10, 40), QRectF(0, 100, 10, 10)};
        const double gap = 40.0 / 3;
        const auto spaced = distributionOffsets(LayerDistribution::VerticalSpacing, uneven);
        QVERIFY(std::abs(spaced[2].height() - (10 + gap - 20)) < .001);
        QVERIFY(std::abs(spaced[1].height() - (10 + gap + 40 + gap - 70)) < .001);
        QVERIFY(spaced[0].height() == 0 && spaced[3].height() == 0);
    }
    void distributeNeedsThreeAndMovesWholePixels()
    {
        EditorSession session; const auto ids = boxes(session, {QRect(0, 0, 10, 10), QRect(50, 0, 10, 10), QRect(101, 0, 10, 10)});
        session.selectLayers({ids[0], ids[1]});
        QVERIFY(!session.canDistributeLayers());
        QVERIFY(!session.distributeLayers(LayerDistribution::HorizontalCenters));
        QCOMPARE(origins(session, ids, true), (QVector<double>{0, 50, 101}));
        session.selectLayers(QSet<QUuid>(ids.begin(), ids.end()));
        QVERIFY(session.distributeLayers(LayerDistribution::HorizontalCenters));
        QCOMPARE(origins(session, ids, true), (QVector<double>{0, 51, 101}));
        QCOMPARE(session.history().undoName(), QStringLiteral("Distribute Horizontal Centers"));
    }
    void alignInTheMoveBarAndLayerMenu()
    {
        MainWindow window; window.show();
        QVERIFY(window.findChild<QToolButton *>("alignLayers"));
        QVERIFY(window.findChild<QAction *>("commandAlignMenu") && window.findChild<QAction *>("commandDistributeMenu"));
        QStringList names; for (QAction *a : window.findChild<QAction *>("commandAlignMenu")->menu()->actions()) names << a->text();
        QCOMPARE(names, (QStringList{"Left Edges", "Horizontal Centers", "Right Edges", "Top Edges", "Vertical Centers", "Bottom Edges"}));
        names.clear(); for (QAction *a : window.findChild<QAction *>("commandDistributeMenu")->menu()->actions()) names << a->text();
        QCOMPARE(names, (QStringList{"Horizontal Centers", "Vertical Centers", "Horizontal Spacing", "Vertical Spacing"}));
    }

    // 36ced5d CropTests.customRatiosAcceptPositiveNumbersAndRejectInvalidInput.
    void customCropRatiosAcceptPositiveNumbersAndRejectInvalidInput()
    {
        QCOMPARE(*DocumentLimits::cropRatioFromText("9:20"), .45);
        QCOMPARE(*DocumentLimits::cropRatioFromText(" 2.5 : 1 "), 2.5);
        QCOMPARE(*DocumentLimits::cropRatioFromText("2,5:1"), 2.5);
        QCOMPARE(*DocumentLimits::cropRatioFromText("900:2000"), .45);
        for (const char *text : {"", "9", ":20", "9:", "9:20:1", "Free", "0:20", "9:0", "-9:20", "9:-20", "nan:1", "1:inf", "1e309:1",
                                 "1e308:1e-308", "1e-308:1e308", "1:30001", "30001:1"})
            QVERIFY2(!DocumentLimits::cropRatioFromText(QLatin1String(text)), text);
    }

    // 36ced5d/a304465 CropTests.customRatioResizesTheFrameAndPreservesTheExistingChoices: the ratios grouped by shape,
    // Canvas in place of Original, 2.39:1, and a frame kept within the side limits.
    void customCropRatioResizesTheFrame()
    {
        MainWindow window; window.show();
        window.session().createDocument(900, 600, true); window.syncDocumentViews();
        CanvasWidget *canvas = window.canvas();
        canvas->setTool(CanvasWidget::Tool::Crop);
        auto *ratio = window.findChild<QComboBox *>("cropRatio");
        QVERIFY(ratio);
        QStringList items; for (int i = 0; i < ratio->count(); ++i) items << (ratio->itemText(i).isEmpty() ? QStringLiteral("—") : ratio->itemText(i));
        QCOMPARE(items, (QStringList{"Free", "Canvas", "—", "1:1", "—", "4:3", "16:9", "2.39:1", "—", "3:4", "9:16", "—", "Custom…"}));
        ratio->setCurrentIndex(ratio->findData("Canvas")); QCOMPARE(canvas->cropRatio(), 1.5);
        ratio->setCurrentIndex(ratio->findData("Free")); QCOMPARE(canvas->cropRatio(), 0.0);
        ratio->setCurrentIndex(ratio->findData("2.39:1")); QCOMPARE(canvas->cropRatio(), 2.39);
        const auto custom = [&](const QString &text) {
            QTimer::singleShot(0, &window, [&window, text] {
                auto *dialog = window.findChild<QDialog *>("customCropRatio");
                if (!dialog) return;
                dialog->findChild<QLineEdit *>()->setText(text);
                dialog->accept();
            });
            ratio->setCurrentIndex(ratio->findData("Custom"));
        };
        canvas->setCropRatio(0); ratio->setCurrentIndex(ratio->findData("Free"));
        custom(QStringLiteral("9:20"));
        QCOMPARE(*canvas->cropRect(), QRectF(0, -700, 900, 2000));
        QCOMPARE(ratio->currentText(), QStringLiteral("9:20"));
        custom(QStringLiteral("1:30000"));
        QCOMPARE(canvas->cropRect()->size(), QSizeF(1, 30000));
        custom(QStringLiteral("30000:1"));
        QCOMPARE(canvas->cropRect()->size(), QSizeF(30000, 1));
    }

    // d693a2a LayerTests.hideAndShowApplyToEverySelectedLayer.
    void hideAndShowApplyToEverySelectedLayer()
    {
        EditorSession session; session.createDocument(10, 10);
        for (int i = 0; i < 3; ++i) session.insertImage(solid(4, 4, Qt::red), "Layer");
        const auto layers = session.document()->layers;
        session.selectLayers({layers[0].id, layers[2].id}, layers[2].id);
        QCOMPARE(session.visibilityTitle(), QStringLiteral("Hide Layers"));
        session.toggleSelectedLayersVisibility();
        const auto visible = [&] { QVector<bool> v; for (const Layer &l : session.document()->layers) v << l.visible; return v; };
        QCOMPARE(visible(), (QVector<bool>{false, true, false}));
        QCOMPARE(session.visibilityTitle(), QStringLiteral("Show Layers"));
        session.toggleLayerVisibility(layers[0].id);
        session.toggleSelectedLayersVisibility();
        QCOMPARE(visible(), (QVector<bool>{true, true, true}));
        session.undo();
        QCOMPARE(visible(), (QVector<bool>{true, true, false}));
    }

    // d693a2a LiveMaskTests.menuClipsAndReleasesEverySelectedLayer.
    void menuClipsAndReleasesEverySelectedLayer()
    {
        EditorSession s; s.createDocument(2, 2);
        for (int i = 0; i < 4; ++i) s.insertImage(solid(2, 2, Qt::white), "Layer");
        QVector<QUuid> ids; for (const Layer &l : s.document()->layers) ids << l.id;
        const auto sources = [&] { QVector<std::optional<QUuid>> v; for (const Layer &l : s.document()->layers) v << l.maskSourceId; return v; };
        using O = std::optional<QUuid>;
        s.selectLayers(QSet<QUuid>(ids.begin(), ids.end()), ids[3]);
        QVERIFY(!s.canToggleSelectedClipping() && s.selectedClippingTitle() == "Create Clipping Mask");
        s.selectLayers({ids[1], ids[2], ids[3]}, ids[3]);
        QVERIFY(s.canToggleSelectedClipping());
        QVERIFY(s.toggleSelectedClipping());
        QCOMPARE(sources(), (QVector<O>{O(), ids[0], ids[0], ids[0]}));
        QCOMPARE(s.selectedClippingTitle(), QStringLiteral("Release Clipping Mask"));
        s.undo();
        for (const auto &source : sources()) QVERIFY(!source);
        s.redo();
        s.selectLayers({ids[2], ids[3]}, ids[3]);
        QVERIFY(s.toggleSelectedClipping());
        QCOMPARE(sources(), (QVector<O>{O(), ids[0], O(), O()}));
        s.selectLayers({ids[1], ids[3]}, ids[3]);
        QCOMPARE(s.selectedClippingTitle(), QStringLiteral("Create Clipping Mask"));
        QVERIFY(s.toggleSelectedClipping());
        QCOMPARE(sources(), (QVector<O>{O(), ids[0], O(), ids[2]}));
    }

    // de0a43d DeepLayerTests: Convert to 16-Bit / 8-Bit on every selected layer as one step, and 5de396b
    // eightBitFiltersKeepTheLayerSixteenBit: an 8-bit edit keeps the layer 16-bit, its untouched pixels exactly.
    void depthConversionsAndEightBitEditsKeepLayersDeep()
    {
        EditorSession s; s.createDocument(40, 10);
        QImage gradient(40, 10, QImage::Format_RGBA64_Premultiplied);
        for (int y = 0; y < 10; ++y) for (int x = 0; x < 40; ++x) gradient.setPixelColor(x, y, QColor::fromRgba64(quint16(x * 1601 + 7), quint16(30001 + x), 1234, 65535));
        s.insertImage(gradient, "Deep");
        s.insertImage(solid(40, 10, Qt::red), "Flat");
        const QUuid deep = s.document()->layers[0].id, flat = s.document()->layers[1].id;
        s.selectLayers({deep, flat}, flat);
        QVERIFY(s.canConvertToSixteenBit() && s.canConvertToEightBit());
        QVERIFY(s.convertLayers(true));
        QVERIFY(DeepImage::isDeep(s.document()->layers[1].image));
        QCOMPARE(s.history().undoName(), QStringLiteral("Convert to 16-Bit"));
        QVERIFY(!s.canConvertToSixteenBit());
        s.undo();
        s.selectLayers({deep}, deep);
        const QImage before = s.activeLayer()->image;
        DitherSettings dither;
        QVERIFY(s.setRectangularSelection(QRect(0, 0, 20, 10)));
        QVERIFY(s.applyDither(dither));
        const QImage after = s.activeLayer()->image;
        QVERIFY2(DeepImage::isDeep(after), "still 16-bit");
        const QImage a = after.convertToFormat(QImage::Format_RGBA64), b = before.convertToFormat(QImage::Format_RGBA64);
        for (int x = 20; x < 40; ++x) QCOMPARE(a.pixelColor(x, 5).rgba64(), b.pixelColor(x, 5).rgba64());
        s.deselect();
        QVERIFY(s.applyDither(dither));
        QVERIFY2(DeepImage::isDeep(s.activeLayer()->image), "a whole-layer 8-bit edit keeps it 16-bit too");
        // An edit that works only at 8 bits on the whole layer (Content-Aware Fill's, here put in by hand) keeps it 16-bit
        // too, its unchanged pixels exactly.
        const QImage deepBefore = s.activeLayer()->image;
        QImage eight = deepBefore.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        { QPainter p(&eight); p.fillRect(QRect(0, 0, 5, 10), Qt::black); }
        s.beginEdit(QStringLiteral("Content-Aware Fill")); s.activeLayer()->image = eight; s.endEdit();
        const QImage restored = s.activeLayer()->image.convertToFormat(QImage::Format_RGBA64);
        QVERIFY(DeepImage::isDeep(s.activeLayer()->image));
        QCOMPARE(restored.pixelColor(2, 5).rgba64().red(), quint16(0));
        for (int x = 5; x < 40; ++x) QCOMPARE(restored.pixelColor(x, 5).rgba64(), deepBefore.convertToFormat(QImage::Format_RGBA64).pixelColor(x, 5).rgba64());
        QVERIFY(s.convertLayers(false));
        QVERIFY2(!DeepImage::isDeep(s.activeLayer()->image), "Convert to 8-Bit still makes it 8-bit");
    }

    // 0673a4a LayerExportTests.eachLayerExportsOnItsOwnTrimmedToItsPixels, layerNamesBecomeFileNames.
    void eachLayerExportsOnItsOwnTrimmedToItsPixels()
    {
        EditorSession s; s.createDocument(10, 10);
        s.insertImage(solid(2, 3, Qt::red), "Red", QPointF(2, 2.5));
        const QUuid red = *s.document()->activeLayerId;
        s.insertImage(solid(2, 10, Qt::green), "Green", QPointF(1, 5));
        const QUuid green = *s.document()->activeLayerId;
        QVERIFY(s.linkMask(red, green));
        s.insertImage(solid(10, 10, Qt::transparent), "Empty");
        const QUuid empty = *s.document()->activeLayerId;
        s.addGroup();
        const QUuid folder = *s.document()->activeLayerId;
        s.setLayerOpacity(folder, .5);
        s.insertImage(solid(6, 1, Qt::blue), "Blue", QPointF(3, 2.5));
        const QUuid blue = *s.document()->activeLayerId;
        if (s.activeLayer()->parentId != folder) QVERIFY(s.placeLayer(blue, folder));
        s.toggleLayerVisibility(blue);
        const auto decode = [](const std::optional<QByteArray> &data) { return data ? QImage::fromData(*data, "PNG").convertToFormat(QImage::Format_RGBA8888) : QImage(); };
        const QImage redPng = decode(ImageExporter::layerPng(*s.document(), red));
        QCOMPARE(redPng.size(), QSize(2, 3));
        QVERIFY2(redPng.pixelColor(0, 1).green() > 230 && redPng.pixelColor(0, 1).red() < 13, "the layer clipped to it comes too");
        const QImage bluePng = decode(ImageExporter::layerPng(*s.document(), blue));
        QCOMPARE(bluePng.size(), QSize(6, 1));
        QVERIFY2(std::abs(bluePng.pixelColor(3, 0).alpha() - 128) <= 5, "its folder's opacity applies");
        QVERIFY2(!ImageExporter::layerPng(*s.document(), folder), "the folder's Blue is hidden");
        QVERIFY(!ImageExporter::layerPng(*s.document(), empty));
        QCOMPARE(ImageExporter::layerFileName("Logo / Dark: v2"), QStringLiteral("Logo - Dark- v2"));
        QCOMPARE(ImageExporter::layerFileName("  "), QStringLiteral("Layer"));
        QCOMPARE(ImageExporter::layerFileName(".hidden"), QStringLiteral("Layer.hidden"));
    }

    // b9a0ed8 RimLightTests: a head and shoulders in front of a dark backdrop (the mac's figure, rows top-down).
    static QImage figure()
    {
        QImage image = solid(600, 400, QColor::fromRgbF(.08f, .09f, .11f));
        QPainter p(&image); p.setRenderHint(QPainter::Antialiasing); p.setPen(Qt::NoPen); p.setBrush(QColor::fromRgbF(.55f, .42f, .35f));
        p.drawEllipse(QRectF(250, 60, 100, 120)); p.drawRect(QRectF(285, 170, 30, 60)); p.drawEllipse(QRectF(170, 210, 260, 310));
        return image;
    }
    static std::pair<int, int> subjectSpan(const QImage &image, int y)
    {
        const QImage mask = RimLighting::subject(image).convertToFormat(QImage::Format_Grayscale8);
        int left = -1, right = -1;
        for (int x = 0; x < mask.width(); ++x) if (mask.constScanLine(y)[x] > 128) { if (left < 0) left = x; right = x; }
        return {left, right};
    }
    static QImage rimLit(const QImage &image, double angle, double strength, double width, double choke = 0, double glow = 10)
    {
        RimLightSettings s; s.glow = glow;
        RimLight light; light.angle = angle; light.color = Qt::white; light.strength = strength; light.width = width; light.choke = choke;
        s.lights = {light};
        return RimLighting::apply(image, s, RenderBackend::Cpu);
    }
    void aRimLightFromTheLeftCatchesTheLeftEdgeOnly()
    {
        if (!SubjectRemoval::isModelAvailable()) QSKIP("subject model unavailable");
        const QImage original = figure(), lit = rimLit(original, 180, 80, 30);
        const auto [left, right] = subjectSpan(original, 110);
        QVERIFY(left > 0 && right > left);
        const int l = pixel(lit, left + 4, 110)[1] - pixel(original, left + 4, 110)[1];
        const int r = pixel(lit, right - 4, 110)[1] - pixel(original, right - 4, 110)[1];
        const int m = pixel(lit, (left + right) / 2, 110)[1] - pixel(original, (left + right) / 2, 110)[1];
        QVERIFY2(l > 25, qPrintable(QStringLiteral("the edge facing the light: %1").arg(l)));
        QVERIFY2(r <= 3, qPrintable(QStringLiteral("the edge facing away: %1").arg(r)));
        QVERIFY2(m <= 3, qPrintable(QStringLiteral("the front: %1").arg(m)));
        QCOMPARE(pixel(lit, 20, 20), pixel(original, 20, 20));
    }
    void rimChokeHoldsTheLightFurtherIn()
    {
        if (!SubjectRemoval::isModelAvailable()) QSKIP("subject model unavailable");
        const QImage original = figure();
        const int x = subjectSpan(original, 110).first + 12;
        const auto lift = [&](double choke) { return pixel(rimLit(original, 180, 15, 100, choke), x, 110)[1] - pixel(original, x, 110)[1]; };
        const int held = lift(100), feathered = lift(0);
        QVERIFY2(held > feathered + 10, qPrintable(QStringLiteral("held %1, feathered %2").arg(held).arg(feathered)));
    }
    void rimGlowLightsTheAirBesideTheLitEdgeAndLeavesTheSubject()
    {
        if (!SubjectRemoval::isModelAvailable()) QSKIP("subject model unavailable");
        const QImage original = figure(), none = rimLit(original, 180, 80, 30, 0, 0), glowing = rimLit(original, 180, 80, 30, 0, 100);
        const auto [left, right] = subjectSpan(original, 110);
        const int beside = pixel(glowing, left - 6, 110)[1] - pixel(none, left - 6, 110)[1];
        const int middle = pixel(glowing, (left + right) / 2, 110)[1] - pixel(none, (left + right) / 2, 110)[1];
        QVERIFY2(beside > 10, qPrintable(QStringLiteral("the air beside the lit edge: %1").arg(beside)));
        QVERIFY2(std::abs(middle) <= 2, qPrintable(QStringLiteral("the subject keeps only its rims: %1").arg(middle)));
        QCOMPARE(pixel(none, left - 6, 110), pixel(original, left - 6, 110));
        // b9a0ed8 glowWrapsALittleWayInAcrossTheLitEdge.
        const int x = left + 4;
        const int wrap = pixel(rimLit(original, 180, 30, 0, 100, 100), x, 110)[1] - pixel(rimLit(original, 180, 30, 0, 100, 0), x, 110)[1];
        QVERIFY2(wrap > 4, qPrintable(QStringLiteral("the glow across the edge: %1").arg(wrap)));
    }
    void eachRimLightWrapsAsFarAsItsOwnWidth()
    {
        if (!SubjectRemoval::isModelAvailable()) QSKIP("subject model unavailable");
        const QImage original = figure();
        const int x = subjectSpan(original, 110).first + 14;
        const auto lift = [&](double width) { return pixel(rimLit(original, 180, 80, width), x, 110)[1] - pixel(original, x, 110)[1]; };
        QVERIFY(lift(100) > lift(0) + 10);
    }
    void rimSubjectIsFoundOnceAndKeptAndLightsAreAddedAcross()
    {
        if (!SubjectRemoval::isModelAvailable()) QSKIP("subject model unavailable");
        const QImage original = figure();
        RimLighting::subject(original);
        QVERIFY(RimLighting::hasSubject(original));
        QVERIFY2(!RimLighting::hasSubject(figure()), "another image's subject isn't this one's");
        RimLightSettings s;
        s.lights.append(s.nextLight());
        QCOMPARE(s.lights[1].angle, -45.0);
        QCOMPARE(s.lights[1].color, QColor::fromRgbF(.7f, .84f, 1));
        s.lights.append(s.nextLight());
        QCOMPARE(s.lights[2].color, QColor(Qt::white));
        s.lights.append(s.nextLight());
        QCOMPARE(s.normalized().lights.size(), 3);
    }

    // 1405545 VolumetricLightTests: a dark room with a bright window crossed by a bar (rows top-down).
    static QImage room()
    {
        QImage image = solid(600, 400, QColor::fromRgbF(.06f, .06f, .07f));
        QPainter p(&image);
        p.fillRect(QRect(260, 40, 80, 80), QColor::fromRgbF(1, .97f, .9f));
        p.fillRect(QRectF(296, 40, 8, 80), QColor::fromRgbF(.04f, .04f, .04f));
        return image;
    }
    static int gray(const QImage &image, int x, int y) { const auto c = pixel(image, x, y); return (c[0] + c[1] + c[2]) / 3; }
    void theLightStartsOnTheBrightestSpotInTheBiggestArea()
    {
        QVERIFY(QRectF(250, 30, 100, 100).contains(VolumetricLighting::brightestPoint(room())));
        QImage image = solid(600, 400, QColor::fromRgbF(.05f, .05f, .05f));
        { QPainter p(&image); p.fillRect(QRect(220, 20, 160, 120), QColor::fromRgbF(.9f, .88f, .85f)); p.fillRect(QRect(100, 330, 30, 30), Qt::white); }
        const QPointF point = VolumetricLighting::brightestPoint(image);
        QVERIFY2(QRectF(220, 20, 160, 120).contains(point), qPrintable(QStringLiteral("in the opening: %1, %2").arg(point.x()).arg(point.y())));
    }
    void beamsLightTheRoomAndTheBarCastsAShaftOfShadow()
    {
        // Vision finds no foreground object in this drawing; U2Net, which stands in for it here, finds the bright window
        // salient, which would then block its own light. As on the mac: no subject.
        SubjectRemoval::setSegmentationProvider([](const QImage &, QString *error, std::atomic<bool> *) {
            if (error) *error = QStringLiteral("No foreground subject was detected in this layer."); return QImage(); });
        const auto restore = qScopeGuard([] { SubjectRemoval::resetSegmentationProvider(); });
        const QImage original = room();
        VolumetricLightSettings s; s.haze = 0; s.shafts = 0; s.length = 100; s.intensity = 80;
        const QImage lit = VolumetricLighting::apply(original, s, QPointF(300, 80), RenderBackend::Cpu);
        const int below = 120 + 60;
        const int shadowed = gray(lit, 300, below), open = gray(lit, 278, below), before = gray(original, 278, below);
        QVERIFY2(open > before + 20, qPrintable(QStringLiteral("the beams light the room below the window: %1 → %2").arg(before).arg(open)));
        QVERIFY2(shadowed < open - 6, qPrintable(QStringLiteral("the bar's shadow runs through them: %1 against %2").arg(shadowed).arg(open)));
        const int corner = gray(lit, 5, 395);
        QVERIFY2(corner < open - 10, qPrintable(QStringLiteral("the far corner gets less: %1 against %2").arg(corner).arg(open)));
    }
    void keepSubjectClearHoldsTheSubjectNear()
    {
        DepthMap far; far.width = far.height = 16; far.values.assign(256, .2f);
        std::vector<float> subject(64 * 64, 0.f);
        for (int y = 16; y < 48; ++y) for (int x = 16; x < 48; ++x) subject[size_t(y) * 64 + x] = 1;
        const float clear = VolumetricLighting::composedDepth(far, &subject, QSize(64, 64), true)[32 * 64 + 32];
        const float own = VolumetricLighting::composedDepth(far, &subject, QSize(64, 64), false)[32 * 64 + 32];
        QVERIFY2(clear >= .89f, qPrintable(QStringLiteral("kept near: %1").arg(clear)));
        QVERIFY2(std::abs(own - .2f) < .05f, qPrintable(QStringLiteral("at its own depth: %1").arg(own)));
    }
    void depthContrastSpreadsBunchedDepthsAndTheRampFollowsThem()
    {
        DepthMap bunched; bunched.width = 5; bunched.height = 1; bunched.values = {0, .01f, .02f, .03f, 1};
        QCOMPARE(bunched.equalized(0).values, bunched.values);
        QCOMPARE(bunched.equalized(1).values, (std::vector<float>{0, .25f, .5f, .75f, 1}));
        const auto half = bunched.equalized(.5f).values;
        QVERIFY(std::is_sorted(half.begin(), half.end()) && half[2] > .2f);
        const auto guessed = VolumetricLighting::ramp(30, nullptr);
        QVERIFY(std::abs(guessed.first - .18f) < 1e-6f && std::abs(guessed.second - .68f) < 1e-6f);
        std::vector<float> distances(101); for (int i = 0; i <= 100; ++i) distances[size_t(i)] = i / 100.f;
        const auto read = VolumetricLighting::ramp(50, &distances);
        QVERIFY(std::abs(read.first - .5f) < .011f && read.second >= read.first + .45f - 1e-6f);
        VolumetricLightSettings zero; zero.intensity = 0; zero.haze = 0;
        QVERIFY(zero.isIdentity());
    }
    // 039d445/0dbb4e3: DepthMini, converted from the mac's Core ML program, runs here and says what's nearer.
    void depthModelSeesTheNearBoxNearer()
    {
        if (!DepthModel::isAvailable()) QSKIP("depth model not installed");
        QImage scene(640, 480, QImage::Format_RGBA8888_Premultiplied);
        QRandomGenerator random(5);
        for (int y = 0; y < 480; ++y) {
            uchar *row = scene.scanLine(y);
            for (int x = 0; x < 640; ++x) {
                const bool sky = y < 192; const int t = sky ? 0 : int(random.bounded(60.0) * (y - 192) / 288.0);
                const int r = sky ? 150 : 90 + t, g = sky ? 180 : 80 + t, b = sky ? 220 : 60 + t;
                row[x * 4] = uchar(r); row[x * 4 + 1] = uchar(g); row[x * 4 + 2] = uchar(b); row[x * 4 + 3] = 255;
            }
        }
        { QPainter p(&scene); p.fillRect(QRect(256, 336, 128, 120), QColor(200, 40, 40)); }
        QCOMPARE(DepthModel::shape(scene.size()), QSize(512, 384));
        QString error;
        const auto depth = DepthModel::depth(scene, &error);
        QVERIFY2(depth, qPrintable(error));
        QCOMPARE(depth->width, 640); QCOMPARE(depth->height, 480);
        const auto at = [&](int x, int y) { return depth->values[size_t(y) * depth->width + x]; };
        QVERIFY2(at(320, 400) > at(320, 250) && at(320, 250) > at(320, 50),
                 qPrintable(QStringLiteral("box %1, floor %2, sky %3").arg(at(320, 400)).arg(at(320, 250)).arg(at(320, 50))));
        const QImage map = VolumetricLighting::depthMap(scene, VolumetricLightSettings());
        QCOMPARE(map.size(), scene.size());
        QVERIFY(pixel(map, 320, 400)[0] > pixel(map, 320, 50)[0]);
    }
    void volumetricLightIsOneUndoStepAndExtractDepthAddsALayer()
    {
        EditorSession s; s.createDocument(600, 400);
        QVERIFY(s.insertImage(room(), "Room"));
        VolumetricLightSettings settings; settings.center = QPointF(300, 80);
        QVERIFY(s.applyVolumetricLight(settings));
        QCOMPARE(s.history().undoName(), QStringLiteral("Volumetric Light"));
        QCOMPARE(s.lastFilterName(), QStringLiteral("Volumetric Light"));
        if (!DepthModel::isAvailable()) return;
        QVERIFY(s.extractDepth(settings));
        QCOMPARE(s.activeLayer()->name, QStringLiteral("Depth"));
        QCOMPARE(s.history().undoName(), QStringLiteral("Extract Depth"));
        QCOMPARE(s.document()->layers.size(), 2);
    }

    // aa22633 SelectionClipboardTests.pasteWithAFolderSelectedGoesAboveTheFolder.
    void pasteWithAFolderSelectedGoesAboveTheFolder()
    {
        EditorSession s; s.createDocument(40, 40);
        s.addGroup(); const QUuid outer = *s.document()->activeLayerId;
        s.addGroup(); const QUuid folder = *s.document()->activeLayerId;
        QVERIFY(s.insertPixelLayer(solid(10, 10, Qt::red), QPointF(), QString(), QStringLiteral("Paste"), true, true));
        const Layer pasted = *s.activeLayer();
        QCOMPARE(pasted.parentId, std::optional<QUuid>(outer));
        int folderIndex = -1, pastedIndex = -1;
        for (int i = 0; i < s.document()->layers.size(); ++i) { if (s.document()->layers[i].id == folder) folderIndex = i; if (s.document()->layers[i].id == pasted.id) pastedIndex = i; }
        QCOMPARE(pastedIndex, folderIndex + 1);
        // a587dcc: with a folder selected, a new shape or layer goes to the top inside it.
        s.selectLayer(folder);
        s.addBlankLayer();
        QCOMPARE(s.activeLayer()->parentId, std::optional<QUuid>(folder));
    }
    // 01e6505 LiveMaskTests.newLayerInsideAClippingGroupJoinsIt.
    void newLayerInsideAClippingGroupJoinsIt()
    {
        EditorSession s; s.createDocument(2, 2);
        s.insertImage(solid(2, 2, Qt::white), "Base"); const QUuid base = *s.document()->activeLayerId;
        s.insertImage(solid(2, 2, Qt::white), "Lower"); const QUuid lower = *s.document()->activeLayerId;
        QVERIFY(s.toggleClippingMask(lower));
        s.insertImage(solid(2, 2, Qt::white), "Upper"); const QUuid upper = *s.document()->activeLayerId;
        QVERIFY(s.toggleClippingMask(upper));
        s.selectLayer(lower); s.addBlankLayer(); QCOMPARE(s.activeLayer()->maskSourceId, std::optional<QUuid>(base));
        s.selectLayer(base); s.addBlankLayer(); QCOMPARE(s.activeLayer()->maskSourceId, std::optional<QUuid>(base));
        s.selectLayer(upper); s.addBlankLayer(); QVERIFY(!s.activeLayer()->maskSourceId);
    }
    // dc3a211 hidingTheBaseHidesItsClippingStack, 3990a25 its eye shows the stack again, af0dcb9 duplicating a base with
    // its clipped layer makes a second stack.
    void clippingStacksHideShowAndDuplicateTogether()
    {
        EditorSession s; s.createDocument(2, 2);
        s.insertImage(solid(2, 2, Qt::red), "Base"); s.insertImage(solid(2, 2, Qt::white), "A"); s.insertImage(solid(2, 2, Qt::white), "B");
        QVector<QUuid> ids; for (const Layer &l : s.document()->layers) ids << l.id;
        QVERIFY(s.linkMask(ids[0], ids[1]) && s.linkMask(ids[0], ids[2]));
        const auto visible = [&] { QSet<QUuid> v; for (const Layer &l : s.document()->layers) if (LayerRenderer::effectivelyVisible(*s.document(), l)) v.insert(l.id); return v; };
        s.toggleLayerVisibility(ids[0]);
        QVERIFY(visible().isEmpty());
        QCOMPARE(pixel(LayerRenderer::flattened(*s.document()), 0, 0)[3], 0);
        s.toggleLayerVisibility(ids[2]);
        QCOMPARE(visible().size(), 3);
        QCOMPARE(s.history().undoName(), QStringLiteral("Show Layers"));
        s.undo();
        QVERIFY(visible().isEmpty());
        s.toggleLayerVisibility(ids[0]);
        s.toggleLayerVisibility(ids[1]);
        QCOMPARE(visible(), (QSet<QUuid>{ids[0], ids[2]}));
        s.toggleLayerVisibility(ids[1]);
        EditorSession d; d.createDocument(2, 2);
        d.insertImage(solid(2, 2, Qt::red), "Base"); d.insertImage(solid(2, 2, Qt::white), "Clipped");
        const QUuid base = d.document()->layers[0].id, clipped = d.document()->layers[1].id;
        QVERIFY(d.linkMask(base, clipped));
        d.selectLayers({base, clipped}, clipped);
        d.duplicateActiveLayer();
        const auto layers = d.document()->layers;
        QCOMPARE(layers.size(), 4);
        QCOMPARE(layers[0].id, base); QCOMPARE(layers[1].id, clipped);
        QCOMPARE(layers[1].maskSourceId, std::optional<QUuid>(base));
        QVERIFY2(!layers[2].maskSourceId, "the base's copy is a base");
        QCOMPARE(layers[3].maskSourceId, std::optional<QUuid>(layers[2].id));
        d.undo();
        QCOMPARE(d.document()->layers.size(), 2);
    }
    // aef9f9b TransformTests.optionArrowDuplicatesAndNudgesTheCopy.
    void optionArrowDuplicatesAndNudgesTheCopy()
    {
        EditorSession s; s.createDocument(400, 200);
        s.insertImage(solid(40, 40, Qt::red), "Painted");
        const Layer original = *s.activeLayer();
        const int count = s.history().undoCount();
        QVERIFY(s.duplicateAndNudgeLayers(QPointF(10, 0)));
        QVERIFY(s.duplicateAndNudgeLayers(QPointF(0, -1)));
        QCOMPARE(s.document()->layers.size(), 3);
        QCOMPARE(s.document()->layers[0].transform, original.transform);
        QCOMPARE(s.activeLayer()->transform.origin, original.transform.origin + QPointF(10, -1));
        QCOMPARE(s.history().undoCount(), count + 2);
        s.undo(); s.undo();
        QCOMPARE(s.document()->layers.size(), 1);
        QCOMPARE(*s.document()->activeLayerId, original.id);
    }

    // 2facf90 ImageSourceTests: a document begun from an image goes back out the way it came.
    void exportStartsWithTheImagesNameAndFormat()
    {
        const QString png = QStringLiteral("/home/someone/Pictures/Sky Edit.png");
        QCOMPARE(ImageExporter::startingFormat(png, QString(), QStringLiteral("tiff")), QStringLiteral("png"));
        QCOMPARE(ImageExporter::startingFormat(QStringLiteral("/a/photo.JPG"), QString(), QStringLiteral("png")), QStringLiteral("jpg"));
        QCOMPARE(ImageExporter::startingFormat(QStringLiteral("/a/photo.webp"), QString(), QStringLiteral("tiff")), QStringLiteral("tiff"));
        QCOMPARE(ImageExporter::exportName(QString(), png, QStringLiteral("png")), QStringLiteral("Sky Edit.png"));
        QCOMPARE(ImageExporter::exportName(QString(), png, QStringLiteral("jpg")), QStringLiteral("Sky Edit.jpg"));
        const QString project = QStringLiteral("/home/someone/Projects/Poster.comp");
        QCOMPARE(ImageExporter::exportName(project, png, QStringLiteral("png")), QStringLiteral("Poster.png"));
        QCOMPARE(ImageExporter::startingFormat(png, project, QStringLiteral("gif")), QStringLiteral("gif"));
        QCOMPARE(ImageExporter::exportName(QString(), QString(), QStringLiteral("png")), QStringLiteral("Untitled.png"));
        QCOMPARE(ImageExporter::exportFolder(QString(), png), QStringLiteral("/home/someone/Pictures"));
    }
    void openingAnImageIntoAnEmptyWindowRemembersIt()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("Icon.png"));
        QVERIFY(solid(8, 8, Qt::red).save(path));
        MainWindow window;
        QVERIFY(window.importImageFiles({path}));
        QCOMPARE(window.session().sourceImagePath, QFileInfo(path).absoluteFilePath());
        QVERIFY(window.importImageFiles({path}));
        QCOMPARE(window.session().sourceImagePath, QFileInfo(path).absoluteFilePath());
        auto *tabs = window.findChild<QTabBar *>();
        QVERIFY(tabs && tabs->tabText(tabs->currentIndex()).startsWith(QStringLiteral("Icon")));
        // Export JPEG is gone; Export As takes its Ctrl+Alt+Shift+S (mac 2facf90).
        QVERIFY(!window.findChild<QAction *>(QStringLiteral("commandExportJpeg")));
        QCOMPARE(window.findChild<QAction *>(QStringLiteral("commandExportAs"))->shortcut(), QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_S));
    }

    // 8e6bca6 StrokeEffectTests: a stroke reaches the same distance every way round, and an outside stroke rounds a
    // square's corners while an inside one keeps them.
    void aCirclesStrokeIsEvenAllTheWayRound()
    {
        QImage disc(200, 200, QImage::Format_RGBA8888_Premultiplied); disc.fill(Qt::transparent);
        { QPainter p(&disc); p.setRenderHint(QPainter::Antialiasing); p.setPen(Qt::NoPen); p.setBrush(Qt::black); p.drawEllipse(QRectF(40, 40, 120, 120)); }
        for (const bool inside : {false, true}) {
            StrokeEffect stroke; stroke.size = 12; stroke.red = 1; stroke.inside = inside;
            const auto ring = LayerEffectsRenderer::strokeCoverage(disc, QRectF(0, 0, 200, 200), QSize(200, 200), stroke);
            for (const double angle : {0.0, M_PI / 4, M_PI / 2, 3 * M_PI / 4}) {
                double first = -1, last = -1;
                for (int step = 0; step <= 360; ++step) {
                    const double d = step * .25;
                    const int x = int(100 + std::cos(angle) * d), y = int(100 + std::sin(angle) * d);
                    if (ring[size_t(y) * 200 + x] > .5f) { if (first < 0) first = d; last = d; }
                }
                QVERIFY2(std::abs(first - (inside ? 48 : 60)) < 1.5 && std::abs(last - (inside ? 60 : 72)) < 1.5,
                         qPrintable(QStringLiteral("inside %1 at %2: %3–%4").arg(inside).arg(angle).arg(first).arg(last)));
            }
        }
        QImage square(100, 100, QImage::Format_RGBA8888_Premultiplied); square.fill(Qt::transparent);
        { QPainter p(&square); p.fillRect(30, 30, 40, 40, Qt::black); }
        StrokeEffect outside; outside.size = 10;
        const auto out = LayerEffectsRenderer::strokeCoverage(square, QRectF(0, 0, 100, 100), QSize(100, 100), outside);
        QVERIFY(out[50 * 100 + 25] > .5f && out[22 * 100 + 22] < .5f);
        StrokeEffect in = outside; in.inside = true;
        const auto inner = LayerEffectsRenderer::strokeCoverage(square, QRectF(0, 0, 100, 100), QSize(100, 100), in);
        QVERIFY(inner[30 * 100 + 30] > .5f && inner[69 * 100 + 69] > .5f);
    }

    // 79f98de FilterTests: with blur the split runs in two passes; past where the split alone reaches (8 pixels from
    // the square's edge, at 40.5) the blur's streak carries on, on both backends.
    void rgbSplitBlurStretchesPastTheSplit()
    {
        QImage square(61, 61, QImage::Format_RGBA8888_Premultiplied); square.fill(Qt::transparent);
        { QPainter p(&square); p.fillRect(28, 28, 5, 5, Qt::white); }
        const auto total = [](const QImage &image, int channel, int from, int to) {
            int sum = 0; for (int x = from; x <= to; ++x) sum += image.pixelColor(x, 30).toRgb().alpha() * (channel == 3) + qRed(image.pixel(x, 30)) * (channel == 0); return sum; };
        RgbSplitSettings flat; flat.distance = 8;
        RgbSplitSettings streaked; streaked.distance = 4; streaked.blur = 16;
        for (const RgbSplitBackend backend : {RgbSplitBackend::Cpu, RgbSplitBackend::Vulkan}) {
            const QImage a = flat.apply(square, 0, backend), b = streaked.apply(square, 0, backend);
            QVERIFY2(total(b, 3, 41, 44) > 0 && total(a, 3, 41, 44) == 0, "blur stretches it further");
            QVERIFY(total(a, 3, 34, 40) > 0);
        }
        QVERIFY(streaked.twoPassPad(QSize(61, 61)) == 6);
    }

    // Shape editing (3adce92, 82a9aeb, 8f8c721, 6dea5ac, 8997bfe, e63b825, a38cb04): ShapeToolTests.
    static EditorSession shapeSession()
    {
        EditorSession s; s.createDocument(100, 80); s.setForegroundColor(Qt::red); s.shapeToolActive = true; return s;
    }
    static QColor at(const EditorSession &s, int x, int y)
    {
        return LayerRenderer::flattened(*s.document()).pixelColor(x, y);
    }
    void anInsideStrokeRecolorsTheEdgeAndKeepsTheShape()
    {
        EditorSession s = shapeSession();
        QVERIFY(s.drawShape(ShapeKind::Rectangle, QRectF(10, 10, 40, 30)));
        LayerShapeStyle plain; plain.kind = ShapeKind::Rectangle; plain.red = 1;
        QCOMPARE(s.activeLayer()->image, EditorSession::shapeImage(plain, QSize(40, 30)));
        QVERIFY(!s.activeLayer()->shapeStyle->strokeWidth);
        s.shapeStrokeColor = Qt::black;
        const int count = s.history().undoCount();
        s.beginShapeStyleEdit();
        QVERIFY(s.setDisplayedStrokeWidth(4));
        QVERIFY(s.setDisplayedStrokeWidth(8));
        s.finishShapeStyleEdit();
        QCOMPARE(s.history().undoCount(), count + 1);
        QCOMPARE(s.history().undoName(), QStringLiteral("Shape Style"));
        QCOMPARE(s.foregroundColor(), QColor(Qt::red));
        QCOMPARE(s.shapeStrokeWidth, 0.0);
        QCOMPARE(*s.activeLayer()->shapeStyle->strokeWidth, 8.0);
        QCOMPARE(*s.activeLayer()->shapeStyle->strokeColor(), QColor(Qt::black));
        QCOMPARE(at(s, 14, 25).rgba(), qRgba(0, 0, 0, 255));
        QCOMPARE(at(s, 30, 25).rgba(), qRgba(255, 0, 0, 255));
        QCOMPARE(at(s, 9, 25).alpha(), 0);
        s.undo(); QVERIFY(!s.activeLayer()->shapeStyle->strokeWidth);
        s.redo(); QCOMPARE(*s.activeLayer()->shapeStyle->strokeWidth, 8.0);
        QVERIFY(s.setDisplayedCornerRadius(10));
        QCOMPARE(s.activeLayer()->shapeStyle->cornerRadius, 10.0);
        QCOMPARE(*s.activeLayer()->shapeStyle->strokeWidth, 8.0);
        s.undo(); QCOMPARE(s.activeLayer()->shapeStyle->cornerRadius, 0.0);
    }
    void anEllipseStrokeStaysInsideAndALineIgnoresTheStrokeWidth()
    {
        EditorSession s = shapeSession(); s.setForegroundColor(Qt::white);
        QVERIFY(s.drawShape(ShapeKind::Ellipse, QRectF(30, 20, 40, 40)));
        s.shapeStrokeColor = Qt::blue;
        QVERIFY(s.setDisplayedStrokeWidth(8));
        QCOMPARE(at(s, 50, 40).rgba(), qRgba(255, 255, 255, 255));
        QVERIFY(at(s, 50, 24).red() == 0 && at(s, 50, 24).alpha() == 255);
        QCOMPARE(at(s, 30, 20).alpha(), 0);
        s.shapeLineWidth = 4;
        QVERIFY(s.drawShape(ShapeKind::Line, QRectF(), QPointF(10, 10), QPointF(90, 10)));
        QCOMPARE(*s.activeLayer()->shapeStyle->lineWidth, 4.0);
        QVERIFY(!s.activeLayer()->shapeStyle->strokeWidth);
    }
    void theNextShapeCopiesTheBar()
    {
        EditorSession s = shapeSession();
        QVERIFY(s.drawShape(ShapeKind::Rectangle, QRectF(10, 10, 30, 20)));
        s.shapeStrokeColor = Qt::black;
        s.setDisplayedStrokeWidth(6); s.setDisplayedCornerRadius(4);
        QVERIFY(s.drawShape(ShapeKind::Rectangle, QRectF(50, 10, 30, 20)));
        const LayerShapeStyle style = *s.activeLayer()->shapeStyle;
        QVERIFY(style.strokeWidth == 6.0 && style.cornerRadius == 4 && style.color() == QColor(Qt::red));
        QCOMPARE(s.document()->formatVersion, 22);
        // 6dea5ac: an unstroked rectangle shows 0 even when the next-shape default is wider, and one drawn from it stays
        // unstroked.
        s.selectLayer(s.document()->layers.first().id);
        s.shapeStrokeWidth = 8;
        s.setDisplayedStrokeWidth(0);
        QCOMPARE(s.displayedStrokeWidth(), 0.0);
        QVERIFY(s.drawShape(ShapeKind::Rectangle, QRectF(10, 50, 20, 20)));
        QVERIFY(!s.activeLayer()->shapeStyle->strokeWidth);
    }
    void fillRecolorsALiveShapeAndASelectionRasterizesIt()
    {
        EditorSession s = shapeSession();
        QVERIFY(s.drawShape(ShapeKind::Rectangle, QRectF(10, 10, 40, 30)));
        s.shapeStrokeColor = Qt::black; s.setDisplayedStrokeWidth(6);
        QVERIFY(s.fillSelection(Qt::blue));
        const LayerShapeStyle style = *s.activeLayer()->shapeStyle;
        QVERIFY(style.blue == 1 && style.red == 0 && style.strokeWidth == 6.0);
        QCOMPARE(s.activeLayer()->image, EditorSession::shapeImage(style, s.activeLayer()->image.size()));
        QCOMPARE(s.history().undoName(), QStringLiteral("Fill"));
        s.undo(); QCOMPARE(s.activeLayer()->shapeStyle->red, 1.0);
        s.selectAll();
        QVERIFY(s.fillSelection(Qt::white));
        QVERIFY(!s.activeLayer()->shapeStyle);
    }
    void theColorPickerPreviewsAndCancelRestoresTheFill()
    {
        EditorSession s = shapeSession();
        QVERIFY(s.drawShape(ShapeKind::Rectangle, QRectF(10, 10, 40, 30)));
        const int count = s.history().undoCount();
        s.previewShapeColor(Qt::green, true);
        QCOMPARE(at(s, 30, 25).rgba(), qRgba(0, 255, 0, 255));
        QCOMPARE(s.foregroundColor(), QColor(Qt::red));
        s.cancelShapeColorPreview();
        QCOMPARE(s.history().undoCount(), count);
        QCOMPARE(s.activeLayer()->shapeStyle->red, 1.0);
        QCOMPARE(at(s, 30, 25).rgba(), qRgba(255, 0, 0, 255));
        s.previewShapeColor(Qt::green, true);
        s.cancelShapeColorPreview();
        QVERIFY(s.pickShapeFill(Qt::green));
        QCOMPARE(s.activeLayer()->shapeStyle->green, 1.0);
        QCOMPARE(s.foregroundColor(), QColor(Qt::red));
        QCOMPARE(s.history().undoCount(), count + 1);
        s.undo(); QCOMPARE(s.activeLayer()->shapeStyle->red, 1.0);
    }
    void strokeSurvivesSavingAndScaling()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        EditorSession s = shapeSession();
        s.shapeStrokeWidth = 5; s.shapeStrokeColor = Qt::blue; s.shapeCornerRadius = 6;
        QVERIFY(s.drawShape(ShapeKind::Rectangle, QRectF(10, 10, 40, 30)));
        const QString path = dir.filePath(QStringLiteral("Stroke.comp"));
        ProjectWriter::save(*s.document(), path);
        EditorSession reopened; QVERIFY(reopened.openProject(path));
        QCOMPARE(reopened.document()->formatVersion, 22);
        const LayerShapeStyle style = *reopened.document()->layers.last().shapeStyle;
        QVERIFY(style.strokeWidth == 5.0 && style.strokeBlue == 1 && style.cornerRadius == 6);
        reopened.selectLayer(reopened.document()->layers.last().id);
        reopened.document()->layers.last().transform.size = QSizeF(80, 50);
        reopened.redrawSelectedShapes();
        QCOMPARE(reopened.document()->layers.last().image, EditorSession::shapeImage(style, QSize(80, 50)));
    }
    void aStrokedEdgeHasNoFringeOfTheFill()
    {
        LayerShapeStyle style; style.kind = ShapeKind::Ellipse; style.red = style.green = style.blue = 1; style.setStroke(4, Qt::black);
        const QImage image = EditorSession::shapeImage(style, QSize(40, 40)).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        int rim = 0;
        for (int y = 0; y < 40; ++y) for (int x = 0; x < 40; ++x) {
            if (std::hypot(x + .5 - 20, y + .5 - 20) <= 19) continue;
            const uchar *p = image.constScanLine(y) + x * 4;
            if (p[3] == 0 || p[3] == 255) continue;
            ++rim;
            QVERIFY2(p[0] == 0, qPrintable(QStringLiteral("white fill in the black rim at %1, %2").arg(x).arg(y)));
        }
        QVERIFY(rim > 20);
        QCOMPARE(int(image.constScanLine(20)[20 * 4 + 3]), 255);
    }
    void editingASelectedLineChangesItsColorAndWidth()
    {
        EditorSession s = shapeSession();
        s.shapeLineWidth = 4;
        QVERIFY(s.drawShape(ShapeKind::Line, QRectF(), QPointF(10, 20), QPointF(90, 20)));
        QVERIFY(s.editingLineLayer());
        QVERIFY(s.pickShapeFill(Qt::green));
        QCOMPARE(s.activeLayer()->shapeStyle->green, 1.0);
        QCOMPARE(s.foregroundColor(), QColor(Qt::red));
        const int count = s.history().undoCount();
        s.beginShapeStyleEdit(); s.setDisplayedLineWidth(10); s.setDisplayedLineWidth(12); s.finishShapeStyleEdit();
        QCOMPARE(s.history().undoCount(), count + 1);
        const Layer layer = *s.activeLayer();
        QCOMPARE(*layer.shapeStyle->lineWidth, 12.0);
        QVERIFY2(std::abs(layer.transform.origin.y() + layer.transform.size.height() / 2 - 20) < .51, "grows around the same row");
        QCOMPARE(at(s, 50, 20).rgba(), qRgba(0, 255, 0, 255));
        QCOMPARE(at(s, 50, 15).alpha(), 255);
        QCOMPARE(at(s, 50, 13).alpha(), 0);
        s.undo();
        QCOMPARE(*s.activeLayer()->shapeStyle->lineWidth, 4.0);
        QCOMPARE(s.activeLayer()->shapeStyle->green, 1.0);
    }
    void aNewLineUsesTheWidthAndColorChosenBeforeItIsDrawn()
    {
        EditorSession s = shapeSession();
        QVERIFY(s.drawShape(ShapeKind::Rectangle, QRectF(10, 10, 30, 20)));
        const QUuid rectangle = *s.document()->activeLayerId;
        QVERIFY(s.setDisplayedLineWidth(16));
        s.setForegroundColor(Qt::blue);
        QCOMPARE(s.shapeLineWidth, 16.0);
        QVERIFY(!s.document()->layers.first().shapeStyle->strokeWidth);
        QVERIFY(s.drawShape(ShapeKind::Line, QRectF(), QPointF(10, 50), QPointF(70, 50)));
        QVERIFY(*s.document()->activeLayerId != rectangle);
        QCOMPARE(*s.activeLayer()->shapeStyle->lineWidth, 16.0);
        QCOMPARE(s.activeLayer()->shapeStyle->blue, 1.0);
        QCOMPARE(s.editingLineLayer()->id, *s.document()->activeLayerId);
        QVERIFY(s.setDisplayedLineWidth(8));
        QCOMPARE(*s.activeLayer()->shapeStyle->lineWidth, 8.0);
    }
    void theNextShapeIsTheForegroundColorOrTheFillJustPicked()
    {
        EditorSession s = shapeSession(); s.setBackgroundColor(Qt::white);
        QVERIFY(s.drawShape(ShapeKind::Rectangle, QRectF(10, 10, 30, 20)));
        const QUuid first = *s.document()->activeLayerId;
        s.swapPaletteColors();
        QCOMPARE(s.activeLayer()->shapeStyle->color(), QColor(Qt::red));
        QCOMPARE(s.displayedShapeFill(), QColor(Qt::white));
        QVERIFY(s.drawShape(ShapeKind::Rectangle, QRectF(50, 10, 30, 20)));
        QCOMPARE(s.activeLayer()->shapeStyle->color(), QColor(Qt::white));
        QVERIFY(s.pickShapeFill(Qt::blue));
        QVERIFY(s.activeLayer()->shapeStyle->color() == QColor(Qt::blue) && s.foregroundColor() == QColor(Qt::white));
        QVERIFY(s.drawShape(ShapeKind::Rectangle, QRectF(10, 40, 30, 30)));
        QCOMPARE(s.activeLayer()->shapeStyle->color(), QColor(Qt::blue));
        s.swapPaletteColors();
        QCOMPARE(s.displayedShapeFill(), QColor(Qt::red));
        QVERIFY(s.drawShape(ShapeKind::Rectangle, QRectF(50, 40, 30, 30)));
        QCOMPARE(s.activeLayer()->shapeStyle->color(), QColor(Qt::red));
        QCOMPARE(s.document()->layers.first().shapeStyle->color(), QColor(Qt::red));
        QCOMPARE(s.document()->layers.first().id, first);
    }
    void theShapeToolAdoptsTheSelectedShape()
    {
        MainWindow window; window.session().createDocument(100, 80);
        window.session().shapeToolActive = true;
        window.session().shapeLineWidth = 4;
        QVERIFY(window.session().drawShape(ShapeKind::Line, QRectF(), QPointF(10, 20), QPointF(80, 20)));
        const QUuid line = *window.session().document()->activeLayerId;
        QVERIFY(window.session().drawShape(ShapeKind::Ellipse, QRectF(10, 40, 20, 20)));
        const QUuid ellipse = *window.session().document()->activeLayerId;
        window.syncDocumentViews();
        auto *canvas = window.findChild<CanvasWidget *>();
        canvas->setTool(CanvasWidget::Tool::Move);
        canvas->setShapeKind(ShapeKind::Rectangle);
        window.session().selectLayer(line); window.syncDocumentViews();
        canvas->setTool(CanvasWidget::Tool::Shape);
        QCOMPARE(canvas->shapeKind(), ShapeKind::Line);
        window.session().selectLayer(ellipse); window.syncDocumentViews();
        QCOMPARE(canvas->shapeKind(), ShapeKind::Ellipse);
        window.session().addBlankLayer(); window.syncDocumentViews();
        QCOMPARE(canvas->shapeKind(), ShapeKind::Ellipse);
    }

    // a0cc980 PSDExportTests: background, a folder holding a masked Multiply layer and a layer clipped to it, a hidden
    // layer and a Levels adjustment on top.
    static EditorSession psdDocument()
    {
        EditorSession s; s.createDocument(120, 80);
        s.insertImage(solid(120, 80, QColor::fromRgbF(.2f, .4f, .9f)), QStringLiteral("Sky"), QPointF(60, 40));
        s.insertImage(solid(40, 30, Qt::red), QStringLiteral("Red"), QPointF(30, 35));
        s.insertImage(solid(40, 30, Qt::green), QStringLiteral("Green"), QPointF(50, 45));
        s.insertImage(solid(10, 10, Qt::yellow), QStringLiteral("Hidden"), QPointF(105, 10));
        auto &layers = s.document()->layers;
        Layer &red = layers[1];
        red.opacity = .5; red.blendMode = BlendMode::Multiply;
        QImage mask(40, 30, QImage::Format_Grayscale8); mask.fill(0);
        for (int y = 0; y < 30; ++y) for (int x = 20; x < 40; ++x) mask.scanLine(y)[x] = 255;
        red.mask = mask;
        layers[2].maskSourceId = red.id;
        layers[3].visible = false;
        const QUuid redId = red.id, greenId = layers[2].id;
        s.selectLayer(layers.last().id);
        s.addAdjustment(QStringLiteral("Levels"));
        s.document()->layers.last().name = QStringLiteral("Levels");
        s.selectLayers({redId, greenId}, greenId);
        s.groupSelectedLayers();
        return s;
    }
    void layersFoldersMasksAndClippingComeBackFromTheFile()
    {
        EditorSession s = psdDocument();
        QString error;
        const QByteArray file = PSDWriter::write(*s.document(), &error);
        QVERIFY2(!file.isEmpty(), qPrintable(error));
        PSDImportResult parsed;
        QVERIFY2(PSDReader::read(file, parsed, &error), qPrintable(error));
        QCOMPARE(parsed.document.canvasSize, QSize(120, 80));
        QStringList names; for (const Layer &l : parsed.document.layers) names << l.name;
        QCOMPARE(names, (QStringList{"Sky", "Red", "Green", "Folder 1", "Hidden"}));
        QHash<QString, Layer> byName; for (const Layer &l : parsed.document.layers) byName.insert(l.name, l);
        const Layer folder = byName.value("Folder 1");
        QVERIFY(folder.group && byName.value("Red").parentId == folder.id && byName.value("Green").parentId == folder.id);
        const Layer red = byName.value("Red");
        QCOMPARE(red.transform.origin, QPointF(10, 20)); QCOMPARE(red.transform.size, QSizeF(40, 30));
        QVERIFY(std::abs(red.opacity - .5) < .01 && red.blendMode == BlendMode::Multiply);
        QVERIFY(!red.mask.isNull());
        QCOMPARE(byName.value("Green").maskSourceId, std::optional<QUuid>(red.id));
        QVERIFY(!red.maskSourceId);
        QVERIFY(!byName.value("Hidden").visible && byName.value("Sky").visible);
        QCOMPARE(byName.value("Green").image.pixelColor(5, 5).rgba(), qRgba(0, 255, 0, 255));
        QVERIFY2(parsed.conversions.isEmpty(), qPrintable(parsed.conversions.isEmpty() ? QString() : parsed.conversions.first().message));
        QCOMPARE(PSDWriter::summary(PSDWriter::notes(*s.document())), QStringLiteral("Adjustment layers left out: “Levels”"));
        // Other apps see the flattened picture: the file's composite matches the document's.
        const QImage theirs = psdComposite(file);
        QVERIFY(!theirs.isNull());
        const QImage ours = LayerRenderer::flattened(*s.document()).convertToFormat(QImage::Format_RGBA8888);
        int largest = 0;
        for (int y = 0; y < 80; ++y) for (int x = 0; x < 120; ++x) for (int c = 0; c < 3; ++c)
            if (ours.constScanLine(y)[x * 4 + 3]) largest = std::max(largest, std::abs(int(ours.constScanLine(y)[x * 4 + c]) - int(theirs.constScanLine(y)[x * 4 + c])));
        QVERIFY2(largest <= 2, qPrintable(QString::number(largest)));
    }
    // The flattened picture at the end of a Photoshop file, as an app that doesn't read layers sees it: straight RGBA,
    // 8 bits (PackBits) or 16 (raw, kept in RGBA64).
    static QImage psdComposite(const QByteArray &file)
    {
        const auto u16 = [&](qsizetype at) { return (int(uchar(file[at])) << 8) | uchar(file[at + 1]); };
        const auto u32 = [&](qsizetype at) { return (qint64(u16(at)) << 16) | u16(at + 2); };
        const int height = int(u32(14)), width = int(u32(18)), depth = u16(22);
        qsizetype at = 26;
        for (int section = 0; section < 3; ++section) at += 4 + u32(at);
        const int compression = u16(at); at += 2;
        QImage image(width, height, depth == 16 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
        if (depth == 16 && compression == 0) {
            for (int c = 0; c < 4; ++c) for (int y = 0; y < height; ++y) {
                auto *line = reinterpret_cast<quint16 *>(image.scanLine(y));
                for (int x = 0; x < width; ++x, at += 2) line[x * 4 + c] = quint16(u16(at));
            }
            return image;
        }
        if (depth != 8 || compression != 1) return {};
        QVector<int> counts; for (int i = 0; i < 4 * height; ++i, at += 2) counts << u16(at);
        for (int c = 0; c < 4; ++c) for (int y = 0; y < height; ++y) {
            const qsizetype end = at + counts[c * height + y];
            uchar *line = image.scanLine(y); int x = 0;
            while (at < end) {
                const int header = qint8(file[at++]);
                if (header >= 0) { for (int i = 0; i <= header; ++i) line[(x++) * 4 + c] = uchar(file[at++]); }
                else { const uchar v = uchar(file[at++]); for (int i = 0; i < 1 - header; ++i) line[(x++) * 4 + c] = v; }
            }
        }
        return image;
    }
    void aSixteenBitDocumentGoesOutAtSixteenBits()
    {
        EditorSession s; s.createDocument(64, 32);
        QImage ramp(64, 32, QImage::Format_RGBA64);
        for (int y = 0; y < 32; ++y) { auto *p = reinterpret_cast<quint16 *>(ramp.scanLine(y)); for (int x = 0; x < 64; ++x) { p[x * 4] = quint16(x * 1000); p[x * 4 + 1] = 20000; p[x * 4 + 2] = 40000; p[x * 4 + 3] = 65535; } }
        s.insertImage(ramp, QStringLiteral("Ramp"), QPointF(32, 16));
        QVERIFY(PSDWriter::sixteenBit(*s.document()));
        const QByteArray file = PSDWriter::write(*s.document());
        QCOMPARE(int(uchar(file[22])), 0); QCOMPARE(int(uchar(file[23])), 16);
        // Red at x = 3 is 3000 of 65535, which 8 bits would round to 2827 or 3084. (LX's own importer reads 8-bit files
        // only, so the composite is read here as an app would.)
        const QImage back = psdComposite(file);
        QVERIFY(!back.isNull());
        const int red = reinterpret_cast<const quint16 *>(back.constScanLine(0))[3 * 4];
        QVERIFY2(std::abs(red - 3000) < 40, qPrintable(QString::number(red)));
    }
    void packBitsRoundTrips()
    {
        QVector<QByteArray> rows{QByteArray(), QByteArray(1, 7), QByteArray("\x01\x01\x01\x01\x02\x03\x03\x04", 8), QByteArray(300, 9)};
        QByteArray ramp; for (int i = 0; i < 300; ++i) ramp.append(char(i % 251)); rows << ramp;
        for (const QByteArray &row : rows) {
            const QByteArray packed = PSDWriter::packBits(reinterpret_cast<const uchar *>(row.constData()), row.size());
            QByteArray out; int index = 0;
            while (index < packed.size()) {
                const int header = qint8(packed[index++]);
                if (header >= 0) { out.append(packed.mid(index, header + 1)); index += header + 1; }
                else { out.append(QByteArray(1 - header, packed[index])); ++index; }
            }
            QCOMPARE(out, row);
        }
    }

    // 1f1d84c PencilTests: the Brush with Anti-alias off is a pencil, every pixel it paints fully painted, whatever
    // Hardness was left at; with it on, a hard brush has the anti-aliased rim macOS's has.
    static QVector<int> brushAlphas(bool antialiased, double hardness, bool erasing = false)
    {
        EditorSession s; s.createDocument(120, 120); s.addBlankLayer();
        if (erasing) { s.fillSelection(Qt::blue); }
        s.brushAntialiased = antialiased;
        if (!s.beginBrushStroke(QPointF(20, 25), Qt::red, 17, hardness, 1, erasing)) return {};
        s.continueBrushStroke(QPointF(95, 70));
        s.endBrushStroke();
        const QImage img = s.activeLayer()->image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        QVector<int> values;
        for (int y = 0; y < img.height(); ++y) for (int x = 0; x < img.width(); ++x) values << img.constScanLine(y)[x * 4 + 3];
        return values;
    }
    void aPencilStrokeHasNoSoftPixels()
    {
        const QVector<int> pencil = brushAlphas(false, .3);
        QVERIFY(pencil.contains(255));
        QVERIFY(std::all_of(pencil.cbegin(), pencil.cend(), [](int a) { return a == 0 || a == 255; }));
        const QVector<int> brush = brushAlphas(true, .3);
        QVERIFY(std::any_of(brush.cbegin(), brush.cend(), [](int a) { return a > 0 && a < 255; }));
        // A hard brush is anti-aliased too, as Core Graphics fills its dabs; only the pencil is aliased.
        const QVector<int> hard = brushAlphas(true, 1);
        QVERIFY(std::any_of(hard.cbegin(), hard.cend(), [](int a) { return a > 0 && a < 255; }));
        // The pencil erases with hard edges too.
        const QVector<int> erased = brushAlphas(false, .3, true);
        QVERIFY(erased.contains(0) && std::all_of(erased.cbegin(), erased.cend(), [](int a) { return a == 0 || a == 255; }));
    }

    // de6837b ShapeToolTests.whatsBeingDraggedOutShowsItsSize, 1315aae the angle while rotating.
    void whatsBeingDraggedOutShowsItsSize()
    {
        MainWindow window; window.resize(900, 700); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.session().createDocument(200, 150, true);
        window.syncDocumentViews();
        CanvasWidget *canvas = window.canvas();
        canvas->setZoom(2);
        const auto at = [&](double x, double y) { return (canvas->canvasRect().topLeft() + QPointF(x, y) * canvas->zoom()).toPoint(); };
        canvas->setTool(CanvasWidget::Tool::Shape);
        canvas->setShapeKind(ShapeKind::Rectangle);
        QTest::mousePress(canvas, Qt::LeftButton, {}, at(31, 33));
        QTest::mouseMove(canvas, at(61, 53));
        QVERIFY(canvas->dragOutSize());
        QCOMPARE(canvas->dragOutSize()->text, QStringLiteral("30 × 20 px"));
        QVERIFY(QLineF(canvas->dragOutSize()->corner, QPointF(61, 53)).length() < 1);
        QVERIFY(window.findChild<QLabel *>(QStringLiteral("statusDimensions"))->text().contains(QStringLiteral("Shape 30 × 20 px")));
        QTest::mouseRelease(canvas, Qt::LeftButton, {}, at(61, 53));
        QVERIFY(!canvas->dragOutSize());
        canvas->setShapeKind(ShapeKind::Line);
        QTest::mousePress(canvas, Qt::LeftButton, {}, at(31, 33));
        QTest::mouseMove(canvas, at(61, 73));
        // 50 px, give or take the widget-to-document rounding of the pointer.
        QVERIFY2(std::abs(canvas->dragOutSize()->text.section(QLatin1Char(' '), 0, 0).toInt() - 50) <= 1, qPrintable(canvas->dragOutSize()->text));
        QTest::mouseRelease(canvas, Qt::LeftButton, {}, at(61, 73));
        canvas->setTool(CanvasWidget::Tool::Marquee);
        // Away from the shapes above, the canvas's middle and its edges, which a Marquee's corners snap to.
        QTest::mousePress(canvas, Qt::LeftButton, {}, at(177, 137));
        QTest::mouseMove(canvas, at(137, 97));
        QCOMPARE(canvas->dragOutSize()->text, QStringLiteral("40 × 40 px"));
        QVERIFY2(QLineF(canvas->dragOutSize()->corner, QPointF(137, 97)).length() < 1, qPrintable(QStringLiteral("%1,%2").arg(canvas->dragOutSize()->corner.x()).arg(canvas->dragOutSize()->corner.y())));
        QTest::mouseRelease(canvas, Qt::LeftButton, {}, at(137, 97));
    }

    // 90e7948 rotate from just outside any corner (no handle above the box); 1315aae the angle beside the pointer.
    void rotatesFromJustOutsideACorner()
    {
        MainWindow window; window.resize(900, 700); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.session().createDocument(200, 150, true);
        window.session().insertImage(solid(60, 40, Qt::red), QStringLiteral("Red"), QPointF(100, 75));
        window.syncDocumentViews();
        CanvasWidget *canvas = window.canvas();
        canvas->setZoom(2);
        canvas->setTool(CanvasWidget::Tool::Move);
        const auto at = [&](QPointF document, QPointF screenOffset = {}) { return (canvas->canvasRect().topLeft() + document * canvas->zoom() + screenOffset).toPoint(); };
        const QUuid id = *window.session().document()->activeLayerId;
        const auto rotation = [&] { for (const Layer &l : window.session().document()->layers) if (l.id == id) return l.transform.rotation; return -1.0; };
        // Where the old handle was, above the top edge's middle, nothing turns.
        QTest::mousePress(canvas, Qt::LeftButton, {}, at(QPointF(100, 55), QPointF(0, -28)));
        QTest::mouseMove(canvas, at(QPointF(140, 55), QPointF(0, -28)));
        QTest::mouseRelease(canvas, Qt::LeftButton, {}, at(QPointF(140, 55), QPointF(0, -28)));
        QCOMPARE(rotation(), 0.0);
        if (window.session().canUndo()) window.session().undo();
        window.syncDocumentViews();
        // Just outside the top-right corner, it turns, and the angle shows beside the pointer.
        QTest::mousePress(canvas, Qt::LeftButton, {}, at(QPointF(130, 55), QPointF(12, -12)));
        QTest::mouseMove(canvas, at(QPointF(150, 85)));
        QVERIFY(std::abs(rotation()) > 1);
        QVERIFY(canvas->dragOutSize() && canvas->dragOutSize()->text.endsWith(QStringLiteral("°")));
        QTest::mouseRelease(canvas, Qt::LeftButton, {}, at(QPointF(150, 85)));
        QVERIFY(!canvas->dragOutSize());
        QVERIFY(std::abs(rotation()) > 1);
    }

    // 8e2659d: an Alt-drag in the Layers panel always duplicates, whatever drop action Qt settles on.
    void anAltDragInTheLayersPanelDuplicates()
    {
        MainWindow window;
        window.session().createDocument(40, 40);
        window.session().insertImage(solid(10, 10, Qt::red), QStringLiteral("A"));
        window.session().insertImage(solid(10, 10, Qt::blue), QStringLiteral("B"));
        window.syncDocumentViews();
        LayerListModel *model = window.layerModel();
        QSignalSpy spy(model, &LayerListModel::layersDropRequested);
        std::unique_ptr<QMimeData> mime(model->mimeData({model->index(1, 0)}));
        QVERIFY(mime);
        model->setDropCopies(true);
        QVERIFY(model->dropMimeData(mime.get(), Qt::MoveAction, 0, 0, QModelIndex()));
        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.last().last().toBool());
        // The flag is for one drop only.
        QVERIFY(model->dropMimeData(mime.get(), Qt::MoveAction, 0, 0, QModelIndex()));
        QVERIFY(!spy.last().last().toBool());
    }

    // 60a4f9c ColorPickerTests: a hex value being typed reads as it stands; the palette's swatches sample into an open
    // picker, leaving the palette alone until OK.
    void aHexValueBeingTypedReadsAsItStands()
    {
        QCOMPARE(*palette::fromTypedHex("F"), QColor(0xF0, 0, 0));
        QCOMPARE(*palette::fromTypedHex("#FF8"), QColor(0xFF, 0x80, 0));
        QCOMPARE(*palette::fromTypedHex("FF800"), QColor(0xFF, 0x80, 0));
        QCOMPARE(*palette::fromTypedHex("12AB34"), QColor(0x12, 0xAB, 0x34));
        QVERIFY(!palette::fromTypedHex(""));
        QVERIFY(!palette::fromTypedHex("FG"));
        QVERIFY(!palette::fromTypedHex("1234567"));
        ColorPickerDialog picker(Qt::black);
        auto *hex = picker.findChild<QLineEdit *>(QStringLiteral("colorPickerHex"));
        hex->setFocus(); hex->clear();
        QTest::keyClicks(hex, QStringLiteral("FF8"));
        QCOMPARE(picker.currentColor(), QColor(0xFF, 0x80, 0));
    }
    void thePalettesSwatchesSampleIntoAnOpenPicker()
    {
        MainWindow window; window.resize(900, 700); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.session().createDocument(20, 20);
        window.session().setForegroundColor(Qt::red); window.session().setBackgroundColor(Qt::blue);
        window.syncDocumentViews();
        auto *foreground = window.findChild<QLabel *>(QStringLiteral("foregroundSwatch"));
        auto *background = window.findChild<QLabel *>(QStringLiteral("backgroundSwatch"));
        QVERIFY(foreground && background);
        QTest::mouseClick(background, Qt::LeftButton);
        auto *picker = window.findChild<ColorPickerDialog *>(QStringLiteral("paletteColorPicker"));
        QVERIFY(picker);
        QTest::mouseClick(foreground, Qt::LeftButton);
        QCOMPARE(picker->currentColor(), QColor(Qt::red));
        QTest::mouseClick(background, Qt::LeftButton);
        QCOMPARE(picker->currentColor(), QColor(Qt::blue));
        QCOMPARE(window.session().backgroundColor(), QColor(Qt::blue));
        QCOMPARE(window.session().foregroundColor(), QColor(Qt::red));
        QVERIFY(picker->windowTitle().contains(QStringLiteral("Background")));
        picker->reject();
    }

    // acfc5eb BrushRecoveryTests: a 3000 × 3000 canvas with a gray 1371 × 835 layer shown at 27.8%, as in the report.
    static EditorSession scaledDownLayer()
    {
        EditorSession s; s.createDocument(3000, 3000);
        QImage image(1371, 835, QImage::Format_RGBA8888_Premultiplied); image.fill(QColor(128, 128, 128));
        { QPainter p(&image); p.fillRect(0, 735, 1371, 100, Qt::white); } // a white band along one edge
        s.insertImage(image, QStringLiteral("Painted"));
        s.activeLayer()->transform.origin = QPointF(1300, 1400);
        s.activeLayer()->transform.size = QSizeF(381, 232);
        return s;
    }
    static QColor sampleLayer(const Layer &layer, QPointF point)
    {
        const QImage image = layer.image.convertToFormat(QImage::Format_RGBA8888);
        const int x = int((point.x() - layer.transform.origin.x()) / layer.transform.size.width() * image.width());
        const int y = int((point.y() - layer.transform.origin.y()) / layer.transform.size.height() * image.height());
        return image.pixelColor(x, y);
    }
    void paintingFarPastAScaledDownLayerPaintsItAtItsDisplayedSize()
    {
        EditorSession s = scaledDownLayer();
        const QPointF edges[] = {QPointF(1320, 1410), QPointF(1320, 1625)};
        const int before[] = {sampleLayer(*s.activeLayer(), edges[0]).red(), sampleLayer(*s.activeLayer(), edges[1]).red()};
        QVERIFY(before[0] != before[1]);
        QVERIFY(s.beginBrushStroke(QPointF(400, 400), Qt::black, 258, .59, 1));
        for (int step = 1; step <= 60; ++step) s.continueBrushStroke(QPointF(400 + step * 40, 400 + step * 35));
        QVERIFY(s.endBrushStroke());
        const Layer layer = *s.activeLayer();
        // One pixel a document pixel, not the 81 megapixels it would grow to at 27.8%.
        QVERIFY2(std::abs(layer.image.width() - layer.transform.size.width()) <= 2, qPrintable(QStringLiteral("%1 wide for %2").arg(layer.image.width()).arg(layer.transform.size.width())));
        QVERIFY(sampleLayer(layer, QPointF(400, 400)).alpha() > 200);
        for (int i = 0; i < 2; ++i) QVERIFY(std::abs(sampleLayer(layer, edges[i]).red() - before[i]) < 12);
        s.undo();
        QCOMPARE(s.activeLayer()->image.width(), 1371);
        QCOMPARE(s.activeLayer()->transform.size.width(), 381.0);
    }
    void aStrokeThatLeavesAScaledDownLayerPartwayIsPaintedWhole()
    {
        EditorSession s = scaledDownLayer();
        const QPointF start(1490, 1516), end(400, 400);
        QVERIFY(s.beginBrushStroke(start, Qt::black, 60, 1, 1));
        for (int step = 1; step <= 40; ++step) s.continueBrushStroke(start + (end - start) * (step / 40.0));
        QVERIFY(s.endBrushStroke());
        const Layer layer = *s.activeLayer();
        QVERIFY(std::abs(layer.image.width() - layer.transform.size.width()) <= 2);
        // Painted from its start, inside the layer, to its end, outside it.
        QVERIFY(sampleLayer(layer, start).red() < 40);
        QVERIFY(sampleLayer(layer, end).alpha() > 200);
        // Painting within a scaled-down layer keeps its resolution.
        EditorSession inside = scaledDownLayer();
        QVERIFY(inside.beginBrushStroke(QPointF(1450, 1500), Qt::black, 20, 1, 1));
        inside.continueBrushStroke(QPointF(1500, 1520));
        QVERIFY(inside.endBrushStroke());
        QCOMPARE(inside.activeLayer()->image.width(), 1371);
    }

    // 8fb163e LayerDistancesTests: the Move tool's pixel gaps, to the canvas's edges and between layers.
    static QStringList gaps(const QVector<DistanceLine> &lines)
    {
        QStringList out;
        for (const DistanceLine &l : lines) out << QStringLiteral("%1 %2–%3").arg(l.horizontal ? "x" : "y").arg(int(l.from)).arg(int(l.to));
        out.sort();
        return out;
    }
    void layerDistancesMeasureGaps()
    {
        QCOMPARE(gaps(LayerDistances::moving(QRectF(100, 50, 300, 200), {}, QSizeF(1000, 800))),
                 (QStringList{"x 0–100", "x 400–1000", "y 0–50", "y 250–800"}));
        const QVector<QRectF> others{QRectF(700, 100, 100, 100), QRectF(500, 600, 50, 50), QRectF(0, 0, 1000, 800)};
        const auto lines = LayerDistances::moving(QRectF(100, 50, 300, 200), others, QSizeF(1000, 800));
        QCOMPARE(gaps(lines), (QStringList{"x 0–100", "x 400–700", "y 0–50", "y 250–800"}));
        for (const DistanceLine &l : lines) if (l.horizontal && l.from == 400) QCOMPARE(l.across, 150.0);
        QCOMPARE(gaps(LayerDistances::between(QRectF(100, 100, 300, 100), QRectF(700, 150, 100, 100))), QStringList{"x 400–700"});
        QCOMPARE(gaps(LayerDistances::between(QRectF(100, 50, 300, 200), QRectF(0, 0, 1000, 800))),
                 (QStringList{"x 0–100", "x 400–1000", "y 0–50", "y 250–800"}));
    }
    void distancesShowWhileMovingALayerWhenTurnedOn()
    {
        MainWindow window; window.resize(900, 700); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.session().createDocument(200, 150, true);
        window.session().insertImage(solid(40, 30, Qt::red), QStringLiteral("Red"), QPointF(100, 75));
        window.syncDocumentViews();
        CanvasWidget *canvas = window.canvas();
        canvas->setZoom(2);
        canvas->setTool(CanvasWidget::Tool::Move);
        const auto at = [&](double x, double y) { return (canvas->canvasRect().topLeft() + QPointF(x, y) * canvas->zoom()).toPoint(); };
        auto *toggle = window.findChild<QAction *>(QStringLiteral("commandShowDistances"));
        QVERIFY(toggle && toggle->isCheckable());
        toggle->setChecked(false);
        QTest::mousePress(canvas, Qt::LeftButton, {}, at(100, 75));
        QTest::mouseMove(canvas, at(90, 70));
        QVERIFY(canvas->distanceLines().isEmpty());
        QTest::mouseRelease(canvas, Qt::LeftButton, {}, at(90, 70));
        toggle->setChecked(true);
        QTest::mousePress(canvas, Qt::LeftButton, {}, at(100, 75));
        QTest::mouseMove(canvas, at(95, 75));
        QTest::mouseMove(canvas, at(90, 75));
        QCOMPARE(canvas->distanceLines().size(), 4); // to the four canvas edges
        QTest::mouseRelease(canvas, Qt::LeftButton, {}, at(90, 75));
        QVERIFY(canvas->distanceLines().isEmpty());
        toggle->setChecked(false);
    }

    // 0.7.1: the welcome panel closes from its corner and comes back; the recent projects show three at most, each removable.
    void welcomePanelClosesAndRecentProjectsCanBeRemoved()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QStringList projects;
        for (int i = 0; i < 4; ++i) { const QString path = dir.filePath(QStringLiteral("Project%1.comp").arg(i)); QDir().mkpath(path); projects << path; }
        QSettings settings;
        const QVariant saved = settings.value(QStringLiteral("recentProjects"));
        settings.setValue(QStringLiteral("recentProjects"), projects);
        {
            MainWindow window; window.resize(1200, 800); window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *panel = window.findChild<QWidget *>(QStringLiteral("newCanvasPanel"));
            auto *close = window.findChild<QToolButton *>(QStringLiteral("newCanvasClose"));
            QVERIFY(panel && close);
            QVERIFY(panel->isVisible());
            QTest::mouseClick(close, Qt::LeftButton);
            QVERIFY(!panel->isVisible());
            // Nothing stands in its place; File › New Canvas brings it back.
            QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("newCanvasReopen")));
            for (QAction *a : window.findChildren<QAction *>()) if (a->text().remove(QLatin1Char('&')).startsWith(QStringLiteral("New Canvas"))) { a->trigger(); break; }
            QTest::qWait(50);
            QVERIFY(window.findChild<QWidget *>(QStringLiteral("newCanvasPanel"))->isVisible());

            const auto entries = [&] { QStringList names; for (auto *b : window.findChildren<QPushButton *>(QStringLiteral("recentProjectEntry"))) names << b->text(); return names; };
            QCOMPARE(entries(), (QStringList{"Project0", "Project1", "Project2"}));
            auto removes = window.findChildren<QToolButton *>(QStringLiteral("recentProjectRemove"));
            QCOMPARE(removes.size(), 3);
            QTest::mouseClick(removes.at(1), Qt::LeftButton);
            QTest::qWait(50);
            QCOMPARE(entries(), (QStringList{"Project0", "Project2", "Project3"}));
            QVERIFY(!QSettings().value(QStringLiteral("recentProjects")).toStringList().contains(projects.at(1)));
            QVERIFY(QFileInfo::exists(projects.at(1))); // the project itself stays
        }
        if (saved.isValid()) settings.setValue(QStringLiteral("recentProjects"), saved); else settings.remove(QStringLiteral("recentProjects"));
    }
};
QTEST_MAIN(TestUpstream170)
#include "TestUpstream170.moc"
