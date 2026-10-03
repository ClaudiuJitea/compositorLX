#pragma once

#include "rendering/Dither.h"
#include "core/CameraRaw.h"
#include "core/DocumentHistory.h"
#include "core/ImageTrim.h"
#include "rendering/RasterOperations.h"
#include "rendering/SubjectRemoval.h"


#include <QSet>

#include <array>
#include <atomic>
#include <functional>

namespace compositor {

struct PSDConversion;
struct PSDImportResult;

struct HueSaturationSettings;
struct CurvesSettings;

enum class SelectionMode { Replace, Add, Subtract };

struct GuideDrag {
    QUuid id;
    CanvasGuide::Axis axis = CanvasGuide::Axis::Horizontal;
    double position = 0.0;
    bool isNew = false;
    std::optional<double> original;
    bool operator==(const GuideDrag &) const = default;
};

/// Platform-neutral editing state. UI code issues commands through this class so every
/// mutation has the same validation and undo semantics as the macOS implementation.
class EditorSession final {
public:
    EditorSession() = default;

    [[nodiscard]] const std::shared_ptr<Document> &document() const { return document_; }
    [[nodiscard]] std::shared_ptr<Document> &document() { return document_; }
    [[nodiscard]] bool hasDocument() const noexcept { return document_ != nullptr; }
    void setDocument(std::shared_ptr<Document> document, bool markSaved = true);

    [[nodiscard]] Layer *activeLayer();
    [[nodiscard]] const Layer *activeLayer() const;
    [[nodiscard]] bool canEditLayers() const { return bool(document_); }
    [[nodiscard]] bool canUndo() const { return history_.canUndo(); }
    [[nodiscard]] bool canRedo() const { return history_.canRedo(); }
    [[nodiscard]] bool isModified() const { return history_.isModified(); }
    [[nodiscard]] const DocumentHistory &history() const { return history_; }

    void createDocument(int width, int height, bool emptyLayer = false);
    bool openProject(const QString &path);
    bool insertImage(const QImage &image, const QString &name, const std::optional<QPointF> &center = std::nullopt);
    bool insertSvg(const QString &path, const std::optional<QPointF> &center = std::nullopt, QString *error = nullptr);
    bool insertPhotoshop(const PSDImportResult &imported, const QString &named,
                         const std::optional<QPointF> &center = std::nullopt, QString *error = nullptr);
    using ConfirmConversionsCallback = std::function<bool(const QVector<PSDConversion> &)>;
    void setConfirmConversionsCallback(ConfirmConversionsCallback cb) { confirmConversions_ = std::move(cb); }
    [[nodiscard]] const ConfirmConversionsCallback &confirmConversionsCallback() const noexcept { return confirmConversions_; }
    bool insertPixelLayer(const QImage &image, const QPointF &origin, const QString &name,
                          const QString &historyName = QStringLiteral("Paste"), bool dropsSelection = true);
    [[nodiscard]] std::optional<QPair<QImage, QPoint>> copiedPixels(bool merged = false) const;
    // Replaces the selection with `mask` as one undo step; an empty mask deselects. (Select > Color Range.)
    bool replaceSelection(const QImage &mask, const QString &historyName);
    // Shows `mask` (or nothing/the original) as the selection without an undo step, for a dialog's live preview.
    void previewSelection(const std::optional<QImage> &mask);
    void selectAll();
    void deselect();
    void invertSelection();
    bool expandSelection(int amount);
    bool contractSelection(int amount);
    bool featherSelection(int amount);
    bool moveSelection(const QPoint &offset);
    bool nudgeSelectedPixels(const QPoint &offset);
    bool setRectangularSelection(const QRect &rect, SelectionMode mode = SelectionMode::Replace);
    bool setEllipticalSelection(const QRect &rect, SelectionMode mode = SelectionMode::Replace, bool antialiased = true);
    bool setPolygonSelection(const QPolygonF &points, SelectionMode mode = SelectionMode::Replace, bool antialiased = true);
    bool magicWand(const QPoint &documentPoint, int tolerance = 32, int sampleRadius = 0,
                   bool contiguous = true, bool sampleAllLayers = false,
                   SelectionMode mode = SelectionMode::Replace, bool antialiased = true);
    struct SelectionSnapshot {
        QUuid documentId;
        quint64 documentGeneration = 0;
        QSize canvasSize;
        QImage sampleImage;
        bool valid = false;
        QString error;
    };

