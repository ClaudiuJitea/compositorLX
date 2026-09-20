#pragma once

#include "core/DocumentHistory.h"
#include "rendering/RasterOperations.h"
#include "rendering/SubjectRemoval.h"

#include <QSet>

#include <array>

namespace compositor {

struct HueSaturationSettings;
struct CurvesSettings;

enum class SelectionMode { Replace, Add, Subtract };

/// Platform-neutral editing state. UI code issues commands through this class so every
/// mutation has the same validation and undo semantics as the macOS implementation.
class EditorSession final {
public:
    EditorSession() = default;

    [[nodiscard]] const std::shared_ptr<Document> &document() const { return document_; }
    [[nodiscard]] std::shared_ptr<Document> &document() { return document_; }
    void setDocument(std::shared_ptr<Document> document, bool markSaved = true);

    [[nodiscard]] Layer *activeLayer();
    [[nodiscard]] const Layer *activeLayer() const;
    [[nodiscard]] bool canEditLayers() const { return bool(document_); }
    [[nodiscard]] bool canUndo() const { return history_.canUndo(); }
    [[nodiscard]] bool canRedo() const { return history_.canRedo(); }
    [[nodiscard]] bool isModified() const { return history_.isModified(); }
    [[nodiscard]] const DocumentHistory &history() const { return history_; }

