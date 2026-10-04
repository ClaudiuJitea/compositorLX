// Layers, masks, folders, clipping, blend modes and the Layers panel: ported from the macOS LayerTests,
// LayerAppearanceTests, LayerMaskTests, LiveMaskTests, MaskAloneTests, MaskTransformTests, GroupTests,
// GroupingSelectionTests, BlendShortcutTests, InnerGlowTests, OuterGlowTests and ProjectWorkspaceTests (layer copy),
// plus pixel checks of the folder pass-through rule the macOS renderer uses. Every assertion reads pixels or session
// state; none can pass if the feature were a no-op.
#include "core/Document.h"
#include "core/EditorSession.h"
#include "io/ProjectReader.h"
#include "io/ProjectWriter.h"
#include "rendering/LayerEffectsRenderer.h"
#include "rendering/LayerRenderer.h"
#include "ui/LayerListModel.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListView>
#include <QSignalSpy>
#include "ui/CanvasWidget.h"
#include <QMimeData>
#include <QMenu>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>

using namespace compositor;

namespace {

QImage solid(int w, int h, const QColor &color)
{
    QImage image(w, h, QImage::Format_RGBA8888_Premultiplied);
    image.fill(color);
    return image;
}

Layer pixelLayer(const QString &name, const QImage &image, QPointF origin = {})
{
    Layer layer;
    layer.id = QUuid::createUuid();
    layer.name = name;
    layer.image = image;
    layer.transform.origin = origin;
    layer.transform.size = image.size();
    layer.transform.sampling = Sampling::Nearest;
    return layer;
}

// Mac LiveMaskTests.asset: (color * alpha / 255, 0, 0, alpha) pixels, 2 x 2.
QImage asset(const QVector<int> &alphas, int color = 255)
{
    QImage image(2, 2, QImage::Format_RGBA8888_Premultiplied);
    for (int i = 0; i < 4; ++i) {
        uchar *p = image.scanLine(i / 2) + (i % 2) * 4;
        p[0] = uchar(color * alphas.at(i) / 255); p[1] = 0; p[2] = 0; p[3] = uchar(alphas.at(i));
    }
    return image;
}

QImage grayMask(int w, int h, const QVector<int> &values)
{
    QImage mask(w, h, QImage::Format_Grayscale8);
    for (int i = 0; i < w * h; ++i) mask.scanLine(i / w)[i % w] = uchar(values.at(i));
    return mask;
}

QVector<int> alphas(const QImage &image)
{
    QVector<int> result;
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) result.push_back(qAlpha(image.pixel(x, y)));
    return result;
}

bool near(const QVector<int> &actual, const QVector<int> &expected, int tolerance = 1)
{
    if (actual.size() != expected.size()) return false;
    for (int i = 0; i < actual.size(); ++i) if (std::abs(actual.at(i) - expected.at(i)) > tolerance) return false;
    return true;
}

QString show(const QVector<int> &values)
{
    QStringList parts;
    for (int v : values) parts << QString::number(v);
    return parts.join(QLatin1Char(','));
}

QImage render(EditorSession &session) { return LayerRenderer::flattened(*session.document()); }

const Layer *find(const EditorSession &session, const QUuid &id)
{
    for (const Layer &layer : session.document()->layers) if (layer.id == id) return &layer;
    return nullptr;
}

Layer *findMutable(EditorSession &session, const QUuid &id)
{
    for (Layer &layer : session.document()->layers) if (layer.id == id) return &layer;
    return nullptr;
}

QUuid active(const EditorSession &session) { return session.document()->activeLayerId.value_or(QUuid()); }

QStringList names(const EditorSession &session)
{
    QStringList result;
    for (const Layer &layer : session.document()->layers) result << layer.name;
    return result;
}

// A document of a 2 x 2 red layer with nearest sampling, as LayerMaskTests builds it.
EditorSession redSession(int size = 2)
{
    EditorSession session;
    session.createDocument(size, size);
    session.insertImage(solid(size, size, Qt::red), QStringLiteral("Red"));
    session.document()->layers[0].transform.sampling = Sampling::Nearest;
    return session;
}

} // namespace

class TestLayers : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qputenv("QT_QPA_PLATFORM", "offscreen"); }

    // LayerTests
    void blankLayersAreTransparentAndInsertedAboveSelection();
    void deletionPreservesCanvasAndChoosesNeighbor();
    void renameAndVisibilityKeepIdentity();
    void reorderTranslatesVisibleOrderAndKeepsSelection();
    void newCanvasStartsWithOneSelectedEmptyLayer();
    void deletingAMultiSelectionRemovesEveryLayerInOneStep();
    void duplicatingALayerByDraggingPlacesTheCopyAsOneStep();
    void duplicatingFoldersAndSeveralLayers();
    void compositingHonorsVisibilityOrderAndBlankLayers();
    void contextMenuHasTheCoreLayerActionsAndTheyWork();
    void contextMenuMaskAndClippingActionsFollowLayerState();
    void rightClickSelection();
    void contextMenuMergeTitleAndUndo();

    // GroupTests / GroupingSelectionTests
    void nestedGroupsMoveOutCollapseAndDeleteUndo();
    void hiddenParentOverridesChildren();
    void ungroupLayersRestoresChildrenAtTheFoldersSpotAndUndoes();
    void ungroupPreservesClippingBetweenTwoOfAFoldersOwnChildren();
    void ungroupingReleasesClippingThatNoLongerMakesSense();
    void singleLayerAndFolderAreWrappedRatherThanCreatingAChildFolder();
    void multipleSelectionPreservesOrderAndSelectedFolderDescendants();
    void itemsFromDifferentFoldersUseCommonParentAndEmptySelectionCreatesEmptyGroup();
    void groupsRoundTripThroughSaveWithFolderOpacityAndMask();

    // Folder opacity / pass-through (mac LayerOpacity, FolderMaskClip)
    void folderOpacityMultipliesIntoEveryLayerInside();
    void folderIsPassThroughForBlendModesAndAdjustments();
    void folderMasksClipEveryLayerInsideAndMultiplyWithTheirOwnMasks();
    void canvasExportAndCopyMergedAgree();

    // BlendShortcutTests / LayerAppearanceTests
    void shiftPlusAndMinusStepTheActiveLayersBlendMode();
    void blendModeListFollowsPhotoshopsOrder();
    void blendModesAndOpacityMatchKnownPixels();
    void opacitySliderDragIsOneUndo();
    void selectedFoldersTakeTypedOpacityAndOpacitySurvivesSave();

    // LayerMaskTests
    void addDisableDeleteUndoAndTargetSelection();
    void coverageOpacityAndDisabledMasksRenderCorrectly();
    void transformedMaskStaysAligned();
    void projectAndPngPreserveCoverageAndDisabledState();
    void folderMaskCanBePaintedInvertedAndLoadedAsASelection();
    void maskFromSelectionRevealsOrHidesIt();

    // LiveMaskTests
    void clippingColorPreservesSoftBaseAlphaWithoutBlackFringe();
    void optionClickCreatesSharedStackAndDragOutReleases();
    void hiddenBlackSourceSuppliesAlphaAndRasterMasksMultiply();
    void liveMaskCyclesUndoPersistenceBakeAndDelete();
    void bakeAndDeleteKeepsTheLayersOwnGridMaskAndEffects();
    void mergeLayersTakesTheTopmostSelectedName();
    void flipLayersLeavesHiddenLayersAlone();
    void releasingClippingSkipsAFoldersContents();
    void copiedPixelsLeaveEffectsBehind();
    void colorDodgeKeepsBlackAndColorBurnKeepsWhite();
    void pasteFindsWhereThePixelsCameFrom();
    void movingSourceChangesCoverageAndChainsMultiply();

    // MaskAloneTests / MaskTransformTests
    void maskAloneToggles();
    void aLayerWithoutAMaskHasNothingToShow();
    void unlinkedMaskKeepsItsPlaceWhenTheLayerFlips();

    // Merge / flip
    void mergeDownMergeLayersAndMergeGroup();

    // Effects
    void innerGlowRendersInsideSourceWithoutBoundsExpansion();
    void outerGlowRendersOmnidirectionally();
    void outerGlowCombinedWithStrokeAndDropShadow();
    void effectsCopyBetweenLayersAndSurviveSave();

    // Whole-layer clipboard and cross-project copy (ProjectWorkspaceTests)
    void wholeLayerCopyAndPasteInsideOneProject();
    void crossProjectCopyRemapsIdentityAndHasIndependentUndo();
    void crossProjectCopyBakesLiveMasksAndAnchorsSeveralLayers();

    // Layer panel / inspector
    void inspectorMovesAnUnlinkedMaskOrLayerSeparately();
    void opacityAndBlendControlsFollowMacEnablingRules();
    void panelRowsShowChainDisabledMaskAndTextBadge();
    void maskAloneBadgeTakesItsClicks();
    void folderMaskShowsWhileItIsBeingPainted();
    void largeGlowsStayCloseToAGaussian();
};

// ---- LayerTests ---------------------------------------------------------------------------------------------

void TestLayers::blankLayersAreTransparentAndInsertedAboveSelection()
{
    EditorSession session;
    session.createDocument(800, 600);
    for (int i = 0; i < 3; ++i) session.addBlankLayer();
    session.selectLayer(session.document()->layers.first().id);
    session.addBlankLayer();
    QCOMPARE(names(session), (QStringList{"Layer 1", "Layer 4", "Layer 2", "Layer 3"}));
    const Layer &added = session.document()->layers.at(1);
    QCOMPARE(added.id, active(session));
    QVERIFY(added.image.isNull());
    QCOMPARE(added.transform.size, QSizeF(800, 600));
    QCOMPARE(added.transform.origin, QPointF());
}

void TestLayers::deletionPreservesCanvasAndChoosesNeighbor()
{
    EditorSession session;
    session.createDocument(800, 600);
    for (int i = 0; i < 3; ++i) session.addBlankLayer();
    const QVector<Layer> layers = session.document()->layers;
    session.selectLayer(layers[1].id);
    // Deleting a layer that is not the active one leaves the active layer alone.
    session.selectLayers({layers[0].id}, layers[0].id);
    session.deleteActiveLayer();
    session.selectLayer(layers[1].id);
    QCOMPARE(active(session), layers[1].id);
    session.deleteActiveLayer();
    QCOMPARE(active(session), layers[2].id);
    session.deleteActiveLayer();
    QVERIFY(session.document()->layers.isEmpty());
    QVERIFY(!session.document()->activeLayerId.has_value());
    QCOMPARE(session.document()->canvasSize, QSize(800, 600));
    session.addBlankLayer();
    QCOMPARE(session.document()->layers.size(), 1);

    // Mac: deleting layers[0] while layers[1] is active keeps layers[1] active.
    EditorSession other;
    other.createDocument(100, 100);
    for (int i = 0; i < 3; ++i) other.addBlankLayer();
    const QVector<Layer> three = other.document()->layers;
    other.selectLayers({three[1].id}, three[1].id);
    other.selectLayers({three[0].id}, three[0].id);
    other.selectLayer(three[1].id);
    other.selectLayers({three[0].id}, three[0].id);
    other.deleteSelectedLayers();
    QCOMPARE(other.document()->layers.size(), 2);
    other.selectLayer(three[1].id);
    other.selectLayers({three[2].id}, three[1].id);   // select 3 but keep 2 as primary: a non-active layer goes
    other.selectLayer(three[1].id);
    other.deleteActiveLayer();
    QCOMPARE(active(other), three[2].id);
}

void TestLayers::renameAndVisibilityKeepIdentity()
{
    EditorSession session;
    session.createDocument(100, 100);
    for (int i = 0; i < 3; ++i) session.addBlankLayer();
    const QUuid id = active(session);
    session.renameLayer(id, QStringLiteral("  Foreground \n"));
    session.renameLayer(id, QStringLiteral(" \n "));
    QCOMPARE(session.activeLayer()->name, QStringLiteral("Foreground"));
    session.toggleLayerVisibility(id);
    QVERIFY(!session.activeLayer()->visible);
    QCOMPARE(active(session), id);
    session.toggleLayerVisibility(id);
    QVERIFY(session.activeLayer()->visible);
}

void TestLayers::reorderTranslatesVisibleOrderAndKeepsSelection()
{
    EditorSession session;
    session.createDocument(100, 100);
    for (int i = 0; i < 3; ++i) session.addBlankLayer();
    const QUuid before = active(session);
    QVERIFY(session.reorderLayers({0}, 3));
    QCOMPARE(names(session), (QStringList{"Layer 3", "Layer 1", "Layer 2"}));
    QCOMPARE(active(session), before);
    QVERIFY(!session.canMoveActiveLayer(-1));
    session.moveActiveLayer(1);
    QCOMPARE(names(session), (QStringList{"Layer 1", "Layer 3", "Layer 2"}));
    QVERIFY(!session.reorderLayers({99}, 0));
    QCOMPARE(session.document()->layers.size(), 3);
}

