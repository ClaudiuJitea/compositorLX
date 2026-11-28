// Regression tests for the Linux UI polish: theme tokens, hold-to-switch tool keys, the non-Alt alternatives and
// opening a project through its manifest.json.
#include "core/EditorSession.h"
#include "io/ProjectWriter.h"
#include "ui/CanvasWidget.h"
#include "ui/EditorStyle.h"
#include "ui/MainWindow.h"
#include <QAction>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QToolButton>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

using namespace compositor;

class TestUiPolish : public QObject
{
    Q_OBJECT
private slots:
    void themeStyleSheetsResolveEveryToken()
    {
        const QRegularExpression leftover(QStringLiteral("@[A-Za-z]+@"));
        const QFont font = QApplication::font();
        for (const QPalette &palette : {theme::darkPalette(theme::systemAccent()), theme::lightPalette(theme::systemAccent()), QPalette()}) {
            const QString sheet = editorStyleSheet(palette, font);
            QVERIFY(!sheet.isEmpty());
            QVERIFY2(!leftover.match(sheet).hasMatch(), qPrintable(leftover.match(sheet).captured()));
            QVERIFY(!sheet.contains(QStringLiteral("Inter")) && !sheet.contains(QStringLiteral("SF Pro")));   // the system font is respected
        }
        QCOMPARE(themeModeFromKey(themeModeKey(ThemeMode::Light)), ThemeMode::Light);
        QCOMPARE(themeModeFromKey(themeModeKey(ThemeMode::System)), ThemeMode::System);
        QCOMPARE(themeModeFromKey(QString()), ThemeMode::Dark);
    }

    void themeMenuOffersThreeModes()
    {
        MainWindow window;
        QVERIFY(window.findChild<QAction *>(QStringLiteral("themeDark")));
        QVERIFY(window.findChild<QAction *>(QStringLiteral("themeLight")));
        QVERIFY(window.findChild<QAction *>(QStringLiteral("themeSystem")));
        QVERIFY(window.findChild<QAction *>(QStringLiteral("commandViewMaskAlone")));
    }