    void createDocument(int width, int height, bool emptyLayer = false);
    bool insertImage(const QImage &image, const QString &name, const std::optional<QPointF> &center = std::nullopt);
    bool insertPixelLayer(const QImage &image, const QPointF &origin, const QString &name,
                          const QString &historyName = QStringLiteral("Paste"), bool dropsSelection = true);
    [[nodiscard]] std::optional<QPair<QImage, QPoint>> copiedPixels(bool merged = false) const;
    void selectAll();
    void deselect();
    void invertSelection();
    bool expandSelection(int amount);
    bool contractSelection(int amount);
    bool moveSelection(const QPoint &offset);
    bool nudgeSelectedPixels(const QPoint &offset);
    bool setRectangularSelection(const QRect &rect, SelectionMode mode = SelectionMode::Replace);
    bool setEllipticalSelection(const QRect &rect, SelectionMode mode = SelectionMode::Replace, bool antialiased = true);
    bool setPolygonSelection(const QPolygonF &points, SelectionMode mode = SelectionMode::Replace, bool antialiased = true);
    bool magicWand(const QPoint &documentPoint, int tolerance = 32, int sampleRadius = 0,
                   bool contiguous = true, bool sampleAllLayers = false,
                   SelectionMode mode = SelectionMode::Replace, bool antialiased = true);
    bool invertActiveLayerPixels();
    bool fillSelection(const QColor &color);
    bool clearSelectedPixels();
    bool beginSelectionTransform(bool duplicate = false);
    bool commitSelectionTransform();
    bool cancelSelectionTransform();
    [[nodiscard]] bool hasFloatingSelection() const { return floating_.has_value(); }
    bool applyGradient(const QPointF &start, const QPointF &end, const QColor &foreground,
                       const QColor &background, bool radial = false, bool foregroundTransparent = false,
                       bool reversed = false, double opacity = 1);
    bool addShape(const QRectF &rect, const QColor &fill, const QColor &stroke,
                  double strokeWidth = 2, bool ellipse = false, double cornerRadius = 0);
    bool addText(const QString &text, const QRectF &box, const QString &fontFamily,
                 int pixelSize, bool bold, bool italic, bool underline, int alignment, const QColor &color, bool areaText = true);
    bool updateText(const QUuid &id, const QString &text, const QRectF &box, const QString &fontFamily,
                    int pixelSize, bool bold, bool italic, bool underline, int alignment, const QColor &color, bool areaText);
    void redrawSelectedShapes();
    bool addNoiseToActiveLayer(float amount, bool gaussian, bool monochromatic, quint32 seed);
    bool distortActiveLayer(double amount);
    bool applyLevels(const LevelsSettings &settings);
    [[nodiscard]] LevelsHistogram levelsHistogram() const;
    [[nodiscard]] std::optional<QColor> levelsSampleAt(const QPointF &documentPoint) const;
    bool applyExposure(double stops, double offset, double gamma);
    bool applyHueSaturation(const HueSaturationSettings &settings);
    bool applyCurves(const CurvesSettings &settings);
    bool applyGradientMap(const QColor &shadows, const QColor &highlights, bool reversed = false);
    bool applyGrain(double amount, double size, double roughness, quint32 seed);
    bool applyGaussianBlur(double radius);
    bool applyMotionBlur(double angleDegrees, double distance);
    bool contentAwareFill();
    bool removeBackground(const SubjectRemovalSettings &settings = {}, const QImage &preparedMask = {}, QString *error = nullptr);
    bool beginBrushStroke(const QPointF &documentPoint, const QColor &color, double diameter = 40,
                          double hardness = 1, double opacity = 1, bool erasing = false,
                          const std::optional<QPointF> &cloneSource = std::nullopt,
                          int healMode = -1, quint32 effectSeed = 0);
    void setCloneSource(const QPointF &documentPoint);
    [[nodiscard]] std::optional<QPointF> cloneSource() const { return cloneSource_; }
    [[nodiscard]] std::optional<QPointF> cloneOffset() const { return cloneOffset_; }
    bool beginCloneStroke(const QPointF &documentPoint, double diameter = 40, double hardness = 1, double opacity = 1,
                          bool aligned = true, bool sampleAllLayers = false);
    bool beginHealingStroke(const QPointF &documentPoint, double diameter = 40, double hardness = 1,
                            double opacity = 1, int mode = 0, quint32 seed = 0);
    bool beginBlurStroke(const QPointF &documentPoint, double diameter = 40, double hardness = 1, double opacity = 1);
    bool beginWarpStroke(const QPointF &documentPoint, int mode, double diameter = 40, double hardness = 1, double strength = 1);
    void continueBrushStroke(const QPointF &documentPoint);
    bool endBrushStroke();
    bool cancelBrushStroke();
    [[nodiscard]] bool isPainting() const { return brush_.has_value() || warp_.has_value(); }
    bool addLayerMask(bool revealing = true, bool useSelection = true);
    bool toggleLayerMask();
    bool toggleMaskLink();
    bool deleteLayerMask();
    bool invertLayerMask();
    bool loadMaskAsSelection(SelectionMode mode = SelectionMode::Replace, const std::optional<QUuid> &id = std::nullopt);
    bool loadLayerAsSelection(SelectionMode mode = SelectionMode::Replace, const std::optional<QUuid> &id = std::nullopt);
    [[nodiscard]] bool canLinkMask(const QUuid &source, const QUuid &target) const;
    bool linkMask(const QUuid &source, const QUuid &target);
    bool toggleClippingMask(const QUuid &target);
    void selectMaskTarget(bool mask);
    [[nodiscard]] bool isMaskSelected() const { return maskSelected_; }
    bool resizeCanvas(const QSize &size, int anchor = 4, const std::optional<QColor> &extension = std::nullopt);
    bool resizeImage(const QSize &size, double resolution, Sampling sampling = Sampling::HighQuality);
    bool crop(const QRect &documentRect);
    bool flipLayers(bool horizontally);
    bool flipCanvas(bool horizontally);
    [[nodiscard]] bool canMergeLayers() const;
    [[nodiscard]] QString mergeTitle() const;
    bool mergeLayers();
    bool flattenImage();
    void selectLayer(const std::optional<QUuid> &id);
    void selectLayers(const QSet<QUuid> &ids, const std::optional<QUuid> &primary = std::nullopt);
    [[nodiscard]] const QSet<QUuid> &selectedLayerIds() const { return selectedLayerIds_; }
    [[nodiscard]] QRectF selectedLayersBounds() const;
    bool transformSelectedLayers(const QRectF &bounds, double rotationDegrees = 0);
    bool distortSelectedLayers(const std::array<QPointF, 4> &corners, bool maskOnly = false);
    void addBlankLayer();
    void addGroup();
    bool addAdjustment(const QString &kind, const QJsonObject &settings = {});
    bool updateAdjustment(const QUuid &id, const QJsonObject &settings, const QString &historyName = QStringLiteral("Edit Adjustment"));
    bool previewAdjustment(const QUuid &id, const QJsonObject &settings);
    void groupSelectedLayers();
    void deleteActiveLayer();
    void deleteSelectedLayers();
    void deleteSelectedLayers(bool bakeLiveMasks);
    [[nodiscard]] QSet<QUuid> selectedDeletionLiveMaskDependents() const;
    void renameLayer(const QUuid &id, const QString &name);
    void toggleLayerVisibility(const QUuid &id);
    void setLayerOpacity(const QUuid &id, double opacity);
    void setSelectedLayersOpacity(double opacity);
    void setLayerBlendMode(const QUuid &id, BlendMode mode);
    [[nodiscard]] bool canMoveActiveLayer(int offset) const;
    void moveActiveLayer(int offset);
    bool reorderLayers(const QVector<int> &topFirstRows, int destination);
    void duplicateActiveLayer();
    [[nodiscard]] QSet<QUuid> descendantIds(const QUuid &id) const;
    [[nodiscard]] bool canPlaceLayer(const QUuid &id, const std::optional<QUuid> &parent) const;
    bool placeLayer(const QUuid &id, const std::optional<QUuid> &parent,
                    const std::optional<QUuid> &above = std::nullopt, bool atBottom = false);
    bool duplicateLayer(const QUuid &id, const std::optional<QUuid> &parent,
                        const std::optional<QUuid> &above = std::nullopt, bool atBottom = false);
    bool moveActiveLayerOutOfGroup();
    void undo();
    void redo();
    void markSaved() { history_.markSaved(); }
    void beginEdit(const QString &name) { history_.begin(name, document_); }
    void endEdit() { history_.end(document_); }

private:
    [[nodiscard]] int indexOf(const QUuid &id) const;
    [[nodiscard]] QString nextName(const QString &base) const;
    [[nodiscard]] QSet<QUuid> selectedTransformLayerIds() const;
    void restore(const DocumentHistory::Snapshot &snapshot);
    void deleteIds(const QSet<QUuid> &ids, const QString &historyName);