void TestLayers::newCanvasStartsWithOneSelectedEmptyLayer()
{
    EditorSession session;
    session.createDocument(640, 480, true);
    QCOMPARE(session.document()->layers.size(), 1);
    QCOMPARE(session.activeLayer()->name, QStringLiteral("Layer 1"));
    QVERIFY(session.activeLayer()->image.isNull());
    QCOMPARE(session.activeLayer()->transform.size, QSizeF(640, 480));
}

void TestLayers::deletingAMultiSelectionRemovesEveryLayerInOneStep()
{
    EditorSession session;
    session.createDocument(100, 100);
    session.addBlankLayer();
    const QUuid keep = active(session);
    session.selectLayer(std::nullopt);
    session.addGroup();
    const QUuid folder = active(session);
    session.addBlankLayer();
    const QUuid child = active(session);
    session.selectLayer(std::nullopt);
    session.addBlankLayer();
    const QUuid top = active(session);
    QCOMPARE(find(session, child)->parentId, std::optional<QUuid>(folder));
    const Document before = *session.document();
    const int count = session.history().undoCount();
    session.selectLayers({folder, top}, top);

    session.deleteLayerOrMask();
    QCOMPARE(session.document()->layers.size(), 1);
    QCOMPARE(session.document()->layers.first().id, keep);
    QCOMPARE(session.history().undoCount(), count + 1);
    QCOMPARE(session.history().undoName(), QStringLiteral("Delete Layers"));
    QCOMPARE(active(session), keep);
    QCOMPARE(session.selectedLayerIds(), (QSet<QUuid>{keep}));
    session.undo();
    QVERIFY(*session.document() == before);

    session.selectLayers({keep}, keep);
    session.deleteLayerOrMask();
    QVERIFY(!find(session, keep));
    QCOMPARE(session.history().undoName(), QStringLiteral("Delete Layer"));
}

void TestLayers::duplicatingALayerByDraggingPlacesTheCopyAsOneStep()
{
    EditorSession session;
    session.createDocument(800, 600);
    for (int i = 0; i < 3; ++i) session.addBlankLayer();
    const QVector<Layer> bottomFirst = session.document()->layers;
    const Layer top = bottomFirst.last(), bottom = bottomFirst.first();
    const Document before = *session.document();
    const int count = session.history().undoCount();
    QVERIFY(session.duplicateLayer(bottom.id, std::nullopt, top.id));
    QCOMPARE(session.document()->layers.size(), 4);
    QCOMPARE(session.history().undoCount(), count + 1);
    QCOMPARE(session.history().undoName(), QStringLiteral("Duplicate Layer"));
    QCOMPARE(session.document()->layers.last().name, bottom.name + QStringLiteral(" copy"));
    QVERIFY(find(session, bottom.id));
    session.undo();
    QVERIFY(*session.document() == before);

    session.addGroup();
    const QUuid folder = active(session);
    QVERIFY2(session.duplicateLayer(folder, std::nullopt, std::nullopt, true), "a folder duplicates with its contents");
}

void TestLayers::duplicatingFoldersAndSeveralLayers()
{
    EditorSession session;
    session.createDocument(100, 100);
    session.addBlankLayer();
    session.addBlankLayer();
    session.addBlankLayer();
    session.groupSelectedLayers();
    const QUuid folder = active(session);
    QVERIFY(session.canUngroupLayers());
    const QSet<QUuid> originalChildren = session.descendantIds(folder);
    QVERIFY(!originalChildren.isEmpty());
    const int before = session.document()->layers.size();
    const int count = session.history().undoCount();
    session.duplicateActiveLayer();
    QVERIFY2(session.document()->layers.size() == before + 1 + originalChildren.size(), "folder and contents are copied");
    QCOMPARE(session.history().undoCount(), count + 1);
    const Layer *copy = session.activeLayer();
    QVERIFY(copy && copy->group && copy->id != folder);
    QCOMPARE(copy->name, session.document()->layers.at(0).name == copy->name ? copy->name : QStringLiteral("Folder 1 copy"));
    const QSet<QUuid> copied = session.descendantIds(copy->id);
    QCOMPARE(copied.size(), originalChildren.size());
    for (const QUuid &id : copied) QCOMPARE(find(session, id)->parentId, std::optional<QUuid>(copy->id));
    for (const QUuid &id : copied) QVERIFY(!originalChildren.contains(id));
    session.undo();
    QCOMPARE(session.document()->layers.size(), before);

    // Several selected layers stack together above the topmost original, in their order.
    EditorSession two;
    two.createDocument(100, 100);
    two.addBlankLayer(); const QUuid a = active(two);
    two.addBlankLayer(); const QUuid b = active(two);
    two.addBlankLayer(); const QUuid c = active(two);
    two.selectLayers({a, c}, c);
    two.duplicateActiveLayer();
    QCOMPARE(two.document()->layers.size(), 5);
    QCOMPARE(two.selectedLayerIds().size(), 2);
    QCOMPARE(names(two).filter(QStringLiteral(" copy")).size(), 2);
    QVERIFY(two.document()->layers.at(3).name.endsWith(QStringLiteral("copy")));
    QCOMPARE(two.document()->layers.at(3).name, find(two, a)->name + QStringLiteral(" copy"));
    QCOMPARE(two.document()->layers.at(4).name, find(two, c)->name + QStringLiteral(" copy"));
    Q_UNUSED(b);
}

void TestLayers::compositingHonorsVisibilityOrderAndBlankLayers()
{
    EditorSession session;
    session.createDocument(8, 8);
    session.insertImage(solid(8, 8, QColor(255, 0, 0)), QStringLiteral("Red"));
    session.insertImage(solid(8, 8, QColor(0, 0, 255)), QStringLiteral("Blue"));
    const QUuid blue = active(session);
    QVERIFY(render(session).pixelColor(4, 4).blue() > 240);
    session.toggleLayerVisibility(blue);
    QVERIFY(render(session).pixelColor(4, 4).red() > 240);
    session.toggleLayerVisibility(blue);
    session.moveActiveLayer(-1);
    QVERIFY(render(session).pixelColor(4, 4).red() > 240);
    session.selectLayer(session.document()->layers.last().id);
    session.addBlankLayer();
    QVERIFY(render(session).pixelColor(4, 4).red() > 240);
    session.selectLayer(session.document()->layers.at(1).id);
    session.deleteActiveLayer();
    QVERIFY(render(session).pixelColor(4, 4).blue() > 240);
}

// ---- right-click menu (mac NativeLayerList.contextMenu) --------------------------------------------------

static QAction *action(QMenu &menu, const QString &name)
{
    for (QAction *candidate : menu.actions()) if (candidate->objectName() == name) return candidate;
    return nullptr;
}

static void seedThreeLayers(MainWindow &window)
{
    window.session().createDocument(200, 200);
    for (int i = 0; i < 3; ++i) window.session().addBlankLayer();
    window.syncDocumentViews();
}

void TestLayers::contextMenuHasTheCoreLayerActionsAndTheyWork()
{
    MainWindow window;
    seedThreeLayers(window);
    EditorSession &session = window.session();
    QMenu menu;
    window.populateLayerContextMenu(menu);
    for (const QString &name : {"layerMenuDuplicate", "layerMenuRename", "layerMenuDelete", "layerMenuClipping", "layerMenuGroup",
                                "layerMenuMoveOut", "layerMenuMerge", "layerMenuAddMask", "layerMenuToggleMask", "layerMenuDeleteMask",
                                "layerMenuLinkMask", "layerMenuVisibility"})
        QVERIFY2(action(menu, name), qPrintable(name));
    QStringList subTitles;
    for (QAction *a : action(menu, QStringLiteral("layerMenuAddMask"))->menu()->actions()) subTitles << a->text();
    QVERIFY(subTitles.contains(QStringLiteral("Reveal All (White)")));
    QVERIFY(subTitles.contains(QStringLiteral("Hide All (Black)")));
    QVERIFY(action(menu, QStringLiteral("layerMenuDuplicate"))->isEnabled());
    QVERIFY(action(menu, QStringLiteral("layerMenuAddMask"))->isEnabled());
    QVERIFY(!action(menu, QStringLiteral("layerMenuDeleteMask"))->isEnabled());
    QVERIFY(!action(menu, QStringLiteral("layerMenuToggleMask"))->isEnabled());
    QVERIFY(!action(menu, QStringLiteral("layerMenuMoveOut"))->isEnabled());

    // Duplicate through the menu: original stays, the copy is active, one undo step.
    const QUuid target = active(session);
    const int count = session.history().undoCount();
    action(menu, QStringLiteral("layerMenuDuplicate"))->trigger();
    QCOMPARE(session.document()->layers.size(), 4);
    QVERIFY(find(session, target));
    QVERIFY(active(session) != target);
    QCOMPARE(session.activeLayer()->name, find(session, target)->name + QStringLiteral(" copy"));
    QCOMPARE(session.history().undoCount(), count + 1);
    QCOMPARE(session.history().undoName(), QStringLiteral("Duplicate Layer"));
    session.undo();
    QCOMPARE(session.document()->layers.size(), 3);
    session.redo();
    QCOMPARE(session.document()->layers.size(), 4);
    session.undo();

    // Visibility from the menu.
    QMenu again;
    session.selectLayer(target);
    window.populateLayerContextMenu(again);
    QCOMPARE(action(again, QStringLiteral("layerMenuVisibility"))->text(), QStringLiteral("Hide Layer"));
    action(again, QStringLiteral("layerMenuVisibility"))->trigger();
    QVERIFY(!session.activeLayer()->visible);
    session.undo();
    QVERIFY(session.activeLayer()->visible);

    // Delete Layer from the menu.
    QMenu del;
    window.populateLayerContextMenu(del);
    QCOMPARE(action(del, QStringLiteral("layerMenuDelete"))->text(), QStringLiteral("Delete Layer"));
    action(del, QStringLiteral("layerMenuDelete"))->trigger();
    QVERIFY(!find(session, target));
}

void TestLayers::contextMenuMaskAndClippingActionsFollowLayerState()
{
    MainWindow window;
    seedThreeLayers(window);
    EditorSession &session = window.session();
    QMenu menu;
    window.populateLayerContextMenu(menu);
    // Clipping: the top layer can clip to the one below.
    QAction *clip = action(menu, QStringLiteral("layerMenuClipping"));
    QCOMPARE(clip->text(), QStringLiteral("Create Clipping Mask"));
    QVERIFY(clip->isEnabled());
    clip->trigger();
    QVERIFY(session.activeLayer()->maskSourceId.has_value());
    QMenu released;
    window.populateLayerContextMenu(released);
    QCOMPARE(action(released, QStringLiteral("layerMenuClipping"))->text(), QStringLiteral("Release Clipping Mask"));
    action(released, QStringLiteral("layerMenuClipping"))->trigger();
    QVERIFY(!session.activeLayer()->maskSourceId.has_value());

    // Mask: add from the submenu, then enable/disable, link and delete follow the layer's state.
    QMenu fresh;
    window.populateLayerContextMenu(fresh);
    auto submenu = action(fresh, QStringLiteral("layerMenuAddMask"))->menu();
    QAction *white = nullptr;
    for (QAction *a : submenu->actions()) if (a->objectName() == QStringLiteral("layerMenuRevealAll")) white = a;
    QVERIFY(white);
    white->trigger();
    QVERIFY(!session.activeLayer()->mask.isNull());
    QVERIFY(session.activeLayer()->maskEnabled);

    QMenu withMask;
    window.populateLayerContextMenu(withMask);
    QVERIFY(!action(withMask, QStringLiteral("layerMenuAddMask"))->isEnabled());
    QCOMPARE(action(withMask, QStringLiteral("layerMenuToggleMask"))->text(), QStringLiteral("Disable Mask"));
    QVERIFY(action(withMask, QStringLiteral("layerMenuToggleMask"))->isEnabled());
    QCOMPARE(action(withMask, QStringLiteral("layerMenuLinkMask"))->text(), QStringLiteral("Unlink Mask"));
    action(withMask, QStringLiteral("layerMenuToggleMask"))->trigger();
    QVERIFY(!session.activeLayer()->maskEnabled);
    QMenu disabled;
    window.populateLayerContextMenu(disabled);
    QCOMPARE(action(disabled, QStringLiteral("layerMenuToggleMask"))->text(), QStringLiteral("Enable Mask"));
    action(disabled, QStringLiteral("layerMenuLinkMask"))->trigger();
    QVERIFY(!session.activeLayer()->maskLinked);
    QMenu unlinked;
    window.populateLayerContextMenu(unlinked);
    QCOMPARE(action(unlinked, QStringLiteral("layerMenuLinkMask"))->text(), QStringLiteral("Link Mask"));
    action(unlinked, QStringLiteral("layerMenuDeleteMask"))->trigger();
    QVERIFY(session.activeLayer()->mask.isNull());
}

