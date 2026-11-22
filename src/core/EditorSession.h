#pragma once
#include "rendering/RgbSplit.h"
#include "rendering/LensEffects.h"
#include "rendering/FilterLayers.h"
#include "rendering/RimLight.h"
#include "rendering/VolumetricLight.h"
#include "rendering/Scanlines.h"
#include "rendering/ProgressiveBlur.h"

#include "rendering/Dither.h"
#include "core/CameraRaw.h"
#include "core/DocumentHistory.h"
#include "core/ImageTrim.h"
#include "rendering/RasterOperations.h"
#include "rendering/SubjectRemoval.h"
#include "core/ToolDefaults.h"
#include "core/LayerAlign.h"


#include <QSet>
#include <QVariantMap>

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

    // mac EditorSession.filterSettings: the last values each filter was committed with, which its panel reopens with.
    CameraRawSettings lastCameraRaw;
    QVariantMap filterMemory;
    DitherSettings lastDither;
    LensEffectSettings lastLensEffects;
    RimLightSettings lastRimLight;
    VolumetricLightSettings lastVolumetricLight;

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

    void createDocument(int width, int height, bool emptyLayer = false, double resolution = 72, const std::optional<QColor> &background = std::nullopt);
    bool openProject(const QString &path);
    bool insertImage(const QImage &image, const QString &name, const std::optional<QPointF> &center = std::nullopt);
    bool insertGif(const QString &path, const std::optional<QPointF> &center = std::nullopt, QString *error = nullptr);
    bool selectGifFrame(int index);
    // The selected frame, or the given one, which is selected first.
    bool duplicateGifFrame(std::optional<int> index = std::nullopt);
    bool deleteGifFrame(std::optional<int> index = std::nullopt);
    bool setGifTiming(std::optional<int> uniformMilliseconds);
    bool setGifDelay(int milliseconds, bool sameForAll = false, bool original = false);
    bool insertPdf(const QString &path, const std::optional<QPointF> &center = std::nullopt, QString *error = nullptr);
    bool insertSvg(const QString &path, const std::optional<QPointF> &center = std::nullopt, QString *error = nullptr);
    bool insertPhotoshop(const PSDImportResult &imported, const QString &named,
                         const std::optional<QPointF> &center = std::nullopt, QString *error = nullptr);
    using ConfirmConversionsCallback = std::function<bool(const QVector<PSDConversion> &)>;
    void setConfirmConversionsCallback(ConfirmConversionsCallback cb) { confirmConversions_ = std::move(cb); }
    [[nodiscard]] const ConfirmConversionsCallback &confirmConversionsCallback() const noexcept { return confirmConversions_; }
    // A new pixel layer above the active one. With a folder active it goes in at the folder's top, or with
    // `besideFolder` (Paste, mac aa22633) just above the folder, outside it; added in the middle of a clipping group, it
    // joins the group (mac 01e6505).
    bool insertPixelLayer(const QImage &image, const QPointF &origin, const QString &name,
                          const QString &historyName = QStringLiteral("Paste"), bool dropsSelection = true, bool besideFolder = false);
    // Where a new layer goes: just above the active layer, or with a folder active, at the top of the folder (mac
    // newLayerIndex, a587dcc). With nothing active, on top.
    [[nodiscard]] int newLayerIndex() const;
    // `id`, added in the middle of a clipping group, joins it: clipped to the base the layer above it is clipped to,
    // when the one below is that base or clipped to it too (mac adoptClipping).
    static void adoptClipping(const QUuid &id, QVector<Layer> &layers);
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
    // `name` is the undo step: "Lasso" for freehand, "Polygonal Lasso" for polygonal (mac Selection.swift finishLasso).
    bool setPolygonSelection(const QPolygonF &points, SelectionMode mode = SelectionMode::Replace, bool antialiased = true,
                             const QString &name = QStringLiteral("Lasso"));
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
    // Paints a text layer's letters in `color`, keeping it text. False when the layer is not live text.
    bool clearSelectedPixels();
    // `historyName` defaults to "Transform Selection" ("Duplicate Pixels" when duplicating); a Cmd-drag or Cmd-arrow move
    // is "Move Pixels" (mac SelectionEdits.swift finishPixelMove).
    bool beginSelectionTransform(bool duplicate = false, const QString &historyName = {});
    bool commitSelectionTransform();
    bool cancelSelectionTransform();
    [[nodiscard]] bool hasFloatingSelection() const { return floating_.has_value(); }
    bool applyGradient(const QPointF &start, const QPointF &end, const QColor &foreground,
                       const QColor &background, bool radial = false, bool foregroundTransparent = false,
                       bool reversed = false, double opacity = 1);
    bool addShape(ShapeKind kind, const QRectF &rect, const QColor &fill, const QColor &stroke = Qt::transparent,
                  double strokeWidth = 2, double cornerRadius = 0,
                  const std::optional<QPointF> &start = std::nullopt,
                  const std::optional<QPointF> &end = std::nullopt, bool smoothing = false,
                  const QString &lineStyle = "Solid", const QString &lineCap = "Square");
    bool addShape(const QRectF &rect, const QColor &fill, const QColor &stroke,
                  double strokeWidth = 2, bool ellipse = false, double cornerRadius = 0);
    bool addText(const QString &text, const QRectF &box, const QString &fontFamily,
                 int pixelSize, bool bold, bool italic, bool underline, int alignment, const QColor &color, bool areaText = true,
                 double tracking = 0.0, double leading = 0.0);
    bool updateText(const QUuid &id, const QString &text, const QRectF &box, const QString &fontFamily,
                    int pixelSize, bool bold, bool italic, bool underline, int alignment, const QColor &color, bool areaText,
                    double tracking = 0.0, double leading = 0.0);
    // Style-centric variants: the style carries content, base face and color, per-letter runs, tracking and leading.
    bool addText(const TextStyle &style, const QRectF &box, bool bold, bool italic, bool underline, bool areaText);
    bool updateText(const QUuid &id, const TextStyle &style, const QRectF &box, bool bold, bool italic, bool underline, bool areaText,
                    const QString &historyName = QStringLiteral("Edit Text"));
    // Fill with a palette color on live text recolors its letters and keeps them editable (mac TypeTool.swift recolorText).
    // False when the layer is not live text or could not be redrawn, so the caller fills as usual.
    bool recolorText(const QUuid &id, const QColor &color);

    // Shape editing (mac 1.7 3adce92 … a38cb04, ShapeTool.swift). A selected rectangle or ellipse that is still the
    // shape it was drawn as takes the Shape bar's fill, inside stroke and corner radius; a selected line its color,
    // width, dashes and caps. With none selected, the bar sets the next shape.
    // `style` drawn into a box of `size`: a rectangle or ellipse filling it, its stroke inside it; a line between its
    // two ends.
    static QImage shapeImage(const LayerShapeStyle &style, QSize size);
    // The rectangle or ellipse, or the line, the bar edits: the only selected layer, still a live shape. The Shape tool
    // must be the one in use, which the window tells the session (`shapeToolActive`).
    [[nodiscard]] const Layer *editingShapeLayer() const;
    [[nodiscard]] const Layer *editingLineLayer() const;
    bool shapeToolActive = false;
    // The next rectangle or ellipse's inside stroke; 0 draws none, as every shape started.
    double shapeStrokeWidth = 0;
    QColor shapeStrokeColor = Qt::black;
    // The bar's values for the next shape when nothing is being edited.
    double shapeCornerRadius = 0, shapeLineWidth = 4;
    bool shapeSmoothing = false;
    QString shapeLineStyle = QStringLiteral("Solid"), shapeLineCap = QStringLiteral("Square");
    // A fill picked in the Shape bar, and the foreground when it was: the next shape uses it until the foreground
    // changes (mac a38cb04). Otherwise the next shape is the foreground color.
    std::optional<std::pair<QColor, QColor>> shapeFillChoice;
    [[nodiscard]] QColor nextShapeFill() const;
    // What the bar shows.
    [[nodiscard]] QColor displayedShapeFill() const { return nextShapeFill(); }
    [[nodiscard]] QColor displayedShapeStroke() const;
    [[nodiscard]] double displayedStrokeWidth() const;
    [[nodiscard]] double displayedCornerRadius() const;
    [[nodiscard]] bool displayedShapeSmoothing() const;
    [[nodiscard]] double displayedLineWidth() const;
    [[nodiscard]] QString displayedLineStyle() const;
    [[nodiscard]] QString displayedLineCap() const;
    // Changing a value redraws the selected shape, one "Shape Style" step each; between begin and finish (a slider or
    // scrub drag) all changes are one step.
    void beginShapeStyleEdit();
    void finishShapeStyleEdit();
    bool setDisplayedStrokeWidth(double width);
    bool setDisplayedCornerRadius(double radius);
    bool setDisplayedShapeSmoothing(bool on);
    bool setDisplayedLineWidth(double width);
    bool setDisplayedLineStyle(const QString &style);
    bool setDisplayedLineCap(const QString &cap);
    // The Fill swatch's pick: recolors the selected shape (or line) and carries on to the next shape until the
    // foreground changes; with nothing selected it is just remembered for the next shape.
    bool pickShapeFill(const QColor &color);
    // The Stroke swatch's pick: the selected shape's stroke when it has one, else the next stroke's color.
    bool pickShapeStroke(const QColor &color);
    // While a swatch's color picker is open, the canvas follows it with nothing recorded; cancel puts the shape back.
    void previewShapeColor(const QColor &color, bool fill);
    void cancelShapeColorPreview();
    // Fill on a live shape with no selection makes it that color and keeps it a shape, stroke included ("Fill").
    bool recolorShape(const QUuid &id, const QColor &color);
    // The kind the Shape tool should open on: the selected live shape's, if there is one (mac 8997bfe).
    [[nodiscard]] std::optional<ShapeKind> selectedShapeKind() const;
    // A dragged-out shape, in what the bar shows: the next shape's fill, a rectangle or ellipse's inside stroke and
    // radius, a line's width, dashes and caps (mac commitShape). A line runs from `start` to `end`.
    bool drawShape(ShapeKind kind, const QRectF &rect, const std::optional<QPointF> &start = std::nullopt,
                   const std::optional<QPointF> &end = std::nullopt);
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
    bool applyGaussianBlur(double radius, bool preserveTransparency = false);
    bool applyScanlines(const ScanlinesSettings &settings);
    bool applyProgressiveBlur(const ProgressiveBlurSettings &settings);
    bool convertDepth(bool sixteenBit);
    // Convert to 16-Bit / Convert to 8-Bit on every selected layer and those in selected folders, as one undo step (mac
    // de0a43d): whichever would change one of them is offered, both for a mix. A blank layer gets clear 16-bit pixels over
    // the canvas; an HDR layer is clipped to white and kept at 16 bits; to 8 bits they're dithered down.
    [[nodiscard]] bool canConvertToSixteenBit() const;
    [[nodiscard]] bool canConvertToEightBit() const;
    bool convertLayers(bool sixteenBit);
    bool applyRgbSplit(const RgbSplitSettings &settings);
    bool applyLensEffects(const LensEffectSettings &settings);
    // Filter › Sharpen and Filter › Reduce Noise (mac b0f0b2b, c1c5356): one undo step each, Last Filter repeats them.
    bool applySharpen(const SharpenSettings &settings);
    bool applyReduceNoise(const ReduceNoiseSettings &settings);
    // Filter › Rim Light (mac b9a0ed8): the lights added to the active layer's subject, one undo step, Last Filter.
    // False with `error` set when the subject can't be found (no model).
    bool applyRimLight(const RimLightSettings &settings, QString *error = nullptr);
    // Filter › Volumetric Light (mac 1405545 …): the light at settings.center (document coordinates; none, the layer's
    // biggest bright area), one undo step, Last Filter. Extract Depth adds the depth its haze goes by as a layer of its
    // own above the layer, white near and black far, one undo step.
    bool applyVolumetricLight(const VolumetricLightSettings &settings);
    bool extractDepth(const VolumetricLightSettings &settings, QString *error = nullptr);
    bool splitChannels();
    bool applyLastFilter();
    bool hasLastFilter() const { return bool(lastFilter_); }
    QString lastFilterName() const { return lastFilterName_; }
    // Off-thread filter previews run a filter on a copy of the document; the result is adopted here as if it had
    // been run in this session (the layer and, inside an edit, the filter Last Filter repeats).
    [[nodiscard]] std::function<bool(EditorSession &)> lastFilterAction() const { return lastFilter_; }
    void adoptPreviewedLayer(const QUuid &id, const Layer &layer, const std::function<bool(EditorSession &)> &filter)
    {
        rollbackLayer(id, layer);
        if (filter && editDepth_ > 0) pendingFilter_ = filter;
    }
    // A filter result rendered elsewhere (e.g. off the GUI thread), committed as one undoable edit of `id`'s pixels:
    // `edited` is clipped to the selection as the filter itself would, and `filter` becomes Last Filter.
    bool commitFilterResult(const QUuid &id, const QImage &edited, const QString &historyName,
                            const std::function<bool(EditorSession &)> &filter);
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

    // `preview`: refined on a copy at most 1400 pixels long, as the macOS panel's preview is; the commit is full size.
    bool removeBackground(const SubjectRemovalSettings &settings = {}, const QImage &preparedMask = {}, QString *error = nullptr,
                          bool preview = false);
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
    // Whether the active layer (or its mask) can take paint, a fill, a gradient or a pixel move (mac canPaint).
    [[nodiscard]] bool canPaint() const { return document_ && activeLayer() && paintRefusal().isEmpty(); }
    bool beginCloneStroke(const QPointF &documentPoint, double diameter = 40, double hardness = 1, double opacity = 1,
                          bool aligned = true, bool sampleAllLayers = false);
    bool beginHealingStroke(const QPointF &documentPoint, double diameter = 40, double hardness = 1,
                            double opacity = 1, int mode = 0, quint32 seed = 0);
    bool beginBlurStroke(const QPointF &documentPoint, double diameter = 40, double hardness = 1, double opacity = 1, double radius = 5);
    bool beginWarpStroke(const QPointF &documentPoint, int mode, double diameter = 40, double hardness = 1, double strength = 1);
    void continueBrushStroke(const QPointF &documentPoint);
    bool endBrushStroke();
    // Brush › Anti-alias. Off, the Brush paints and erases hard pixel edges, like a pencil (mac 1f1d84c); other tools
    // always anti-alias.
    bool brushAntialiased = true;
    bool cancelBrushStroke();
    void setPenInput(double pressure, bool size, bool opacity) { penPressure_ = std::clamp(pressure, 0.0, 1.0); penSize_ = size; penOpacity_ = opacity; }
    void setBrushSmoothing(double smoothing) { brushSmoothing_ = std::clamp(smoothing, 0.0, 100.0); }
    [[nodiscard]] double brushSmoothing() const { return brushSmoothing_; }
    void setViewportZoom(double zoom) { viewportZoom_ = std::max(0.01, zoom); }
    [[nodiscard]] double viewportZoom() const { return viewportZoom_; }
    [[nodiscard]] std::optional<QPointF> brushAnchor() const { return brushAnchor_; }
    [[nodiscard]] std::optional<QPointF> brushPointer() const { return brushPointer_; }
    [[nodiscard]] bool brushSmoothingEnabled() const { return brushSmoothingEnabled_; }
    [[nodiscard]] bool isPainting() const { return brush_.has_value() || warp_.has_value(); }
    // The palette (mac ColorPalette.swift). On a mask the swatches are black and white: the foreground one is the
    // colour the brush paints (maskPaintWhite), X flips it and D puts it back to black (hide).
    [[nodiscard]] QColor foregroundColor() const { return foreground_; }
    [[nodiscard]] QColor backgroundColor() const { return background_; }
    void setForegroundColor(const QColor &color) { if (color.isValid()) foreground_ = color.toRgb(); }
    void setBackgroundColor(const QColor &color) { if (color.isValid()) background_ = color.toRgb(); }
    [[nodiscard]] bool maskPaintWhite() const { return maskPaintWhite_; }
    void setMaskPaintWhite(bool white) { maskPaintWhite_ = white; }
    [[nodiscard]] QColor paletteColor(bool background) const;
    void setPaletteColor(const QColor &color, bool background);
    void swapPaletteColors();
    void resetPaletteColors();
    // Composited sRGB colour of the visible layers at one document pixel, as shown on the canvas; none outside the
    // canvas or over fully transparent pixels.
    [[nodiscard]] std::optional<QColor> sampleCompositeColor(const QPointF &documentPoint) const;
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
    // The menus' Hide/Show Layer(s) and Create/Release Clipping Mask, on every selected layer as one step (mac d693a2a):
    // all hidden, or with the active one hidden, all shown; each not yet clipped, from the bottom up, clipped to the layer
    // below or that layer's base, so a run of selected layers all clip to the layer under the lowest; releasing when
    // every one is clipped, each taking the layers clipped above it to the same base along.
    [[nodiscard]] QString visibilityTitle() const;
    void toggleSelectedLayersVisibility();
    [[nodiscard]] bool releasesSelectedClipping() const;
    [[nodiscard]] QString selectedClippingTitle() const;
    [[nodiscard]] bool canToggleSelectedClipping() const;
    bool toggleSelectedClipping();
    [[nodiscard]] bool canToggleClippingMask(const QUuid &id) const;
    // Layers and folders alike take a mask; one layer must be selected.
    [[nodiscard]] bool canEditMask() const;
    // With one layer's mask targeted only the mask is deleted, otherwise every selected layer (mac deleteLayerOrMask).
    void deleteLayerOrMask();
    bool cycleBlendMode(bool forward);
    void selectMaskTarget(bool mask);
    [[nodiscard]] bool isMaskSelected() const { return maskSelected_; }
    // Option-click on a mask thumbnail shows that mask alone on the canvas, in grayscale, for painting it with nothing
    // else in the way (mac b3419ab). It ends when the layer's pixels are targeted or another layer is picked, so the id
    // only counts while its layer is active with its mask targeted.
    [[nodiscard]] std::optional<QUuid> maskAloneLayerId() const;
    void toggleMaskAlone(const QUuid &id);
    bool resizeCanvas(const QSize &size, int anchor = 4, const std::optional<QColor> &extension = std::nullopt);
    bool resizeImage(const QSize &size, double resolution, Sampling sampling = Sampling::HighQuality);
    bool crop(const QRect &documentRect);
    bool trim(const TrimOptions &options = TrimOptions());
    [[nodiscard]] std::optional<QRect> selectionBounds() const;
    bool flipLayers(bool horizontally);
    // Align and Distribute (mac LayerAlign.swift): with the selection when there is one, with the canvas when one thing
    // is selected, otherwise with each other; a selected folder moves as one. Whole pixels, one undo step each.
    struct AlignmentItem { QVector<QUuid> members; QRectF box; };
    [[nodiscard]] QVector<AlignmentItem> alignmentItems() const;
    [[nodiscard]] bool canAlignLayers() const;
    [[nodiscard]] bool canDistributeLayers() const;
    bool alignLayers(LayerAlignment alignment);
    bool distributeLayers(LayerDistribution distribution);
    bool flipCanvas(bool horizontally);
    bool rotateCanvas(bool clockwise);
    [[nodiscard]] bool canMergeLayers() const;
    [[nodiscard]] QString mergeTitle() const;
    bool mergeLayers();
    bool flattenImage();
    void selectLayer(const std::optional<QUuid> &id);
    void selectLayers(const QSet<QUuid> &ids, const std::optional<QUuid> &primary = std::nullopt);
    // Ctrl-Shift-click on the canvas: adds a layer to the selection, or takes it out again when it is already in it.
    void extendSelection(const QUuid &id);
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
    // A clipped layer whose base is hidden: the base and every layer clipped to it, to show together from any of their
    // eyes, as in Photoshop (mac 3990a25). None otherwise.
    [[nodiscard]] std::optional<QVector<QUuid>> hiddenClippingStack(const QUuid &id) const;
    // Copies made of layers in a clipping stack form a stack of their own: a base's copy stays a base and the clipped
    // copies clip to it, rather than to the original base (mac af0dcb9). `copiesOf` maps each original to its copy.
    void relinkCopiedClipping(const QHash<QUuid, QUuid> &copiesOf);
    void setLayerOpacity(const QUuid &id, double opacity);
    void setSelectedLayersOpacity(double opacity);
    void setLayerBlendMode(const QUuid &id, BlendMode mode);
    [[nodiscard]] bool canMoveActiveLayer(int offset) const;
    void moveActiveLayer(int offset);
    bool reorderLayers(const QVector<int> &topFirstRows, int destination);
    void duplicateActiveLayer();
    // Move tool, Alt-arrow: the selected layers duplicated and the copies moved by `offset`, as one undo step (mac
    // aef9f9b duplicateAndNudgeLayer).
    bool duplicateAndNudgeLayers(const QPointF &offset);
    // The image file the document began as, opened or dropped into an empty window: until the document is saved as a
    // project, Export offers that file's name, folder and format, and Save As its name and folder (mac 2facf90).
    QString sourceImagePath;
    // The format (a file extension) this document was last exported in, offered first by Export As next time.
    QString exportedFormat;
    // Every selected layer (a folder with all it holds) copied as one undo step; the copies end up selected.
    bool duplicateLayers(const QVector<QUuid> &ids, const QString &editName = QStringLiteral("Duplicate Layer"));
    // The layers Copy and Duplicate take whole, in document order, leaving out any inside a selected folder.
    [[nodiscard]] QVector<QUuid> copiedLayerIds() const;
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

    // Where a tab's canvas was zoomed and panned to when it was last shown, so switching tabs brings it back as it was
    // (mac keeps a viewport per session).
    struct ViewState { bool valid = false; double zoom = 1.0; QPointF pan; bool fit = true; };
    [[nodiscard]] const ViewState &viewState() const { return viewState_; }
    void setViewState(const ViewState &state) { viewState_ = state; }

    // View › Show › Guides (Ctrl+;), as Show Extras leaves it: Ctrl+H outside the Move tool hides the guides for the
    // session only, so the setting underneath stays; turning guides on, or making one, brings them back (mac 9cd704d).
    [[nodiscard]] bool showsGuides() const { return showsGuides_ && !extrasHideGuides_; }
    void setShowsGuides(bool shows) { showsGuides_ = shows; extrasHideGuides_ = false; ToolDefaults::set(QStringLiteral("guides"), showsGuides_); }
    // The current selection's marching ants (mac bb5c8cc): hidden for this selection only. It still limits edits, and any
    // other selection, or none, shows its outline again, so a new one is never invisible.
    [[nodiscard]] bool showsSelectionEdges() const;
    void setShowsSelectionEdges(bool shows);
    // View › Show Extras (Ctrl+H outside the Move tool, mac 9cd704d): the selection's edges and the guides together. On
    // only while both show, so it hides whatever is showing and, pressed again, brings it all back; guides turned off
    // with Ctrl+; stay off.
    [[nodiscard]] bool showsExtras() const { return showsSelectionEdges() && !extrasHideGuides_; }
    void setShowsExtras(bool shows) { setShowsSelectionEdges(shows); extrasHideGuides_ = !shows; }
    // The status bar's selection size in whole pixels (mac 63f1013): the box around the selection, cut to the canvas.
    // The canvas shows a Marquee being drawn itself. None without a selection.
    [[nodiscard]] std::optional<QSize> selectionSize() const;
    // The selection's feather in whole pixels, for the status bar (mac 820b8fa); none for a hard edge.
    [[nodiscard]] std::optional<int> selectionFeather() const;
    // Expand, Contract and Feather in the selection tools' bar and the Select menu share one amount (mac 9986d12).
    [[nodiscard]] int selectionModifyAmount() const { return selectionModifyAmount_; }
    void setSelectionModifyAmount(int amount) { selectionModifyAmount_ = std::clamp(amount, 1, 500); }
    [[nodiscard]] bool locksGuides() const { return locksGuides_; }
    void setLocksGuides(bool locks) { locksGuides_ = locks; ToolDefaults::set(QStringLiteral("lockGuides"), locksGuides_); }
    [[nodiscard]] bool showsRulers() const { return showsRulers_; }
    void setShowsRulers(bool shows) { showsRulers_ = shows; ToolDefaults::set(QStringLiteral("rulers"), showsRulers_); }
    [[nodiscard]] const LayoutGrid &layoutGrid() const { return layoutGrid_; }
    void setLayoutGrid(const LayoutGrid &grid) { layoutGrid_ = grid; }
    [[nodiscard]] const GridAppearance &gridAppearance() const { return gridAppearance_; }
    void setGridAppearance(const GridAppearance &appearance) { gridAppearance_ = appearance; }
    [[nodiscard]] bool showsGrid() const { return showsGrid_; }
    void setShowsGrid(bool shows) { showsGrid_ = shows; ToolDefaults::set(QStringLiteral("grid"), showsGrid_); }
    [[nodiscard]] bool snapEnabled() const { return snapEnabled_; }
    void setSnapEnabled(bool enabled) { snapEnabled_ = enabled; ToolDefaults::set(QStringLiteral("snap"), snapEnabled_); }
    [[nodiscard]] bool snapToGuides() const { return snapToGuides_; }
    void setSnapToGuides(bool enabled) { snapToGuides_ = enabled; ToolDefaults::set(QStringLiteral("snapGuides"), snapToGuides_); }
    [[nodiscard]] bool snapToGrid() const { return snapToGrid_; }
    void setSnapToGrid(bool enabled) { snapToGrid_ = enabled; ToolDefaults::set(QStringLiteral("snapGrid"), snapToGrid_); }
    [[nodiscard]] bool snapToDocumentBounds() const { return snapToDocumentBounds_; }
    void setSnapToDocumentBounds(bool enabled) { snapToDocumentBounds_ = enabled; ToolDefaults::set(QStringLiteral("snapBounds"), snapToDocumentBounds_); }
    [[nodiscard]] bool snapToLayers() const { return snapToLayers_; }
    void setSnapToLayers(bool enabled) { snapToLayers_ = enabled; ToolDefaults::set(QStringLiteral("snapLayers"), snapToLayers_); }

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
    void beginEdit(const QString &name) {
        if (editDepth_++ == 0) { captureGifEdits(); pendingFilter_ = {}; editRevision_ = history_.currentRevision(); }
        history_.begin(name, document_);
    }
    void endEdit() {
        if (editDepth_ == 1) commitGifEdits();
        if (editDepth_ == 1) if (const auto *before = history_.endingEditBefore(); before && *before) keepLayerDepths(**before);
        raiseFormatVersion();
        history_.end(document_);
        if (editDepth_ > 0 && --editDepth_ == 0) {
            if (history_.currentRevision() != editRevision_ && pendingFilter_) { lastFilter_ = pendingFilter_; lastFilterName_ = history_.undoName(); }
            pendingFilter_ = {};
        }
        advanceRevision();
    }