    void heldToolKeySpringsBack()
    {
        MainWindow window; window.resize(1000, 700); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        CanvasWidget *canvas = window.canvas();
        canvas->setTool(CanvasWidget::Tool::Brush);
        const auto pressAndRelease = [&](int heldMs) {
            QKeyEvent override(QEvent::ShortcutOverride, Qt::Key_I, Qt::NoModifier);
            QCoreApplication::sendEvent(canvas, &override);
            canvas->setTool(CanvasWidget::Tool::Eyedropper);   // what the I shortcut does
            QTest::qWait(heldMs);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_I, Qt::NoModifier);
            QCoreApplication::sendEvent(canvas, &release);
        };
        pressAndRelease(450);
        QCOMPARE(canvas->tool(), CanvasWidget::Tool::Brush);       // held: back to the brush
        pressAndRelease(0);
        QCOMPARE(canvas->tool(), CanvasWidget::Tool::Eyedropper);  // tapped: the switch sticks
    }

    // Every tool key switches the tool when pressed for real, from the canvas and from a tool button that was
    // clicked; two window shortcuts on one key would make Qt fire neither.
    void toolKeysSwitchTools()
    {
        MainWindow window; window.resize(1200, 800); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&window));
        window.session().createDocument(400, 300); window.syncDocumentViews();
        CanvasWidget *canvas = window.canvas();
        QHash<QKeySequence, QStringList> owners;
        for (QAction *action : window.findChildren<QAction *>())
            for (const QKeySequence &key : action->shortcuts())
                if (!key.isEmpty() && action->isEnabled()
                    && (action->parent() == &window || action->associatedObjects().contains(&window)))
                    owners[key] << (action->objectName().isEmpty() ? action->text() : action->objectName());
        QStringList clashes;
        for (auto it = owners.cbegin(); it != owners.cend(); ++it)
            if (it.value().size() > 1) clashes << it.key().toString() + QStringLiteral(": ") + it.value().join(QStringLiteral(", "));
        QVERIFY2(clashes.isEmpty(), qPrintable(clashes.join(QStringLiteral(" | "))));
        using T = CanvasWidget::Tool;
        const QList<QPair<Qt::Key, T>> keys = {{Qt::Key_B, T::Brush}, {Qt::Key_S, T::Clone}, {Qt::Key_E, T::Eraser},
            {Qt::Key_J, T::Healing}, {Qt::Key_I, T::Eyedropper}, {Qt::Key_G, T::Gradient}, {Qt::Key_V, T::Move},
            {Qt::Key_H, T::Hand}, {Qt::Key_Z, T::Zoom}, {Qt::Key_T, T::Text}, {Qt::Key_C, T::Crop}};
        // After a number in the tool bar was typed in, its field keeps the focus: letters still switch tools there,
        // digits still edit the number.
        canvas->setTool(T::Brush);
        QDoubleSpinBox *size = window.brushSizeField();
        QVERIFY(size && size->isVisible());
        size->setFocus(); QTRY_VERIFY(size->hasFocus());
        size->selectAll(); QTest::keyClicks(QApplication::focusWidget(), QStringLiteral("37"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QCOMPARE(size->value(), 37.0);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_S);
        QCOMPARE(canvas->tool(), T::Clone);
        for (const bool fromButton : {false, true}) {
            canvas->setFocus(); QTRY_VERIFY(canvas->hasFocus());
            for (const auto &[key, tool] : keys) {
                QWidget *target = QApplication::focusWidget() ? QApplication::focusWidget() : &window;
                QTest::keyClick(target, key);
                QVERIFY2(canvas->tool() == tool, qPrintable(QStringLiteral("key %1 from %2 left tool %3")
                    .arg(QKeySequence(key).toString(), target->metaObject()->className()).arg(int(canvas->tool()))));
                if (fromButton) {   // click the tool's own button, as a user would, then go on with keys
                    for (QToolButton *button : window.findChildren<QToolButton *>())
                        if (button->isVisible() && button->isCheckable() && button->isChecked() && button->toolTip().contains(QStringLiteral("Brush"))) { QTest::mouseClick(button, Qt::LeftButton); break; }
                }
            }
        }
    }

    void zoomOutModeInvertsTheClick()
    {
        MainWindow window; window.resize(1000, 700); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.session().createDocument(400, 300); window.syncDocumentViews();
        CanvasWidget *canvas = window.canvas();
        canvas->setZoom(1.0); canvas->setTool(CanvasWidget::Tool::Zoom);
        const QPoint centre = canvas->canvasRect().center().toPoint();
        canvas->setZoomOutMode(true);
        QTest::mouseClick(canvas, Qt::LeftButton, {}, centre);
        QCOMPARE(canvas->zoom(), 0.5);
        QTest::mouseClick(canvas, Qt::LeftButton, Qt::AltModifier, canvas->canvasRect().center().toPoint());   // Alt still inverts
        QCOMPARE(canvas->zoom(), 1.0);
    }

    void numberPromptsUseThePanel()
    {
        MainWindow window; window.resize(1000, 700); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.session().createDocument(200, 200); window.session().addBlankLayer();
        window.session().setPolygonSelection(QPolygonF(QRectF(60, 60, 80, 80)), SelectionMode::Replace, false); window.syncDocumentViews();
        QAction *feather = window.findChild<QAction *>(QStringLiteral("commandFeatherSelection"));
        QVERIFY(feather); QVERIFY(feather->isEnabled());
        const auto answer = [](int value, QDialogButtonBox::StandardButton button) {
            QTimer::singleShot(200, [value, button] {
                for (QWidget *top : QApplication::topLevelWidgets())
                    if (top->isVisible() && top->objectName() == QLatin1String("messageDialog")) {
                        top->findChild<QSpinBox *>()->setValue(value);
                        top->findChild<QDialogButtonBox *>()->button(button)->click();
                    }
            });
        };
        answer(7, QDialogButtonBox::Ok);
        feather->trigger();
        // Expand, Contract and Feather share one amount (mac 9986d12).
        QCOMPARE(window.session().selectionModifyAmount(), 7);
        answer(19, QDialogButtonBox::Cancel);
        feather->trigger();
        QCOMPARE(window.session().selectionModifyAmount(), 7);   // cancelled: unchanged
    }

    void renderingSettingsUseThePanel()
    {
        MainWindow window; window.resize(1000, 700); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QAction *rendering = nullptr;
        for (QAction *candidate : window.findChildren<QAction *>()) if (candidate->text() == QStringLiteral("Rendering…")) rendering = candidate;
        QVERIFY(rendering);
        bool seen = false;
        QTimer::singleShot(200, [&seen] {
            for (QWidget *top : QApplication::topLevelWidgets())
                if (top->isVisible() && top->objectName() == QLatin1String("messageDialog") && top->property("panelKind") == QStringLiteral("rendering")) {
                    QVERIFY(top->findChild<QWidget *>(QStringLiteral("messagePanel")));
                    QVERIFY(top->findChild<QWidget *>(QStringLiteral("renderingDiagnostics")));
                    QVERIFY(top->findChild<QWidget *>(QStringLiteral("renderingCanvas")));
                    seen = true;
                    top->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();
                }
        });
        rendering->trigger();
        QVERIFY(seen);
    }

    void manifestOpensItsProject()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString project = dir.filePath(QStringLiteral("manifest-test.comp"));
        {
            EditorSession session; session.createDocument(64, 64); session.addBlankLayer();
            ProjectWriter::save(*session.document(), project);
        }
        QVERIFY(QFileInfo::exists(project + QStringLiteral("/manifest.json")));
        MainWindow window;
        QVERIFY(window.openProject(project + QStringLiteral("/manifest.json")));
        QVERIFY(window.session().document());
        QCOMPARE(window.session().document()->canvasSize, QSize(64, 64));
    }
};

QTEST_MAIN(TestUiPolish)
#include "TestUiPolish.moc"
