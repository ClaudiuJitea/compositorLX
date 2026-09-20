#include "ui/MainWindow.h"
#include "ui/CanvasWidget.h"
#include "ui/LayerListModel.h"
#include "ui/EditorStyle.h"
#include "io/ProjectReader.h"
#include "io/ProjectWriter.h"
#include "rendering/TextLayout.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QColorDialog>
#include <QClipboard>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QInputDialog>
#include <QListView>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QSpinBox>
#include <QFontComboBox>
#include <QStyleOptionComboBox>
#include <QStandardPaths>
#include <QSlider>
#include <QTimer>
#include <QTabBar>
#include <QTextEdit>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>

using namespace compositor;

class MainWindowTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void nativeMenusExposeTheCompleteCommandSurface();
    void newCanvasLayerAndUndoWorkThroughWindowActions();
    void mountingCanvasGivesItKeyboardFocus();
    void layerListModifierCursorsRespectThumbnailHitZones();
    void selectionToolsExposeNativeOptionControls();
    void keyboardToolsKeepRailAndOptionsInSync();
    void textToolCreatesAnUndoableLayer();
    void toolOptionsFitSmallWindows();
    void textFormattingPreservesSelectionAndReplacementEditor();
    void editingTextLayersPreservesTheirStyleAndSavesPendingText();
    void styledTextEditorPreviewsTheCommittedLayout();
    void textFieldsKeepNativeEditingCommandsAndIgnoreEditorKeys();
    void commandStatesAndTitlesFollowTheDocument();
    void opacityDragCreatesOneUndoEntry();
    void newCanvasSuggestsClipboardImageDimensions();
    void imageSizeCanChangeResolutionWithoutResampling();
    void canvasSizeOffersAllExtensionChoices();
    void blendModeShortcutsCycleAndWrap();
    void opacityDigitsSupportExactTwoDigitValuesAndGradient();
    void layerThumbnailsUseCanvasPlacementAndMaskEdgeTone();
    void projectTabsKeepIndependentDocumentsAndUndo();
    void layerDropsCopyAcrossProjectsAndIntoNewTabs();
    void imageDropsCreateNewTabsOrTargetExistingTabs();
    void canvasSizeRelativeRatioAndUnitsMatchOriginalCalculations();
    void imageSizeUnitsResolutionAndResamplingMatchOriginalCalculations();
    void colorPickerIsNonModalSamplesCanvasAndCommitsOnlyOnAccept();
    void jpegExportShowsEncodedPreviewBeforeDestination();
    void numericTransformsStayPendingAndApplyOrCancelAsOneEdit();
    void newTabsCommitPendingTransformsToTheirOriginalDocument();
    void hueSaturationExposesRangeSamplingAndTargetedControls();
    void liveFilterPreviewsUseFloatingDialogs();
    void curvesGraphAddsSelectsAndDragsPoints();
    void documentMutationsInvalidateTheCachedCanvas();
    void selectionAndRenameAvoidCompositeAndRowReloads();
    void levelsPanelProvidesHistogramHandlesPreviewAndReset();
    void gradientMapUsesLiveNonModalEndpointPicker();
    void newGradientMapAdjustmentStartsFromPalette();
    void layerModelCollapsesNestedFoldersAndRenamesInline();
    void numericZoomCoversTheMacRange();
};

static QString plainText(QString text)
{
    text.remove(QLatin1Char('&')); text.replace(QStringLiteral("…"), QString());
    return text.trimmed();
}

static QAction *actionNamed(MainWindow &window, const QString &name)
{
    for (QAction *action : window.findChildren<QAction *>()) if (plainText(action->text()) == name) return action;
    return nullptr;
}

static void createEmbeddedCanvas(MainWindow &window, int width = 1920, int height = 1080)
{
    auto *widthField = window.findChild<QSpinBox *>(QStringLiteral("newCanvasWidth")); QVERIFY(widthField);
    auto *heightField = window.findChild<QSpinBox *>(QStringLiteral("newCanvasHeight")); QVERIFY(heightField);
    auto *create = window.findChild<QPushButton *>(QStringLiteral("createCanvas")); QVERIFY(create);
    widthField->setValue(width); heightField->setValue(height); create->click();
}

void MainWindowTests::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void MainWindowTests::nativeMenusExposeTheCompleteCommandSurface()
{
    MainWindow window; window.show(); QTest::qWait(1);
    QVERIFY(window.menuBar()->isVisible());
    QStringList menus; for (QAction *action : window.menuBar()->actions()) menus << plainText(action->text());
    QCOMPARE(menus, QStringList({QStringLiteral("File"), QStringLiteral("Edit"), QStringLiteral("Layer"),
        QStringLiteral("Select"), QStringLiteral("Image"), QStringLiteral("Filter"), QStringLiteral("View"), QStringLiteral("Help")}));
    const QStringList commands{QStringLiteral("New Canvas"), QStringLiteral("Open Project"), QStringLiteral("Save"),
        QStringLiteral("Copy Merged"), QStringLiteral("Transform Layer"), QStringLiteral("Group Selected Layers"),
        QStringLiteral("Levels"), QStringLiteral("Hue/Saturation"), QStringLiteral("Gaussian Blur"),
        QStringLiteral("Content-Aware Fill"), QStringLiteral("Remove Background"), QStringLiteral("Pixel Grid (800% and above)")};
    for (const QString &command : commands) QVERIFY2(actionNamed(window, command), qPrintable(command));
    QCOMPARE(actionNamed(window, QStringLiteral("Save"))->shortcut(), QKeySequence::Save);
    QCOMPARE(actionNamed(window, QStringLiteral("Remove Background"))->shortcut(), QKeySequence());
    auto *quickFileMenu = window.findChild<QMenu *>(QStringLiteral("quickFileMenu")); QVERIFY(quickFileMenu);
    auto *quickExportMenu = quickFileMenu->findChild<QMenu *>(QStringLiteral("quickExportMenu")); QVERIFY(quickExportMenu);
    QStringList quickActions; for (QAction *action : quickFileMenu->actions()) if (!action->isSeparator()) quickActions << plainText(action->text());
    QCOMPARE(quickActions, QStringList({QStringLiteral("Open Project"), QStringLiteral("Save"), QStringLiteral("Save As"), QStringLiteral("Export"),
        QStringLiteral("File"), QStringLiteral("Edit"), QStringLiteral("Layer"), QStringLiteral("Select"), QStringLiteral("Image"),
        QStringLiteral("Filter"), QStringLiteral("View"), QStringLiteral("Help"), QStringLiteral("Show Menu Bar")}));
    for (QAction *category : window.menuBar()->actions()) QVERIFY(quickFileMenu->actions().contains(category));
    auto *menuButton = window.findChild<QToolButton *>(QStringLiteral("menuRestoreButton")); QVERIFY(menuButton);
    QVERIFY(!menuButton->menu());
    QTest::mouseClick(menuButton, Qt::LeftButton);
    QVERIFY(!window.menuBar()->isVisible()); QVERIFY(!quickFileMenu->isVisible());
    QTest::mouseClick(menuButton, Qt::LeftButton);
    QVERIFY(window.menuBar()->isVisible());
    menuButton->customContextMenuRequested(QPoint(5, 5));
    QVERIFY(quickFileMenu->isVisible()); quickFileMenu->hide();
    QStringList exportActions; for (QAction *action : quickExportMenu->actions()) exportActions << plainText(action->text());
    QCOMPARE(exportActions, QStringList({QStringLiteral("Export PNG"), QStringLiteral("Export JPEG")}));
    if (qEnvironmentVariableIsSet("COMPOSITOR_CAPTURE_OPTIONS")) {
        const QString previousStyle = qApp->styleSheet(); qApp->setStyleSheet(editorStyleSheet());
        quickFileMenu->popup(menuButton->mapToGlobal(QPoint(0, menuButton->height())));
        QCoreApplication::processEvents();
        quickFileMenu->grab().save(QStringLiteral("/tmp/compositor-application-menu.png"));
        quickFileMenu->hide(); qApp->setStyleSheet(previousStyle);
    }
    QVERIFY(window.findChild<CanvasWidget *>());
}

void MainWindowTests::newCanvasLayerAndUndoWorkThroughWindowActions()
{
    MainWindow window; window.show();
    createEmbeddedCanvas(window, 64, 48);
    auto *layers = window.findChild<QListView *>(QStringLiteral("layerList")); QVERIFY(layers); QCOMPARE(layers->model()->rowCount(), 1);
    QAction *newLayer = actionNamed(window, QStringLiteral("New Layer")); QVERIFY(newLayer); newLayer->trigger();
    QCOMPARE(layers->model()->rowCount(), 2);
    QAction *undo = actionNamed(window, QStringLiteral("Undo")); QVERIFY(undo); undo->trigger();
    QCOMPARE(layers->model()->rowCount(), 1);
    QAction *folder = actionNamed(window, QStringLiteral("New Folder")); QVERIFY(folder); folder->trigger();
    QCOMPARE(layers->model()->rowCount(), 2);
}

void MainWindowTests::mountingCanvasGivesItKeyboardFocus()
{
    MainWindow window; window.show(); QTest::qWait(1);
    auto *canvas = window.findChild<CanvasWidget *>(); QVERIFY(canvas); QVERIFY(!canvas->isVisible());
    createEmbeddedCanvas(window, 100, 100);
    QVERIFY(canvas->isVisible()); QTRY_VERIFY(canvas->hasFocus());
}

