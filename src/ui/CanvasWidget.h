#pragma once

#include "core/Document.h"

#include <QWidget>
#include <QSet>
#include <QHash>
#include <QFutureWatcher>
#include <QPainterPath>

#include <memory>
#include <array>
#include <algorithm>

namespace compositor {

class CanvasWidget final : public QWidget {
    Q_OBJECT
public:
    enum class Tool { Move, Marquee, Lasso, Wand, Crop, Brush, Eraser, Healing, Clone, Blur, Gradient, Shape, Text, Eyedropper, Hand, Zoom, Other };
    explicit CanvasWidget(QWidget *parent = nullptr);

    void setDocument(std::shared_ptr<Document> document, bool invalidate = true);
    void fitCanvas();
    void actualPixels();
    void zoomIn();
    void zoomOut();
    void setZoom(double zoom) { zoomTo(zoom, rect().center()); }
    [[nodiscard]] double zoom() const { return zoom_; }
    [[nodiscard]] std::optional<QPointF> documentPointAt(const QPointF &widgetPoint) const;
    [[nodiscard]] QRectF widgetRectForDocumentRect(const QRectF &rect) const;
    [[nodiscard]] QRectF documentRectForWidgetRect(const QRectF &rect) const;
    void setTool(Tool tool);
    [[nodiscard]] Tool tool() const { return tool_; }
    void setBrushDiameter(double diameter) { brushDiameter_ = diameter; update(); }
    void setShapeCornerRadius(double radius) { shapeCornerRadius_ = radius; }
    void setCropRatio(double ratio);
    void setTransformAutoSelect(bool enabled) { transformAutoSelect_ = enabled; }
    void setLockTransformRatio(bool enabled) { lockTransformRatio_ = enabled; }
    void setShowTransformControls(bool enabled) { showTransformControls_ = enabled; update(); }
    [[nodiscard]] bool transformAutoSelects() const { return transformAutoSelect_; }
    [[nodiscard]] bool locksTransformRatio() const { return lockTransformRatio_; }
    [[nodiscard]] bool showsTransformControls() const { return showTransformControls_; }
    void setShowPixelGrid(bool enabled) { showPixelGrid_ = enabled; update(); }
    void setShowSampleRing(bool enabled) { showSampleRing_ = enabled; update(); }
    void setPaletteForeground(const QColor &color) { paletteForeground_ = color; }
    [[nodiscard]] bool showsPixelGrid() const { return showPixelGrid_; }
    void setCloneSource(const QPointF &point) { setCloneTracking(point, std::nullopt); }
    void setCloneTracking(const std::optional<QPointF> &source, const std::optional<QPointF> &offset)
    { cloneSource_ = source; cloneOffset_ = offset; update(); }
    void setCloneAligned(bool aligned) { cloneAligned_ = aligned; update(); }
    [[nodiscard]] std::optional<QPointF> cloneIndicatorPosition() const
    {
        if (cloneSource_ && cloneOffset_ && cursorDocument_ && (brushDrawing_ || cloneAligned_)) return *cursorDocument_ + *cloneOffset_;
        return cloneSource_;
    }
    void setTransformMask(bool enabled) { transformMask_ = enabled; update(); }
    void setMaskTarget(bool enabled) { maskTarget_ = enabled; }
    void setEditorInteractionBlocked(bool blocked);
    [[nodiscard]] bool editorInteractionBlocked() const { return editorInteractionBlocked_; }
    void setHueTargeting(bool enabled) { hueTargeting_ = enabled; hueTargetDragging_ = false; update(); }
    void setFloatingTransform(bool enabled) { floatingTransform_ = enabled; if (!enabled) { pixelDragging_ = false; pixelDragOffset_ = {}; } update(); }
    void setPersistentTransform(bool enabled) { persistentTransform_ = enabled; update(); }
    void setSelectedLayerIds(const QSet<QUuid> &ids) { selectedLayerIds_=ids; update(); }
    void toggleMarqueeKind() { setMarqueeElliptical(!ellipticalMarquee_); }
    void toggleLassoKind() { setPolygonalLasso(!polygonalLasso_); }
    void setMarqueeElliptical(bool enabled) { if (ellipticalMarquee_ == enabled) return; ellipticalMarquee_ = enabled; emit marqueeKindChanged(enabled); update(); }
    void setPolygonalLasso(bool enabled) { if (polygonalLasso_ == enabled) return; polygonalLasso_ = enabled; lassoPoints_.clear(); selectionDragging_ = false; emit lassoKindChanged(enabled); update(); }
    void setSelectionMode(int mode) { selectionMode_ = std::clamp(mode, 0, 2); }
    void setSelectionAntialiased(bool enabled) { selectionAntialiased_ = enabled; }
    void toggleShapeKind() { setEllipticalShape(!ellipticalShape_); }
    void setEllipticalShape(bool enabled) { if (ellipticalShape_ == enabled) return; ellipticalShape_ = enabled; emit shapeKindChanged(enabled); update(); }
    void refreshPendingGradient();
    void resolvePendingGradient(bool commit = true);
    void resolvePendingDistortion(bool apply = true);
    void resolvePendingCrop(bool apply = true);
    void invalidateDocument();
    [[nodiscard]] quint64 renderGeneration() const { return renderGeneration_; }
    void setBlendModePreview(const std::optional<QUuid> &layerId, const std::optional<BlendMode> &mode);
    void setTextEditingLayer(const std::optional<QUuid> &id) { textEditingLayer_ = id; invalidateDocument(); }

signals:
    void zoomChanged(double zoom);
    void toolChanged(Tool tool);
    void marqueeKindChanged(bool elliptical);
    void lassoKindChanged(bool polygonal);
    void shapeKindChanged(bool elliptical);
    void layerTransformStarted(bool duplicate);
    void layerTransformChanged();
    void layerTransformFinished();
    void transformApplyRequested();
    void transformCancelRequested();
    void layerDistortionRequested(const QPolygonF &corners, bool maskOnly);
    void pendingDistortionChanged(bool pending);
    void pendingCropChanged(bool pending);
    void duplicateLayerForTransformRequested();
    void rectangularSelectionRequested(const QRect &rect, int mode);
    void ellipticalSelectionRequested(const QRect &rect, int mode, bool antialiased);
    void polygonSelectionRequested(const QPolygonF &points, int mode, bool antialiased);
    void selectionMoveRequested(const QPoint &offset);
    void selectedPixelsNudgeRequested(const QPoint &offset);
    void selectedPixelsDragStarted(bool duplicate);
    void cropRequested(const QRect &rect);
    void magicWandRequested(const QPoint &point, int mode);
    void brushStrokeStarted(const QPointF &point, bool erasing);
    void brushStrokeContinued(const QPointF &point);
    void brushStrokeFinished();
    void brushStrokeCancelRequested();
    void cloneSourceRequested(const QPointF &point);
    void cloneStrokeStarted(const QPointF &point);
    void healingStrokeStarted(const QPointF &point);
    void blurStrokeStarted(const QPointF &point);
    void gradientRequested(const QPointF &start, const QPointF &end);
    void gradientCommitRequested();
    void gradientCancelRequested();
    void shapeRequested(const QRectF &rect, bool ellipse, double cornerRadius);
    void textBoxRequested(const QRectF &box, bool areaText);
    void textLayerEditRequested(const QUuid &id);
    void colorSampleRequested(const QPoint &point);
    void hueTargetStarted(const QPoint &point);
    void hueTargetDragged(double viewDelta, bool adjustsHue);
    void hueTargetFinished();
    void floatingTransformCommitRequested();
    void floatingTransformCancelRequested();
    void layerSelectionRequested(const QUuid &id);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    [[nodiscard]] QRectF canvasRect() const;
    [[nodiscard]] QRectF selectedLayersBounds() const;
    [[nodiscard]] QSet<QUuid> transformLayerIds() const;
    [[nodiscard]] bool transformsAsGroup() const;
    [[nodiscard]] bool transformControlsVisible() const { return showTransformControls_ || persistentTransform_; }
    void drawCheckerboard(QPainter &painter, const QRectF &area) const;
    void startBackgroundRender();
    void zoomTo(double value, const QPointF &anchor);