    struct SelectionComputationResult {
        bool success = false;
        QImage mask;
        QString error;
        QUuid documentId;
        quint64 documentGeneration = 0;
        quint64 requestId = 0;
    };

    [[nodiscard]] quint64 sessionRevision() const noexcept { return sessionRevision_; }
    [[nodiscard]] quint64 documentGeneration() const noexcept { return document_ ? sessionRevision_ : 0; }
    quint64 advanceRevision() noexcept;
    [[nodiscard]] std::optional<QUuid> documentId() const { return document_ ? std::optional<QUuid>(document_->id) : std::nullopt; }

    [[nodiscard]] SelectionSnapshot createSelectionSnapshot(bool sampleAllLayers, QString *error = nullptr) const;

    [[nodiscard]] static SelectionComputationResult computeSubjectSelection(
        const SelectionSnapshot &snapshot,
        quint64 requestId = 0,
        std::atomic<bool> *cancelled = nullptr);

    [[nodiscard]] static SelectionComputationResult computeObjectSelection(
        const SelectionSnapshot &snapshot,
        const QPoint &documentPoint,
        int edgeOffset,
        bool smoothEdges,
        quint64 requestId = 0,
        std::atomic<bool> *cancelled = nullptr);

    bool applySelectionResult(
        const SelectionComputationResult &result,
        SelectionMode mode,
        const QString &historyName,
        QString *error = nullptr);

    bool selectSubject(bool sampleAllLayers = true, SelectionMode mode = SelectionMode::Replace,
                       QString *error = nullptr, std::atomic<bool> *cancelled = nullptr);
    bool selectObject(const QPoint &documentPoint, int edgeOffset = 0, bool smoothEdges = true,
                      bool sampleAllLayers = true, SelectionMode mode = SelectionMode::Replace,
                      QString *error = nullptr, std::atomic<bool> *cancelled = nullptr);
    [[nodiscard]] int selectionFeatherAmount() const { return selectionFeatherAmount_; }
    void setSelectionFeatherAmount(int amount) { selectionFeatherAmount_ = std::clamp(amount, 1, 250); }
    [[nodiscard]] int objectSelectionEdgeOffset() const { return objectSelectionEdgeOffset_; }
    void setObjectSelectionEdgeOffset(int offset) { objectSelectionEdgeOffset_ = std::clamp(offset, -10, 10); }
    [[nodiscard]] bool objectSelectionSmoothEdges() const { return objectSelectionSmoothEdges_; }
    void setObjectSelectionSmoothEdges(bool smooth) { objectSelectionSmoothEdges_ = smooth; }
    [[nodiscard]] bool objectSelectionSampleAllLayers() const { return objectSelectionSampleAllLayers_; }
    void setObjectSelectionSampleAllLayers(bool sampleAll) { objectSelectionSampleAllLayers_ = sampleAll; }
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
    bool addShape(ShapeKind kind, const QRectF &rect, const QColor &fill, const QColor &stroke = Qt::transparent,
                  double strokeWidth = 2, double cornerRadius = 0,
                  const std::optional<QPointF> &start = std::nullopt,
                  const std::optional<QPointF> &end = std::nullopt);
    bool addShape(const QRectF &rect, const QColor &fill, const QColor &stroke,
                  double strokeWidth = 2, bool ellipse = false, double cornerRadius = 0);
    bool addText(const QString &text, const QRectF &box, const QString &fontFamily,
                 int pixelSize, bool bold, bool italic, bool underline, int alignment, const QColor &color, bool areaText = true,
                 double tracking = 0.0, double leading = 0.0);
    bool updateText(const QUuid &id, const QString &text, const QRectF &box, const QString &fontFamily,
                    int pixelSize, bool bold, bool italic, bool underline, int alignment, const QColor &color, bool areaText,
                    double tracking = 0.0, double leading = 0.0);
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
    bool applyBlackWhite(const float *weights, bool tint, double tintHue, double tintSaturation);
    bool applyColorBalance(const float *shadows, const float *midtones, const float *highlights, bool preserveLuminosity);
    bool applyVignette(double amount, const QColor &color, double midpoint = 50.0, double roundness = 100.0, double feather = 60.0, double highlights = 25.0);
    bool applyBloomGlow(double amount, double radius);
    bool applyDither(const DitherSettings &settings);
    bool applyTonalContrast(double amount, double radius, double shadows = 40.0, double midtones = 60.0, double highlights = 30.0);
    bool applyCameraRaw(const CameraRawSettings &settings);
    bool applyCameraRaw(const QUuid &id, const CameraRawSettings &settings);
    bool contentAwareFill();