void MainWindowTests::layerListModifierCursorsRespectThumbnailHitZones()
{
    MainWindow window; window.show(); createEmbeddedCanvas(window, 400, 300);
    auto *layers = window.findChild<QListView *>(QStringLiteral("layerList")); QVERIFY(layers); QVERIFY(layers->model()->rowCount() == 1);
    const QModelIndex row = layers->model()->index(0, 0); const QRect rect = layers->visualRect(row); QVERIFY(rect.isValid());
    QWidget *viewport = layers->viewport();
    const auto move = [viewport](const QPointF &point, Qt::KeyboardModifiers modifiers) {
        QMouseEvent event(QEvent::MouseMove, point, viewport->mapToGlobal(point.toPoint()), Qt::NoButton, Qt::NoButton, modifiers);
        QApplication::sendEvent(viewport, &event);
    };
    const QPointF namePoint(std::min(viewport->width() - 5, 160), rect.center().y());
    move(namePoint, Qt::AltModifier); QCOMPARE(viewport->cursor().shape(), Qt::DragCopyCursor);
    move(QPointF(50, rect.center().y()), Qt::AltModifier); QCOMPARE(viewport->cursor().shape(), Qt::PointingHandCursor);
    move(QPointF(50, rect.center().y()), Qt::ControlModifier); QCOMPARE(viewport->cursor().shape(), Qt::PointingHandCursor);
    move(namePoint, Qt::NoModifier); QCOMPARE(viewport->cursor().shape(), Qt::ArrowCursor);
    QEvent leave(QEvent::Leave); QApplication::sendEvent(viewport, &leave); QCOMPARE(viewport->cursor().shape(), Qt::ArrowCursor);
}

void MainWindowTests::selectionToolsExposeNativeOptionControls()
{
    MainWindow window; window.show();
    QToolButton *wand = nullptr;
    for (QToolButton *button : window.findChildren<QToolButton *>()) if (button->toolTip() == QStringLiteral("Magic Wand")) { wand = button; break; }
    QVERIFY(wand); wand->click();
    QVERIFY(window.findChild<QSpinBox *>(QStringLiteral("wandTolerance"))->isVisible());
    QVERIFY(window.findChild<QComboBox *>(QStringLiteral("wandSampleSize"))->isVisible());
    QVERIFY(window.findChild<QComboBox *>(QStringLiteral("wandSample"))->isVisible());
    QVERIFY(window.findChild<QCheckBox *>(QStringLiteral("wandContiguous"))->isVisible());
    QVERIFY(window.findChild<QComboBox *>(QStringLiteral("selectionMode"))->isVisible());
    QVERIFY(window.findChild<QCheckBox *>(QStringLiteral("selectionAntialias"))->isVisible());
}

void MainWindowTests::keyboardToolsKeepRailAndOptionsInSync()
{
    MainWindow window; window.show();
    const auto shortcut = [&window](const QKeySequence &key) -> QAction * {
        for (QAction *action : window.findChildren<QAction *>()) if (action->shortcut() == key) return action;
        return nullptr;
    };
    auto *erase = shortcut(QKeySequence(Qt::Key_E)); QVERIFY(erase); erase->trigger();
    auto *brushMode = window.findChild<QComboBox *>(QStringLiteral("brushMode")); QVERIFY(brushMode->isVisible()); QCOMPARE(brushMode->currentIndex(), 1);
    QToolButton *brushButton = nullptr;
    for (QToolButton *button : window.findChildren<QToolButton *>()) if (button->toolTip() == QStringLiteral("Brush")) brushButton = button;
    QVERIFY(brushButton && brushButton->isChecked());
    auto *paint = shortcut(QKeySequence(Qt::Key_B)); QVERIFY(paint); paint->trigger(); QCOMPARE(brushMode->currentIndex(), 0);
    auto *shape = shortcut(QKeySequence(Qt::Key_U)); QVERIFY(shape); shape->trigger();
    auto *shapeKind = window.findChild<QComboBox *>(QStringLiteral("shapeKind")); QVERIFY(shapeKind->isVisible()); QCOMPARE(shapeKind->currentIndex(), 0);
    auto *toggleShape = shortcut(QKeySequence(Qt::SHIFT | Qt::Key_U)); QVERIFY(toggleShape); toggleShape->trigger(); QCOMPARE(shapeKind->currentIndex(), 1);

    auto *brushSize = window.findChild<QDoubleSpinBox *>(QStringLiteral("brushSize")); QVERIFY(brushSize);
    auto *brushHardness = window.findChild<QDoubleSpinBox *>(QStringLiteral("brushHardness")); QVERIFY(brushHardness);
    auto *largerBrush = shortcut(QKeySequence(Qt::Key_BracketRight)); QVERIFY(largerBrush);
    auto *softerBrush = shortcut(QKeySequence(Qt::SHIFT | Qt::Key_BracketLeft)); QVERIFY(softerBrush);
    paint->trigger();
    brushSize->setValue(20); brushHardness->setValue(50);
    largerBrush->trigger(); QVERIFY(brushSize->value() > 20);
    softerBrush->trigger(); QCOMPARE(brushHardness->value(), 25.0);

    auto *move = shortcut(QKeySequence(Qt::Key_V)); QVERIFY(move); move->trigger();
    const double movedToolSize = brushSize->value();
    const double movedToolHardness = brushHardness->value();
    largerBrush->trigger(); softerBrush->trigger();
    QCOMPARE(brushSize->value(), movedToolSize); QCOMPARE(brushHardness->value(), movedToolHardness);

    paint->trigger();
    QLineEdit textField; textField.show(); textField.activateWindow(); textField.setFocus(); QTest::qWait(1);
    const double textFocusSize = brushSize->value();
    largerBrush->trigger();
    QCOMPARE(brushSize->value(), textFocusSize);
}

void MainWindowTests::textToolCreatesAnUndoableLayer()
{
    MainWindow window; window.show(); createEmbeddedCanvas(window, 320, 200);
    QToolButton *textTool = nullptr;
    for (QToolButton *button : window.findChildren<QToolButton *>()) if (button->toolTip() == QStringLiteral("Text")) textTool = button;
    QVERIFY(textTool); textTool->click();
    auto *font = window.findChild<QComboBox *>(QStringLiteral("textAlignment")); QVERIFY(font); QVERIFY(font->isVisible());
    auto *canvas = window.findChild<CanvasWidget *>(); QVERIFY(canvas);
    QVERIFY(QMetaObject::invokeMethod(canvas, "textBoxRequested", Qt::DirectConnection, Q_ARG(QRectF, QRectF(24, 30, 220, 80)), Q_ARG(bool, true)));
    auto *editor = canvas->findChild<QTextEdit *>(QStringLiteral("inlineTextEditor")); QVERIFY(editor); editor->setPlainText(QStringLiteral("Hello Compositor"));
    QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier); QCoreApplication::processEvents();
    auto *layers = window.findChild<QListView *>(QStringLiteral("layerList")); QVERIFY(layers); QCOMPARE(layers->model()->rowCount(), 2);
    QAction *undo = actionNamed(window, QStringLiteral("Undo")); QVERIFY(undo); undo->trigger(); QCOMPARE(layers->model()->rowCount(), 1);
}

void MainWindowTests::toolOptionsFitSmallWindows()
{
    const QString previousStyle = qApp->styleSheet();
    qApp->setStyleSheet(editorStyleSheet());
    MainWindow window; window.show(); createEmbeddedCanvas(window, 320, 200);
    auto *canvas = window.findChild<CanvasWidget *>();
    auto *bar = window.findChild<QWidget *>(QStringLiteral("transformBar")); QVERIFY(bar);
    for (int width : {900, 1440, 1873}) {
        window.resize(width, 860);
        QCoreApplication::processEvents();
        for (QAction *category : window.menuBar()->actions()) {
            const QRect geometry = window.menuBar()->actionGeometry(category);
            QVERIFY(!geometry.isEmpty()); QVERIFY(window.menuBar()->rect().contains(geometry));
        }
        for (auto tool : {CanvasWidget::Tool::Move, CanvasWidget::Tool::Wand, CanvasWidget::Tool::Marquee,
                         CanvasWidget::Tool::Lasso, CanvasWidget::Tool::Brush, CanvasWidget::Tool::Eraser,
                         CanvasWidget::Tool::Healing, CanvasWidget::Tool::Clone, CanvasWidget::Tool::Blur,
                         CanvasWidget::Tool::Gradient, CanvasWidget::Tool::Shape, CanvasWidget::Tool::Text,
                         CanvasWidget::Tool::Crop, CanvasWidget::Tool::Hand, CanvasWidget::Tool::Zoom, CanvasWidget::Tool::Eyedropper}) {
            canvas->setTool(tool); QCoreApplication::processEvents();
            const auto controls = bar->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
            for (auto *control : controls) {
                if (!control->isVisible()) continue;
                QVERIFY2(bar->rect().contains(control->geometry()), qPrintable(control->objectName()));
                if (auto *combo = qobject_cast<QComboBox *>(control)) {
                    QStyleOptionComboBox option; option.initFrom(combo); option.editable = combo->isEditable();
                    option.currentText = combo->currentText();
                    const QRect textRect = combo->style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, combo);
                    const QStringList labels = qobject_cast<QFontComboBox *>(combo) ? QStringList{combo->currentText()} : [&] {
                        QStringList result; for (int i = 0; i < combo->count(); ++i) result << combo->itemText(i); return result;
                    }();
                    for (const QString &label : labels)
                        QVERIFY2(combo->fontMetrics().horizontalAdvance(label) <= textRect.width(), qPrintable(label));
                }
                if (auto *field = qobject_cast<QAbstractSpinBox *>(control)) {
                    auto *edit = field->findChild<QLineEdit *>(); QVERIFY(edit);
                    QVERIFY2(edit->fontMetrics().horizontalAdvance(edit->text()) <= edit->contentsRect().width() - 4, qPrintable(edit->text()));
                }
                for (auto *other : controls) {
                    if (other == control || !other->isVisible()) continue;
                    QVERIFY2(!other->geometry().intersects(control->geometry()), qPrintable(control->objectName()));
                }
            }
            QCOMPARE(window.width(), width);
            if (qEnvironmentVariableIsSet("COMPOSITOR_CAPTURE_OPTIONS") &&
                (tool == CanvasWidget::Tool::Move || tool == CanvasWidget::Tool::Text || tool == CanvasWidget::Tool::Wand || tool == CanvasWidget::Tool::Clone))
                window.grab().save(QStringLiteral("/tmp/compositor-options-%1-%2.png").arg(width).arg(int(tool)));
        }
    }
    qApp->setStyleSheet(previousStyle);
}

