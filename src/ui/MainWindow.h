#pragma once

#include "core/EditorSession.h"
#include "io/ProjectDigest.h"
#include "io/ProjectWatcher.h"
#include "io/ProjectWriter.h"

#include <QMainWindow>
#include <QPointer>
#include <QVector>
#include <QMap>
#include <QFuture>
#include <QMessageBox>

#include <memory>
#include <functional>

class QListView;
class QLabel;
class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QSlider;
class QToolButton;
class QTimer;
class QCloseEvent;
class QTabBar;
class QStackedWidget;
class QSpinBox;
class QColorDialog;
class QPushButton;
class QMenu;

namespace compositor {

class CanvasWidget;
class CanvasRulerWidget;
class CanvasRulerCornerWidget;
class Document;
class LayerListModel;
class ScrubLabel;
class SegmentedControl;
class InlineTextEditor;

void restoreLayer(Document *doc, EditorSession &session, const QUuid &target, const Layer &original);

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    bool openProject(const QString &path);
    bool importImageFiles(const QStringList &paths, const std::optional<QPointF> &center = std::nullopt);

    [[nodiscard]] EditorSession &session() { return session_; }
    [[nodiscard]] const EditorSession &session() const { return session_; }
    [[nodiscard]] std::shared_ptr<Document> document() const { return document_; }
    [[nodiscard]] LayerListModel *layerModel() const { return layerModel_; }
    [[nodiscard]] QListView *layerView() const { return layerView_; }
    [[nodiscard]] CanvasWidget *canvas() const { return canvas_; }
    [[nodiscard]] InlineTextEditor *inlineTextEditor() const;
    [[nodiscard]] CanvasRulerWidget *horizontalRuler() const { return horizontalRuler_; }
    [[nodiscard]] CanvasRulerWidget *verticalRuler() const { return verticalRuler_; }
    [[nodiscard]] CanvasRulerCornerWidget *rulerCorner() const { return rulerCorner_; }
    [[nodiscard]] std::optional<LayerEffectKind> selectedEffectKind() const { return selectedEffect_ ? std::optional<LayerEffectKind>(selectedEffect_->kind) : std::nullopt; }
    [[nodiscard]] QColor foregroundColor() const { return session_.foregroundColor(); }
    [[nodiscard]] QColor backgroundColor() const { return session_.backgroundColor(); }
    [[nodiscard]] QToolButton *menuRestoreButton() const { return menuRestoreButton_; }
    [[nodiscard]] QMenu *quickFileMenu() const { return quickFileMenu_; }
    [[nodiscard]] bool isMenuBarVisible() const;
    void updateMenuRestoreButton();
    void syncDocumentViews(bool compositeChanged = true);
    void updateRulerVisibility();

    [[nodiscard]] ScrubLabel *brushSizeLabel() const { return brushSizeLabel_; }
    [[nodiscard]] ScrubLabel *brushHardnessLabel() const { return brushHardnessLabel_; }
    [[nodiscard]] ScrubLabel *brushOpacityLabel() const { return brushOpacityLabel_; }
    [[nodiscard]] ScrubLabel *brushSmoothingLabel() const { return brushSmoothingLabel_; }
    [[nodiscard]] ScrubLabel *xLabel() const { return xLabel_; }
    [[nodiscard]] ScrubLabel *yLabel() const { return yLabel_; }
    [[nodiscard]] ScrubLabel *widthLabel() const { return widthLabel_; }
    [[nodiscard]] ScrubLabel *heightLabel() const { return heightLabel_; }
    [[nodiscard]] ScrubLabel *rotationLabel() const { return rotationLabel_; }
    [[nodiscard]] ScrubLabel *shapeRadiusLabel() const { return shapeRadiusLabel_; }
    [[nodiscard]] ScrubLabel *shapeLineWidthLabel() const { return shapeLineWidthLabel_; }
    [[nodiscard]] ScrubLabel *wandToleranceLabel() const { return wandToleranceLabel_; }
    [[nodiscard]] ScrubLabel *objectEdgeOffsetLabel() const { return objectEdgeOffsetLabel_; }
    [[nodiscard]] ScrubLabel *selectionAmountLabel() const { return selectionAmountLabel_; }
    [[nodiscard]] ScrubLabel *gradientOpacityLabel() const { return gradientOpacityLabel_; }
    [[nodiscard]] ScrubLabel *textSizeLabel() const { return textSizeLabel_; }
    [[nodiscard]] ScrubLabel *textTrackingLabel() const { return textTrackingLabel_; }
    [[nodiscard]] ScrubLabel *textLeadingLabel() const { return textLeadingLabel_; }
    [[nodiscard]] ScrubLabel *layerOpacityLabel() const { return layerOpacityLabel_; }