    std::shared_ptr<Document> document_;
    QSet<QUuid> selectedLayerIds_;
    DocumentHistory history_;
    struct BrushState {
        QUuid layerId;
        QPointF previousPixel;
        QColor color;
        double diameter;
        double hardness;
        double opacity;
        bool erasing;
        bool mask;
        bool clone;
        int maskValue;
        QPointF cloneOffsetPixel;
        int healMode;
        quint32 effectSeed;
        QImage original;
        QImage cloneSample;
        QImage coverage;
        QImage settledCoverage;
        QImage selectionCoverage;
        QVector<QPointF> samples;
        QRect tailBounds;
        QRect dirtyPixels;
        bool changed = false;
    };
    std::optional<BrushState> brush_;
    struct WarpState {
        QUuid layerId;
        int mode;
        double diameter;
        double hardness;
        double strength;
        QPointF previousPixel;
        QImage original;
        QImage working;
        QImage coverage;
        QVector<float> carried;
        bool changed = false;
    };
    std::optional<WarpState> warp_;
    std::optional<QPointF> cloneSource_;
    std::optional<QPointF> cloneOffset_;
    bool maskSelected_ = false;
    struct FloatingSelectionState {
        QUuid sourceId;
        QUuid layerId;
        Document before;
        std::optional<QUuid> beforeActive;
        LayerTransform original;
        QSize pixelSize;
        bool duplicate = false;
    };
    std::optional<FloatingSelectionState> floating_;
};

} // namespace compositor