void MainWindowTests::textFormattingPreservesSelectionAndReplacementEditor()
{
    MainWindow window; window.show(); createEmbeddedCanvas(window, 320, 200);
    auto *canvas = window.findChild<CanvasWidget *>(); canvas->setTool(CanvasWidget::Tool::Text);
    QCoreApplication::processEvents(); canvas->zoomOut();
    auto *done = window.findChild<QPushButton *>(QStringLiteral("textDone")); QVERIFY(!done->isEnabled());
    canvas->textBoxRequested(QRectF(12.5, 20.5, 80, 40), true);
    auto *editor = canvas->findChild<QTextEdit *>(QStringLiteral("inlineTextEditor")); QVERIFY(editor);
    editor->setPlainText(QStringLiteral("First paragraph\nSecond paragraph"));
    auto cursor = editor->textCursor(); cursor.setPosition(2); cursor.setPosition(7, QTextCursor::KeepAnchor); editor->setTextCursor(cursor);
    window.findChild<QToolButton *>(QStringLiteral("textBold"))->click();
    QCOMPARE(editor->textCursor().anchor(), 2); QCOMPARE(editor->textCursor().position(), 7);
    auto *size = window.findChild<QSpinBox *>(QStringLiteral("textSize")); size->setValue(36);
    QCOMPARE(editor->textCursor().charFormat().font().pixelSize(), qRound(36 * canvas->zoom()));
    QVERIFY(done->isEnabled());
    // Do not drain deferred deletes: a new editor must immediately be discoverable.
    canvas->textBoxRequested(QRectF(100, 30, 120, 80), true);
    auto *replacement = canvas->findChild<QTextEdit *>(QStringLiteral("inlineTextEditor"));
    QVERIFY(replacement); QVERIFY(replacement != editor);
    replacement->setPlainText(QStringLiteral("Replacement")); size->setValue(52);
    QCOMPARE(replacement->textCursor().charFormat().font().pixelSize(), qRound(52 * canvas->zoom()));
    window.findChild<QPushButton *>(QStringLiteral("textCancel"))->click();
    QVERIFY(!done->isEnabled());
    auto *layers = window.findChild<QListView *>(QStringLiteral("layerList")); QCOMPARE(layers->model()->rowCount(), 2);
    actionNamed(window, QStringLiteral("Undo"))->trigger(); QCOMPARE(layers->model()->rowCount(), 1);
}

void MainWindowTests::editingTextLayersPreservesTheirStyleAndSavesPendingText()
{
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("text.comp"));
    EditorSession session; session.createDocument(320, 200, true);
    const QRectF firstBox(12.5, 20.5, 60, 30);
    QVERIFY(session.addText(QStringLiteral("First"), firstBox, QStringLiteral("Sans"), 18, false, false, false, 0, Qt::red, true));
    const QUuid first = session.activeLayer()->id;
    QVERIFY(session.addText(QStringLiteral("Second"), QRectF(100, 40, 120, 60), QStringLiteral("Serif"), 32, true, false, false, 0, Qt::blue, true));
    const QUuid second = session.activeLayer()->id;
    ProjectWriter::save(*session.document(), path);
    MainWindow window; window.show(); QVERIFY(window.openProject(path));
    auto *canvas = window.findChild<CanvasWidget *>(); canvas->setTool(CanvasWidget::Tool::Text);
    QCoreApplication::processEvents(); canvas->actualPixels(); canvas->zoomOut();
    canvas->textLayerEditRequested(first);
    auto *editor = canvas->findChild<QTextEdit *>(QStringLiteral("inlineTextEditor")); QVERIFY(editor);
    editor->setPlainText(QStringLiteral("First edited"));
    canvas->textLayerEditRequested(second);
    editor = canvas->findChild<QTextEdit *>(QStringLiteral("inlineTextEditor")); QVERIFY(editor);
    QCOMPARE(editor->toPlainText(), QStringLiteral("Second"));
    editor->setPlainText(QStringLiteral("Second edited"));
    window.findChild<QAction *>(QStringLiteral("commandSave"))->trigger();
    QVERIFY(!canvas->findChild<QTextEdit *>(QStringLiteral("inlineTextEditor")));
    const Document saved = ProjectReader::load(path);
    for (const Layer &layer : saved.layers) {
        if (layer.id == first) {
            QCOMPARE(layer.shape.value(QStringLiteral("text")).toString(), QStringLiteral("First edited"));
            QCOMPARE(layer.shape.value(QStringLiteral("pixelSize")).toInt(), 18);
            QCOMPARE(layer.transform.origin, firstBox.topLeft()); QCOMPARE(layer.transform.size, firstBox.size());
            QCOMPARE(QColor(layer.shape.value(QStringLiteral("fill")).toString()), QColor(Qt::red));
            QVERIFY(layer.visible);
        } else if (layer.id == second) {
            QCOMPARE(layer.shape.value(QStringLiteral("text")).toString(), QStringLiteral("Second edited"));
            QCOMPARE(layer.shape.value(QStringLiteral("pixelSize")).toInt(), 32); QVERIFY(layer.visible);
        }
    }
}

void MainWindowTests::styledTextEditorPreviewsTheCommittedLayout()
{
    const QString previousStyle = qApp->styleSheet(); qApp->setStyleSheet(editorStyleSheet());
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("wysiwyg.comp"));
    EditorSession session; session.createDocument(800, 600, true); ProjectWriter::save(*session.document(), path);
    MainWindow window; window.show(); QVERIFY(window.openProject(path));
    auto *canvas = window.findChild<CanvasWidget *>(); canvas->setTool(CanvasWidget::Tool::Text);
    QCoreApplication::processEvents(); canvas->actualPixels();
    const QRectF box(40, 40, 420, 180); canvas->textBoxRequested(box, true);
    auto *editor = canvas->findChild<QTextEdit *>(QStringLiteral("inlineTextEditor")); QVERIFY(editor);
    auto *sizeField = window.findChild<QSpinBox *>(QStringLiteral("textSize"));
    QTest::mouseClick(sizeField, Qt::LeftButton, Qt::NoModifier, QPoint(sizeField->width() - 10, 7));
    QCOMPARE(sizeField->value(), 49);
    QTest::mouseClick(sizeField, Qt::LeftButton, Qt::NoModifier, QPoint(sizeField->width() - 10, sizeField->height() - 7));
    QCOMPARE(sizeField->value(), 48);
    editor->setFocus();
    QTest::keyClicks(editor, "WYSIWYG preview");
    QTest::keyClick(editor, Qt::Key_Return); QTest::keyClicks(editor, "Second line");
    QCoreApplication::processEvents();
    QVERIFY2(editor->cursorRect().height() >= 48, "Typing must use the selected 48 px font, not the 12 px UI font");
    QCOMPARE(editor->document()->defaultFont().pixelSize(), 48);
    QCOMPARE(editor->geometry(), canvas->widgetRectForDocumentRect(box).toAlignedRect());
    QImage preview(box.size().toSize(), QImage::Format_RGBA8888_Premultiplied); preview.fill(Qt::transparent);
    { QPainter painter(&preview); painter.translate(4, 3); editor->document()->drawContents(&painter, QRectF(0, 0, 412, 174)); }
    const int normalHeight = editor->cursorRect().height(); canvas->zoomOut(); QCoreApplication::processEvents();
    QVERIFY(editor->cursorRect().height() < normalHeight);
    QCOMPARE(editor->geometry(), canvas->widgetRectForDocumentRect(box).toAlignedRect());
    canvas->actualPixels();
    if (qEnvironmentVariableIsSet("COMPOSITOR_CAPTURE_OPTIONS")) window.grab().save(QStringLiteral("/tmp/compositor-text-wysiwyg.png"));
    window.findChild<QPushButton *>(QStringLiteral("textDone"))->click();
    window.findChild<QAction *>(QStringLiteral("commandSave"))->trigger();
    const Document saved = ProjectReader::load(path);
    if (qEnvironmentVariableIsSet("COMPOSITOR_CAPTURE_OPTIONS")) {
        saved.layers.constLast().image.save(QStringLiteral("/tmp/compositor-text-saved.png"));
        preview.save(QStringLiteral("/tmp/compositor-text-preview.png"));
    }
    QCOMPARE(saved.layers.constLast().image.convertToFormat(QImage::Format_RGBA8888), preview.convertToFormat(QImage::Format_RGBA8888));
    qApp->setStyleSheet(previousStyle);
}