void TestLayers::rightClickSelection()
{
    MainWindow window;
    seedThreeLayers(window);
    EditorSession &session = window.session();
    QListView *view = window.layerView();
    const QVector<Layer> layers = session.document()->layers;     // bottom first; the panel lists top first
    const QModelIndex row0 = window.layerModel()->index(0, 0), row1 = window.layerModel()->index(1, 0);
    session.selectLayers({layers[2].id}, layers[2].id);
    window.syncDocumentViews();
    QCOMPARE(window.layerModel()->layerId(row1), std::optional<QUuid>(layers[1].id));
    // Right-click on an unselected layer selects it alone.
    window.selectRowForContextMenu(row1);
    QCOMPARE(session.selectedLayerIds(), (QSet<QUuid>{layers[1].id}));
    QCOMPARE(active(session), layers[1].id);
    // Right-click inside a multi-selection keeps it, the clicked row becomes primary.
    session.selectLayers({layers[0].id, layers[1].id, layers[2].id}, layers[2].id);
    window.syncDocumentViews();
    window.selectRowForContextMenu(row1);
    QCOMPARE(session.selectedLayerIds().size(), 3);
    QCOMPARE(active(session), layers[1].id);
    QVERIFY(view);
    Q_UNUSED(row0);
}

void TestLayers::contextMenuMergeTitleAndUndo()
{
    MainWindow window;
    seedThreeLayers(window);
    EditorSession &session = window.session();
    const QVector<Layer> layers = session.document()->layers;
    QMenu menu;
    window.populateLayerContextMenu(menu);
    QAction *merge = action(menu, QStringLiteral("layerMenuMerge"));
    QCOMPARE(merge->text(), QStringLiteral("Merge Down"));
    QCOMPARE(merge->isEnabled(), session.canMergeLayers());
    session.selectLayers({layers[2].id, layers[1].id}, layers[2].id);
    QMenu multi;
    window.populateLayerContextMenu(multi);
    QCOMPARE(action(multi, QStringLiteral("layerMenuMerge"))->text(), QStringLiteral("Merge Layers"));
    QCOMPARE(action(multi, QStringLiteral("layerMenuDelete"))->text(), QStringLiteral("Delete Selected Layers"));
    QVERIFY(!action(multi, QStringLiteral("layerMenuRename"))->isEnabled());
    // Blank layers hold no pixels, so merging them would produce nothing; the menu item still follows canMergeLayers.
    QCOMPARE(action(multi, QStringLiteral("layerMenuMerge"))->isEnabled(), session.canMergeLayers());
    const Document before = *session.document();
    const int count = session.history().undoCount();
    QMenu dup;
    window.populateLayerContextMenu(dup);
    action(dup, QStringLiteral("layerMenuDuplicate"))->trigger();
    QCOMPARE(session.document()->layers.size(), 5);
    QCOMPARE(session.history().undoCount(), count + 1);
    session.undo();
    QVERIFY(*session.document() == before);
}

// ---- GroupTests ------------------------------------------------------------------------------------------

void TestLayers::nestedGroupsMoveOutCollapseAndDeleteUndo()
{
    EditorSession session;
    session.createDocument(100, 100);
    session.addGroup();
    const QUuid outer = active(session);
    session.addGroup();
    const QUuid inner = active(session);
    session.addBlankLayer();
    const QUuid child = active(session);
    QCOMPARE(session.activeLayer()->parentId, std::optional<QUuid>(inner));
    QVERIFY(!session.placeLayer(outer, inner));
    QVERIFY(!session.placeLayer(inner, inner));
    session.selectLayer(child);
    QVERIFY(session.moveActiveLayerOutOfGroup());
    QCOMPARE(session.activeLayer()->parentId, std::optional<QUuid>(outer));
    session.undo();
    QCOMPARE(find(session, child)->parentId, std::optional<QUuid>(inner));
    session.selectLayer(outer);
    session.deleteActiveLayer();
    QVERIFY(session.document()->layers.isEmpty());
    session.undo();
    QCOMPARE(session.document()->layers.size(), 3);
    QCOMPARE(find(session, child)->parentId, std::optional<QUuid>(inner));

    // The panel's folder rows: collapsing hides contents, depth follows nesting.
    MainWindow window;
    window.session().setDocument(std::make_shared<Document>(*session.document()));
    window.syncDocumentViews();
    LayerListModel *model = window.layerModel();
    QCOMPARE(model->rowCount(), 3);
    QCOMPARE(model->index(0, 0).data(Qt::UserRole + 1).toInt(), 2);   // depth of the innermost row (top first)
    model->toggleExpanded(outer);
    QCOMPARE(model->rowCount(), 1);
    model->toggleExpanded(outer);
    QCOMPARE(model->rowCount(), 3);
}

void TestLayers::hiddenParentOverridesChildren()
{
    EditorSession session;
    session.createDocument(64, 32);
    session.addGroup();
    const QUuid group = active(session);
    session.insertImage(solid(8, 8, Qt::red), QStringLiteral("Child"), QPointF(32, 16));
    const QUuid child = active(session);
    QCOMPARE(session.activeLayer()->parentId, std::optional<QUuid>(group));
    QCOMPARE(qAlpha(render(session).pixel(32, 16)), 255);
    session.toggleLayerVisibility(group);
    QVERIFY(session.activeLayer()->visible);       // the child's own flag is untouched
    QVERIFY(!LayerRenderer::effectivelyVisible(*session.document(), *find(session, child)));
    QCOMPARE(qAlpha(render(session).pixel(32, 16)), 0);
    session.toggleLayerVisibility(group);
    QCOMPARE(qAlpha(render(session).pixel(32, 16)), 255);
    // A second folder placed above: draw order follows the groups.
    session.selectLayer(std::nullopt);
    session.addGroup();
    const QUuid other = active(session);
    session.insertImage(solid(8, 8, Qt::blue), QStringLiteral("Other"), QPointF(32, 16));
    const QUuid otherChild = active(session);
    QCOMPARE(render(session).pixelColor(32, 16).blue(), 255);
    session.selectLayer(other);
    session.moveActiveLayer(-1);
    QCOMPARE(render(session).pixelColor(32, 16).red(), 255);
    QCOMPARE(find(session, otherChild)->parentId, std::optional<QUuid>(other));
}

void TestLayers::ungroupLayersRestoresChildrenAtTheFoldersSpotAndUndoes()
{
    EditorSession session;
    session.createDocument(100, 100);
    session.addBlankLayer();
    const QUuid below = active(session);
    QVERIFY(!session.canUngroupLayers());
    session.addGroup();
    const QUuid group = active(session);
    session.addBlankLayer();
    const QUuid childA = active(session);
    session.addBlankLayer();
    const QUuid childB = active(session);
    session.selectLayer(std::nullopt);
    session.addBlankLayer();
    const QUuid above = active(session);
    QCOMPARE(session.document()->layers.size(), 5);
    session.selectLayer(group);
    QVERIFY(session.canUngroupLayers());
    const int count = session.history().undoCount();
    session.ungroupLayers();
    QVERIFY(!find(session, group));
    QVERIFY(!find(session, childA)->parentId.has_value());
    QVERIFY(!find(session, childB)->parentId.has_value());
    QVector<QUuid> order;
    for (const Layer &layer : session.document()->layers) order << layer.id;
    QVERIFY(order.indexOf(below) < order.indexOf(childA));
    QVERIFY(order.indexOf(childA) < order.indexOf(childB));
    QVERIFY(order.indexOf(childB) < order.indexOf(above));
    QCOMPARE(session.selectedLayerIds(), (QSet<QUuid>{childA, childB}));
    QCOMPARE(session.history().undoCount(), count + 1);
    QCOMPARE(session.history().undoName(), QStringLiteral("Ungroup Layers"));
    session.undo();
    QCOMPARE(find(session, childA)->parentId, std::optional<QUuid>(group));
    QVERIFY(find(session, group)->group);
    session.redo();
    QCOMPARE(session.document()->layers.size(), 4);
}

void TestLayers::ungroupPreservesClippingBetweenTwoOfAFoldersOwnChildren()
{
    EditorSession session;
    session.createDocument(100, 100);
    session.addBlankLayer();
    const QUuid base = active(session);
    session.addBlankLayer();
    const QUuid clipped = active(session);
    QVERIFY(session.linkMask(base, clipped));
    session.selectLayers({base, clipped}, base);
    session.groupSelectedLayers();
    QVERIFY(session.canUngroupLayers());
    session.ungroupLayers();
    QCOMPARE(find(session, clipped)->maskSourceId, std::optional<QUuid>(base));
}

void TestLayers::ungroupingReleasesClippingThatNoLongerMakesSense()
{
    EditorSession session;
    session.createDocument(100, 100);
    session.addBlankLayer(); const QUuid outsideBase = active(session);
    session.addBlankLayer(); const QUuid between = active(session);
    session.addBlankLayer(); const QUuid childSource = active(session);
    QVERIFY(session.linkMask(outsideBase, childSource));
    session.selectLayer(childSource);
    session.groupSelectedLayers();
    const QUuid group = active(session);
    QVector<QUuid> order;
    for (const Layer &layer : session.document()->layers) order << layer.id;
    QCOMPARE(order, (QVector<QUuid>{outsideBase, between, group, childSource}));
    session.ungroupLayers();
    order.clear();
    for (const Layer &layer : session.document()->layers) order << layer.id;
    QCOMPARE(order, (QVector<QUuid>{outsideBase, between, childSource}));
    QVERIFY(!find(session, childSource)->maskSourceId.has_value());
}

// ---- GroupingSelectionTests ------------------------------------------------------------------------------

void TestLayers::singleLayerAndFolderAreWrappedRatherThanCreatingAChildFolder()
{
    EditorSession session;
    session.createDocument(100, 100);
    session.addBlankLayer();
    const QUuid layer = active(session);
    session.groupSelectedLayers();
    const QUuid inner = active(session);
    QCOMPARE(find(session, layer)->parentId, std::optional<QUuid>(inner));
    session.groupSelectedLayers();
    const QUuid outer = active(session);
    QCOMPARE(find(session, inner)->parentId, std::optional<QUuid>(outer));
    QCOMPARE(find(session, layer)->parentId, std::optional<QUuid>(inner));
    QVERIFY(!session.activeLayer()->parentId.has_value());
    session.undo();
    QVERIFY(!find(session, inner)->parentId.has_value());
    QCOMPARE(session.document()->layers.size(), 2);
}

void TestLayers::multipleSelectionPreservesOrderAndSelectedFolderDescendants()
{
    EditorSession session;
    session.createDocument(100, 100);
    session.addGroup();
    const QUuid folder = active(session);
    session.addBlankLayer();
    const QUuid child = active(session);
    session.selectLayer(std::nullopt);
    session.addBlankLayer();
    const QUuid sibling = active(session);
    const Document before = *session.document();
    session.selectLayers({folder, child, sibling}, sibling);
    QCOMPARE(session.selectedLayerIds().size(), 3);
    session.groupSelectedLayers();
    const QUuid wrapper = active(session);
    QCOMPARE(find(session, folder)->parentId, std::optional<QUuid>(wrapper));
    QCOMPARE(find(session, sibling)->parentId, std::optional<QUuid>(wrapper));
    QCOMPARE(find(session, child)->parentId, std::optional<QUuid>(folder));
    QCOMPARE(session.selectedLayerIds(), (QSet<QUuid>{wrapper}));
    session.undo();
    QVERIFY(*session.document() == before);
    session.redo();
    QCOMPARE(session.document()->layers.size(), 4);
    // The wrapper keeps both drawn layers: child (inside the folder) under sibling.
    Layer *c = findMutable(session, child); Layer *s = findMutable(session, sibling);
    c->image = solid(100, 100, Qt::red); c->transform.size = QSizeF(100, 100);
    s->image = solid(40, 40, Qt::blue); s->transform.size = QSizeF(40, 40);
    const QImage image = render(session);
    QCOMPARE(image.pixelColor(10, 10).blue(), 255);
    QCOMPARE(image.pixelColor(80, 80).red(), 255);
}

void TestLayers::itemsFromDifferentFoldersUseCommonParentAndEmptySelectionCreatesEmptyGroup()
{
    EditorSession session;
    session.createDocument(100, 100);
    session.groupSelectedLayers();                       // nothing selected: an empty folder
    const QUuid first = active(session);
    QVERIFY(find(session, first)->group);
    session.addBlankLayer();
    const QUuid a = active(session);
    session.selectLayer(std::nullopt);
    session.groupSelectedLayers();
    const QUuid second = active(session);
    session.addBlankLayer();
    const QUuid b = active(session);
    session.selectLayers({a, b}, b);
    session.groupSelectedLayers();
    const QUuid group = active(session);
    QVERIFY(!session.activeLayer()->parentId.has_value());
    QCOMPARE(find(session, a)->parentId, std::optional<QUuid>(group));
    QCOMPARE(find(session, b)->parentId, std::optional<QUuid>(group));
    QVERIFY(!find(session, first)->parentId.has_value());
    QVERIFY(!find(session, second)->parentId.has_value());
}

