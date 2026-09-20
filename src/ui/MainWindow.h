#pragma once

#include "core/EditorSession.h"

#include <QMainWindow>
#include <QPointer>
#include <QVector>

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

namespace compositor {

class CanvasWidget;
class Document;
class LayerListModel;
class SegmentedControl;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget *parent = nullptr);

    bool openProject(const QString &path);

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
    bool importImageFiles(const QStringList &paths, const std::optional<QPointF> &center = std::nullopt);
    bool saveProject();
    bool saveProjectAs();
    bool confirmReplacement();
    void autosave();
    void offerRecovery();
    [[nodiscard]] QString recoveryPath() const;
    [[nodiscard]] QString newRecoveryPath() const;
    [[nodiscard]] QString recoveryDirectory() const;
    void removeRecovery();
    void exportPng(bool jpegDefault = false);
    void resizeImageDialog();
    void resizeCanvasDialog();
    void cropDialog();
    void levelsDialog();
    void exposureDialog();
    void hueSaturationDialog();
    void curvesDialog();
    void gradientMapDialog();
    void grainDialog();
    void previewGradient(const QPointF &start, const QPointF &end, bool radial,
                         bool foregroundTransparent, bool reversed, double opacity);
    void finishGradient(bool commit);
    void gaussianBlurDialog();
    void motionBlurDialog();
    void removeBackgroundDialog();
    void openColorPicker(bool background);
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
    void syncDocumentViews(bool compositeChanged = true);
    void stashCurrentTab();
    void activateTab(int index);
    void closeTab(int index);
    void installTabCloseButton(int index);
    void installInNewTab(EditorSession session, const QString &title);
    bool copyLayersToTab(const QVector<QUuid> &ids, int targetIndex, bool newTab);

    CanvasWidget *canvas_ = nullptr;
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
    QCheckBox *showTransformControls_ = nullptr;
    QSlider *opacitySlider_ = nullptr;
    QDoubleSpinBox *xField_ = nullptr;
    QDoubleSpinBox *yField_ = nullptr;
    QDoubleSpinBox *widthField_ = nullptr;
    QDoubleSpinBox *heightField_ = nullptr;
    QDoubleSpinBox *scaleField_ = nullptr;
    QDoubleSpinBox *rotationField_ = nullptr;
    QDoubleSpinBox *brushSizeField_ = nullptr;
    QDoubleSpinBox *brushHardnessField_ = nullptr;
    QDoubleSpinBox *brushOpacityField_ = nullptr;
    QDoubleSpinBox *gradientOpacityField_ = nullptr;
    QPushButton *transformApply_ = nullptr;
    QPushButton *transformCancel_ = nullptr;
    std::shared_ptr<Document> document_;
    EditorSession session_;
    double brushDiameter_ = 40;
    double brushHardness_ = 1;
    double brushOpacity_ = 1;
    int pendingOpacityDigit_ = -1;
    qint64 pendingOpacityAt_ = 0;
    QColor foregroundColor_ = QColor(22, 134, 232);
    QColor backgroundColor_ = QColor(31, 15, 11);
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
};

} // namespace compositor
