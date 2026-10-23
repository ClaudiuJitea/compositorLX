#include "io/ImageExporter.h"
#include "rendering/GpuRenderer.h"
#include "rendering/LayerRenderer.h"
#include "ui/AsyncImagePreview.h"
#include "ui/CameraRawDialog.h"
#include "ui/CanvasWidget.h"
#include "ui/MainWindow.h"
#include "rendering/RasterOperations.h"
#include "ui/RenderingSettingsDialog.h"
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QDialog>
#include <QPainter>
#include <QSemaphore>
#include <QSettings>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QtTest>
#include <atomic>

using namespace compositor;
class TestGpuUi : public QObject {
    Q_OBJECT
  private slots:
    void latestPreviewWins() {
        AsyncImagePreview preview;
        QSemaphore release;
        std::atomic<bool> started = false;
        int applied = 0;
        preview.request(
            [&] {
                started = true;
                release.acquire();
                return QImage(1, 1, QImage::Format_RGB32);
            },
            [&](QImage) { applied = 1; });
        QTRY_VERIFY(started.load());
        preview.request([] { return QImage(2, 2, QImage::Format_RGB32); }, [&](QImage) { applied = 2; });
        preview.request([] { return QImage(3, 3, QImage::Format_RGB32); },
                        [&](QImage image) {
                            QCOMPARE(QThread::currentThread(), qApp->thread());
                            QCOMPARE(image.size(), QSize(3, 3));
                            applied = 3;
                        });
        release.release();
        QTRY_COMPARE(applied, 3);
        QTRY_VERIFY(!preview.busy());
    }
    void completedWorkerWithQueuedSignal() {
        AsyncImagePreview preview;
        std::atomic<bool> done = false;
        int applied = 0;
        preview.request(
            [&] {
                done = true;
                return QImage();
            },
            [&](QImage) { applied = 1; });
        QThreadPool::globalInstance()->waitForDone();
        QVERIFY(done.load());
        // The worker is finished, but its completion event has not been delivered.
        preview.request([] { return QImage(4, 4, QImage::Format_RGB32); },
                        [&](QImage image) {
                            QCOMPARE(image.size(), QSize(4, 4));
                            applied = 2;
                        });
        QTRY_COMPARE(applied, 2);
        QTRY_VERIFY(!preview.busy());
    }
    void cancelledAndDestroyedPreviews() {
        int applied = 0;
        {
            AsyncImagePreview preview;
            preview.request([] { return QImage(); }, [&](QImage) { ++applied; });
            preview.cancel();
            QThreadPool::globalInstance()->waitForDone();
            QCoreApplication::processEvents();
            QCOMPARE(applied, 0);
        }
        {
            AsyncImagePreview preview;
            preview.request([] { return QImage(); }, [&](QImage) { ++applied; });
        }
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        QCOMPARE(applied, 0);
    }
    void editingUndoAndExport() {
        qputenv("COMPOSITOR_RENDER_BACKEND", "vulkan");
        QImage image(512, 512, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor(80, 130, 190, 180));
        EditorSession session;
        QVERIFY(session.insertImage(image, "GPU"));
        const QImage before = session.activeLayer()->image;
        QVERIFY(session.applyExposure(.7, 0, 1));
        const QImage changed = session.activeLayer()->image;
        QVERIFY(changed != before);
        session.undo();
        QCOMPARE(session.activeLayer()->image, before);
        session.redo();
        QCOMPARE(session.activeLayer()->image, changed);
        const auto flattened = LayerRenderer::flattened(*session.document());
        const auto png = ImageExporter::png(flattened, 72);
        QVERIFY(png);
        const auto decoded = QImage::fromData(*png).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        const auto expected = flattened.convertToFormat(decoded.format());
        QCOMPARE(decoded.size(), expected.size());
        for (int y = 0; y < decoded.height(); ++y)
            for (int x = 0; x < decoded.width() * 4; ++x)
                QVERIFY(std::abs(int(decoded.constScanLine(y)[x]) - int(expected.constScanLine(y)[x])) <= 1);
        QCOMPARE(decoded.pixelColor(0, 0).alpha(), expected.pixelColor(0, 0).alpha());
        qunsetenv("COMPOSITOR_RENDER_BACKEND");
    }
    // C1: twenty rapid changes in a filter dialog on a 24 MP layer never block the event loop; only the last result
    // reaches the layer, and OK leaves one undo step.
    void asynchronousFilterPreviews() {
        MainWindow window;
        QImage photo(6000, 4000, QImage::Format_RGBA8888_Premultiplied);
        photo.fill(QColor(120, 100, 80));
        window.session().createDocument(6000, 4000);
        window.session().insertImage(photo, "Photo");
        window.syncDocumentViews();
        const QImage original = window.session().activeLayer()->image;
        const int undoSteps = window.session().history().undoCount();
        auto *exposure = window.findChild<QAction *>("cmdExposure");
        QVERIFY(exposure);
        double slowest = 0;
        bool driven = false;
        QTimer::singleShot(0, &window, [&] {
            QDialog *dialog = nullptr;
            for (auto *candidate : window.findChildren<QDialog *>())
                if (candidate->isVisible() && candidate->windowTitle() == "Exposure")
                    dialog = candidate;
            QVERIFY(dialog);
            auto *stops = dialog->findChild<QDoubleSpinBox *>("exposureStops");
            QVERIFY(stops);
            for (int i = 1; i <= 20; ++i) {
                QElapsedTimer timer;
                timer.start();
                stops->setValue(i * .05);
                slowest = std::max(slowest, timer.nsecsElapsed() / 1e6);
                QTest::qWait(2);
            }
            QImage pixel(1, 1, QImage::Format_RGBA8888_Premultiplied);
            pixel.fill(QColor(120, 100, 80));
            const QColor expected = RasterOperations::exposure(pixel, 1., 0, 1).pixelColor(0, 0);
            QTRY_COMPARE_WITH_TIMEOUT(window.session().activeLayer()->image.pixelColor(10, 10), expected, 20000);
            driven = true;
            dialog->accept();
        });
        exposure->trigger();
        QVERIFY(driven);
        QVERIFY2(slowest < 50, qPrintable(QString("a value change blocked for %1 ms").arg(slowest)));
        QCOMPARE(window.session().history().undoCount(), undoSteps + 1);
        window.session().undo();
        QCOMPARE(window.session().activeLayer()->image, original);
    }
    void cameraRawCancellation() {
        QImage image(512, 512, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor(80, 130, 190));
        EditorSession session;
        QVERIFY(session.insertImage(image, "Preview"));
        const auto original = session.activeLayer()->image;
        const int history = session.history().undoCount();
        CameraRawDialog dialog(nullptr, session, session.activeLayer()->id, [] {});
        CameraRawSettings first;
        first.exposure = 1;
        dialog.setSettings(first);
        CameraRawSettings last;
        last.exposure = -1;
        dialog.setSettings(last);
        dialog.reject();
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        QCOMPARE(session.activeLayer()->image, original);
        QCOMPARE(session.history().undoCount(), history);
    }
    void rendererPreferences() {
        qunsetenv("COMPOSITOR_RENDER_BACKEND");
        GpuRenderer::setBackend(RenderBackend::Auto);
        const auto choose = [](bool accept) {
            QTimer::singleShot(0, [accept] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                dialog->findChild<QComboBox *>("renderingBackend")->setCurrentIndex(1);
                dialog->findChild<QComboBox *>("renderingMemoryBudget")->setCurrentIndex(1);
                if (accept)
                    dialog->accept();
                else
                    dialog->reject();
            });
        };
        choose(false);
        QVERIFY(!showRenderingSettings(nullptr));
        QCOMPARE(GpuRenderer::backend(), RenderBackend::Auto);
        choose(true);
        QVERIFY(showRenderingSettings(nullptr));
        QCOMPARE(GpuRenderer::backend(), RenderBackend::Cpu);
        QCOMPARE(GpuRenderer::memoryBudgetMiB(), 128);
        GpuRenderer::setBackend(RenderBackend::Auto);
        GpuRenderer::setMemoryBudgetMiB(512);
    }
    // A7: environment overrides are reported, their controls disabled, and OK never saves them as preferences.
    void renderingOverrides() {
        GpuRenderer::setBackend(RenderBackend::Auto);
        GpuRenderer::setMemoryBudgetMiB(512);
        qputenv("COMPOSITOR_RENDER_BACKEND", "vulkan");
        qputenv("COMPOSITOR_GPU_MEMORY_MIB", "333");
        bool checked = false;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *backend = dialog->findChild<QComboBox *>("renderingBackend");
            auto *memory = dialog->findChild<QComboBox *>("renderingMemoryBudget");
            QVERIFY(!backend->isEnabled());
            QVERIFY(!memory->isEnabled());
            QCOMPARE(backend->currentIndex(), int(RenderBackend::Auto)); // The saved preference, not the override.
            QCOMPARE(memory->currentData().toInt(), 512);
            QVERIFY(dialog->findChild<QLabel *>("renderingOverride"));
            QVERIFY(dialog->findChild<QPushButton *>("renderingSelfCheck"));
            QVERIFY(dialog->findChild<QPushButton *>("renderingCopyDiagnostics"));
            checked = true;
            dialog->accept();
        });
        QVERIFY(showRenderingSettings(nullptr));
        QVERIFY(checked);
        qunsetenv("COMPOSITOR_RENDER_BACKEND");
        qunsetenv("COMPOSITOR_GPU_MEMORY_MIB");
        QCOMPARE(GpuRenderer::backend(), RenderBackend::Auto);
        QCOMPARE(GpuRenderer::memoryBudgetMiB(), 512);
        // A budget saved outside the list stays selected instead of becoming 64 MiB.
        QSettings().setValue("rendering/memoryMiB", 333);
        GpuRenderer::setMemoryBudgetMiB(333);
        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QCOMPARE(dialog->findChild<QComboBox *>("renderingMemoryBudget")->currentData().toInt(), 333);
            dialog->accept();
        });
        QVERIFY(showRenderingSettings(nullptr));
        QCOMPARE(GpuRenderer::memoryBudgetMiB(), 333);
        GpuRenderer::setMemoryBudgetMiB(512);
    }
    void canvasPresentation() {
        qputenv("COMPOSITOR_RENDER_BACKEND", "auto");
        auto document = std::make_shared<Document>();
        document->canvasSize = {128, 96};
        Layer layer;
        layer.id = QUuid::createUuid();
        layer.image = QImage(document->canvasSize, QImage::Format_RGBA8888_Premultiplied);
        layer.image.fill(QColor(32, 120, 200));
        layer.transform.size = layer.image.size();
        document->layers.push_back(layer);
        CanvasWidget canvas;
        canvas.resize(512, 400);
        canvas.setDocument(document);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        canvas.fitCanvas();
        QTest::qWait(100);
        auto *viewport = canvas.findChild<QWidget *>("gpuCanvasViewport");
        if (qEnvironmentVariableIsSet("COMPOSITOR_REQUIRE_GPU_CANVAS")) {
            QVERIFY(viewport);
            QVERIFY(viewport->isVisible());
            QVERIFY(!viewport->property("failed").toBool());
            qInfo() << viewport->property("renderer");
        }
        if (QGuiApplication::platformName() == "offscreen")
            QVERIFY(!viewport);
        const QImage gpu = canvas.grab().toImage();
        const QPoint center = canvas.canvasRect().center().toPoint();
        const QColor shown = gpu.pixelColor(center * canvas.devicePixelRatioF());
        QVERIFY(std::abs(shown.red() - 32) <= 1);
        QVERIFY(std::abs(shown.green() - 120) <= 1);
        QVERIFY(std::abs(shown.blue() - 200) <= 1);
        if (viewport) {
            // Canvas repaints (zoom, document edits) must reach the GL viewport, not only the first frame.
            struct PaintCounter : QObject {
                int paints = 0;
                bool eventFilter(QObject *, QEvent *event) override {
                    paints += event->type() == QEvent::Paint;
                    return false;
                }
            } counter;
            viewport->installEventFilter(&counter);
            canvas.setZoom(canvas.zoom() * 1.5);
            QTRY_VERIFY(counter.paints > 0);
            const int beforeEdit = counter.paints;
            document->layers.front().image.fill(QColor(200, 40, 20));
            canvas.invalidateDocument();
            QTRY_VERIFY(counter.paints > beforeEdit);
            viewport->removeEventFilter(&counter);
            document->layers.front().image.fill(QColor(32, 120, 200));
            canvas.invalidateDocument();
            canvas.fitCanvas();
            QTest::qWait(100);
        }
        qputenv("COMPOSITOR_RENDER_BACKEND", "cpu");
        canvas.invalidateDocument();
        QTest::qWait(100);
        if (viewport)
            QVERIFY(viewport->isHidden());
        const QImage cpu = canvas.grab().toImage();
        QCOMPARE(cpu.pixelColor(center * canvas.devicePixelRatioF()), shown);
        qunsetenv("COMPOSITOR_RENDER_BACKEND");
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    app.setOrganizationName("CompositorTests");
    app.setApplicationName("GpuUi");
    TestGpuUi test;
    const int status = QTest::qExec(&test, argc, argv);
    QThreadPool::globalInstance()->waitForDone();
    return status;
}
#include "TestGpuUi.moc"