void TestLayers::groupsRoundTripThroughSaveWithFolderOpacityAndMask()
{
    EditorSession session;
    session.createDocument(20, 10);
    session.addGroup();
    const QUuid group = active(session);
    session.renameLayer(group, QStringLiteral("Artwork"));
    session.setLayerOpacity(group, 0.4);
    session.addLayerMask(false, false);
    session.addBlankLayer();
    const QUuid child = active(session);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Groups.comp"));
    ProjectWriter::save(*session.document(), path);
    const Document loaded = ProjectReader::load(path);
    const Layer *folder = nullptr, *kid = nullptr;
    for (const Layer &layer : loaded.layers) { if (layer.id == group) folder = &layer; if (layer.id == child) kid = &layer; }
    QVERIFY(folder && kid);
    QVERIFY(folder->group);
    QCOMPARE(folder->name, QStringLiteral("Artwork"));
    QVERIFY(std::abs(folder->opacity - 0.4) < 0.001);
    QVERIFY2(!folder->mask.isNull(), "the folder's mask must survive the save");
    QCOMPARE(folder->mask.pixelColor(0, 0).red(), 0);
    QCOMPARE(kid->parentId, std::optional<QUuid>(group));
}

// ---- folder pass-through (mac LayerOpacity / FolderMaskClip) ---------------------------------------------

void TestLayers::folderOpacityMultipliesIntoEveryLayerInside()
{
    EditorSession session;
    session.createDocument(10, 10);
    session.addGroup();
    const QUuid folder = active(session);
    session.insertImage(solid(10, 10, Qt::red), QStringLiteral("Child"));
    const QUuid child = active(session);
    session.setLayerOpacity(folder, 0.5);
    session.setLayerOpacity(child, 0.5);
    QVERIFY(std::abs(qAlpha(render(session).pixel(5, 5)) - 64) <= 2);
    QVERIFY2(std::abs(find(session, child)->opacity - 0.5) < 1e-9, "the layer itself still reads 50%");
    // A folder inside a folder multiplies too: 50% * 50% * 50%.
    session.selectLayer(folder);
    session.groupSelectedLayers();
    const QUuid outer = active(session);
    session.setLayerOpacity(outer, 0.5);
    QVERIFY(std::abs(qAlpha(render(session).pixel(5, 5)) - 32) <= 2);
    // Hiding a folder hides what is inside, whatever its own flag.
    session.toggleLayerVisibility(outer);
    QCOMPARE(qAlpha(render(session).pixel(5, 5)), 0);
}

void TestLayers::folderIsPassThroughForBlendModesAndAdjustments()
{
    // Two overlapping opaque layers in a 50% folder over transparent: the top layer composites over the lower one
    // (mac pass-through), so the red shows through the translucent blue: purple, not pure blue.
    {
        EditorSession session;
        session.createDocument(10, 10);
        session.addGroup();
        const QUuid folder = active(session);
        session.insertImage(solid(10, 10, Qt::red), QStringLiteral("Red"));
        session.insertImage(solid(10, 10, Qt::blue), QStringLiteral("Blue"));
        session.setLayerOpacity(folder, 0.5);
        const QColor pixel = render(session).pixelColor(5, 5);
        QVERIFY2(pixel.red() > 20 && pixel.blue() > 100, qPrintable(QStringLiteral("rgba %1 %2 %3 %4").arg(pixel.red()).arg(pixel.green()).arg(pixel.blue()).arg(pixel.alpha())));
        QVERIFY(pixel.alpha() > 150);   // 1 - 0.5 * 0.5 = 0.75 coverage
    }
    // A Multiply layer inside a folder multiplies with what is below the folder, not with the folder alone.
    {
        EditorSession session;
        session.createDocument(10, 10);
        session.insertImage(solid(10, 10, QColor(255, 255, 0)), QStringLiteral("Yellow"));
        session.addGroup();
        session.insertImage(solid(10, 10, QColor(0, 255, 255)), QStringLiteral("Cyan"));
        session.setLayerBlendMode(active(session), BlendMode::Multiply);
        const QColor pixel = render(session).pixelColor(5, 5);
        QCOMPARE(pixel.red(), 0);
        QCOMPARE(pixel.green(), 255);
        QCOMPARE(pixel.blue(), 0);   // yellow x cyan = green
    }
    // An adjustment layer inside a folder acts on everything composited below it, the backdrop included.
    {
        EditorSession session;
        session.createDocument(10, 10);
        session.insertImage(solid(10, 10, Qt::white), QStringLiteral("White"));
        session.addGroup();
        QVERIFY(session.addAdjustment(QStringLiteral("Invert")));
        const QColor pixel = render(session).pixelColor(5, 5);
        QCOMPARE(pixel.red(), 0);
        QCOMPARE(pixel.green(), 0);
    }
}

void TestLayers::folderMasksClipEveryLayerInsideAndMultiplyWithTheirOwnMasks()
{
    EditorSession session = redSession();
    const QUuid red = active(session);
    session.groupSelectedLayers();
    const QUuid folder = active(session);
    QVERIFY(session.canEditMask());
    const int count = session.history().undoCount();
    QVERIFY(session.addLayerMask(false, false));
    QVERIFY(session.isMaskSelected() && !session.activeLayer()->mask.isNull());
    QCOMPARE(session.history().undoCount(), count + 1);
    QCOMPARE(alphas(render(session)), (QVector<int>{0, 0, 0, 0}));
    session.toggleLayerMask();
    QCOMPARE(alphas(render(session)), (QVector<int>{255, 255, 255, 255}));
    session.toggleLayerMask();
    // Soft folder coverage, then multiplied with the layer's own soft mask.
    Layer *f = findMutable(session, folder);
    f->mask = grayMask(2, 2, {255, 0, 128, 255});
    f->maskEnabled = true;
    f->transform.sampling = Sampling::Nearest;
    QVERIFY2(near(alphas(render(session)), {255, 0, 128, 255}), qPrintable(show(alphas(render(session)))));
    findMutable(session, red)->mask = grayMask(2, 2, {255, 0, 128, 255});
    QVERIFY2(near(alphas(render(session)), {255, 0, 64, 255}), qPrintable(show(alphas(render(session)))));
    // An enclosing folder's mask applies as well.
    findMutable(session, red)->mask = {};
    session.selectLayer(folder);
    session.groupSelectedLayers();
    QVERIFY(session.activeLayer()->group && active(session) != folder);
    QVERIFY(session.addLayerMask(false, false));
    QCOMPARE(alphas(render(session)), (QVector<int>{0, 0, 0, 0}));
    session.deleteLayerMask();
    QVERIFY(near(alphas(render(session)), {255, 0, 128, 255}));
    session.undo();
    QCOMPARE(alphas(render(session)), (QVector<int>{0, 0, 0, 0}));
}

void TestLayers::canvasExportAndCopyMergedAgree()
{
    EditorSession session;
    session.createDocument(20, 20);
    session.insertImage(solid(20, 20, QColor(255, 255, 255)), QStringLiteral("Paper"));
    session.addGroup();
    const QUuid folder = active(session);
    session.insertImage(solid(12, 12, QColor(200, 30, 30)), QStringLiteral("A"), QPointF(8, 8));
    session.setLayerBlendMode(active(session), BlendMode::Multiply);
    session.insertImage(solid(12, 12, QColor(30, 30, 200)), QStringLiteral("B"), QPointF(12, 12));
    session.setLayerOpacity(folder, 0.6);
    findMutable(session, folder)->mask = grayMask(2, 2, {255, 255, 0, 255});
    findMutable(session, folder)->transform.size = QSizeF(20, 20);
    const QImage flat = render(session);
    const auto merged = session.copiedPixels(true);
    QVERIFY(merged.has_value());
    QCOMPARE(merged->first, flat);
    // The composite is built from the same renderer the project export uses.
    QTemporaryDir dir;
    ProjectWriter::save(*session.document(), dir.filePath(QStringLiteral("x.comp")));
    const Document reloaded = ProjectReader::load(dir.filePath(QStringLiteral("x.comp")));
    QCOMPARE(LayerRenderer::flattened(reloaded), flat);
}

// ---- BlendShortcutTests / LayerAppearanceTests -----------------------------------------------------------

void TestLayers::shiftPlusAndMinusStepTheActiveLayersBlendMode()
{
    EditorSession session;
    session.createDocument(40, 20);
    session.addBlankLayer();
    QVERIFY(session.cycleBlendMode(true));
    QCOMPARE(session.activeLayer()->blendMode, BlendMode::Darken);   // after Normal comes the first darkening mode
    QVERIFY(session.cycleBlendMode(false));
    QVERIFY(session.cycleBlendMode(false));
    QCOMPARE(session.activeLayer()->blendMode, BlendMode::Luminosity);   // wraps past Normal to the last mode
    session.undo();
    QCOMPARE(session.activeLayer()->blendMode, BlendMode::Normal);
    // Folders and multiple selections have nothing to step.
    session.addGroup();
    QVERIFY(!session.cycleBlendMode(true));

    // Through the window's shortcut actions, as a key press arrives; not while a text field has the focus.
    MainWindow window;
    window.session().createDocument(40, 20);
    window.session().addBlankLayer();
    window.syncDocumentViews();
    QAction *next = window.findChild<QAction *>(QStringLiteral("nextBlendMode"));
    QAction *previous = window.findChild<QAction *>(QStringLiteral("previousBlendMode"));
    QVERIFY(next && previous);
    next->trigger();
    QCOMPARE(window.session().activeLayer()->blendMode, BlendMode::Darken);
    previous->trigger();
    QCOMPARE(window.session().activeLayer()->blendMode, BlendMode::Normal);
    QVERIFY(next->shortcuts().contains(QKeySequence(Qt::SHIFT | Qt::Key_Equal)));
    QVERIFY(previous->shortcuts().contains(QKeySequence(Qt::SHIFT | Qt::Key_Minus)));
}

void TestLayers::blendModeListFollowsPhotoshopsOrder()
{
    const QStringList expected{"Normal", "Darken", "Multiply", "Color Burn", "Linear Burn", "Lighten", "Screen", "Color Dodge",
                               "Linear Dodge (Add)", "Overlay", "Soft Light", "Hard Light", "Vivid Light", "Linear Light",
                               "Pin Light", "Hard Mix", "Difference", "Exclusion", "Subtract", "Divide", "Hue", "Saturation",
                               "Color", "Luminosity"};
    for (int i = 0; i < expected.size(); ++i) {
        QCOMPARE(blendModeToString(BlendMode(i)), expected.at(i));
        QCOMPARE(blendModeFromString(expected.at(i)), std::optional<BlendMode>(BlendMode(i)));
    }
    MainWindow window;
    auto *combo = window.findChild<QComboBox *>(QStringLiteral("blendMode"));
    QVERIFY(combo);
    QStringList shown;
    for (int i = 0; i < combo->count(); ++i) shown << combo->itemText(i);
    QCOMPARE(shown, expected);
}

void TestLayers::blendModesAndOpacityMatchKnownPixels()
{
    EditorSession session;
    session.createDocument(4, 4);
    const auto gray = [](double g) { return solid(4, 4, QColor::fromRgbF(g, g, g)); };
    session.insertImage(gray(0.4), QStringLiteral("Base"));
    session.insertImage(gray(0.8), QStringLiteral("Top"));
    const QUuid top = active(session);
    const struct { BlendMode mode; double expected; } cases[] = {
        {BlendMode::Normal, 0.8}, {BlendMode::Multiply, 0.32}, {BlendMode::Screen, 0.88}, {BlendMode::Overlay, 0.64},
        {BlendMode::Darken, 0.4}, {BlendMode::Lighten, 0.8}, {BlendMode::Difference, 0.4}, {BlendMode::ColorDodge, 1.0},
        {BlendMode::ColorBurn, 0.25},
    };
    for (const auto &c : cases) {
        session.setLayerBlendMode(top, c.mode);
        const QColor pixel = render(session).pixelColor(1, 1);
        QVERIFY2(std::abs(pixel.redF() - c.expected) < 0.02, qPrintable(blendModeToString(c.mode) + QStringLiteral(": ") + QString::number(pixel.redF())));
        QCOMPARE(pixel.alpha(), 255);
    }
    session.setLayerBlendMode(top, BlendMode::Normal);
    session.setLayerOpacity(top, 0.5);
    QVERIFY(std::abs(render(session).pixelColor(1, 1).redF() - 0.6) < 0.02);
    session.setLayerOpacity(top, 0);
    QVERIFY(std::abs(render(session).pixelColor(1, 1).redF() - 0.4) < 0.02);
}

void TestLayers::opacitySliderDragIsOneUndo()
{
    MainWindow window;
    window.session().createDocument(4, 4);
    window.session().insertImage(solid(4, 4, QColor(200, 200, 200)), QStringLiteral("Gray"));
    window.syncDocumentViews();
    auto *slider = window.findChild<QSlider *>(QStringLiteral("layerOpacity"));
    QVERIFY(slider);
    const int count = window.session().history().undoCount();
    slider->setSliderDown(true);
    for (int value = 90; value >= 20; value -= 10) slider->setValue(value);
    slider->setSliderDown(false);
    QCOMPARE(window.session().history().undoCount(), count + 1);
    QVERIFY(std::abs(window.session().activeLayer()->opacity - 0.2) < 0.001);
    window.session().undo();
    QCOMPARE(window.session().activeLayer()->opacity, 1.0);
    window.session().redo();
    QVERIFY(std::abs(window.session().activeLayer()->opacity - 0.2) < 0.001);
}