    [[nodiscard]] bool isMaskDragPending() const { return pendingMaskDrag_; }
    [[nodiscard]] QPoint maskDragStartPos() const { return maskDragStartPos_; }
    [[nodiscard]] QUuid maskDragLayerId() const { return maskDragLayerId_; }

    [[nodiscard]] QDoubleSpinBox *brushSizeField() const { return brushSizeField_; }
    [[nodiscard]] QDoubleSpinBox *brushHardnessField() const { return brushHardnessField_; }
    [[nodiscard]] QDoubleSpinBox *brushOpacityField() const { return brushOpacityField_; }
    [[nodiscard]] QDoubleSpinBox *blurRadiusField() const { return blurRadiusField_; }
    [[nodiscard]] QDoubleSpinBox *brushSmoothingField() const { return brushSmoothingField_; }
    [[nodiscard]] QDoubleSpinBox *xField() const { return xField_; }
    [[nodiscard]] QDoubleSpinBox *yField() const { return yField_; }
    [[nodiscard]] QDoubleSpinBox *widthField() const { return widthField_; }
    [[nodiscard]] QDoubleSpinBox *heightField() const { return heightField_; }
    [[nodiscard]] QDoubleSpinBox *rotationField() const { return rotationField_; }
    [[nodiscard]] QDoubleSpinBox *shapeRadiusField() const { return shapeRadiusField_; }
    [[nodiscard]] QDoubleSpinBox *shapeLineWidthField() const { return shapeLineWidthField_; }
    [[nodiscard]] QSpinBox *wandToleranceField() const { return wandToleranceField_; }
    [[nodiscard]] QSpinBox *objectEdgeOffsetField() const { return objectEdgeOffsetField_; }
    [[nodiscard]] QSpinBox *selectionAmountField() const { return selectionAmountField_; }
    [[nodiscard]] QDoubleSpinBox *gradientOpacityField() const { return gradientOpacityField_; }
    [[nodiscard]] QSpinBox *textSizeField() const { return textSizeField_; }
    [[nodiscard]] QDoubleSpinBox *textTrackingField() const { return textTrackingField_; }
    [[nodiscard]] QDoubleSpinBox *textLeadingField() const { return textLeadingField_; }
    [[nodiscard]] QSlider *opacitySlider() const { return opacitySlider_; }
    [[nodiscard]] SegmentedControl *smearMode() const { return smearMode_; }
    [[nodiscard]] QLabel *statusHint() const { return statusHint_; }
    void openColorPicker(bool background = false);
    // Swatches, the canvas's copy of the foreground and a pending gradient follow the session's palette (on a mask the
    // swatches show black and white, the brush's Paint choice).
    void refreshPaletteSwatches();
    void cycleSmearMode();
    void cycleToolMode();
    void updateSmearStatusHint();
    void updateBlurRadiusVisibility();