void MainWindowTests::textFieldsKeepNativeEditingCommandsAndIgnoreEditorKeys()
{
    MainWindow window; window.show(); createEmbeddedCanvas(window, 32, 24);
    auto *canvas = window.findChild<CanvasWidget *>(); QVERIFY(canvas); QCOMPARE(canvas->tool(), CanvasWidget::Tool::Move);
    const auto shortcut = [&window](const QKeySequence &key) -> QAction * {
        for (QAction *action : window.findChildren<QAction *>()) if (action->shortcut() == key) return action;
        return nullptr;
    };
    auto *field = new QLineEdit(&window); field->setGeometry(80, 80, 180, 28); field->show(); field->raise();
    window.activateWindow(); QTest::qWait(1); field->setFocus(Qt::OtherFocusReason);
    QTRY_VERIFY(field->hasFocus());

    QAction *brush = shortcut(QKeySequence(Qt::Key_B)); QVERIFY(brush); brush->trigger();
    QCOMPARE(canvas->tool(), CanvasWidget::Tool::Move);

    field->setText(QStringLiteral("alpha beta")); field->setSelection(6, 4);
    QAction *copy = actionNamed(window, QStringLiteral("Copy")); QVERIFY(copy); copy->trigger();
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("beta"));
    QAction *cut = actionNamed(window, QStringLiteral("Cut")); QVERIFY(cut); cut->trigger(); QCOMPARE(field->text(), QStringLiteral("alpha "));
    QAction *undo = actionNamed(window, QStringLiteral("Undo")); QVERIFY(undo); undo->trigger(); QCOMPARE(field->text(), QStringLiteral("alpha beta"));
    QAction *redo = actionNamed(window, QStringLiteral("Redo")); QVERIFY(redo); redo->trigger(); QCOMPARE(field->text(), QStringLiteral("alpha "));
    QGuiApplication::clipboard()->setText(QStringLiteral("gamma")); field->setCursorPosition(field->text().size());
    QAction *paste = actionNamed(window, QStringLiteral("Paste")); QVERIFY(paste); paste->trigger(); QCOMPARE(field->text(), QStringLiteral("alpha gamma"));
    QAction *selectAll = actionNamed(window, QStringLiteral("Select All")); QVERIFY(selectAll); selectAll->trigger(); QCOMPARE(field->selectedText(), field->text());

    field->setText(QStringLiteral("one two")); field->setCursorPosition(field->text().size());
    QAction *fillForeground = actionNamed(window, QStringLiteral("Fill with Foreground Color")); QVERIFY(fillForeground); fillForeground->trigger(); QCOMPARE(field->text(), QStringLiteral("one "));
    QAction *fillBackground = actionNamed(window, QStringLiteral("Fill with Background Color")); QVERIFY(fillBackground); fillBackground->trigger(); QCOMPARE(field->text(), QString());
    field->setText(QStringLiteral("abc")); field->setCursorPosition(1);
    QAction *remove = actionNamed(window, QStringLiteral("Delete Layer")); QVERIFY(remove); remove->trigger(); QCOMPARE(field->text(), QStringLiteral("ac"));
}

void MainWindowTests::commandStatesAndTitlesFollowTheDocument()
{
    MainWindow window; window.show();
    const auto command = [&window](const char *name) { return window.findChild<QAction *>(QString::fromLatin1(name)); };
    QAction *save = command("commandSave"), *transform = command("commandTransform"), *duplicate = command("commandDuplicate");
    QAction *remove = command("commandDelete"), *visibility = command("commandVisibility"), *merge = command("commandMerge"), *clipping = command("commandClipping");
    QVERIFY(save); QVERIFY(transform); QVERIFY(duplicate); QVERIFY(remove); QVERIFY(visibility); QVERIFY(merge); QVERIFY(clipping);
    QVERIFY(!save->isEnabled()); QVERIFY(!transform->isEnabled()); QCOMPARE(plainText(transform->text()), QStringLiteral("Transform Layer"));

    createEmbeddedCanvas(window, 32, 24);
    QVERIFY(save->isEnabled()); QVERIFY(!transform->isEnabled()); QCOMPARE(plainText(duplicate->text()), QStringLiteral("Duplicate Layer"));
    QAction *fill = actionNamed(window, QStringLiteral("Fill with Foreground Color")); QVERIFY(fill); fill->trigger();
    QVERIFY(transform->isEnabled()); QVERIFY(!merge->isEnabled());

    QAction *selectAll = actionNamed(window, QStringLiteral("Select All")); QVERIFY(selectAll); selectAll->trigger();
    QCOMPARE(plainText(transform->text()), QStringLiteral("Transform Selection")); QCOMPARE(plainText(duplicate->text()), QStringLiteral("Layer via Copy"));
    QAction *deselect = actionNamed(window, QStringLiteral("Deselect")); QVERIFY(deselect); deselect->trigger();
    QCOMPARE(plainText(transform->text()), QStringLiteral("Transform Layer"));

    QAction *newLayer = actionNamed(window, QStringLiteral("New Layer")); QVERIFY(newLayer); newLayer->trigger();
    QVERIFY(merge->isEnabled()); QCOMPARE(plainText(merge->text()), QStringLiteral("Merge Down"));
    QVERIFY(clipping->isEnabled()); QCOMPARE(plainText(clipping->text()), QStringLiteral("Create Clipping Mask"));
    clipping->trigger(); QCOMPARE(plainText(clipping->text()), QStringLiteral("Release Clipping Mask"));
    visibility->trigger(); QCOMPARE(plainText(visibility->text()), QStringLiteral("Show Layer"));
    QCOMPARE(plainText(remove->text()), QStringLiteral("Delete Layer"));
}

void MainWindowTests::opacityDragCreatesOneUndoEntry()
{
    MainWindow window; window.show();
    createEmbeddedCanvas(window);
    auto *opacity = window.findChild<QSlider *>(QStringLiteral("layerOpacity")); QVERIFY(opacity); QCOMPARE(opacity->value(), 100);
    QVERIFY(QMetaObject::invokeMethod(opacity, "sliderPressed"));
    opacity->setValue(80); opacity->setValue(60); opacity->setValue(35);
    QVERIFY(QMetaObject::invokeMethod(opacity, "sliderReleased"));
    QCOMPARE(opacity->value(), 35);
    QAction *undo = actionNamed(window, QStringLiteral("Undo")); QVERIFY(undo); undo->trigger();
    QCOMPARE(opacity->value(), 100);
    QTest::mouseClick(opacity, Qt::LeftButton, {}, QPoint(opacity->width() / 4, opacity->height() / 2));
    QVERIFY(opacity->value() >= 20 && opacity->value() <= 30); undo->trigger(); QCOMPARE(opacity->value(), 100);
}

void MainWindowTests::newCanvasSuggestsClipboardImageDimensions()
{
    QImage clipboard(64, 32, QImage::Format_RGBA8888); clipboard.fill(Qt::red);
    QGuiApplication::clipboard()->setImage(clipboard);
    MainWindow window; window.show();
    QAction *create = actionNamed(window, QStringLiteral("New Canvas")); QVERIFY(create);
    create->trigger();
    QVERIFY(!QApplication::activeModalWidget());
    QCOMPARE(window.findChild<QTabBar *>()->count(), 2);
    QCOMPARE(window.findChild<QSpinBox *>(QStringLiteral("newCanvasWidth"))->value(), 64);
    QCOMPARE(window.findChild<QSpinBox *>(QStringLiteral("newCanvasHeight"))->value(), 32);
    QGuiApplication::clipboard()->clear();
}

void MainWindowTests::imageSizeCanChangeResolutionWithoutResampling()
{
    MainWindow window; window.show();
    createEmbeddedCanvas(window);
    QAction *imageSize = actionNamed(window, QStringLiteral("Image Size")); QVERIFY(imageSize);
    QTimer::singleShot(0, &window, [] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog);
        auto *resample = dialog->findChild<QCheckBox *>(QStringLiteral("imageSizeResample")); QVERIFY(resample); resample->setChecked(false);
        QVERIFY(dialog->findChild<QDoubleSpinBox *>(QStringLiteral("imageSizeWidth"))->isEnabled());
        QVERIFY(!dialog->findChild<QComboBox *>(QStringLiteral("imageSizeSampling"))->isEnabled());
        dialog->findChild<QDoubleSpinBox *>(QStringLiteral("imageSizeResolution"))->setValue(300);
        dialog->accept();
    });
    imageSize->trigger();
    QTimer::singleShot(0, &window, [] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog);
        QCOMPARE(dialog->findChild<QDoubleSpinBox *>(QStringLiteral("imageSizeWidth"))->value(), 1920.0);
        QCOMPARE(dialog->findChild<QDoubleSpinBox *>(QStringLiteral("imageSizeHeight"))->value(), 1080.0);
        QCOMPARE(dialog->findChild<QDoubleSpinBox *>(QStringLiteral("imageSizeResolution"))->value(), 300.0);
        dialog->reject();
    });
    imageSize->trigger();
}

void MainWindowTests::imageSizeUnitsResolutionAndResamplingMatchOriginalCalculations()
{
    MainWindow window; window.show(); createEmbeddedCanvas(window, 1000, 500);
    QAction *imageSize = actionNamed(window, QStringLiteral("Image Size")); QVERIFY(imageSize);
    QTimer::singleShot(0, &window, [] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog);
        auto *units = dialog->findChild<QComboBox *>(QStringLiteral("imageSizeUnits"));
        auto *width = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("imageSizeWidth"));
        auto *height = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("imageSizeHeight"));
        auto *resolution = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("imageSizeResolution"));
        auto *resample = dialog->findChild<QCheckBox *>(QStringLiteral("imageSizeResample"));
        QVERIFY(units); QVERIFY(width); QVERIFY(height); QVERIFY(resolution); QVERIFY(resample);
        units->setCurrentIndex(1); QCOMPARE(width->value(), 100.0); QCOMPARE(height->value(), 100.0);
        width->setValue(50); QCOMPARE(height->value(), 50.0);
        units->setCurrentIndex(2); QVERIFY(qAbs(width->value() - 500.0 / 72.0) < .002);
        resolution->setValue(144); QVERIFY(qAbs(width->value() - 500.0 / 72.0) < .002); QVERIFY(qAbs(height->value() - 250.0 / 72.0) < .002);
        units->setCurrentIndex(0); QCOMPARE(width->value(), 1000.0); QCOMPARE(height->value(), 500.0);
        resample->setChecked(false); QCOMPARE(units->currentIndex(), 2); QVERIFY(!dialog->findChild<QComboBox *>(QStringLiteral("imageSizeSampling"))->isEnabled());
        width->setValue(10); QCOMPARE(resolution->value(), 100.0); QVERIFY(qAbs(height->value() - 5) < .002);
        dialog->reject();
    });
    imageSize->trigger();
}