void TestLayers::selectedFoldersTakeTypedOpacityAndOpacitySurvivesSave()
{
    EditorSession session;
    session.createDocument(4, 4);
    session.insertImage(solid(4, 4, QColor(50, 50, 50)), QStringLiteral("A"));
    const QUuid first = active(session);
    session.insertImage(solid(4, 4, QColor(200, 200, 200)), QStringLiteral("B"));
    const QUuid second = active(session);
    session.addGroup();
    const QUuid folder = active(session);
    session.selectLayers({first, second, folder}, second);
    const int count = session.history().undoCount();
    session.setSelectedLayersOpacity(0.5);
    QCOMPARE(find(session, first)->opacity, 0.5);
    QCOMPARE(find(session, second)->opacity, 0.5);
    QCOMPARE(find(session, folder)->opacity, 0.5);     // a selected folder takes the value too
    QCOMPARE(session.history().undoCount(), count + 1);
    QVERIFY(session.document()->formatVersion >= 8);   // folder opacity needs format 8
    session.setSelectedLayersOpacity(1.0);
    QCOMPARE(find(session, first)->opacity, 1.0);
    session.undo();
    QCOMPARE(find(session, first)->opacity, 0.5);

    // Appearance persists through save.
    session.selectLayer(first);
    session.setLayerBlendMode(first, BlendMode::Multiply);
    session.setLayerOpacity(first, 0.25);
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.comp"));
    ProjectWriter::save(*session.document(), path);
    const Document loaded = ProjectReader::load(path);
    for (const Layer &layer : loaded.layers) if (layer.id == first) {
        QCOMPARE(layer.blendMode, BlendMode::Multiply);
        QVERIFY(std::abs(layer.opacity - 0.25) < 0.001);
    }
    for (const Layer &layer : loaded.layers) if (layer.id == folder) QVERIFY(std::abs(layer.opacity - 0.5) < 0.001);
}

// ---- LayerMaskTests --------------------------------------------------------------------------------------

void TestLayers::addDisableDeleteUndoAndTargetSelection()
{
    EditorSession session = redSession();
    const QImage original = session.activeLayer()->image;
    const int count = session.history().undoCount();
    QVERIFY(session.addLayerMask(false, false));
    QVERIFY(session.isMaskSelected() && !session.activeLayer()->mask.isNull());
    QCOMPARE(session.activeLayer()->mask.width(), 1);
    QCOMPARE(session.history().undoCount(), count + 1);
    QVERIFY(!session.addLayerMask(true, false));       // do not overwrite an existing mask
    QCOMPARE(session.history().undoCount(), count + 1);
    session.toggleLayerMask();
    QVERIFY(!session.activeLayer()->maskEnabled);
    session.deleteLayerMask();
    QVERIFY(session.activeLayer()->mask.isNull() && !session.isMaskSelected());
    session.undo();
    QVERIFY(!session.activeLayer()->maskEnabled);
    session.undo();
    QVERIFY(session.activeLayer()->maskEnabled);
    session.undo();
    QVERIFY(session.activeLayer()->mask.isNull());
    session.redo();
    session.selectMaskTarget(true);
    QVERIFY(session.isMaskSelected());
    session.selectMaskTarget(false);
    QVERIFY(!session.isMaskSelected());
    QCOMPARE(session.activeLayer()->image, original);
    session.addGroup();
    QVERIFY(session.addLayerMask(true, false));
    QVERIFY(session.activeLayer()->group && !session.activeLayer()->mask.isNull());
}

void TestLayers::coverageOpacityAndDisabledMasksRenderCorrectly()
{
    EditorSession session = redSession();
    const QUuid id = active(session);
    session.addLayerMask(true, false);
    QCOMPARE(alphas(render(session)), (QVector<int>{255, 255, 255, 255}));
    session.deleteLayerMask();
    session.addLayerMask(false, false);
    QCOMPARE(alphas(render(session)), (QVector<int>{0, 0, 0, 0}));
    session.toggleLayerMask();
    QCOMPARE(alphas(render(session)), (QVector<int>{255, 255, 255, 255}));
    findMutable(session, id)->mask = grayMask(2, 2, {255, 0, 128, 255});
    findMutable(session, id)->maskEnabled = true;
    QCOMPARE(alphas(render(session)), (QVector<int>{255, 0, 128, 255}));
    session.setLayerOpacity(id, 0.5);
    QVERIFY2(near(alphas(render(session)), {128, 0, 64, 128}), qPrintable(show(alphas(render(session)))));
}

void TestLayers::transformedMaskStaysAligned()
{
    EditorSession session = redSession();
    const QUuid id = active(session);
    findMutable(session, id)->mask = grayMask(2, 2, {255, 0, 128, 255});
    findMutable(session, id)->transform.flipX = true;
    QCOMPARE(alphas(render(session)), (QVector<int>{0, 255, 255, 128}));
    // Rotation uses the same local coverage as the pixels: the covered area is unchanged.
    findMutable(session, id)->transform.flipX = false;
    findMutable(session, id)->transform.rotation = 90;
    QVector<int> rotated = alphas(render(session));
    std::sort(rotated.begin(), rotated.end());
    QVERIFY2(near(rotated, {0, 128, 255, 255}), qPrintable(show(rotated)));
    // Canvas Size keeps the mask where it was relative to its layer.
    findMutable(session, id)->transform.rotation = 0;
    QVERIFY(session.resizeCanvas(QSize(6, 6)));
    const Layer *layer = find(session, id);
    QCOMPARE(layer->mask.size(), QSize(2, 2));
    QCOMPARE(layer->mask.pixelColor(1, 1).red(), 255);
    session.undo();
    QCOMPARE(session.document()->canvasSize, QSize(2, 2));
}

void TestLayers::projectAndPngPreserveCoverageAndDisabledState()
{
    EditorSession session = redSession();
    const QUuid id = active(session);
    findMutable(session, id)->mask = grayMask(2, 2, {255, 0, 128, 255});
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("Masks.comp"));
    for (bool enabled : {true, false}) {
        findMutable(session, id)->maskEnabled = enabled;
        ProjectWriter::save(*session.document(), path);
        const Document loaded = ProjectReader::load(path);
        QCOMPARE(loaded.layers.first().maskEnabled, enabled);
        QCOMPARE(loaded.layers.first().mask.pixelColor(0, 1).red(), 128);
        const QVector<int> expected = enabled ? QVector<int>{255, 0, 128, 255} : QVector<int>{255, 255, 255, 255};
        QVERIFY2(near(alphas(LayerRenderer::flattened(loaded)), expected), qPrintable(show(alphas(LayerRenderer::flattened(loaded)))));
    }
}

void TestLayers::folderMaskCanBePaintedInvertedAndLoadedAsASelection()
{
    EditorSession session;
    session.createDocument(40, 20);
    session.insertImage(solid(40, 20, Qt::red), QStringLiteral("Red"));
    session.groupSelectedLayers();
    const QUuid folder = active(session);
    session.addLayerMask(true, false);
    QVERIFY(session.isMaskSelected());
    // Painting black on the folder's mask hides the layer inside only there.
    QVERIFY(session.beginBrushStroke(QPointF(10, 10), Qt::black, 8, 1.0, 1.0, false));
    session.continueBrushStroke(QPointF(11, 10));
    session.endBrushStroke();
    QImage image = render(session);
    QCOMPARE(qAlpha(image.pixel(10, 10)), 0);
    QCOMPARE(qAlpha(image.pixel(30, 10)), 255);
    QCOMPARE(active(session), folder);
    QVERIFY(session.isMaskSelected());
    // Invert swaps what the folder hides.
    QVERIFY(session.invertActiveLayerPixels());
    image = render(session);
    QCOMPARE(qAlpha(image.pixel(10, 10)), 255);
    QCOMPARE(qAlpha(image.pixel(30, 10)), 0);
    // Choosing "Mask's Black Areas" selects what the mask hides.
    QVERIFY(session.loadMaskAsSelection(SelectionMode::Replace, folder));
    const auto bounds = session.selectionBounds();
    QVERIFY(bounds && bounds->width() > 30);
}

void TestLayers::maskFromSelectionRevealsOrHidesIt()
{
    EditorSession session;
    session.createDocument(20, 20);
    session.insertImage(solid(20, 20, Qt::red), QStringLiteral("Red"));
    QVERIFY(session.setRectangularSelection(QRect(0, 0, 10, 20)));
    QVERIFY(session.addLayerMask(true, true));      // Reveal Selection
    QVERIFY2(!session.document()->selection.has_value(), "the selection is used up");
    QImage image = render(session);
    QCOMPARE(qAlpha(image.pixel(5, 5)), 255);
    QCOMPARE(qAlpha(image.pixel(15, 5)), 0);
    session.undo();
    QVERIFY(session.document()->selection.has_value());
    QVERIFY(session.activeLayer()->mask.isNull());
    QVERIFY(session.addLayerMask(false, true));     // Hide Selection (Option-click)
    image = render(session);
    QCOMPARE(qAlpha(image.pixel(5, 5)), 0);
    QCOMPARE(qAlpha(image.pixel(15, 5)), 255);
}

// ---- LiveMaskTests ---------------------------------------------------------------------------------------

void TestLayers::clippingColorPreservesSoftBaseAlphaWithoutBlackFringe()
{
    EditorSession s;
    s.createDocument(2, 2);
    s.insertImage(asset({255, 128, 32, 0}, 0), QStringLiteral("Base"));
    s.insertImage(asset({255, 255, 255, 255}), QStringLiteral("Top"));
    const QUuid top = active(s);
    QVERIFY(s.toggleClippingMask(top));
    for (Layer &layer : s.document()->layers) layer.transform.sampling = Sampling::Nearest;
    QImage result = render(s);
    QCOMPARE(alphas(result), (QVector<int>{255, 128, 32, 0}));
    for (int i = 0; i < 4; ++i) {
        const uchar *p = result.constScanLine(i / 2) + (i % 2) * 4;
        QCOMPARE(int(p[0]), int(p[3]));          // premultiplied red = alpha: no black fringe
        QCOMPARE(int(p[1]), 0);
        QCOMPARE(int(p[2]), 0);
    }
    findMutable(s, top)->opacity = 0.5;
    QCOMPARE(alphas(render(s)), (QVector<int>{255, 128, 32, 0}));   // the clipped layer's own opacity does not weaken the base
    findMutable(s, top)->opacity = 1;
    s.insertImage(solid(2, 2, Qt::white), QStringLiteral("White"));
    const QUuid background = active(s);
    QVERIFY(s.placeLayer(background, std::nullopt, std::nullopt, true));
    const QImage flattened = render(s);
    QCOMPARE(alphas(flattened), (QVector<int>{255, 255, 255, 255}));
    for (int i = 0; i < 4; ++i) QCOMPARE(int(flattened.constScanLine(i / 2)[(i % 2) * 4]), 255);
}

void TestLayers::optionClickCreatesSharedStackAndDragOutReleases()
{
    EditorSession s;
    s.createDocument(2, 2);
    for (int i = 0; i < 3; ++i) s.insertImage(asset({255, 255, 255, 255}), QStringLiteral("L%1").arg(i));
    QVector<QUuid> ids;
    for (const Layer &layer : s.document()->layers) ids << layer.id;
    QVERIFY(!s.canToggleClippingMask(ids[0]) && s.canToggleClippingMask(ids[1]));
    QVERIFY(!s.toggleClippingMask(ids[0]));
    QVERIFY(!find(s, ids[0])->maskSourceId.has_value());
    s.toggleClippingMask(ids[1]); s.toggleClippingMask(ids[2]);
    QCOMPARE(find(s, ids[1])->maskSourceId, std::optional<QUuid>(ids[0]));
    QCOMPARE(find(s, ids[2])->maskSourceId, std::optional<QUuid>(ids[0]));
    QVERIFY(s.canToggleClippingMask(ids[2]));
    s.toggleClippingMask(ids[2]);
    QVERIFY(!find(s, ids[2])->maskSourceId.has_value());
    QCOMPARE(find(s, ids[1])->maskSourceId, std::optional<QUuid>(ids[0]));
    s.toggleClippingMask(ids[2]); s.toggleClippingMask(ids[1]);
    QVERIFY(!find(s, ids[1])->maskSourceId.has_value() && !find(s, ids[2])->maskSourceId.has_value());
    s.undo();
    QCOMPARE(find(s, ids[2])->maskSourceId, std::optional<QUuid>(ids[0]));
    QVERIFY(s.placeLayer(ids[2], std::nullopt, std::nullopt, true));
    QCOMPARE(s.document()->layers.first().id, ids[2]);
    QVERIFY(!s.document()->layers.first().maskSourceId.has_value());
    QCOMPARE(s.document()->layers.last().maskSourceId, std::optional<QUuid>(ids[0]));
    s.undo();
    QCOMPARE(s.document()->layers.last().id, ids[2]);
    QCOMPARE(s.document()->layers.last().maskSourceId, std::optional<QUuid>(ids[0]));
    // Dropped between a base and a layer clipped to it, a layer joins the clipping group.
    EditorSession joined;
    joined.createDocument(2, 2);
    for (int i = 0; i < 3; ++i) joined.insertImage(asset({255, 255, 255, 255}), QStringLiteral("J%1").arg(i));
    QVector<QUuid> j;
    for (const Layer &layer : joined.document()->layers) j << layer.id;
    joined.toggleClippingMask(j[2]);                  // j2 clips to j1
    joined.insertImage(asset({255, 255, 255, 255}), QStringLiteral("Dropped"));
    const QUuid dropped = active(joined);
    QVERIFY(joined.placeLayer(dropped, std::nullopt, j[1]));
    QCOMPARE(find(joined, dropped)->maskSourceId, std::optional<QUuid>(j[1]));
}