    QFuture<ProjectWriter::SaveResult> saveProjectAsync(int tabIndex = -1, bool asNew = false, const QString &explicitDestination = QString(), bool isAutosave = false);
    QFuture<ProjectWriter::SaveResult> saveRecoveryAsync(int tabIndex = -1);
    bool saveProject(bool wait = false, const QString &explicitDestination = QString());
    bool saveProjectAs(const QString &explicitDestination, bool wait = false);
    bool saveProjectAs(bool wait = false);
    void finishWriting(const QString &destination = QString());
    [[nodiscard]] bool hasInFlightSave(int tabIndex = -1) const;

    static void setMessageDialogHook(std::function<std::optional<QMessageBox::StandardButton>(const QString &title, const QString &text)> hook);

    void handleExternalChangeValidated(int tabIndex, const QUuid &capturedDocId, uint64_t requestId, const QString &path, const std::optional<ProjectDigest> &expectedDigest, const ProjectDigest &newDigest, const std::shared_ptr<Document> &loadedDoc);
    void handleExternalChange(int tabIndex, const QString &path, const ProjectDigest &newDigest);
    void handleExternalRemoval(int tabIndex, const QString &path);
    void promptExternalChange(int tabIndex, const QString &path, const ProjectDigest &newDigest);
    void activateTab(int index);
    void closeTab(int index);
    void setupWatcherForTab(int tabIndex, const QString &path);
    [[nodiscard]] ProjectWatcher *tabWatcher(int tabIndex) const;
    void noteRecentProject(const QString &path);
    [[nodiscard]] QStringList recentProjects() const;
    [[nodiscard]] QString recoveryPath(int tabIndex = -1) const;
    [[nodiscard]] QString recoveryDirectory() const;
    void removeRecovery(int tabIndex = -1);
    void autosave();
    void offerRecovery();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void finishInlineText();
    void createActions();
    void chooseProject();
    void newProject();
    void importImages();
    bool confirmReplacement();
    [[nodiscard]] QString newRecoveryPath() const;
    void exportPng(bool jpegDefault = false);
    void resizeImageDialog();
    void resizeCanvasDialog();
    void cropDialog();
    void trimDialog();
    void levelsDialog();
    void exposureDialog();
    void hueSaturationDialog();
    void colorRangeDialog();
    void gridSettingsDialog();
    void curvesDialog();
    void gradientMapDialog();
    void grainDialog();
    void blackWhiteDialog();
    void colorBalanceDialog();
    void addNoiseDialog();
    void previewGradient(const QPointF &start, const QPointF &end, bool radial,
                         bool foregroundTransparent, bool reversed, double opacity);
    void finishGradient(bool commit);
    void gaussianBlurDialog();
    void motionBlurDialog();
    void cameraRawDialog();
    void vignetteDialog();
    void bloomGlowDialog();
    void ditherDialog();
    void tonalContrastDialog();
    void removeBackgroundDialog();
    void keyboardShortcutsDialog();
    void selectSubjectAction();
    void selectObjectRequested(const QPoint &point, int mode, int edgeOffset, bool smoothEdges, bool sampleAllLayers);
    void layerEffectsDialog(const std::optional<QUuid> &layerId = std::nullopt,
                            std::optional<LayerEffectKind> initialKind = std::nullopt);
    void beginPersistentTransform(const QString &name);
    void finishPersistentTransform(bool apply);
    void updateTransformButtons();
    void copyPixels(bool merged = false);
    void cutPixels();
    void pastePixels();
    void layerViaCopy();
    void deleteLayersWithMaskChoice();
    void showAbout();
    void checkForUpdates();
    void refreshTitle();
    void updateInspector();
    void updateCommandStates();
    void selectLayer(const QModelIndex &index);
    void stashCurrentTab();
    void installTabCloseButton(int index);
    void installInNewTab(EditorSession session, const QString &title);
    bool copyLayersToTab(const QVector<QUuid> &ids, int targetIndex, bool newTab);

