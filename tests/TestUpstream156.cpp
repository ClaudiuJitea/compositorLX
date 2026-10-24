#include "core/EditorSession.h"
#include "io/GifIO.h"
#include "ui/MainWindow.h"
#include "ui/CanvasWidget.h"
#include "ui/GifTimeline.h"
#include "ui/LayerListModel.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QAction>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>

using namespace compositor;
// Compositor 1.5.6: the GIF timeline's play button, delay menu and frame menu; the Layers panel's search and kind
// toggles; Canvas Size keeping the zoom.
class TestUpstream156 : public QObject {
    Q_OBJECT
    static QImage solid(QColor c, QSize size = {32, 32}) { QImage image(size, QImage::Format_RGBA8888_Premultiplied); image.fill(c); return image; }
    // Three 8 × 8 frames, red, green and blue, 40 / 80 / 120 ms.
    static QString fixture(QTemporaryDir &dir)
    {
        const QString path = dir.filePath("colors.gif"); QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(QByteArray::fromBase64("R0lGODlhCAAIAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQIBAAAACwAAAAACAAIAAAIDwABCBxIsKDBgwgTKkwYEAAh+QQICAAAACwAAAAACAAIAIEAgAAAAAAAAAAAAAAIDwABCBxIsKDBgwgTKkwYEAAh+QQIDAAAACwAAAAACAAIAIEAAP8AAAAAAAAAAAAIDwABCBxIsKDBgwgTKkwYEAA7"));
        return path;
    }
private slots:
    void initTestCase() { QCoreApplication::setOrganizationName("compositor-tests"); QCoreApplication::setApplicationName("upstream156"); }

    void canvasSizeKeepsTheZoomAndRecenters() {
        MainWindow w; w.resize(1000, 800); w.show(); w.session().createDocument(600, 400, true); w.syncDocumentViews(); QTest::qWait(50);
        auto view = w.canvas()->viewState(); view.fit = false; view.pan = QPointF(40, -25); w.canvas()->restoreViewState(view);
        const double zoom = w.canvas()->viewState().zoom;
        QTimer::singleShot(0, &w, [&] {
            auto *d = w.findChild<QDialog *>("canvasSizeDialog"); QVERIFY(d);
            d->findChild<QDoubleSpinBox *>("canvasSizeWidth")->setValue(300); d->findChild<QDoubleSpinBox *>("canvasSizeHeight")->setValue(200); d->accept();
        });
        w.findChild<QAction *>("cmdCanvasSize")->trigger();
        QCOMPARE(w.session().document()->canvasSize, QSize(300, 200));
        QCOMPARE(w.canvas()->viewState().zoom, zoom);
        QCOMPARE(w.canvas()->viewState().pan, QPointF());
        w.session().markSaved();
    }

    void layerKindsCombineAndFoldersBringTheirContents() {
        EditorSession s; s.createDocument(50, 50);
        s.insertImage(solid(Qt::red), "Photo"); s.groupSelectedLayers(); const auto folder = s.activeLayer()->id;
        QVERIFY(s.addAdjustment("Invert"));
        LayerListModel model; model.setDocument(s.document());
        QCOMPARE(LayerListModel::layerType(*s.activeLayer()), QStringLiteral("Adjustments"));
        model.setFilter("", {"Adjustments"}); QCOMPARE(model.rowCount(), 1);
        model.setFilter("", {"Adjustments", "Folders"}); QCOMPARE(model.rowCount(), 3);
        model.setFilter("", {"Images"}); QCOMPARE(model.rowCount(), 1);
        model.setFilter("  photo ", {}); QCOMPARE(model.rowCount(), 1);
        model.setFilter("", {"Selected"}, {folder}); QCOMPARE(model.rowCount(), 3); // the folder, the photo and the adjustment added in it
        model.setFilter("", {"Text"}); QCOMPARE(model.rowCount(), 0);
    }