static EditorSession liveFixture(QUuid *target, QUuid *source)
{
    EditorSession s;
    s.createDocument(2, 2);
    s.insertImage(asset({255, 255, 255, 255}), QStringLiteral("Target"));
    s.insertImage(asset({255, 0, 128, 255}, 0), QStringLiteral("Source"));
    for (Layer &layer : s.document()->layers) layer.transform.sampling = Sampling::Nearest;
    s.document()->layers[1].visible = false;
    *target = s.document()->layers[0].id;
    *source = s.document()->layers[1].id;
    s.linkMask(*source, *target);
    return s;
}

void TestLayers::hiddenBlackSourceSuppliesAlphaAndRasterMasksMultiply()
{
    QUuid target, source;
    EditorSession s = liveFixture(&target, &source);
    QCOMPARE(alphas(render(s)), (QVector<int>{255, 0, 128, 255}));
    findMutable(s, source)->opacity = 0.5;
    const QVector<int> a = alphas(render(s));
    QVERIFY2(std::abs(a[0] - 128) <= 1 && a[1] == 0 && std::abs(a[2] - 64) <= 1, qPrintable(show(a)));
    QImage gray(2, 2, QImage::Format_Grayscale8);
    gray.fill(128);
    findMutable(s, target)->mask = gray;
    QVERIFY(alphas(render(s))[0] < a[0]);
}

void TestLayers::liveMaskCyclesUndoPersistenceBakeAndDelete()
{
    QUuid target, source;
    EditorSession s = liveFixture(&target, &source);
    QVERIFY(!s.linkMask(target, source));      // a cycle
    QVERIFY(!s.linkMask(target, target));
    s.undo();
    QVERIFY(!find(s, target)->maskSourceId.has_value());
    s.redo();
    QCOMPARE(find(s, target)->maskSourceId, std::optional<QUuid>(source));
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("Live.comp"));
    ProjectWriter::save(*s.document(), path);
    const Document loaded = ProjectReader::load(path);
    QCOMPARE(loaded.layers[0].maskSourceId, std::optional<QUuid>(source));
    const QVector<int> before = alphas(LayerRenderer::flattened(loaded));
    // Bake and delete: the dependent keeps its masked look as plain pixels.
    s.selectLayer(source);
    QVERIFY(!s.selectedDeletionLiveMaskDependents().isEmpty());
    s.deleteSelectedLayers(true);
    QCOMPARE(s.document()->layers.size(), 1);
    QVERIFY(!s.document()->layers[0].maskSourceId.has_value());
    QVERIFY2(near(alphas(render(s)), before), qPrintable(show(alphas(render(s))) + QStringLiteral(" vs ") + show(before)));
    s.undo();
    QCOMPARE(s.document()->layers.size(), 2);
    QCOMPARE(s.document()->layers[0].maskSourceId, std::optional<QUuid>(source));
    // Remove Links and Delete: the pixels come back unmasked.
    s.selectLayer(source);
    s.deleteSelectedLayers(false);
    QCOMPARE(alphas(render(s)), (QVector<int>{255, 255, 255, 255}));
}

// Audit: baking keeps only the live coverage in the pixels. The layer keeps its transform (and its pixels past the
// canvas), its raster mask and its effects, so its effects are drawn once, not baked in and drawn again (mac LiveMaskBaker).
void TestLayers::bakeAndDeleteKeepsTheLayersOwnGridMaskAndEffects()
{
    EditorSession s;
    s.createDocument(100, 100);
    s.insertImage(solid(40, 40, Qt::white), QStringLiteral("Base"), QPointF(50, 50));
    const QUuid base = active(s);
    s.insertImage(solid(160, 60, Qt::blue), QStringLiteral("Top"), QPointF(50, 50));   // reaches past the canvas
    const QUuid top = active(s);
    QImage mask(160, 60, QImage::Format_Grayscale8); mask.fill(255);
    for (int y = 0; y < 60; ++y) for (int x = 0; x < 80; ++x) mask.scanLine(y)[x] = 0;    // hides its left half
    findMutable(s, top)->mask = mask;
    LayerEffects effects; StrokeEffect stroke; stroke.size = 3; stroke.red = 1; effects.stroke = stroke;
    QVERIFY(s.setLayerEffects(top, effects, QStringLiteral("Stroke")));
    QVERIFY(s.toggleClippingMask(top));
    const LayerTransform placed = find(s, top)->transform;
    s.selectLayer(base);
    s.deleteSelectedLayers(true);
    const Layer *baked = find(s, top);
    QVERIFY(baked);
    QCOMPARE(baked->transform, placed);
    QCOMPARE(baked->image.size(), QSize(160, 60));
    QCOMPARE(baked->mask, mask);
    QCOMPARE(baked->effects, std::optional<LayerEffects>(effects));
    QVERIFY(!baked->maskSourceId.has_value());
    // Inside the old base the pixels stay; outside it (and past the canvas) the clip took them away.
    QCOMPARE(qAlpha(baked->image.pixel(80, 30)), 255);
    QCOMPARE(qAlpha(baked->image.pixel(150, 30)), 0);
    QCOMPARE(qAlpha(baked->image.pixel(100, 2)), 0);
}

void TestLayers::mergeLayersTakesTheTopmostSelectedName()
{
    EditorSession s;
    s.createDocument(20, 20);
    s.insertImage(solid(10, 10, Qt::red), QStringLiteral("Bottom"));
    const QUuid bottom = active(s);
    s.insertImage(solid(10, 10, Qt::blue), QStringLiteral("Top"));
    const QUuid top = active(s);
    s.selectLayers({bottom, top}, bottom);          // the lower one active
    QVERIFY(s.mergeLayers());
    QCOMPARE(s.document()->layers.size(), 1);
    QCOMPARE(s.document()->layers.first().name, QStringLiteral("Top"));
}

void TestLayers::flipLayersLeavesHiddenLayersAlone()
{
    EditorSession s;
    s.createDocument(100, 100);
    s.insertImage(solid(10, 10, Qt::red), QStringLiteral("Shown"), QPointF(20, 50));
    const QUuid shown = active(s);
    s.insertImage(solid(10, 10, Qt::blue), QStringLiteral("Hidden"), QPointF(80, 50));
    const QUuid hidden = active(s);
    findMutable(s, hidden)->visible = false;
    const LayerTransform before = find(s, hidden)->transform;
    s.selectLayers({shown, hidden}, shown);
    QVERIFY(s.flipLayers(true));
    QCOMPARE(find(s, hidden)->transform, before);
    QVERIFY(find(s, shown)->transform.flipX);
}

void TestLayers::releasingClippingSkipsAFoldersContents()
{
    EditorSession s;
    s.createDocument(20, 20);
    s.insertImage(solid(10, 10, Qt::red), QStringLiteral("Base"));
    const QUuid base = active(s);
    s.insertImage(solid(10, 10, Qt::blue), QStringLiteral("One"));
    const QUuid one = active(s);
    s.insertImage(solid(10, 10, Qt::green), QStringLiteral("Two"));
    const QUuid two = active(s);
    QVERIFY(s.toggleClippingMask(one));
    QVERIFY(s.toggleClippingMask(two));
    QCOMPARE(find(s, two)->maskSourceId, std::optional<QUuid>(base));
    // A folder's child lying between the clipped siblings in the flat list (not one of their siblings).
    Layer stray = pixelLayer(QStringLiteral("Stray"), solid(4, 4, Qt::black));
    s.addGroup();
    stray.parentId = active(s);
    s.document()->layers.insert(s.document()->layers.indexOf(*find(s, two)), stray);
    QVERIFY(s.toggleClippingMask(one));             // releases One and every clipped sibling above it
    QVERIFY(!find(s, one)->maskSourceId.has_value());
    QVERIFY(!find(s, two)->maskSourceId.has_value());
}

void TestLayers::copiedPixelsLeaveEffectsBehind()
{
    EditorSession s;
    s.createDocument(40, 40);
    s.insertImage(solid(10, 10, Qt::blue), QStringLiteral("Square"), QPointF(20, 20));
    LayerEffects effects; StrokeEffect stroke; stroke.size = 6; stroke.red = 1; effects.stroke = stroke;
    QVERIFY(s.setLayerEffects(active(s), effects, QStringLiteral("Stroke")));
    s.selectAll();
    const auto copied = s.copiedPixels(false);
    QVERIFY(copied);
    QCOMPARE(copied->first.size(), QSize(40, 40));      // the selection's bounds
    QCOMPARE(qAlpha(copied->first.pixel(20, 20)), 255);
    QCOMPARE(qAlpha(copied->first.pixel(13, 20)), 0);    // where the stroke shows on the canvas: just the pixels came
}

void TestLayers::colorDodgeKeepsBlackAndColorBurnKeepsWhite()
{
    EditorSession s;
    s.createDocument(4, 4);
    s.insertImage(solid(4, 4, Qt::black), QStringLiteral("Black"));
    s.insertImage(solid(4, 4, Qt::white), QStringLiteral("Dodge"));
    s.setLayerBlendMode(active(s), BlendMode::ColorDodge);
    QCOMPARE(QColor(render(s).pixel(1, 1)), QColor(Qt::black));
    EditorSession b;
    b.createDocument(4, 4);
    b.insertImage(solid(4, 4, Qt::white), QStringLiteral("White"));
    b.insertImage(solid(4, 4, Qt::black), QStringLiteral("Burn"));
    b.setLayerBlendMode(active(b), BlendMode::ColorBurn);
    QCOMPARE(QColor(render(b).pixel(1, 1)), QColor(Qt::white));
}

void TestLayers::pasteFindsWhereThePixelsCameFrom()
{
    MainWindow window;
    EditorSession &s = window.session();
    s.createDocument(100, 100);
    s.insertImage(solid(10, 10, Qt::red), QStringLiteral("Square"), QPointF(20, 20));
    QVERIFY(s.setRectangularSelection(QRect(15, 15, 10, 10)));
    window.syncDocumentViews();
    window.findChild<QAction *>(QStringLiteral("commandCopy"))->trigger();
    // The clipboard holds the picture re-encoded, as a clipboard manager leaves it, with the origin beside it.
    const QMimeData *held = QGuiApplication::clipboard()->mimeData();
    QVERIFY(held && held->hasFormat(QStringLiteral("application/x-compositor-pixel-origin")));
    auto *reencoded = new QMimeData;
    reencoded->setImageData(qvariant_cast<QImage>(held->imageData()).convertToFormat(QImage::Format_ARGB32));
    reencoded->setData(QStringLiteral("application/x-compositor-pixel-origin"), held->data(QStringLiteral("application/x-compositor-pixel-origin")));
    QGuiApplication::clipboard()->setMimeData(reencoded);
    window.findChild<QAction *>(QStringLiteral("commandPaste"))->trigger();
    QCOMPARE(s.activeLayer()->transform.origin, QPointF(15, 15));
    // A picture from elsewhere, with no origin, is centered.
    QGuiApplication::clipboard()->setImage(solid(10, 10, Qt::blue));
    window.findChild<QAction *>(QStringLiteral("commandPaste"))->trigger();
    QCOMPARE(s.activeLayer()->transform.origin, QPointF(45, 45));
}

void TestLayers::movingSourceChangesCoverageAndChainsMultiply()
{
    QUuid target, source;
    EditorSession s = liveFixture(&target, &source);
    findMutable(s, source)->transform.origin.rx() += 1;
    QCOMPARE(alphas(render(s)), (QVector<int>{0, 255, 0, 128}));
    findMutable(s, source)->transform.origin.rx() -= 1;
    s.insertImage(asset({0, 255, 255, 255}, 0), QStringLiteral("Second"));
    const QUuid second = active(s);
    findMutable(s, second)->visible = false;
    findMutable(s, second)->transform.sampling = Sampling::Nearest;
    QVERIFY(s.linkMask(second, source));
    QCOMPARE(alphas(render(s)), (QVector<int>{0, 0, 128, 255}));
}

// ---- MaskAloneTests / MaskTransformTests -----------------------------------------------------------------

void TestLayers::maskAloneToggles()
{
    EditorSession s;
    s.createDocument(200, 100);
    s.insertImage(solid(200, 100, Qt::red), QStringLiteral("Red"));
    const QUuid id = active(s);
    QImage mask(200, 100, QImage::Format_Grayscale8);
    mask.fill(0);
    for (int y = 0; y < 100; ++y) for (int x = 0; x < 100; ++x) mask.scanLine(y)[x] = 255;
    findMutable(s, id)->mask = mask;
    s.toggleMaskAlone(id);
    QCOMPARE(s.maskAloneLayerId(), std::optional<QUuid>(id));
    QVERIFY(s.isMaskSelected());
    s.toggleMaskAlone(id);
    QVERIFY(!s.maskAloneLayerId());
    QVERIFY(s.isMaskSelected());
    s.toggleMaskAlone(id);
    s.selectMaskTarget(false);
    QVERIFY(!s.maskAloneLayerId());
    s.toggleMaskAlone(id);
    s.deleteLayerMask();
    QVERIFY(!s.maskAloneLayerId());
    s.undo();
    QVERIFY(!s.maskAloneLayerId());
}