private:
    std::optional<QUuid> shapeStyleEditLayerId_;
    bool restartingBrush_ = false;
    void restartBrushAtDisplayedSize(const QPointF &documentPoint);
    struct ShapeColorPreview { QUuid layerId; QImage image; std::optional<LayerShapeStyle> style; QJsonObject shape; };
    std::optional<ShapeColorPreview> shapeColorPreview_;
    bool paintShape(int index, const LayerShapeStyle &style, std::optional<LayerTransform> placement = std::nullopt);
    bool changeLiveShape(const std::function<void(LayerShapeStyle &, LayerTransform &)> &change, bool line);
    void captureGifEdits();
    void commitGifEdits();
    QMap<QUuid, Layer> gifBefore_;
    QSize gifCanvasBefore_;
    QString lastFilterName_;
    int editDepth_ = 0;
    QUuid editRevision_;
    std::function<bool(EditorSession &)> lastFilter_, pendingFilter_;
    // Whatever an edit just put into the document (a mask, a blend mode, an adjustment...) may need a newer project
    // format than the one it was opened as; the version is raised with the edit, inside its undo step, so Undo restores
    // the older one and saving never refuses a document the user could edit.
    void raiseFormatVersion();
    // Keeps a layer at the depth it was given (mac 5de396b): an edit that left a 16-bit or HDR layer with only 8-bit
    // pixels (Content-Aware Fill, Remove Background, Dither, an 8-bit tool) is made 16-bit again, its 16-bit pixels kept
    // wherever the edit didn't change them. Only Convert to 8-Bit makes a layer 8-bit.
    void keepLayerDepths(const Document &before);
    bool convertsToEightBit_ = false;
    mutable qint64 selectionBoundsKey_ = 0;
    mutable std::optional<QRect> selectionBoundsCache_;
    // A filter that reaches past the layer's edge (blur, bloom): runs on the layer padded by `margin` pixels on every
    // side, then trims the empty padding again (mac PixelFilter.trimmed / FilterEdit.growForBlur).
    bool applySpreadingFilter(const QString &historyName, double margin, const std::function<QImage(const QImage &)> &filter);
    bool moveAlignmentItems(const QVector<AlignmentItem> &items, const QVector<QSizeF> &offsets, const QString &name);
    [[nodiscard]] QSet<QUuid> clippingTargets() const;
    [[nodiscard]] QVector<int> depthTargets() const;
    [[nodiscard]] std::optional<QVector<Layer>> clippedSelection() const;
    [[nodiscard]] int indexOf(const QUuid &id) const;
    std::optional<QUuid> insertCopyOf(const QUuid &id);
    [[nodiscard]] QString nextName(const QString &base) const;
    [[nodiscard]] QSet<QUuid> selectedTransformLayerIds() const;
    void restore(const DocumentHistory::Snapshot &snapshot);
    void deleteIds(const QSet<QUuid> &ids, const QString &historyName);
    void translateCanvas(const QSize &newSize, const QPointF &offset);

    std::shared_ptr<Document> document_;
    QSet<QUuid> selectedLayerIds_;
    DocumentHistory history_;
    double penPressure_ = 1;
    bool penSize_ = false, penOpacity_ = false;
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
        double blurRadius = 5;
        double settledCarry = 0;   // distance to the next soft dab along the settled path
        // A stroke on a blank layer fills one in, and one past a layer's edge grows it: the layer as it was, to put back
        // when nothing was painted, and where its own pixels sit in the grown grid (empty for a blank layer).
        struct Restore { bool valid = false; QImage image; LayerTransform transform; QImage mask; std::optional<LayerTransform> maskPlacement; };
        Restore restore;
        bool blank = false, grown = false;
        // A brush on a mask grows it over the canvas (mac growsMask): where its old pixels sit in the grown grid.
        bool maskGrown = false;
        QRect maskSourceRect;
        QRect sourceRect;
        QImage deepBase;
        bool sparseBlank = false;
        bool pressureStroke = false, pressureSize = false, pressureOpacity = false;
        double previousPressure = 1;
        // Anti-alias off: hard pixel edges, like a pencil.
        bool pencil = false;
        // A scaled-down layer not grown at the press (see beginBrushStroke), and the stroke's points so far, to paint
        // again on the layer at its displayed size if the stroke reaches past it (mac acfc5eb).
        bool deferredGrowth = false;
        QVector<QPointF> inputs;
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
        QByteArray carried;
        QImage deepWorking;
        QVector<float> deepCarried;
        // Liquify: how far each pixel's source lies from it, in tiles allocated as the stroke reaches them.
        QHash<qint64, QVector<float>> offsets;
        bool changed = false;
        // The layer grown over the canvas so pixels can be pushed past its edge (mac: the warp runs at canvas size): the
        // layer as it was, and where its own pixels sit in the grown grid.
        BrushState::Restore restore;
        bool grown = false;
        QRect sourceRect;
        QImage deepBase;
    };
    std::optional<WarpState> warp_;
    std::optional<QPointF> cloneSource_;
    std::optional<QPointF> cloneOffset_;
    QColor foreground_ = Qt::black;
    QColor background_ = Qt::white;
    bool maskPaintWhite_ = false;
    bool maskSelected_ = false;
    std::optional<QUuid> maskAlone_;
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
    // Moving an outline is lossless: what leaves the canvas comes back when the outline is moved back (mac keeps the path
    // unclipped). `base` is the mask before the first of a run of moves; valid while the document's selection is `resultKey`.
    struct SelectionMoveRun { QImage base; QPoint offset; qint64 resultKey = 0; };
    std::optional<SelectionMoveRun> selectionMoveRun_;
    int selectionFeatherAmount_ = 6;
    int selectionModifyAmount_ = 5;
    bool extrasHideGuides_ = false;
    std::optional<qint64> hiddenSelectionEdges_;
    int objectSelectionEdgeOffset_ = 0;
    bool objectSelectionSmoothEdges_ = true;
    bool objectSelectionSampleAllLayers_ = true;
    bool showsGuides_ = ToolDefaults::boolean(QStringLiteral("guides"), true);
    bool locksGuides_ = ToolDefaults::boolean(QStringLiteral("lockGuides"), false);
    bool showsRulers_ = ToolDefaults::boolean(QStringLiteral("rulers"), false);
    bool showsGrid_ = ToolDefaults::boolean(QStringLiteral("grid"), false);
    // The grid is how the app shows every project, not part of one: shared by all sessions (tabs), set in Grid Settings.
    static inline LayoutGrid layoutGrid_;
    static inline GridAppearance gridAppearance_;
    bool snapEnabled_ = ToolDefaults::boolean(QStringLiteral("snap"), true);
    bool snapToGuides_ = ToolDefaults::boolean(QStringLiteral("snapGuides"), true);
    bool snapToGrid_ = ToolDefaults::boolean(QStringLiteral("snapGrid"), true);
    bool snapToDocumentBounds_ = ToolDefaults::boolean(QStringLiteral("snapBounds"), true);
    bool snapToLayers_ = ToolDefaults::boolean(QStringLiteral("snapLayers"), true);
    std::optional<QPointF> smoothedBrushPoint(const QPointF &point);
    void continueBrushStrokeInternal(const QPointF &documentPoint);
    void updateBlurStroke(const QRect &dirty);

    double brushSmoothing_ = 0.0;
    double viewportZoom_ = 1.0;
    ViewState viewState_;
    std::optional<QPointF> brushAnchor_;
    std::optional<QPointF> brushPointer_;
    bool brushSmoothingEnabled_ = false;

    std::optional<GuideDrag> guideDrag_;
    ConfirmConversionsCallback confirmConversions_;
    quint64 sessionRevision_ = 1;
};

} // namespace compositor