    bool removeBackground(const SubjectRemovalSettings &settings = {}, const QImage &preparedMask = {}, QString *error = nullptr);
    bool beginBrushStroke(const QPointF &documentPoint, const QColor &color, double diameter = 40,
                          double hardness = 1, double opacity = 1, bool erasing = false,
                          const std::optional<QPointF> &cloneSource = std::nullopt,
                          int healMode = -1, quint32 effectSeed = 0);
    void setCloneSource(const QPointF &documentPoint);
    [[nodiscard]] std::optional<QPointF> cloneSource() const { return cloneSource_; }
    [[nodiscard]] std::optional<QPointF> cloneOffset() const { return cloneOffset_; }
    // Why a brush, gradient, smudge or liquify cannot paint right now, in words for the user; empty when it can
    // (mac 133c34a). `forMask` is true when the target is the layer's mask.
    [[nodiscard]] QString paintRefusal() const;
    bool beginCloneStroke(const QPointF &documentPoint, double diameter = 40, double hardness = 1, double opacity = 1,
                          bool aligned = true, bool sampleAllLayers = false);
    bool beginHealingStroke(const QPointF &documentPoint, double diameter = 40, double hardness = 1,
                            double opacity = 1, int mode = 0, quint32 seed = 0);
    bool beginBlurStroke(const QPointF &documentPoint, double diameter = 40, double hardness = 1, double opacity = 1);
    bool beginWarpStroke(const QPointF &documentPoint, int mode, double diameter = 40, double hardness = 1, double strength = 1);
    void continueBrushStroke(const QPointF &documentPoint);
    bool endBrushStroke();
    bool cancelBrushStroke();
    void setBrushSmoothing(double smoothing) { brushSmoothing_ = std::clamp(smoothing, 0.0, 100.0); }
    [[nodiscard]] double brushSmoothing() const { return brushSmoothing_; }
    void setViewportZoom(double zoom) { viewportZoom_ = std::max(0.01, zoom); }
    [[nodiscard]] double viewportZoom() const { return viewportZoom_; }
    [[nodiscard]] std::optional<QPointF> brushAnchor() const { return brushAnchor_; }
    [[nodiscard]] std::optional<QPointF> brushPointer() const { return brushPointer_; }
    [[nodiscard]] bool brushSmoothingEnabled() const { return brushSmoothingEnabled_; }
    [[nodiscard]] bool isPainting() const { return brush_.has_value() || warp_.has_value(); }
    bool addLayerMask(bool revealing = true, bool useSelection = true);
    bool toggleLayerMask();
    bool toggleMaskLink();
    bool deleteLayerMask();
    bool invertLayerMask();
    [[nodiscard]] bool canCopyLayerMask(const QUuid &sourceId, const QUuid &targetId) const;
    bool copyLayerMask(const QUuid &sourceId, const QUuid &targetId);
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
    bool trim(const TrimOptions &options = TrimOptions());
    [[nodiscard]] std::optional<QRect> selectionBounds() const;
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
    [[nodiscard]] bool canSaveAdjustment(const QString &kind) const;
    [[nodiscard]] bool canSaveAdjustment(AdjustmentKind kind) const;
    bool addAdjustment(const QString &kind, const QJsonObject &settings = {});
    bool updateAdjustment(const QUuid &id, const QJsonObject &settings, const QString &historyName = QStringLiteral("Edit Adjustment"));
    bool previewAdjustment(const QUuid &id, const QJsonObject &settings);
    void groupSelectedLayers();
    // The active layer must be a folder, so there is something to unwrap.
    [[nodiscard]] bool canUngroupLayers() const;
    void ungroupLayers();
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
    [[nodiscard]] bool canEditEffects() const;
    [[nodiscard]] std::optional<LayerEffects> activeLayerEffects() const;
    [[nodiscard]] std::optional<LayerEffects> layerEffects(const QUuid &id) const;
    bool setLayerEffects(const QUuid &id, const LayerEffects &effects, const QString &historyName = QStringLiteral("Layer Effects"));
    bool addLayerEffect(const QUuid &id, LayerEffectKind kind);
    bool toggleLayerEffect(const QUuid &id, LayerEffectKind kind);
    bool removeLayerEffect(const QUuid &id, LayerEffectKind kind);
    [[nodiscard]] bool canCopyLayerEffect(LayerEffectKind kind, const QUuid &sourceId, const QUuid &targetId) const;
    bool copyLayerEffect(LayerEffectKind kind, const QUuid &sourceId, const QUuid &targetId);
    bool copyAllLayerEffects(const QUuid &sourceId, const QUuid &targetId);
    bool clearLayerEffects(const QUuid &id);
    bool previewLayerEffects(const QUuid &id, const std::optional<LayerEffects> &effects);
    bool previewLayerImage(const QUuid &id, const QImage &image);
    bool rollbackLayer(const QUuid &id, const Layer &originalLayer);
    bool rollbackLayerImage(const QUuid &id, const QImage &originalImage);
    bool previewCameraRaw(const QUuid &id, const CameraRawSettings &settings,
                          CameraRawClipping clipping = CameraRawClipping::None);