void MainWindowTests::colorPickerIsNonModalSamplesCanvasAndCommitsOnlyOnAccept()
{
    MainWindow window; window.show(); createEmbeddedCanvas(window, 4, 4);
    auto *foreground = window.findChild<QLabel *>(QStringLiteral("foregroundSwatch"));
    auto *background = window.findChild<QLabel *>(QStringLiteral("backgroundSwatch"));
    auto *canvas = window.findChild<CanvasWidget *>();
    QVERIFY(foreground); QVERIFY(background); QVERIFY(canvas);

    QTest::mouseClick(foreground, Qt::LeftButton);
    auto *picker = window.findChild<QColorDialog *>(QStringLiteral("paletteColorPicker")); QVERIFY(picker);
    QVERIFY(picker->isVisible()); QVERIFY(!picker->isModal());
    QCOMPARE(picker->currentColor().toRgb(), QColor(22, 134, 232).toRgb());
    picker->move(37, 53);
    picker->setCurrentColor(Qt::green); picker->reject(); QCoreApplication::processEvents();

    QTest::mouseClick(foreground, Qt::LeftButton);
    picker = window.findChild<QColorDialog *>(QStringLiteral("paletteColorPicker")); QVERIFY(picker);
    QCOMPARE(picker->currentColor().toRgb(), QColor(22, 134, 232).toRgb());
    QCOMPARE(picker->pos(), QPoint(37, 53));
    picker->setCurrentColor(Qt::red); picker->accept(); QCoreApplication::processEvents();
    QAction *fill = actionNamed(window, QStringLiteral("Fill with Foreground Color")); QVERIFY(fill); fill->trigger();

    const QString backgroundBefore = background->styleSheet();
    QTest::mouseClick(background, Qt::LeftButton);
    picker = window.findChild<QColorDialog *>(QStringLiteral("paletteColorPicker")); QVERIFY(picker);
    QVERIFY(QMetaObject::invokeMethod(canvas, "colorSampleRequested", Q_ARG(QPoint, QPoint(0, 0))));
    QCOMPARE(picker->currentColor().toRgb(), QColor(Qt::red).toRgb());
    QCOMPARE(background->styleSheet(), backgroundBefore);
    picker->accept(); QCoreApplication::processEvents();

    QTest::mouseClick(background, Qt::LeftButton);
    picker = window.findChild<QColorDialog *>(QStringLiteral("paletteColorPicker")); QVERIFY(picker);
    QCOMPARE(picker->currentColor().toRgb(), QColor(Qt::red).toRgb());
    picker->reject();
}

void MainWindowTests::jpegExportShowsEncodedPreviewBeforeDestination()
{
    MainWindow window; window.show(); createEmbeddedCanvas(window, 64, 32);
    QAction *exportJpeg = actionNamed(window, QStringLiteral("Export JPEG")); QVERIFY(exportJpeg);
    QTimer::singleShot(0, &window, [] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog);
        QCOMPARE(dialog->objectName(), QStringLiteral("jpegExportDialog"));
        auto *preview = dialog->findChild<QLabel *>(QStringLiteral("jpegPreview")); QVERIFY(preview); QVERIFY(!preview->pixmap().isNull());
        auto *quality = dialog->findChild<QSlider *>(QStringLiteral("jpegQuality")); QVERIFY(quality); QCOMPARE(quality->value(), 85);
        auto *result = dialog->findChild<QLabel *>(QStringLiteral("jpegResult")); QVERIFY(result); QVERIFY(result->text().contains(QStringLiteral("encoded preview")));
        dialog->reject();
    });
    exportJpeg->trigger();
}

void MainWindowTests::newTabsCommitPendingTransformsToTheirOriginalDocument()
{
    MainWindow window; window.show(); createEmbeddedCanvas(window, 64, 48);
    QAction *fill = actionNamed(window, QStringLiteral("Fill with Foreground Color")); QVERIFY(fill); fill->trigger();
    auto *x = window.findChild<QDoubleSpinBox *>(QStringLiteral("transformX"));
    auto *apply = window.findChild<QPushButton *>(QStringLiteral("primaryButton"));
    auto *cancel = window.findChild<QPushButton *>(QStringLiteral("transformCancel"));
    auto *tabs = window.findChild<QTabBar *>();
    auto *dimensions = window.findChild<QDoubleSpinBox *>(QStringLiteral("transformWidth"));
    QVERIFY(x); QVERIFY(apply); QVERIFY(cancel); QVERIFY(tabs); QVERIFY(dimensions);
    x->setValue(12); QVERIFY(QMetaObject::invokeMethod(x, "editingFinished")); QVERIFY(apply->isEnabled());
    QAction *newCanvas = actionNamed(window, QStringLiteral("New Canvas")); QVERIFY(newCanvas); newCanvas->trigger();
    QCOMPARE(tabs->count(), 2); QVERIFY(!apply->isEnabled()); QVERIFY(!cancel->isEnabled());
    createEmbeddedCanvas(window, 32, 24);
    QCOMPARE(dimensions->value(), 32.0);
    tabs->setCurrentIndex(0); QCOMPARE(x->value(), 12.0); QCOMPARE(dimensions->value(), 64.0);
    QAction *undo = actionNamed(window, QStringLiteral("Undo")); QVERIFY(undo); undo->trigger();
    QCOMPARE(x->value(), 0.0);
    tabs->setCurrentIndex(1); QCOMPARE(dimensions->value(), 32.0); QCOMPARE(x->value(), 0.0);
}

void MainWindowTests::numericTransformsStayPendingAndApplyOrCancelAsOneEdit()
{
    MainWindow window; window.show(); createEmbeddedCanvas(window, 64, 48);
    QAction *fill = actionNamed(window, QStringLiteral("Fill with Foreground Color")); QVERIFY(fill); fill->trigger();
    auto *x = window.findChild<QDoubleSpinBox *>(QStringLiteral("transformX"));
    auto *apply = window.findChild<QPushButton *>(QStringLiteral("primaryButton"));
    auto *cancel = window.findChild<QPushButton *>(QStringLiteral("transformCancel"));
    auto *canvas = window.findChild<CanvasWidget *>();
    QVERIFY(x); QVERIFY(apply); QVERIFY(cancel); QVERIFY(canvas); QCOMPARE(x->value(), 0.0);

    auto *autoSelect = window.findChild<QCheckBox *>(QStringLiteral("transformAutoSelect"));
    auto *showControls = window.findChild<QCheckBox *>(QStringLiteral("transformShowControls"));
    auto *ratioLock = window.findChild<QToolButton *>(QStringLiteral("transformRatioLock"));
    auto *width = window.findChild<QDoubleSpinBox *>(QStringLiteral("transformWidth"));
    auto *height = window.findChild<QDoubleSpinBox *>(QStringLiteral("transformHeight"));
    auto *scale = window.findChild<QDoubleSpinBox *>(QStringLiteral("transformScale"));
    auto *rotation = window.findChild<QDoubleSpinBox *>(QStringLiteral("transformRotation"));
    QVERIFY(autoSelect); QVERIFY(showControls); QVERIFY(ratioLock); QVERIFY(width); QVERIFY(height); QVERIFY(scale); QVERIFY(rotation);
    QVERIFY(!canvas->transformAutoSelects()); QVERIFY(canvas->showsTransformControls()); QVERIFY(canvas->locksTransformRatio());
    autoSelect->setChecked(true); showControls->setChecked(false); ratioLock->setChecked(false);
    QVERIFY(canvas->transformAutoSelects()); QVERIFY(!canvas->showsTransformControls()); QVERIFY(!canvas->locksTransformRatio());
    autoSelect->setChecked(false); showControls->setChecked(true); ratioLock->setChecked(true);

    x->setValue(12); QVERIFY(QMetaObject::invokeMethod(x, "editingFinished"));
    QVERIFY(apply->isEnabled()); QVERIFY(cancel->isEnabled()); QCOMPARE(x->value(), 12.0);
    cancel->click(); QCOMPARE(x->value(), 0.0); QVERIFY(!apply->isEnabled());

    x->setValue(5); QVERIFY(QMetaObject::invokeMethod(x, "editingFinished"));
    QTest::keyClick(canvas, Qt::Key_Right); QCOMPARE(x->value(), 6.0);
    QTest::keyClick(canvas, Qt::Key_Escape); QCOMPARE(x->value(), 0.0);

    x->setValue(9); QVERIFY(QMetaObject::invokeMethod(x, "editingFinished")); apply->click(); QCOMPARE(x->value(), 9.0);
    QAction *undo = actionNamed(window, QStringLiteral("Undo")); QVERIFY(undo); undo->trigger(); QCOMPARE(x->value(), 0.0);

    QCOMPARE(width->value(), 64.0); QCOMPARE(height->value(), 48.0);
    width->setValue(96); QVERIFY(QMetaObject::invokeMethod(width, "editingFinished"));
    QCOMPARE(width->value(), 96.0); QCOMPARE(height->value(), 72.0);
    cancel->click(); QCOMPARE(width->value(), 64.0); QCOMPARE(height->value(), 48.0);

    ratioLock->setChecked(false);
    rotation->setValue(15); QVERIFY(QMetaObject::invokeMethod(rotation, "editingFinished"));
    QVERIFY(apply->isEnabled()); apply->click(); QCOMPARE(rotation->value(), 15.0);
    undo->trigger(); QCOMPARE(rotation->value(), 0.0);

    scale->setValue(150); QVERIFY(QMetaObject::invokeMethod(scale, "editingFinished"));
    QCOMPARE(width->value(), 96.0); QCOMPARE(height->value(), 72.0); apply->click();
    undo->trigger(); QCOMPARE(width->value(), 64.0); QCOMPARE(height->value(), 48.0); QCOMPARE(x->value(), 0.0);
}