void TestLayers::aLayerWithoutAMaskHasNothingToShow()
{
    EditorSession s;
    s.createDocument(20, 10);
    s.addBlankLayer();
    const QUuid id = active(s);
    s.toggleMaskAlone(id);
    QVERIFY(!s.maskAloneLayerId() && !s.isMaskSelected());
}

void TestLayers::unlinkedMaskKeepsItsPlaceWhenTheLayerFlips()
{
    // Mac MaskTransformTests: a linked mask follows its layer; unlinked it stays where it was on the canvas.
    EditorSession s;
    s.createDocument(40, 20);
    s.insertImage(solid(40, 20, Qt::red), QStringLiteral("Layer"));
    const QUuid id = active(s);
    QImage mask(40, 20, QImage::Format_Grayscale8);
    mask.fill(255);
    for (int y = 5; y < 15; ++y) for (int x = 10; x < 20; ++x) mask.scanLine(y)[x] = 0;
    findMutable(s, id)->mask = mask;
    findMutable(s, id)->transform.sampling = Sampling::Nearest;
    QVERIFY(s.flipLayers(true));                   // linked: the hole flips with the layer to x 20..30
    QCOMPARE(qAlpha(render(s).pixel(25, 10)), 0);
    QCOMPARE(qAlpha(render(s).pixel(15, 10)), 255);
    s.undo();
    QVERIFY(s.toggleMaskLink());
    QVERIFY(!find(s, id)->maskLinked);
    QVERIFY(s.flipLayers(true));                   // unlinked: the hole stays at x 10..20
    QCOMPARE(qAlpha(render(s).pixel(15, 10)), 0);
    QCOMPARE(qAlpha(render(s).pixel(25, 10)), 255);
    QVERIFY(find(s, id)->maskPlacement.has_value());
    s.undo();
    QVERIFY(!find(s, id)->maskPlacement.has_value());
}

// ---- merge -----------------------------------------------------------------------------------------------

void TestLayers::mergeDownMergeLayersAndMergeGroup()
{
    EditorSession s;
    s.createDocument(20, 20);
    s.insertImage(solid(10, 10, Qt::red), QStringLiteral("Red"), QPointF(5, 5));
    s.insertImage(solid(10, 10, QColor(0, 0, 255)), QStringLiteral("Blue"), QPointF(10, 10));
    const QUuid blue = active(s);
    QCOMPARE(s.mergeTitle(), QStringLiteral("Merge Down"));
    QVERIFY(s.canMergeLayers());
    const QImage before = render(s);
    QVERIFY(s.mergeLayers());
    QCOMPARE(s.document()->layers.size(), 1);
    QCOMPARE(s.document()->layers.first().name, QStringLiteral("Red"));          // takes the lower layer's name
    QCOMPARE(s.document()->layers.first().transform.size, QSizeF(15, 15));        // trimmed to what is there
    QCOMPARE(render(s), before);
    QCOMPARE(s.history().undoName(), QStringLiteral("Merge Down"));
    s.undo();
    QCOMPARE(s.document()->layers.size(), 2);
    QVERIFY(find(s, blue));

    // Merge Group flattens the folder's contents, opacity and blend modes baked in; the folder goes.
    EditorSession g;
    g.createDocument(20, 20);
    g.addGroup();
    const QUuid folder = g.document()->activeLayerId.value();
    g.insertImage(solid(10, 10, Qt::red), QStringLiteral("A"), QPointF(5, 5));
    g.setLayerOpacity(active(g), 0.5);
    g.insertImage(solid(10, 10, Qt::blue), QStringLiteral("B"), QPointF(10, 10));
    g.selectLayer(folder);
    QCOMPARE(g.mergeTitle(), QStringLiteral("Merge Group"));
    const QImage groupLook = render(g);
    QVERIFY(g.mergeLayers());
    QCOMPARE(g.document()->layers.size(), 1);
    QVERIFY(!g.document()->layers.first().group);
    QCOMPARE(g.document()->layers.first().name, find(g, g.document()->layers.first().id)->name);
    QCOMPARE(render(g), groupLook);

    // Several selected layers merge together, blend modes baked in.
    EditorSession m;
    m.createDocument(10, 10);
    m.insertImage(solid(10, 10, QColor(255, 255, 0)), QStringLiteral("Y"));
    const QUuid y = active(m);
    m.insertImage(solid(10, 10, QColor(0, 255, 255)), QStringLiteral("C"));
    m.setLayerBlendMode(active(m), BlendMode::Multiply);
    const QUuid c = active(m);
    m.selectLayers({y, c}, c);
    QCOMPARE(m.mergeTitle(), QStringLiteral("Merge Layers"));
    QVERIFY(m.mergeLayers());
    QCOMPARE(m.document()->layers.size(), 1);
    const QColor merged = render(m).pixelColor(5, 5);
    QCOMPARE(merged.red(), 0);
    QCOMPARE(merged.green(), 255);
    QCOMPARE(merged.blue(), 0);
}

// ---- layer effects (InnerGlowTests / OuterGlowTests) -----------------------------------------------------