    CanvasWidget *canvas_ = nullptr;
    CanvasRulerCornerWidget *rulerCorner_ = nullptr;
    CanvasRulerWidget *horizontalRuler_ = nullptr;
    CanvasRulerWidget *verticalRuler_ = nullptr;
    QAction *actionShowRulers_ = nullptr;
    QAction *actionShowGuides_ = nullptr;
    QAction *actionShowGrid_ = nullptr;
    QAction *actionSnap_ = nullptr;
    QAction *actionSnapToGuides_ = nullptr;
    QAction *actionSnapToGrid_ = nullptr;
    QAction *actionSnapToLayers_ = nullptr;
    QAction *actionSnapToDocumentBounds_ = nullptr;
    QAction *actionLockGuides_ = nullptr;
    QAction *actionClearGuides_ = nullptr;
    QStackedWidget *canvasStack_ = nullptr;
    QSpinBox *newCanvasWidth_ = nullptr;
    QSpinBox *newCanvasHeight_ = nullptr;
    LayerListModel *layerModel_ = nullptr;
    QListView *layerView_ = nullptr;
    QLabel *tabTitle_ = nullptr;
    QTabBar *tabs_ = nullptr;
    QToolButton *layerMenuButton_ = nullptr;
    QDoubleSpinBox *zoomField_ = nullptr;
    QLabel *layerCount_ = nullptr;
    QLabel *statusDimensions_ = nullptr;
    QLabel *statusHint_ = nullptr;
    QLabel *foregroundSwatch_ = nullptr;
    QLabel *backgroundSwatch_ = nullptr;
    QComboBox *blendMode_ = nullptr;
    SegmentedControl *sampling_ = nullptr;
    SegmentedControl *smearMode_ = nullptr;
    QCheckBox *showTransformControls_ = nullptr;
    bool autoSelectFlipped_ = false;
    bool ratioLockFlipped_ = false;
    void syncHeldModifiers();
    void updateTabOverflow();
    QToolButton *tabOverflow_ = nullptr;
    QSlider *opacitySlider_ = nullptr;
    ScrubLabel *xLabel_ = nullptr;
    ScrubLabel *yLabel_ = nullptr;
    ScrubLabel *widthLabel_ = nullptr;
    ScrubLabel *heightLabel_ = nullptr;
    ScrubLabel *rotationLabel_ = nullptr;
    ScrubLabel *brushSizeLabel_ = nullptr;
    ScrubLabel *brushHardnessLabel_ = nullptr;
    ScrubLabel *brushOpacityLabel_ = nullptr;
    ScrubLabel *blurRadiusLabel_ = nullptr;
    ScrubLabel *brushSmoothingLabel_ = nullptr;
    ScrubLabel *shapeRadiusLabel_ = nullptr;
    ScrubLabel *shapeLineWidthLabel_ = nullptr;
    ScrubLabel *wandToleranceLabel_ = nullptr;
    ScrubLabel *objectEdgeOffsetLabel_ = nullptr;
    ScrubLabel *selectionAmountLabel_ = nullptr;
    ScrubLabel *gradientOpacityLabel_ = nullptr;
    ScrubLabel *textSizeLabel_ = nullptr;
    ScrubLabel *textTrackingLabel_ = nullptr;
    ScrubLabel *textLeadingLabel_ = nullptr;
    ScrubLabel *layerOpacityLabel_ = nullptr;
    QDoubleSpinBox *xField_ = nullptr;
    QDoubleSpinBox *yField_ = nullptr;
    QDoubleSpinBox *widthField_ = nullptr;
    QDoubleSpinBox *heightField_ = nullptr;
    QDoubleSpinBox *scaleField_ = nullptr;
    QDoubleSpinBox *rotationField_ = nullptr;
    QDoubleSpinBox *brushSizeField_ = nullptr;
    QDoubleSpinBox *brushHardnessField_ = nullptr;
    QDoubleSpinBox *brushOpacityField_ = nullptr;
    QDoubleSpinBox *blurRadiusField_ = nullptr;
    QDoubleSpinBox *brushSmoothingField_ = nullptr;
    QDoubleSpinBox *shapeRadiusField_ = nullptr;
    QDoubleSpinBox *shapeLineWidthField_ = nullptr;
    QSpinBox *wandToleranceField_ = nullptr;
    QSpinBox *objectEdgeOffsetField_ = nullptr;
    QSpinBox *selectionAmountField_ = nullptr;
    QDoubleSpinBox *gradientOpacityField_ = nullptr;
    QSpinBox *textSizeField_ = nullptr;
    QDoubleSpinBox *textTrackingField_ = nullptr;
    QDoubleSpinBox *textLeadingField_ = nullptr;
    QPushButton *transformApply_ = nullptr;
    QPushButton *transformCancel_ = nullptr;
    std::shared_ptr<Document> document_;
    EditorSession session_;
    double brushDiameter_ = 40;
    double brushHardness_ = 1;
    double brushOpacity_ = 1;
    double blurRadius_ = 5;
    double brushSmoothing_ = 0;
    int pendingOpacityDigit_ = -1;
    qint64 pendingOpacityAt_ = 0;
    QImage clipboardImage_;
    QPoint clipboardOrigin_;
    QTimer *autosaveTimer_ = nullptr;
    QVector<EditorSession> workspaceTabs_;
    QVector<QString> tabRecoveryPaths_;
    int currentTab_ = 0;
    bool suggestClipboardOnEmpty_ = false;
    std::function<void(const QPoint &)> colorSampleOverride_;
    QPointer<QColorDialog> colorPicker_;
    std::optional<QPoint> colorPickerPosition_;
    int colorPickerPreviousTool_ = -1;
    bool colorPickerBackground_ = false;
    std::optional<Document> transformOriginalDocument_;
    bool canvasPendingTransform_ = false;
    std::optional<Layer> gradientOriginal_;
    std::optional<QUuid> gradientTarget_;
    bool gradientMaskTarget_ = false;
    bool visibilitySwipeActive_ = false;
    bool visibilitySwipeValue_ = false;
    QSet<QUuid> visibilitySwipeVisited_;
    bool pendingMaskDrag_ = false;
    QPoint maskDragStartPos_;
    QUuid maskDragLayerId_;