    [[nodiscard]] bool showsGuides() const { return showsGuides_; }

    void setShowsGuides(bool shows) { showsGuides_ = shows; }
    [[nodiscard]] bool locksGuides() const { return locksGuides_; }
    void setLocksGuides(bool locks) { locksGuides_ = locks; }
    [[nodiscard]] bool showsRulers() const { return showsRulers_; }
    void setShowsRulers(bool shows) { showsRulers_ = shows; }
    [[nodiscard]] const LayoutGrid &layoutGrid() const { return layoutGrid_; }
    void setLayoutGrid(const LayoutGrid &grid) { layoutGrid_ = grid; }
    [[nodiscard]] const GridAppearance &gridAppearance() const { return gridAppearance_; }
    void setGridAppearance(const GridAppearance &appearance) { gridAppearance_ = appearance; }
    [[nodiscard]] bool showsGrid() const { return showsGrid_; }
    void setShowsGrid(bool shows) { showsGrid_ = shows; }
    [[nodiscard]] bool snapEnabled() const { return snapEnabled_; }
    void setSnapEnabled(bool enabled) { snapEnabled_ = enabled; }
    [[nodiscard]] bool snapToGuides() const { return snapToGuides_; }
    void setSnapToGuides(bool enabled) { snapToGuides_ = enabled; }
    [[nodiscard]] bool snapToGrid() const { return snapToGrid_; }
    void setSnapToGrid(bool enabled) { snapToGrid_ = enabled; }
    [[nodiscard]] bool snapToDocumentBounds() const { return snapToDocumentBounds_; }
    void setSnapToDocumentBounds(bool enabled) { snapToDocumentBounds_ = enabled; }
    [[nodiscard]] bool snapToLayers() const { return snapToLayers_; }
    void setSnapToLayers(bool enabled) { snapToLayers_ = enabled; }

    [[nodiscard]] std::optional<GuideDrag> guideDrag() const { return guideDrag_; }
    [[nodiscard]] QVector<CanvasGuide> displayedGuides() const;
    [[nodiscard]] std::optional<CanvasGuide> hitGuide(const QPointF &docPoint, double tolerance = 5.0) const;

    [[nodiscard]] bool canClearGuides() const;
    [[nodiscard]] bool canEditGuides() const;

    void beginGuideCreation(CanvasGuide::Axis axis, double position);
    void beginGuideMove(const CanvasGuide &guide);
    void moveGuideDrag(double position);
    void finishGuideDrag(bool deleteGuide);
    void cancelGuideDrag();
    void clearGuides();
    void addGuide(const CanvasGuide &guide);

    [[nodiscard]] double snappedGuidePosition(double position, CanvasGuide::Axis axis,
                                              const std::optional<QUuid> &excluding = std::nullopt,
                                              double tolerance = 8.0) const;