void MainWindowTests::canvasSizeOffersAllExtensionChoices()
{
    MainWindow window; window.show();
    createEmbeddedCanvas(window);
    QAction *canvasSize = actionNamed(window, QStringLiteral("Canvas Size")); QVERIFY(canvasSize);
    QTimer::singleShot(0, &window, [] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog);
        auto *extension = dialog->findChild<QComboBox *>(QStringLiteral("canvasExtension")); QVERIFY(extension);
        QStringList values; for (int i = 0; i < extension->count(); ++i) values << extension->itemText(i);
        QCOMPARE(values, QStringList({QStringLiteral("Transparent"), QStringLiteral("Foreground"), QStringLiteral("Background"),
            QStringLiteral("Black"), QStringLiteral("White"), QStringLiteral("Custom")}));
        dialog->reject();
    });
    canvasSize->trigger();
}

void MainWindowTests::canvasSizeRelativeRatioAndUnitsMatchOriginalCalculations()
{
    MainWindow window; window.show(); createEmbeddedCanvas(window, 1000, 500);
    QAction *canvasSize = actionNamed(window, QStringLiteral("Canvas Size")); QVERIFY(canvasSize);
    QTimer::singleShot(0, &window, [] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog);
        auto *units = dialog->findChild<QComboBox *>(QStringLiteral("canvasSizeUnits"));
        auto *width = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("canvasSizeWidth"));
        auto *height = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("canvasSizeHeight"));
        auto *relative = dialog->findChild<QCheckBox *>(QStringLiteral("canvasSizeRelative"));
        auto *locked = dialog->findChild<QCheckBox *>(QStringLiteral("canvasSizeLocked"));
        QVERIFY(units); QVERIFY(width); QVERIFY(height); QVERIFY(relative); QVERIFY(locked);
        relative->setChecked(true); locked->setChecked(true); width->setValue(200);
        QCOMPARE(width->value(), 200.0); QCOMPARE(height->value(), 100.0);
        height->setValue(-250); QCOMPARE(width->value(), -500.0); QCOMPARE(height->value(), -250.0);
        relative->setChecked(false); QCOMPARE(width->value(), 500.0); QCOMPARE(height->value(), 250.0);
        units->setCurrentIndex(2); QVERIFY(qAbs(width->value() - 500.0 / 72.0) < .002);
        width->setValue(10); QVERIFY(qAbs(height->value() - 5) < .002);
        units->setCurrentIndex(1); QVERIFY(qAbs(width->value() - 72) < .002); QVERIFY(qAbs(height->value() - 72) < .002);
        width->setValue(50); QCOMPARE(height->value(), 50.0);
        units->setCurrentIndex(3); QVERIFY(qAbs(width->value() - (500.0 / 72.0 * 2.54)) < .002);
        dialog->reject();
    });
    canvasSize->trigger();
}

void MainWindowTests::blendModeShortcutsCycleAndWrap()
{
    MainWindow window; window.show();
    createEmbeddedCanvas(window);
    auto *blend = window.findChild<QComboBox *>(QStringLiteral("blendMode")); QVERIFY(blend); QCOMPARE(blend->currentIndex(), 0);
    auto *next = window.findChild<QAction *>(QStringLiteral("nextBlendMode")); QVERIFY(next); next->trigger();
    QCOMPARE(blend->currentIndex(), 1);
    auto *previous = window.findChild<QAction *>(QStringLiteral("previousBlendMode")); QVERIFY(previous); previous->trigger(); previous->trigger();
    QCOMPARE(blend->currentIndex(), int(BlendMode::Luminosity));
    QAction *undo = actionNamed(window, QStringLiteral("Undo")); QVERIFY(undo); undo->trigger();
    QCOMPARE(blend->currentIndex(), 0);

    QLineEdit textField; textField.show(); textField.activateWindow(); textField.setFocus(); QTest::qWait(1);
    next->trigger();
    QCOMPARE(blend->currentIndex(), 0);
}

void MainWindowTests::opacityDigitsSupportExactTwoDigitValuesAndGradient()
{
    MainWindow window; window.show();
    createEmbeddedCanvas(window);
    const auto digitAction = [&window](int digit) -> QAction * {
        for (QAction *action : window.findChildren<QAction *>()) if (action->shortcut() == QKeySequence(Qt::Key_0 + digit)) return action;
        return nullptr;
    };
    QAction *four = digitAction(4), *five = digitAction(5); QVERIFY(four); QVERIFY(five);
    auto *layers = window.findChild<QListView *>(QStringLiteral("layerList")); QVERIFY(layers); window.activateWindow(); layers->setFocus(); QTest::qWait(20);
    QTest::keyClick(layers, Qt::Key_4); QTest::keyClick(layers, Qt::Key_5);
    QCOMPARE(window.findChild<QSlider *>(QStringLiteral("layerOpacity"))->value(), 45);

    QAction *gradient = nullptr;
    for (QAction *action : window.findChildren<QAction *>()) if (action->shortcut() == QKeySequence(Qt::Key_G)) gradient = action;
    QVERIFY(gradient); gradient->trigger();
    QAction *three = digitAction(3); QVERIFY(three); three->trigger(); five->trigger();
    QCOMPARE(window.findChild<QDoubleSpinBox *>(QStringLiteral("gradientOpacity"))->value(), 35.0);
}

void MainWindowTests::layerThumbnailsUseCanvasPlacementAndMaskEdgeTone()
{
    auto document = std::make_shared<Document>(); document->canvasSize = QSize(400, 200);
    Layer layer; layer.id = QUuid::createUuid(); layer.name = QStringLiteral("Placed"); layer.transform.size = QSizeF(100, 100);
    layer.image = QImage(100, 100, QImage::Format_RGBA8888_Premultiplied); layer.image.fill(Qt::red);
    layer.mask = QImage(20, 20, QImage::Format_Grayscale8); layer.mask.fill(255);
    QPainter maskPainter(&layer.mask); maskPainter.fillRect(QRect(5, 5, 10, 10), Qt::black); maskPainter.end();
    layer.maskPlacement = LayerTransform{QPointF(100, 50), QSizeF(100, 100)};
    document->layers = {layer};
    LayerListModel model; model.setDocument(document); const QModelIndex index = model.index(0);
    const QIcon icon = qvariant_cast<QIcon>(model.data(index, Qt::DecorationRole));
    QCOMPARE(icon.actualSize(QSize(36, 36)), QSize(36, 18));
    const QImage pixels = icon.pixmap(QSize(36, 18)).toImage();
    QVERIFY(pixels.pixelColor(2, 2).red() > 240);
    QVERIFY(pixels.pixelColor(30, 15).red() < 150);
    const QImage mask = model.data(index, Qt::UserRole + 3).value<QImage>();
    QCOMPARE(mask.size(), QSize(36, 18));
    QVERIFY(qGray(mask.pixel(2, 2)) > 245);
    QVERIFY(qGray(mask.pixel(14, 9)) < 10);
}

void MainWindowTests::projectTabsKeepIndependentDocumentsAndUndo()
{
    MainWindow window; window.show();
    createEmbeddedCanvas(window, 80, 60);
    auto *layers = window.findChild<QListView *>(QStringLiteral("layerList")); QVERIFY(layers);
    QAction *newLayer = actionNamed(window, QStringLiteral("New Layer")); QVERIFY(newLayer); newLayer->trigger();
    QCOMPARE(layers->model()->rowCount(), 2);

    QAction *newCanvas = actionNamed(window, QStringLiteral("New Canvas")); QVERIFY(newCanvas); newCanvas->trigger();
    auto *tabs = window.findChild<QTabBar *>(); QVERIFY(tabs); QCOMPARE(tabs->count(), 2);
    QVERIFY(!QApplication::activeModalWidget()); QCOMPARE(layers->model()->rowCount(), 0);
    createEmbeddedCanvas(window, 20, 10); newLayer->trigger(); QCOMPARE(layers->model()->rowCount(), 2);
    QAction *undo = actionNamed(window, QStringLiteral("Undo")); QVERIFY(undo); undo->trigger(); QCOMPARE(layers->model()->rowCount(), 1);

    tabs->setCurrentIndex(0); QCOMPARE(layers->model()->rowCount(), 2);
    tabs->setCurrentIndex(1); QCOMPARE(layers->model()->rowCount(), 1);
}