    struct EffectSelection {
        QUuid layerId;
        LayerEffectKind kind;
        bool operator==(const EffectSelection &other) const {
            return layerId == other.layerId && kind == other.kind;
        }
    };
    std::optional<EffectSelection> selectedEffect_;
    quint64 currentSelectionRequestId_ = 0;
    std::shared_ptr<std::atomic<bool>> currentSelectionCancelToken_;
    void cancelActiveSelectionTask();
    QToolButton *menuRestoreButton_ = nullptr;
    QMenu *quickFileMenu_ = nullptr;

    QVector<QPointer<ProjectWatcher>> tabWatchers_;
    QVector<std::optional<ProjectDigest>> tabDigests_;
    QVector<bool> tabPendingExternalChange_;
    QVector<std::optional<ProjectDigest>> tabPendingExternalDigest_;
    QVector<std::shared_ptr<Document>> tabPendingExternalDoc_;
    struct InFlightSave {
        int tabIndex = -1;
        QFuture<ProjectWriter::SaveResult> future;
        QPointer<QFutureWatcher<ProjectWriter::SaveResult>> watcher;
        std::function<void(const ProjectWriter::SaveResult &)> completion;
        bool completed = false;
    };
    QMap<QString, InFlightSave> inFlightSaves_;
    void onSaveCompleted(int capturedTabIndex, const QUuid &capturedDocId, const QString &capturedOriginalPath, const QString &capturedDestination, const QUuid &capturedRevision, bool isSaveAs, const ProjectWriter::SaveResult &result, bool isAutosave = false);
};

} // namespace compositor