    std::shared_ptr<Document> document_;
    QImage renderedDocument_;
    bool renderDirty_ = true;
    bool renderAgain_ = false;
    quint64 renderGeneration_ = 0;
    QFutureWatcher<QImage> renderWatcher_;
    std::optional<QPair<QUuid, BlendMode>> blendModePreview_;
    std::optional<QUuid> textEditingLayer_;
    double zoom_ = 1.0;
    bool fitPending_ = true;
    QPointF panOffset_;
    QPoint lastMousePosition_;
    bool panning_ = false;
    bool movingLayer_ = false;
    bool duplicateOnMove_ = false;
    bool duplicateIssued_ = false;
    enum class TransformDrag { None, Move, Resize, Rotate, Distort };
    TransformDrag transformDrag_ = TransformDrag::None;
    QPointF moveStartDocument_;
    QPointF moveStartOrigin_;
    LayerTransform transformStart_;
    std::optional<LayerTransform> maskPlacementStart_;
    bool transformMask_ = false;
    bool maskTarget_ = false;
    bool editorInteractionBlocked_ = false;
    bool hueTargeting_ = false;
    bool hueTargetDragging_ = false;
    QPointF hueTargetStart_;
    bool transformAutoSelect_ = false;
    bool lockTransformRatio_ = true;
    bool showTransformControls_ = true;
    bool showPixelGrid_ = true;
    bool floatingTransform_ = false;
    bool pixelDragging_ = false;
    QPointF pixelDragStart_;
    QPointF pixelDragOriginalOrigin_;
    QPoint pixelDragOffset_;
    bool persistentTransform_ = false;
    QSet<QUuid> selectedLayerIds_;
    QHash<QUuid,LayerTransform> transformOriginals_;
    QHash<QUuid,LayerTransform> maskPlacementOriginals_;
    double rotateStartAngle_ = 0.0;
    std::optional<double> snapGuideX_;
    std::optional<double> snapGuideY_;
    QPoint resizeSign_;
    QPointF resizeOpposite_;
    std::array<QPointF, 4> distortionOriginal_{};
    std::array<QPointF, 4> distortionCorners_{};
    bool pendingDistortion_ = false;
    bool distortMoveAll_ = false;
    Tool tool_ = Tool::Move;
    bool selectionDragging_ = false;
    bool movingSelection_ = false;
    enum class CropDrag { None, Create, Move, Resize };
    CropDrag cropDrag_ = CropDrag::None;
    std::optional<QRectF> cropRect_;
    QRectF cropOriginal_;
    QPointF cropStart_;
    QPoint cropHandle_;
    double cropRatio_ = 0;
    QPoint selectionAnchor_;
    QPoint selectionCurrent_;
    QPolygonF lassoPoints_;
    bool polygonalLasso_ = false;
    int lassoMode_ = 0;
    bool ellipticalMarquee_ = false;
    int selectionMode_ = 0;
    bool selectionAntialiased_ = true;
    bool brushDrawing_ = false;
    std::optional<QPointF> lastBrushPoint_;
    std::optional<QUuid> lastBrushLayerId_;
    bool lastBrushMask_ = false;
    bool creationDragging_ = false;
    int gradientHandle_ = 0; // 0: new/end, 1: start, 2: end
    std::optional<QPair<QPointF, QPointF>> pendingGradient_;
    bool creationSquare_ = false;
    bool creationFromCenter_ = false;
    QPointF creationAnchor_;
    QPointF creationCurrent_;
    bool ellipticalShape_ = false;
    double shapeCornerRadius_ = 0;
    double brushDiameter_ = 40;
    std::optional<QPointF> cursorDocument_;
    std::optional<QPointF> cloneSource_;
    std::optional<QPointF> cloneOffset_;
    bool cloneAligned_ = true;
    bool showSampleRing_ = true;
    bool samplingColor_ = false;
    QColor paletteForeground_ = Qt::black;
    QColor sampleOriginal_ = Qt::black;
    QColor sampleCurrent_ = Qt::black;
    qint64 selectionOutlineKey_ = 0;
    QPainterPath selectionOutline_;
};

} // namespace compositor