void MainWindowTests::layerDropsCopyAcrossProjectsAndIntoNewTabs()
{
    MainWindow window; window.show(); createEmbeddedCanvas(window, 100, 80);
    auto *layers = window.findChild<QListView *>(QStringLiteral("layerList")); QVERIFY(layers); QCOMPARE(layers->model()->rowCount(), 1);
    std::unique_ptr<QMimeData> sourceMime(layers->model()->mimeData({layers->model()->index(0, 0)}));
    const QByteArray sourceId = sourceMime->data(QStringLiteral("application/x-compositor-layers")); QVERIFY(!sourceId.isEmpty());

    QAction *newCanvas = actionNamed(window, QStringLiteral("New Canvas")); QVERIFY(newCanvas); newCanvas->trigger();
    auto *tabs = window.findChild<QTabBar *>(); QVERIFY(tabs); QCOMPARE(tabs->count(), 2);
    QDragEnterEvent tabEnter(tabs->tabRect(1).center(), Qt::CopyAction, sourceMime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tabs, &tabEnter); QVERIFY(tabEnter.isAccepted());
    QDropEvent tabDrop(QPointF(tabs->tabRect(1).center()), Qt::CopyAction, sourceMime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tabs, &tabDrop); QVERIFY(tabDrop.isAccepted());
    QCOMPARE(layers->model()->rowCount(), 1);
    std::unique_ptr<QMimeData> copiedMime(layers->model()->mimeData({layers->model()->index(0, 0)}));
    QVERIFY(copiedMime->data(QStringLiteral("application/x-compositor-layers")) != sourceId);
    QAction *undo = actionNamed(window, QStringLiteral("Undo")); QVERIFY(undo); undo->trigger(); QCOMPARE(layers->model()->rowCount(), 0);

    auto *newTab = window.findChild<QToolButton *>(QStringLiteral("newTabButton")); QVERIFY(newTab);
    QDragEnterEvent newEnter(newTab->rect().center(), Qt::CopyAction, sourceMime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(newTab, &newEnter); QVERIFY(newEnter.isAccepted());
    QDropEvent newDrop(QPointF(newTab->rect().center()), Qt::CopyAction, sourceMime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(newTab, &newDrop); QVERIFY(newDrop.isAccepted());
    QCOMPARE(tabs->count(), 3); QCOMPARE(layers->model()->rowCount(), 1);
    tabs->setCurrentIndex(0); QCOMPARE(layers->model()->rowCount(), 1);
}

void MainWindowTests::imageDropsCreateNewTabsOrTargetExistingTabs()
{
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QString firstPath = directory.filePath(QStringLiteral("first.png"));
    const QString secondPath = directory.filePath(QStringLiteral("second.png"));
    QImage first(5, 3, QImage::Format_RGBA8888); first.fill(Qt::red); QVERIFY(first.save(firstPath));
    QImage second(7, 4, QImage::Format_RGBA8888); second.fill(Qt::blue); QVERIFY(second.save(secondPath));
    QMimeData files; files.setUrls({QUrl::fromLocalFile(firstPath), QUrl::fromLocalFile(secondPath)});

    MainWindow window; window.show();
    auto *newTab = window.findChild<QToolButton *>(QStringLiteral("newTabButton")); QVERIFY(newTab);
    QDragEnterEvent enter(newTab->rect().center(), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(newTab, &enter); QVERIFY(enter.isAccepted());
    QDropEvent drop(QPointF(newTab->rect().center()), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(newTab, &drop); QVERIFY(drop.isAccepted());
    auto *tabs = window.findChild<QTabBar *>(); auto *layers = window.findChild<QListView *>(QStringLiteral("layerList"));
    QVERIFY(tabs); QVERIFY(layers); QCOMPARE(tabs->count(), 2); QCOMPARE(layers->model()->rowCount(), 1);
    tabs->setCurrentIndex(0); QCOMPARE(layers->model()->rowCount(), 1);

    QDragEnterEvent targetEnter(tabs->tabRect(0).center(), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tabs, &targetEnter); QVERIFY(targetEnter.isAccepted());
    QDropEvent targetDrop(QPointF(tabs->tabRect(0).center()), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tabs, &targetDrop); QVERIFY(targetDrop.isAccepted());
    QCOMPARE(tabs->count(), 2); QCOMPARE(layers->model()->rowCount(), 3);

    QMimeData imagePayload; QImage promised(9, 6, QImage::Format_RGBA8888); promised.fill(Qt::green); imagePayload.setImageData(promised);
    QDragEnterEvent promisedEnter(newTab->rect().center(), Qt::CopyAction, &imagePayload, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(newTab, &promisedEnter); QVERIFY(promisedEnter.isAccepted());
    QDropEvent promisedDrop(QPointF(newTab->rect().center()), Qt::CopyAction, &imagePayload, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(newTab, &promisedDrop); QVERIFY(promisedDrop.isAccepted()); QCOMPARE(tabs->count(), 3); QCOMPARE(layers->model()->rowCount(), 1);
}

void MainWindowTests::hueSaturationExposesRangeSamplingAndTargetedControls()
{
    MainWindow window; window.show(); createEmbeddedCanvas(window, 64, 48);
    auto *canvas = window.findChild<CanvasWidget *>(); QVERIFY(canvas); QCOMPARE(canvas->tool(), CanvasWidget::Tool::Move);
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = window.findChild<QDialog *>(QStringLiteral("hueSaturationDialog")); QVERIFY(dialog); QVERIFY(dialog->isVisible());
        auto *range = dialog->findChild<QComboBox *>(QStringLiteral("hueRange")); QVERIFY(range); range->setCurrentIndex(1);
        auto *spectrum = dialog->findChild<QWidget *>(QStringLiteral("hueSpectrum")); QVERIFY(spectrum); QVERIFY(spectrum->isVisible());
        auto *hue = dialog->findChild<QSpinBox *>(QStringLiteral("hueValue")); auto *hueSlider = dialog->findChild<QSlider *>(QStringLiteral("hueValueSlider")); QVERIFY(hue); QVERIFY(hueSlider); hueSlider->setValue(750); QCOMPARE(hue->value(), 90);
        auto *sample = dialog->findChild<QToolButton *>(QStringLiteral("hueSample")); QVERIFY(sample); QVERIFY(sample->isVisible()); sample->click();
        QCOMPARE(canvas->tool(), CanvasWidget::Tool::Eyedropper);
        auto *targeted = dialog->findChild<QToolButton *>(QStringLiteral("hueTargetedAdjustment")); QVERIFY(targeted); targeted->click();
        QVERIFY(targeted->isChecked()); QCOMPARE(canvas->tool(), CanvasWidget::Tool::Move);
        dialog->reject();
    });
    QAction *action = actionNamed(window, QStringLiteral("Hue/Saturation")); QVERIFY(action); action->trigger();
    QCOMPARE(canvas->tool(), CanvasWidget::Tool::Move);
}

void MainWindowTests::liveFilterPreviewsUseFloatingDialogs()
{
    MainWindow window; window.show(); createEmbeddedCanvas(window, 32, 24);
    auto *canvas = window.findChild<CanvasWidget *>(); QVERIFY(canvas);
    QTimer::singleShot(0, &window, [&] {
        QDialog *dialog = nullptr;
        for (QDialog *candidate : window.findChildren<QDialog *>()) if (candidate->windowTitle() == QStringLiteral("Gaussian Blur")) dialog = candidate;
        QVERIFY(dialog); QVERIFY(dialog->isVisible()); QCOMPARE(dialog->windowModality(), Qt::NonModal); QVERIFY(window.isEnabled());
        QVERIFY(canvas->editorInteractionBlocked()); for (const QString &name : {QStringLiteral("tabBar"), QStringLiteral("transformBar"), QStringLiteral("toolRail"), QStringLiteral("inspector")}) { QWidget *chrome = window.findChild<QWidget *>(name); QVERIFY(chrome); QVERIFY(!chrome->isEnabled()); }
        dialog->reject();
    });
    QAction *action = actionNamed(window, QStringLiteral("Gaussian Blur")); QVERIFY(action); action->trigger();
    QVERIFY(!canvas->editorInteractionBlocked()); QVERIFY(window.findChild<QWidget *>(QStringLiteral("inspector"))->isEnabled());
}

void MainWindowTests::curvesGraphAddsSelectsAndDragsPoints()
{
    MainWindow window; window.show(); createEmbeddedCanvas(window, 32, 24);
    QTimer::singleShot(0, &window, [&] {
        QDialog *dialog = nullptr; for (QDialog *candidate : window.findChildren<QDialog *>()) if (candidate->windowTitle() == QStringLiteral("Curves")) dialog = candidate;
        QVERIFY(dialog); auto *graph = dialog->findChild<QWidget *>(QStringLiteral("curvesGraph")); QVERIFY(graph); QVERIFY(graph->height() >= 260);
        auto *input = dialog->findChild<QSpinBox *>(QStringLiteral("curveInput")); auto *output = dialog->findChild<QSpinBox *>(QStringLiteral("curveOutput")); auto *remove = dialog->findChild<QPushButton *>(QStringLiteral("removeCurvePoint"));
        QVERIFY(input); QVERIFY(output); QVERIFY(remove); QVERIFY(!remove->isEnabled());
        const QPoint added(graph->width() / 2, graph->height() / 4); QTest::mouseClick(graph, Qt::LeftButton, {}, added);
        QVERIFY(remove->isEnabled()); QVERIFY(input->value() >= 126 && input->value() <= 129); QVERIFY(output->value() >= 190 && output->value() <= 193);
        QTest::mousePress(graph, Qt::LeftButton, {}, added); QTest::mouseMove(graph, QPoint(added.x(), graph->height() * 3 / 4)); QTest::mouseRelease(graph, Qt::LeftButton, {}, QPoint(added.x(), graph->height() * 3 / 4));
        QVERIFY(output->value() >= 62 && output->value() <= 65); dialog->reject();
    });
    QAction *action = actionNamed(window, QStringLiteral("Curves")); QVERIFY(action); action->trigger();
}

void MainWindowTests::documentMutationsInvalidateTheCachedCanvas()
{
    MainWindow window; window.resize(900, 650); window.show(); createEmbeddedCanvas(window, 32, 24);
    auto *canvas = window.findChild<CanvasWidget *>(); auto *opacity = window.findChild<QSlider *>(QStringLiteral("layerOpacity")); QVERIFY(canvas); QVERIFY(opacity);
    QAction *fill = actionNamed(window, QStringLiteral("Fill with Foreground Color")); QVERIFY(fill); fill->trigger();
    const QPoint center = canvas->rect().center(); QTRY_VERIFY_WITH_TIMEOUT(canvas->grab().toImage().pixelColor(center).blue() > 150, 3000);
    const QColor opaque = canvas->grab().toImage().pixelColor(center);
    opacity->setValue(0); QTRY_VERIFY_WITH_TIMEOUT(canvas->grab().toImage().pixelColor(center) != opaque, 3000);
    opacity->setValue(100); QTRY_COMPARE_WITH_TIMEOUT(canvas->grab().toImage().pixelColor(center), opaque, 3000);

    QTimer::singleShot(0, &window, [&] {
        QDialog *dialog = nullptr; for (QDialog *candidate : window.findChildren<QDialog *>()) if (candidate->windowTitle() == QStringLiteral("Exposure")) dialog = candidate;
        QVERIFY(dialog); auto *stops = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("exposureStops")); auto *slider = dialog->findChild<QSlider *>(QStringLiteral("exposureStopsSlider")); auto *preview = dialog->findChild<QCheckBox *>(QStringLiteral("filterPreview")); QVERIFY(stops); QVERIFY(slider); QVERIFY(preview); slider->setValue(550); QCOMPARE(stops->value(), 2.0); stops->setValue(4); QCOMPARE(slider->value(), 600); stops->setValue(2);
        QTRY_VERIFY_WITH_TIMEOUT(canvas->grab().toImage().pixelColor(center) != opaque, 3000); preview->setChecked(false); QTRY_COMPARE_WITH_TIMEOUT(canvas->grab().toImage().pixelColor(center), opaque, 3000); preview->setChecked(true); QTRY_VERIFY_WITH_TIMEOUT(canvas->grab().toImage().pixelColor(center) != opaque, 3000); dialog->reject();
    });
    QAction *exposure = actionNamed(window, QStringLiteral("Exposure")); QVERIFY(exposure); exposure->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(canvas->grab().toImage().pixelColor(center), opaque, 3000);
}

void MainWindowTests::selectionAndRenameAvoidCompositeAndRowReloads()
{
    MainWindow window; window.show(); createEmbeddedCanvas(window, 32, 24);
    auto *canvas = window.findChild<CanvasWidget *>(); auto *layers = window.findChild<QListView *>(QStringLiteral("layerList"));
    QVERIFY(canvas); QVERIFY(layers);
    QAction *fill = actionNamed(window, QStringLiteral("Fill with Foreground Color")); QVERIFY(fill); fill->trigger();
    const quint64 renderedGeneration = canvas->renderGeneration();
    QSignalSpy resets(layers->model(), &QAbstractItemModel::modelReset);

    QAction *selectAll = actionNamed(window, QStringLiteral("Select All")); QVERIFY(selectAll); selectAll->trigger();
    QCOMPARE(canvas->renderGeneration(), renderedGeneration); QCOMPARE(resets.count(), 0);

    QAction *rename = actionNamed(window, QStringLiteral("Rename Layer")); QVERIFY(rename); rename->trigger();
    auto *editor = layers->findChild<QLineEdit *>(); QVERIFY(editor); editor->selectAll(); QTest::keyClicks(editor, QStringLiteral("Renamed")); QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(canvas->renderGeneration(), renderedGeneration); QCOMPARE(resets.count(), 0);
    QTRY_COMPARE(layers->model()->index(0, 0).data(Qt::DisplayRole).toString(), QStringLiteral("Renamed"));

    QAction *visibility = actionNamed(window, QStringLiteral("Hide Layer")); QVERIFY(visibility); visibility->trigger();
    QVERIFY(canvas->renderGeneration() > renderedGeneration); QCOMPARE(resets.count(), 1);
}

void MainWindowTests::levelsPanelProvidesHistogramHandlesPreviewAndReset()
{
    MainWindow window; window.show(); createEmbeddedCanvas(window, 32, 24);
    QAction *fill = actionNamed(window, QStringLiteral("Fill with Foreground Color")); QVERIFY(fill); fill->trigger();
    QTimer::singleShot(0, &window, [&] {
        QDialog *dialog = nullptr; for (QDialog *candidate : window.findChildren<QDialog *>()) if (candidate->windowTitle() == QStringLiteral("Levels")) dialog = candidate;
        QVERIFY(dialog); auto *histogram = dialog->findChild<QWidget *>(QStringLiteral("levelsHistogram")); auto *handles = dialog->findChild<QWidget *>(QStringLiteral("levelsInputHandles")); auto *black = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("levelsInputBlack"));
        auto *preview = dialog->findChild<QCheckBox *>(QStringLiteral("levelsPreview")); auto *reset = dialog->findChild<QPushButton *>(QStringLiteral("levelsReset"));
        QVERIFY(histogram); QVERIFY(handles); QVERIFY(black); QVERIFY(preview); QVERIFY(reset); QVERIFY(histogram->height() >= 130); QVERIFY(preview->isChecked());
        QTest::mousePress(handles, Qt::LeftButton, {}, QPoint(1, handles->height() / 2)); QTest::mouseMove(handles, QPoint(handles->width() / 4, handles->height() / 2)); QTest::mouseRelease(handles, Qt::LeftButton, {}, QPoint(handles->width() / 4, handles->height() / 2));
        QVERIFY(black->value() >= 62 && black->value() <= 65); reset->click(); QCOMPARE(black->value(), 0.0); dialog->reject();
    });
    QAction *action = actionNamed(window, QStringLiteral("Levels")); QVERIFY(action); action->trigger();
}

void MainWindowTests::gradientMapUsesLiveNonModalEndpointPicker()
{
    MainWindow window; window.show(); createEmbeddedCanvas(window, 32, 24);
    QAction *fill = actionNamed(window, QStringLiteral("Fill with Foreground Color")); QVERIFY(fill); fill->trigger();
    QTimer::singleShot(0, &window, [&] {
        QDialog *dialog = nullptr; for (QDialog *candidate : window.findChildren<QDialog *>()) if (candidate->windowTitle() == QStringLiteral("Gradient Map")) dialog = candidate;
        QVERIFY(dialog); auto *shadows = dialog->findChild<QPushButton *>(QStringLiteral("gradientMapShadows")); auto *strip = dialog->findChild<QLabel *>(QStringLiteral("gradientMapPreview")); QVERIFY(shadows); QVERIFY(strip);
        QTimer::singleShot(0, dialog, [&] {
            auto *picker = dialog->findChild<QColorDialog *>(); QVERIFY(picker); QCOMPARE(picker->windowModality(), Qt::NonModal); picker->setCurrentColor(Qt::red); picker->accept();
        });
        shadows->click(); QVERIFY(strip->pixmap().toImage().pixelColor(1, 10).red() > 200); dialog->reject();
    });
    QAction *action = actionNamed(window, QStringLiteral("Gradient Map")); QVERIFY(action); action->trigger();
}

void MainWindowTests::newGradientMapAdjustmentStartsFromPalette()
{
    MainWindow window; window.show(); createEmbeddedCanvas(window, 32, 24);
    QMenu *adjustments = nullptr;
    for (QMenu *menu : window.findChildren<QMenu *>()) if (plainText(menu->title()) == QStringLiteral("New Adjustment Layer")) { adjustments = menu; break; }
    QVERIFY(adjustments);
    QAction *gradientMap = nullptr;
    for (QAction *action : adjustments->actions()) if (plainText(action->text()) == QStringLiteral("Gradient Map")) { gradientMap = action; break; }
    QVERIFY(gradientMap);
    QTimer::singleShot(0, &window, [&] {
        QDialog *dialog = nullptr; for (QDialog *candidate : window.findChildren<QDialog *>()) if (candidate->windowTitle() == QStringLiteral("Gradient Map")) dialog = candidate;
        QVERIFY(dialog); auto *shadows = dialog->findChild<QPushButton *>(QStringLiteral("gradientMapShadows")); QVERIFY(shadows);
        QTimer::singleShot(0, dialog, [&] {
            auto *picker = dialog->findChild<QColorDialog *>(); QVERIFY(picker);
            QCOMPARE(picker->currentColor().toRgb(), QColor(22, 134, 232).toRgb());
            picker->reject();
        });
        shadows->click(); dialog->reject();
    });
    gradientMap->trigger();
}

void MainWindowTests::layerModelCollapsesNestedFoldersAndRenamesInline()
{
    auto document = std::make_shared<Document>(); document->id = QUuid::createUuid(); document->canvasSize = QSize(20, 20);
    Layer root; root.id = QUuid::createUuid(); root.name = QStringLiteral("Root"); root.group = true;
    Layer folder; folder.id = QUuid::createUuid(); folder.name = QStringLiteral("Folder"); folder.group = true; folder.parentId = root.id;
    Layer child; child.id = QUuid::createUuid(); child.name = QStringLiteral("Child"); child.parentId = folder.id;
    document->layers = {child, folder, root};
    LayerListModel model; model.setDocument(document);
    QCOMPARE(model.rowCount(), 3); QCOMPARE(model.index(0, 0).data(Qt::UserRole + 1).toInt(), 0); QCOMPARE(model.index(2, 0).data(Qt::UserRole + 1).toInt(), 2);
    model.toggleExpanded(root.id); QCOMPARE(model.rowCount(), 1); QVERIFY(!model.isExpanded(root.id));
    model.toggleExpanded(root.id); QCOMPARE(model.rowCount(), 3);
    QSignalSpy rename(&model, &LayerListModel::renameRequested); QVERIFY(model.setData(model.index(2, 0), QStringLiteral("Renamed"), Qt::EditRole));
    QCOMPARE(rename.count(), 1); QCOMPARE(rename.takeFirst().at(0).toUuid(), child.id);
}

void MainWindowTests::numericZoomCoversTheMacRange()
{
    MainWindow window; window.show(); createEmbeddedCanvas(window, 100, 100);
    auto *zoom = window.findChild<QDoubleSpinBox *>(QStringLiteral("zoomPercentage")); auto *canvas = window.findChild<CanvasWidget *>();
    QVERIFY(zoom); QVERIFY(canvas); QCOMPARE(zoom->minimum(), .1); QCOMPARE(zoom->maximum(), 3200.0);
    zoom->setValue(.1); QCOMPARE(canvas->zoom(), .001);
    zoom->setValue(3200); QCOMPARE(canvas->zoom(), 32.0);
}

QTEST_MAIN(MainWindowTests)
#include "MainWindowTests.moc"