    struct SnapTargets {
        QVector<double> xs;
        QVector<double> ys;
    };
    [[nodiscard]] SnapTargets alignmentSnapTargets(const QSet<QUuid> &excludingLayers = {}, bool includeCenters = true) const;
    [[nodiscard]] SnapTargets transformSnapTargets(const QSet<QUuid> &excludingLayers = {}) const { return alignmentSnapTargets(excludingLayers, true); }
    [[nodiscard]] SnapTargets cropSnapTargets() const { return alignmentSnapTargets({}, false); }
    // A resize handle dragged to `point`: the pointer nudged so the edges the handle moves land on a nearby target
    // (canvas, other layers, guides, per View > Snap To) within `tolerance` document pixels, as a moved layer's do
    // (mac c459f88). Each edge snaps on its own; with the ratio locked only the nearer one does and the other follows
    // it. An upright layer only: a turned one's edges don't run along the targets. `guideX/guideY` get the targets hit.
    [[nodiscard]] QPointF snappedResizePoint(const QPointF &point, const LayerTransform &start, const QPointF &startPoint,
                                             const QPoint &sign, bool fromCenter, bool lockRatio, const QSet<QUuid> &moving,
                                             double tolerance, std::optional<double> *guideX = nullptr,
                                             std::optional<double> *guideY = nullptr) const;
    // `point` moved onto the nearest crop target within `tolerance`, each axis on its own: where a marquee or shape
    // starts and where its corner is dragged to (mac d6e3e93).
    [[nodiscard]] QPointF snappedPoint(const QPointF &point, double tolerance, std::optional<double> *guideX = nullptr,
                                       std::optional<double> *guideY = nullptr) const;
    // A selection being moved by `offset`, nudged so its edges or middle meet a nearby target, each axis on its own;
    // an axis Shift has locked doesn't snap (mac 8b1369a).
    [[nodiscard]] QPointF snappedSelectionOffset(const QRectF &selectionBounds, const QPointF &offset, double tolerance,
                                                 bool horizontal = true, bool vertical = true,
                                                 std::optional<double> *guideX = nullptr, std::optional<double> *guideY = nullptr) const;
    [[nodiscard]] QVector<double> cropSnapTargetsX() const;
    [[nodiscard]] QVector<double> cropSnapTargetsY() const;

    void undo();
    void redo();
    [[nodiscard]] QUuid currentRevision() const { return history_.currentRevision(); }
    void markSaved() { history_.markSaved(); }
    void markSaved(const QUuid &revision) { history_.markSaved(revision); }
    void beginEdit(const QString &name) { history_.begin(name, document_); }
    void endEdit() {
        history_.end(document_);
        advanceRevision();
    }

private:
    [[nodiscard]] int indexOf(const QUuid &id) const;
    [[nodiscard]] QString nextName(const QString &base) const;
    [[nodiscard]] QSet<QUuid> selectedTransformLayerIds() const;
    void restore(const DocumentHistory::Snapshot &snapshot);
    void deleteIds(const QSet<QUuid> &ids, const QString &historyName);
    void translateCanvas(const QSize &newSize, const QPointF &offset);

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
    int selectionFeatherAmount_ = 6;
    int objectSelectionEdgeOffset_ = 0;
    bool objectSelectionSmoothEdges_ = true;
    bool objectSelectionSampleAllLayers_ = true;
    bool showsGuides_ = true;
    bool locksGuides_ = false;
    bool showsRulers_ = false;
    bool showsGrid_ = false;
    LayoutGrid layoutGrid_;
    GridAppearance gridAppearance_;
    bool snapEnabled_ = true;
    bool snapToGuides_ = true;
    bool snapToGrid_ = true;
    bool snapToDocumentBounds_ = true;
    bool snapToLayers_ = true;
    std::optional<QPointF> smoothedBrushPoint(const QPointF &point);
    void continueBrushStrokeInternal(const QPointF &documentPoint);

    double brushSmoothing_ = 0.0;
    double viewportZoom_ = 1.0;
    std::optional<QPointF> brushAnchor_;
    std::optional<QPointF> brushPointer_;
    bool brushSmoothingEnabled_ = false;

    std::optional<GuideDrag> guideDrag_;
    ConfirmConversionsCallback confirmConversions_;
    quint64 sessionRevision_ = 1;
};

} // namespace compositor