static QImage squareWithMargin(int size, int inner, const QColor &color)
{
    QImage image(size, size, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    const int margin = (size - inner) / 2;
    for (int y = margin; y < margin + inner; ++y) for (int x = margin; x < margin + inner; ++x) image.setPixelColor(x, y, color);
    return image;
}

void TestLayers::innerGlowRendersInsideSourceWithoutBoundsExpansion()
{
    QImage source(40, 40, QImage::Format_RGBA8888_Premultiplied);
    source.fill(Qt::black);
    InnerGlowEffect glow; glow.red = 1; glow.green = 1; glow.blue = 0; glow.size = 12; glow.opacity = 1.0;
    LayerEffects effects; effects.innerGlow = glow;
    QCOMPARE(LayerEffectsRenderer::margin(effects), 2.0);       // an inner glow does not grow the layer
    const auto [image, inset] = LayerEffectsRenderer::render(source, QImage(), effects);
    const int in = int(inset);
    QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
    const QColor edge = image.pixelColor(in + 2, in + 20);
    QVERIFY(edge.alphaF() > 0.9);
    QVERIFY(edge.redF() > 0.3 && edge.greenF() > 0.3);
    const QColor center = image.pixelColor(in + 20, in + 20);
    QVERIFY(center.redF() < 0.2 && center.greenF() < 0.2);
}

void TestLayers::outerGlowRendersOmnidirectionally()
{
    const QImage image = squareWithMargin(40, 20, Qt::white);
    LayerEffects effects;
    effects.outerGlow = OuterGlowEffect{.enabled = true, .size = 10, .red = 0, .green = 1, .blue = 0, .opacity = 1.0};
    const auto [rendered, inset] = LayerEffectsRenderer::render(image, QImage(), effects);
    QVERIFY(inset > 0);
    const int in = int(inset);
    const QColor centerPixel = rendered.pixelColor(in + 20, in + 20);
    QVERIFY(centerPixel.alphaF() > 0.95 && centerPixel.redF() > 0.95 && centerPixel.greenF() > 0.95 && centerPixel.blueF() > 0.95);
    const QColor left = rendered.pixelColor(in + 5, in + 20), right = rendered.pixelColor(in + 35, in + 20);
    const QColor topGlow = rendered.pixelColor(in + 20, in + 5), bottom = rendered.pixelColor(in + 20, in + 35);
    for (const QColor &glow : {left, right, topGlow, bottom}) {
        QVERIFY(glow.alphaF() > 0.1);
        QVERIFY(glow.greenF() > 0.8);
    }
    QVERIFY(std::abs(left.alphaF() - right.alphaF()) < 0.05);
    QVERIFY(std::abs(topGlow.alphaF() - bottom.alphaF()) < 0.05);
    QVERIFY(std::abs(left.alphaF() - topGlow.alphaF()) < 0.05);

    // Size and opacity variations.
    LayerEffects small, large;
    small.outerGlow = OuterGlowEffect{.enabled = true, .size = 4, .red = 1, .green = 0, .blue = 0, .opacity = 1.0};
    large.outerGlow = OuterGlowEffect{.enabled = true, .size = 20, .red = 1, .green = 0, .blue = 0, .opacity = 1.0};
    const auto [smallImage, smallInset] = LayerEffectsRenderer::render(image, QImage(), small);
    const auto [largeImage, largeInset] = LayerEffectsRenderer::render(image, QImage(), large);
    QVERIFY(largeInset > smallInset);
    QVERIFY(largeImage.pixelColor(int(largeInset) + 2, int(largeInset) + 20).alphaF() > smallImage.pixelColor(int(smallInset) + 2, int(smallInset) + 20).alphaF());
    LayerEffects low, high;
    low.outerGlow = OuterGlowEffect{.enabled = true, .size = 10, .red = 0, .green = 0, .blue = 1, .opacity = 0.2};
    high.outerGlow = OuterGlowEffect{.enabled = true, .size = 10, .red = 0, .green = 0, .blue = 1, .opacity = 1.0};
    const auto [lowImage, lowInset] = LayerEffectsRenderer::render(image, QImage(), low);
    const auto [highImage, highInset] = LayerEffectsRenderer::render(image, QImage(), high);
    QVERIFY(highImage.pixelColor(int(highInset) + 5, int(highInset) + 20).alphaF() > lowImage.pixelColor(int(lowInset) + 5, int(lowInset) + 20).alphaF());
}

void TestLayers::outerGlowCombinedWithStrokeAndDropShadow()
{
    const QImage image = squareWithMargin(50, 20, Qt::white);
    LayerEffects effects;
    effects.stroke = StrokeEffect{.enabled = true, .size = 3, .red = 0, .green = 0, .blue = 0, .opacity = 1.0, .inside = false};
    effects.outerGlow = OuterGlowEffect{.enabled = true, .size = 10, .red = 1, .green = 0, .blue = 0, .opacity = 1.0};
    effects.shadow = ShadowEffect{.enabled = true, .angle = 180, .distance = 25, .blur = 4, .red = 0, .green = 0, .blue = 1, .opacity = 1.0};
    const auto [rendered, inset] = LayerEffectsRenderer::render(image, QImage(), effects);
    const int in = int(inset);
    const QColor center = rendered.pixelColor(in + 25, in + 25);
    QVERIFY(center.redF() > 0.9 && center.greenF() > 0.9 && center.blueF() > 0.9);
    const QColor stroke = rendered.pixelColor(in + 13, in + 25);
    QVERIFY(stroke.alphaF() > 0.9 && stroke.redF() < 0.2 && stroke.greenF() < 0.2 && stroke.blueF() < 0.2);
    const QColor glow = rendered.pixelColor(in + 10, in + 25);
    QVERIFY(glow.alphaF() > 0.05 && glow.redF() > 0.6);
    const QColor shadow = rendered.pixelColor(in + 50, in + 25);
    QVERIFY2(shadow.alphaF() > 0.1 && shadow.blueF() > 0.6, qPrintable(QStringLiteral("shadow %1 %2").arg(shadow.alphaF()).arg(shadow.blueF())));
}

void TestLayers::effectsCopyBetweenLayersAndSurviveSave()
{
    EditorSession s;
    s.createDocument(60, 60);
    s.insertImage(squareWithMargin(40, 16, Qt::white), QStringLiteral("A"), QPointF(30, 30));
    const QUuid a = active(s);
    s.insertImage(squareWithMargin(40, 16, Qt::white), QStringLiteral("B"), QPointF(30, 30));
    const QUuid b = active(s);
    QVERIFY(s.addLayerEffect(a, LayerEffectKind::OuterGlow));
    QVERIFY(s.addLayerEffect(a, LayerEffectKind::InnerGlow));
    QVERIFY(s.canCopyLayerEffect(LayerEffectKind::OuterGlow, a, b));
    QVERIFY(s.copyLayerEffect(LayerEffectKind::OuterGlow, a, b));
    QVERIFY(s.layerEffects(b)->contains(LayerEffectKind::OuterGlow));
    QVERIFY(!s.layerEffects(b)->contains(LayerEffectKind::InnerGlow));
    QVERIFY(s.copyAllLayerEffects(a, b));
    QVERIFY(s.layerEffects(b)->contains(LayerEffectKind::InnerGlow));
    // The copied effect actually draws: the canvas has glow pixels around B's square.
    findMutable(s, a)->visible = false;
    const QImage image = render(s);
    int glowPixels = 0;
    for (int y = 0; y < 60; ++y) for (int x = 0; x < 60; ++x) if (qAlpha(image.pixel(x, y)) > 0 && (x < 38 || x > 54)) ++glowPixels;
    QVERIFY(glowPixels > 50);
    QTemporaryDir dir;
    ProjectWriter::save(*s.document(), dir.filePath(QStringLiteral("fx.comp")));
    const Document loaded = ProjectReader::load(dir.filePath(QStringLiteral("fx.comp")));
    QCOMPARE(LayerRenderer::flattened(loaded), image);
}

// ---- clipboard / cross-project copy ----------------------------------------------------------------------

void TestLayers::wholeLayerCopyAndPasteInsideOneProject()
{
    MainWindow window;
    window.session().createDocument(30, 30);
    window.session().addGroup();
    const QUuid folder = active(window.session());
    window.session().insertImage(solid(10, 10, Qt::red), QStringLiteral("Red"), QPointF(5, 5));
    window.session().selectLayer(folder);
    window.syncDocumentViews();
    // No selection: Copy takes the folder itself, whole, Paste puts a copy of it and its contents above it.
    QAction *copy = window.findChild<QAction *>(QStringLiteral("commandCopy"));
    QAction *paste = window.findChild<QAction *>(QStringLiteral("commandPaste"));
    QVERIFY(copy && paste);
    copy->trigger();
    const int before = window.session().document()->layers.size();
    paste->trigger();
    QCOMPARE(window.session().document()->layers.size(), before + 2);
    QCOMPARE(window.session().history().undoName(), QStringLiteral("Paste"));
    const Layer *pasted = window.session().activeLayer();
    QVERIFY(pasted && pasted->group && pasted->id != folder);
    QCOMPARE(window.session().descendantIds(pasted->id).size(), 1);
    window.session().undo();
    QCOMPARE(window.session().document()->layers.size(), before);

    // A plain pixel layer copied the same way pastes a layer (not just pixels), with its mask and effects.
    window.session().selectLayer(window.session().document()->layers.last().id);
    Layer *layer = findMutable(window.session(), active(window.session()));
    layer->mask = grayMask(1, 1, {128});
    window.syncDocumentViews();
    copy->trigger();
    paste->trigger();
    const Layer *copyOfLayer = window.session().activeLayer();
    QVERIFY(copyOfLayer && copyOfLayer->id != layer->id);
    QVERIFY2(!copyOfLayer->mask.isNull(), "a whole-layer paste carries the mask");
    QCOMPARE(copyOfLayer->name, layer->name + QStringLiteral(" copy"));
}

void TestLayers::crossProjectCopyRemapsIdentityAndHasIndependentUndo()
{
    MainWindow window;
    window.session().createDocument(100, 100);
    window.session().insertImage(solid(20, 20, Qt::white), QStringLiteral("White"), QPointF(50, 50));
    const QUuid original = active(window.session());
    const QImage originalPixels = window.session().activeLayer()->image;
    window.syncDocumentViews();
    // Copy it in this project, switch to a new 200 x 200 project and paste: a copy with a new identity, centred.
    QAction *copy = window.findChild<QAction *>(QStringLiteral("commandCopy"));
    QAction *paste = window.findChild<QAction *>(QStringLiteral("commandPaste"));
    copy->trigger();
    QAction *newProject = nullptr;
    for (QAction *candidate : window.findChildren<QAction *>()) if (candidate->text().contains(QStringLiteral("New Canvas"))) newProject = candidate;
    QVERIFY(newProject);
    newProject->trigger();
    window.session().createDocument(200, 200);
    window.syncDocumentViews();
    paste->trigger();
    QCOMPARE(window.session().document()->layers.size(), 1);
    const Layer &copied = window.session().document()->layers.first();
    QVERIFY(copied.id != original);
    QCOMPARE(copied.transform.center(), QPointF(100, 100));
    QCOMPARE(copied.image, originalPixels);
    window.session().undo();
    QVERIFY(window.session().document()->layers.isEmpty());
    window.session().redo();
    QCOMPARE(window.session().document()->layers.size(), 1);
}

void TestLayers::crossProjectCopyBakesLiveMasksAndAnchorsSeveralLayers()
{
    MainWindow window;
    window.session().createDocument(100, 100);
    // A hidden 20x20 source supplies a live mask to a 40x40 red layer; copying only the red layer bakes the clip.
    window.session().insertImage(solid(20, 20, Qt::black), QStringLiteral("Source"), QPointF(50, 50));
    const QUuid source = active(window.session());
    window.session().insertImage(solid(40, 40, Qt::red), QStringLiteral("Red"), QPointF(50, 50));
    const QUuid red = active(window.session());
    QVERIFY(window.session().linkMask(source, red));
    findMutable(window.session(), source)->visible = false;
    const QImage before = render(window.session());
    QCOMPARE(qAlpha(before.pixel(50, 50)), 255);
    QCOMPARE(qAlpha(before.pixel(35, 50)), 0);
    window.syncDocumentViews();
    QAction *copy = window.findChild<QAction *>(QStringLiteral("commandCopy"));
    QAction *paste = window.findChild<QAction *>(QStringLiteral("commandPaste"));
    window.session().selectLayer(red);
    window.syncDocumentViews();
    copy->trigger();
    QAction *newProject = nullptr;
    for (QAction *candidate : window.findChildren<QAction *>()) if (candidate->text().contains(QStringLiteral("New Canvas"))) newProject = candidate;
    QVERIFY(newProject);
    newProject->trigger();
    window.session().createDocument(100, 100);
    window.syncDocumentViews();
    paste->trigger();
    QCOMPARE(window.session().document()->layers.size(), 1);
    QVERIFY2(!window.session().document()->layers.first().maskSourceId.has_value(), "the link to a layer that stayed behind is baked away");
    const QImage after = render(window.session());
    QCOMPARE(qAlpha(after.pixel(50, 50)), 255);
    QCOMPARE(qAlpha(after.pixel(35, 50)), 0);     // the clip survived as pixels
    QCOMPARE(qAlpha(after.pixel(65, 65)), 0);
}

void TestLayers::inspectorMovesAnUnlinkedMaskOrLayerSeparately()
{
    MainWindow window;
    EditorSession &s = window.session();
    s.createDocument(400, 200);
    s.insertImage(solid(400, 200, Qt::red), QStringLiteral("Layer"));
    const QUuid id = active(s);
    QImage mask(400, 200, QImage::Format_Grayscale8);
    mask.fill(255);
    for (int y = 50; y < 150; ++y) for (int x = 100; x < 200; ++x) mask.scanLine(y)[x] = 0;
    findMutable(s, id)->mask = mask;
    findMutable(s, id)->transform.sampling = Sampling::Nearest;
    window.syncDocumentViews();
    QDoubleSpinBox *x = window.transformXField();
    QVERIFY(x);
    // Linked: the mask moves with its layer whichever thumbnail is selected, so the hole moves too.
    QVERIFY(s.toggleMaskLink());                       // unlink
    QVERIFY(!find(s, id)->maskLinked);
    s.selectMaskTarget(true);
    window.syncDocumentViews();
    x->setValue(100);
    QMetaObject::invokeMethod(x, "editingFinished");
    QCOMPARE(find(s, id)->transform.origin.x(), 0.0);                // the layer stays put
    QVERIFY(find(s, id)->maskPlacement.has_value());
    QCOMPARE(find(s, id)->maskPlacement->origin.x(), 100.0);         // the mask moved alone
    QCOMPARE(find(s, id)->mask, mask);                               // moving never resamples it
    QImage image = render(s);
    QCOMPARE(qAlpha(image.pixel(150, 100)), 255);                    // old hole now revealed
    QCOMPARE(qAlpha(image.pixel(250, 100)), 0);                      // hole moved to x 200..300
    QCOMPARE(qAlpha(image.pixel(50, 100)), 255);
    s.undo();
    QVERIFY(!find(s, id)->maskPlacement.has_value());
    // Unlinked, with the layer selected: the layer moves and the mask stays on the canvas.
    s.selectMaskTarget(false);
    window.syncDocumentViews();
    x->setValue(50);
    QMetaObject::invokeMethod(x, "editingFinished");
    QCOMPARE(find(s, id)->transform.origin.x(), 50.0);
    QVERIFY(find(s, id)->maskPlacement.has_value());
    QCOMPARE(find(s, id)->maskPlacement->origin.x(), 0.0);
    image = render(s);
    QCOMPARE(qAlpha(image.pixel(150, 100)), 0);                      // hole still at x 100..200
    QCOMPARE(qAlpha(image.pixel(25, 100)), 0);                       // outside the moved layer
    QCOMPARE(qAlpha(image.pixel(250, 100)), 255);
}

void TestLayers::opacityAndBlendControlsFollowMacEnablingRules()
{
    MainWindow window;
    EditorSession &s = window.session();
    s.createDocument(40, 40);
    s.addBlankLayer(); const QUuid a = active(s);
    s.addBlankLayer(); const QUuid b = active(s);
    s.addGroup(); const QUuid folder = active(s);
    auto *slider = window.findChild<QSlider *>(QStringLiteral("layerOpacity"));
    auto *blend = window.findChild<QComboBox *>(QStringLiteral("blendMode"));
    window.syncDocumentViews();
    QVERIFY(slider->isEnabled());                  // a folder takes an opacity of its own
    QVERIFY(!blend->isEnabled());                  // folders are pass-through: no blend mode
    s.selectLayer(a);
    window.syncDocumentViews();
    QVERIFY(slider->isEnabled() && blend->isEnabled());
    s.selectLayers({a, b}, a);
    window.syncDocumentViews();
    QVERIFY(!slider->isEnabled() && !blend->isEnabled());
    Q_UNUSED(folder);
}

void TestLayers::panelRowsShowChainDisabledMaskAndTextBadge()
{
    MainWindow window;
    EditorSession &s = window.session();
    s.createDocument(40, 40);
    s.addBlankLayer();
    s.addLayerMask(true, false);
    window.syncDocumentViews();
    LayerListModel *model = window.layerModel();
    QModelIndex row = model->index(0, 0);
    QVERIFY(model->data(row, Qt::UserRole + 8).toBool());                // chain slot shown
    QVERIFY(model->data(row, Qt::UserRole + 9).toBool());                // linked
    QVERIFY(!model->data(row, Qt::UserRole + 7).toBool());
    s.toggleLayerMask();
    s.toggleMaskLink();
    window.syncDocumentViews();
    row = model->index(0, 0);
    QVERIFY(model->data(row, Qt::UserRole + 7).toBool());                // disabled mark
    QVERIFY(!model->data(row, Qt::UserRole + 9).toBool());               // unlinked: empty chain
    QVERIFY(!model->data(row, Qt::UserRole + 20).toBool());
}

void TestLayers::maskAloneBadgeTakesItsClicks()
{
    MainWindow window;
    EditorSession &s = window.session();
    s.createDocument(200, 100);
    s.addBlankLayer();
    s.addLayerMask(true, false);
    s.toggleMaskAlone(active(s));
    window.syncDocumentViews();
    CanvasWidget *canvas = window.canvas();
    canvas->resize(600, 400);
    QSignalSpy spy(canvas, &CanvasWidget::maskAloneExitRequested);
    const QRectF badge((canvas->width() - 220) / 2.0, canvas->height() - 26 - 14.0, 220, 26);   // CanvasWidget::maskAloneBadgeRect
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, badge.center().toPoint());
    QCOMPARE(spy.count(), 1);
}

void TestLayers::folderMaskShowsWhileItIsBeingPainted()
{
    EditorSession s;
    s.createDocument(200, 100);
    s.insertImage(solid(200, 100, Qt::red), QStringLiteral("Red"));
    s.groupSelectedLayers();
    s.addLayerMask(true, false);
    QVERIFY(s.beginBrushStroke(QPointF(100, 50), Qt::black, 30, 1.0, 1.0, false));
    s.continueBrushStroke(QPointF(101, 50));
    // Before the stroke ends, the composite already shows the hole and still shows the layer elsewhere.
    QVERIFY(s.isPainting());
    const QImage mid = render(s);
    QCOMPARE(qAlpha(mid.pixel(100, 50)), 0);
    QCOMPARE(qAlpha(mid.pixel(140, 50)), 255);
    s.cancelBrushStroke();
    QCOMPARE(qAlpha(render(s).pixel(100, 50)), 255);
}

void TestLayers::largeGlowsStayCloseToAGaussian()
{
    // A 100 px square with a 60 px outer glow: sigma 30. Across the edge the glow follows a Gaussian CDF.
    const QImage image = squareWithMargin(160, 100, Qt::white);
    LayerEffects effects;
    effects.outerGlow = OuterGlowEffect{.enabled = true, .size = 60, .red = 1, .green = 1, .blue = 1, .opacity = 1.0};
    const auto [rendered, inset] = LayerEffectsRenderer::render(image, QImage(), effects);
    const int in = int(inset);
    const int y = in + 80;
    for (int distance : {5, 20, 40, 70}) {
        const int x = in + 30 - distance;                              // distance px left of the square's edge
        const double expected = 0.5 * std::erfc((distance - 0.5) / (30.0 * std::sqrt(2.0))) * std::erf(50.0 / (30.0 * std::sqrt(2.0)));   // times the share of the kernel inside the square's height
        const double got = rendered.pixelColor(x, y).alphaF();
        QVERIFY2(std::abs(got - expected) < 0.012, qPrintable(QStringLiteral("distance %1: %2 vs %3").arg(distance).arg(got).arg(expected)));
    }
}

QTEST_MAIN(TestLayers)
#include "TestLayers.moc"