    void layerSearchOpensClosesAndClearsInWindow() {
        MainWindow w; w.resize(1100, 700); w.show();
        w.session().insertImage(solid(Qt::red), "First"); w.session().insertImage(solid(Qt::blue), "Second"); w.syncDocumentViews(); QTest::qWait(20);
        auto *search = w.findChild<QLineEdit *>("layerSearch"); auto *button = w.findChild<QToolButton *>("layerSearchButton");
        QVERIFY(search && button); QVERIFY(!search->isVisible());
        button->click(); QVERIFY(search->isVisible());
        search->setText("Sec"); QCOMPARE(w.layerModel()->rowCount(), 1);
        // Escape clears the search, and on an empty one closes it.
        QTest::keyClick(search, Qt::Key_Escape); QVERIFY(search->text().isEmpty()); QVERIFY(search->isVisible()); QCOMPARE(w.layerModel()->rowCount(), 2);
        QTest::keyClick(search, Qt::Key_Escape); QVERIFY(!search->isVisible());
        // Nothing matching shows Clear All, which puts every layer back.
        w.findChild<QAction *>("layerFilterText")->setChecked(true);
        auto *noMatch = w.findChild<QWidget *>("layerNoMatch"); QVERIFY(noMatch && noMatch->isVisible());
        QVERIFY(w.findChild<QToolButton *>("layerFilterButton")->isChecked());
        w.findChild<QPushButton *>("layerFilterClearAll")->click();
        QVERIFY(!noMatch->isVisible()); QCOMPARE(w.layerModel()->rowCount(), 2); QVERIFY(!w.findChild<QToolButton *>("layerFilterButton")->isChecked());
        w.session().markSaved();
    }

    void gifDelayMenuSetsOriginalOrOneDelayForAll() {
        QTemporaryDir dir; EditorSession s; s.createDocument(8, 8); QVERIFY(s.insertGif(fixture(dir)));
        const auto &a = [&]() -> const GifAnimation & { return *s.activeLayer()->gif; };
        const int undo = s.history().undoCount();
        QVERIFY(s.setGifTiming(50)); QVERIFY(a().sameDelay); QCOMPARE(a().delay(2), 50);
        QVERIFY(s.setGifTiming(50)); QCOMPARE(s.history().undoCount(), undo + 1); // the same delay again is no edit
        QVERIFY(s.setGifTiming(std::nullopt)); QVERIFY(!a().sameDelay); QCOMPARE(a().delay(2), 120);
        QVERIFY(!s.setGifTiming(10));
        s.undo(); QVERIFY(a().sameDelay); s.undo(); QVERIFY(!a().sameDelay);
    }

    void gifFrameMenuActsOnTheFrameClicked() {
        QTemporaryDir dir; EditorSession s; s.createDocument(8, 8); QVERIFY(s.insertGif(fixture(dir)));
        QCOMPARE(s.activeLayer()->gif->selected, 0);
        QVERIFY(s.duplicateGifFrame(2));
        const auto &a = *s.activeLayer()->gif; QCOMPARE(a.frames.size(), 4); QCOMPARE(a.frames[3].sourceIndex, 2);
        QCOMPARE(a.selected, a.frames[3].index);
        // A copy without a delay of its own keeps following the source's.
        QVERIFY(!a.delays.contains(a.selected)); QCOMPARE(a.delay(a.selected), 120);
        QVERIFY(s.deleteGifFrame(1)); QCOMPARE(s.activeLayer()->gif->frames.size(), 3); QCOMPARE(s.activeLayer()->gif->selected, 2);
    }

    void gifTimelineControlsInWindow() {
        QTemporaryDir dir; MainWindow w; w.resize(1100, 700); w.show(); QVERIFY(w.openProject(fixture(dir))); QCoreApplication::processEvents();
        auto *timeline = w.findChild<QWidget *>("gifTimeline"); QVERIFY(timeline && timeline->isVisible()); QCOMPARE(timeline->height(), 110);
        auto *frames = w.findChild<QListWidget *>("gifFrames"); QCOMPARE(frames->count(), 3); QCOMPARE(frames->item(1)->text(), QStringLiteral("2"));
        // 1.5.7: the label shows the GIF's own delays, shortest to longest, rather than "Original".
        auto *delay = w.findChild<QToolButton *>("gifDelay"); QCOMPARE(delay->text(), QStringLiteral("40–120ms"));
        w.findChild<QAction *>("gifDelay100")->trigger();
        QVERIFY(w.session().activeLayer()->gif->sameDelay); QCOMPARE(delay->text(), QStringLiteral("100ms"));
        QVERIFY(w.findChild<QAction *>("gifDelay100")->isChecked());
        w.findChild<QAction *>("gifDelayOriginal")->trigger(); QCOMPARE(delay->text(), QStringLiteral("40–120ms"));
        auto *play = w.findChild<QToolButton *>("gifPlay"); QVERIFY(play->isEnabled());
        play->click(); QTRY_VERIFY_WITH_TIMEOUT(w.session().activeLayer()->gif->selected != 0, 1000);
        play->click(); QCOMPARE(play->toolTip(), QStringLiteral("Play"));
        auto *remove = w.findChild<QAction *>("gifDelete"); remove->trigger(); remove->trigger();
        QCOMPARE(frames->count(), 1); QVERIFY(!play->isEnabled());
        w.session().markSaved();
    }
};
QTEST_MAIN(TestUpstream156)
#include "TestUpstream156.moc"
