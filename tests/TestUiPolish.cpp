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
#include <QSpinBox>
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
        QCOMPARE(window.session().selectionFeatherAmount(), 7);
        answer(19, QDialogButtonBox::Cancel);
        feather->trigger();
        QCOMPARE(window.session().selectionFeatherAmount(), 7);   // cancelled: unchanged
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
