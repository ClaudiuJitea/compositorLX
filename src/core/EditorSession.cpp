#include "core/EditorSession.h"
#include "core/SelectionOps.h"
#include "io/ProjectReader.h"
#include "io/ProjectWriter.h"
#include "io/PSDReader.h"
#include "io/SvgImporter.h"
#include "rendering/TextLayout.h"

#include "rendering/LayerRenderer.h"
#include "rendering/RasterOperations.h"
#include "rendering/SubjectRemoval.h"
extern "C" {
#include "BrushPixels.h"
}

#include <QPainter>
#include <QPainterPath>
#include <QFontMetrics>
#include <QFileInfo>
#include <QJsonArray>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>
#include <deque>

namespace compositor {

quint64 EditorSession::advanceRevision() noexcept
{
    ++sessionRevision_;
    if (document_) {
        document_->generation = sessionRevision_;
    }
    return sessionRevision_;
}

void EditorSession::setDocument(std::shared_ptr<Document> document, bool markSaved)
{
    floating_.reset();
    document_ = std::move(document);
    selectedLayerIds_.clear();
    advanceRevision();
    if (document_) {
        if (document_->activeLayerId) selectedLayerIds_.insert(*document_->activeLayerId);
    }
    history_.reset();
    if (markSaved) history_.markSaved(); else history_.markModified();
}

int EditorSession::indexOf(const QUuid &id) const
{
    if (!document_) return -1;
    for (int i = 0; i < document_->layers.size(); ++i) if (document_->layers.at(i).id == id) return i;
    return -1;
}

Layer *EditorSession::activeLayer()
{
    if (!document_ || !document_->activeLayerId) return nullptr;
    const int index = indexOf(*document_->activeLayerId);
    return index < 0 ? nullptr : &document_->layers[index];
}

const Layer *EditorSession::activeLayer() const
{
    if (!document_ || !document_->activeLayerId) return nullptr;
    const int index = indexOf(*document_->activeLayerId);
    return index < 0 ? nullptr : &document_->layers.at(index);
}

void EditorSession::createDocument(int width, int height, bool emptyLayer)
{
    if (width < 1 || width > 30000 || height < 1 || height > 30000) return;
    beginEdit(QStringLiteral("New Canvas"));
    document_ = std::make_shared<Document>();
    document_->formatVersion = 9;
    document_->id = QUuid::createUuid();
    document_->generation = 1;
    document_->canvasSize = QSize(width, height);
    if (emptyLayer) {
        Layer layer;
        layer.id = QUuid::createUuid();
        layer.name = QStringLiteral("Layer 1");
        layer.transform.size = document_->canvasSize;
        document_->layers.push_back(layer);
        document_->activeLayerId = layer.id;
        selectedLayerIds_ = {layer.id};
    } else {
        selectedLayerIds_.clear();
    }
    endEdit();
}

bool EditorSession::openProject(const QString &path)
{
    ProjectWriter::recoverInterruptedPackage(path);
    auto loaded = std::make_shared<Document>(ProjectReader::load(path));
    setDocument(std::move(loaded));
    return true;
}

bool EditorSession::insertImage(const QImage &image, const QString &name, const std::optional<QPointF> &center)
{
    if (image.isNull() || image.width() < 1 || image.height() < 1 || image.width() > 30000 || image.height() > 30000) return false;
    qint64 used = 0;
    if (document_) {
        for (const Layer &layer : document_->layers) {
            used += qint64(layer.image.width()) * layer.image.height();
            used += qint64(layer.mask.width()) * layer.mask.height();
        }
    }
    if (qint64(image.width()) * image.height() > 100000000LL - used) return false;
    beginEdit(QStringLiteral("Import Image"));
    if (!document_) {
        document_ = std::make_shared<Document>();
        document_->formatVersion = 9;
        document_->id = QUuid::createUuid();
        document_->generation = 1;
        document_->canvasSize = image.size();
    }
    Layer layer;
    layer.id = QUuid::createUuid();
    layer.name = name.trimmed().isEmpty() ? nextName(QStringLiteral("Layer")) : name.trimmed();
    layer.image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    layer.transform.size = image.size();
    const QPointF target = center.value_or(QPointF(document_->canvasSize.width() / 2.0, document_->canvasSize.height() / 2.0));
    layer.transform.origin = QPointF(std::floor(target.x() - image.width() / 2.0), std::floor(target.y() - image.height() / 2.0));
    if (const Layer *active = activeLayer()) layer.parentId = active->group ? document_->activeLayerId : active->parentId;
    document_->layers.push_back(layer);
    selectLayer(layer.id);
    endEdit();
    return true;
}

bool EditorSession::insertSvg(const QString &path, const std::optional<QPointF> &center, QString *error)
{
    qint64 usedPixels = 0;
    if (document_) {
        for (const Layer &l : document_->layers) {
            if (!l.image.isNull()) usedPixels += (qint64(l.image.width()) * l.image.height());
            if (!l.mask.isNull()) usedPixels += (qint64(l.mask.width()) * l.mask.height());
        }
    }
    const qint64 remaining = std::max(0LL, 100000000LL - usedPixels);
    const std::optional<QSize> fitting = document_ ? std::make_optional(document_->canvasSize) : std::nullopt;
    const QImage image = SvgImporter::read(path, fitting, remaining, error);
    if (image.isNull()) return false;
    return insertImage(image, QFileInfo(path).completeBaseName(), center);
}

bool EditorSession::insertPhotoshop(const PSDImportResult &imported, const QString &named,
                                    const std::optional<QPointF> &center, QString *error)
{
    if (confirmConversions_ && !imported.conversions.isEmpty()) {
        if (!confirmConversions_(imported.conversions)) {
            if (error) *error = QStringLiteral("Import cancelled by user.");
            return false;
        }
    }

    const auto &incoming = imported.document.layers;
    const bool wrapping = (document_ != nullptr);
    const int added = incoming.size() + (wrapping ? 1 : 0);
    const int currentCount = document_ ? document_->layers.size() : 0;
    if (currentCount + added > 10000) {
        if (error) *error = QStringLiteral("The document exceeds the 10,000 layer limit.");
        return false;
    }

    qint64 usedPixels = 0;
    if (document_) {
        for (const Layer &l : document_->layers) {
            if (!l.image.isNull()) usedPixels += (qint64(l.image.width()) * l.image.height());
            if (!l.mask.isNull()) usedPixels += (qint64(l.mask.width()) * l.mask.height());
        }
    }
    for (const Layer &l : incoming) {
        if (!l.image.isNull()) usedPixels += (qint64(l.image.width()) * l.image.height());
        if (!l.mask.isNull()) usedPixels += (qint64(l.mask.width()) * l.mask.height());
    }
    if (usedPixels > 100000000LL) {
        if (error) *error = QStringLiteral("The image is too large to import.");
        return false;
    }

    beginEdit(QStringLiteral("Import Photoshop File"));

    if (!document_) {
        document_ = std::make_shared<Document>();
        document_->formatVersion = 9;
        document_->id = QUuid::createUuid();
        document_->generation = 1;
        document_->canvasSize = imported.document.canvasSize;
        document_->resolution = imported.document.resolution;
        document_->layers = incoming;
        QUuid activeId;
        for (int i = document_->layers.size() - 1; i >= 0; --i) {
            if (!document_->layers[i].parentId) {
                activeId = document_->layers[i].id;
                break;
            }
        }
        if (activeId.isNull() && !document_->layers.isEmpty()) {
            activeId = document_->layers.last().id;
        }
        document_->activeLayerId = activeId;
        selectedLayerIds_ = activeId.isNull() ? QSet<QUuid>{} : QSet<QUuid>{activeId};
        endEdit();
        return true;
    }

    Layer group;
    group.id = QUuid::createUuid();
    group.name = named.trimmed().isEmpty() ? nextName(QStringLiteral("Folder")) : named.trimmed();
    group.group = true;
    group.transform.origin = QPointF(0, 0);
    group.transform.size = document_->canvasSize;

    const Layer *active = activeLayer();
    group.parentId = (active && active->group) ? std::optional<QUuid>(active->id) : (active ? active->parentId : std::nullopt);

    QVector<Layer> adjustedIncoming = incoming;
    if (center.has_value()) {
        QRectF box;
        bool hasBox = false;
        for (const Layer &l : adjustedIncoming) {
            if (!l.group && l.transform.size.width() > 0 && l.transform.size.height() > 0) {
                const QRectF r(l.transform.origin, l.transform.size);
                box = hasBox ? box.united(r) : r;
                hasBox = true;
            }
        }
        if (hasBox && !box.isNull() && !box.isEmpty() && std::isfinite(box.x()) && std::isfinite(box.y())) {
            const double dx = center->x() - box.center().x();
            const double dy = center->y() - box.center().y();
            for (Layer &l : adjustedIncoming) {
                l.transform.origin.rx() += dx;
                l.transform.origin.ry() += dy;
            }
        }
    }

    for (Layer &l : adjustedIncoming) {
        if (!l.parentId) {
            l.parentId = group.id;
        }
    }

    document_->layers.push_back(std::move(group));
    for (Layer &l : adjustedIncoming) {
        document_->layers.push_back(std::move(l));
    }

    document_->activeLayerId = group.id;
    selectedLayerIds_ = {group.id};

    endEdit();
    return true;
}

bool EditorSession::insertPixelLayer(const QImage &image, const QPointF &origin, const QString &name,
                                     const QString &historyName, bool dropsSelection)
{
    if (!document_ || image.isNull() || image.width() > 30000 || image.height() > 30000
        || qint64(image.width()) * image.height() > 100000000LL) return false;
    Layer layer; layer.id = QUuid::createUuid(); layer.name = name.trimmed().isEmpty() ? nextName(QStringLiteral("Layer")) : name.trimmed();
    layer.image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied); layer.transform.origin = origin; layer.transform.size = image.size();
    const Layer *active = activeLayer(); layer.parentId = active && active->group ? document_->activeLayerId : active ? active->parentId : std::nullopt;
    const int index = active ? indexOf(active->id) + 1 : document_->layers.size();
    beginEdit(historyName); document_->layers.insert(index, layer); if (dropsSelection) document_->selection.reset(); selectLayer(layer.id); endEdit(); return true;
}

std::optional<QPair<QImage, QPoint>> EditorSession::copiedPixels(bool merged) const
{
    if (!document_) return std::nullopt;
    QImage canvas;
    if (merged) canvas = LayerRenderer::flattened(*document_);
    else {
        const Layer *layer = activeLayer(); if (!layer || layer->group) return std::nullopt;
        Layer copy = *layer; copy.parentId.reset(); copy.maskSourceId.reset(); copy.visible = true; copy.opacity = 1; copy.blendMode = BlendMode::Normal;
        if (maskSelected_) {
            if (copy.mask.isNull()) return std::nullopt;
            copy.image = copy.mask.convertToFormat(QImage::Format_RGBA8888); copy.mask = {};
        } else copy.mask = {};
        Document one = *document_; one.layers = {copy}; one.selection.reset(); canvas = LayerRenderer::flattened(one);
    }
    QRect region(QPoint(), document_->canvasSize);
    if (document_->selection) {
        const QImage selection = document_->selection->convertToFormat(QImage::Format_Grayscale8);
        int left = selection.width(), top = selection.height(), right = -1, bottom = -1;
        for (int y = 0; y < selection.height(); ++y) { const uchar *row = selection.constScanLine(y); for (int x = 0; x < selection.width(); ++x) if (row[x]) {
            left = std::min(left, x); top = std::min(top, y); right = std::max(right, x); bottom = std::max(bottom, y);
        } }
        if (right < left) return std::nullopt;
        region = QRect(QPoint(left, top), QPoint(right, bottom));
        QImage coverage(selection.size(), QImage::Format_RGBA8888_Premultiplied); coverage.fill(Qt::transparent);
        for (int y = 0; y < selection.height(); ++y) { const uchar *source = selection.constScanLine(y); uchar *out = coverage.scanLine(y); for (int x = 0; x < selection.width(); ++x) {
            out[x * 4] = out[x * 4 + 1] = out[x * 4 + 2] = out[x * 4 + 3] = source[x];
        } }
        QPainter clip(&canvas); clip.setCompositionMode(QPainter::CompositionMode_DestinationIn); clip.drawImage(0, 0, coverage); clip.end();
    }
    return QPair<QImage, QPoint>(canvas.copy(region), region.topLeft());
}

static bool maskHasCoverage(const QImage &source);

bool EditorSession::replaceSelection(const QImage &mask, const QString &historyName)
{
    if (!document_) return false;
    if (mask.isNull() || !maskHasCoverage(mask)) { deselect(); return false; }
    beginEdit(historyName);
    document_->selection = mask.convertToFormat(QImage::Format_Grayscale8);
    endEdit();
    return true;
}

void EditorSession::previewSelection(const std::optional<QImage> &mask)
{
    if (!document_) return;
    document_->selection = mask;
}

void EditorSession::selectAll()
{
    if (!document_) return;
    QImage mask(document_->canvasSize, QImage::Format_Grayscale8); mask.fill(255);
    if (document_->selection && *document_->selection == mask) return;
    beginEdit(QStringLiteral("Select All")); document_->selection = mask; endEdit();
}

void EditorSession::deselect()
{
    if (!document_ || !document_->selection) return;
    beginEdit(QStringLiteral("Deselect")); document_->selection.reset(); endEdit();
}

void EditorSession::invertSelection()
{
    if (!document_ || !document_->selection) return;
    beginEdit(QStringLiteral("Inverse"));
    QImage mask = document_->selection->convertToFormat(QImage::Format_Grayscale8);
    for (int y = 0; y < mask.height(); ++y) {
        uchar *row = mask.scanLine(y);
        for (int x = 0; x < mask.width(); ++x) row[x] = uchar(255 - row[x]);
    }
    // The inverse of everything is no selection at all, as in Photoshop (mac 133c34a) - not an invisible empty one
    // that quietly stops every brush.
    const bool empty = std::all_of(mask.constBits(), mask.constBits() + mask.sizeInBytes(), [](uchar v) { return v == 0; });
    if (empty) document_->selection.reset(); else document_->selection = mask;
    endEdit();
}

static bool maskHasCoverage(const QImage &source)
{
    const QImage mask = source.convertToFormat(QImage::Format_Grayscale8);
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *row = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x) if (row[x]) return true;
    }
    return false;
}

static bool resizeSelectionMask(std::shared_ptr<Document> &document, int amount, bool expand, DocumentHistory &history)
{
    if (!document || !document->selection || amount < 1 || amount > 500 || !maskHasCoverage(*document->selection)) return false;
    const QImage result = expand ? SelectionOps::dilated(*document->selection, amount) : SelectionOps::eroded(*document->selection, amount);
    if (result == *document->selection) return false;
    history.begin(expand ? QStringLiteral("Expand Selection") : QStringLiteral("Contract Selection"), document);
    document->selection = result;
    history.end(document);
    return true;
}

bool EditorSession::expandSelection(int amount) { return resizeSelectionMask(document_, amount, true, history_); }
bool EditorSession::contractSelection(int amount) { return resizeSelectionMask(document_, amount, false, history_); }

bool EditorSession::featherSelection(int amount)
{
    if (!document_ || !document_->selection || amount < 1 || amount > 250 || !maskHasCoverage(*document_->selection))
        return false;
    const QImage result = RasterOperations::featherMask(*document_->selection, amount);
    if (result == *document_->selection) return false;
    beginEdit(QStringLiteral("Feather Selection"));
    document_->selection = result;
    endEdit();
    return true;
}

bool EditorSession::moveSelection(const QPoint &offset)
{
    if (!document_ || !document_->selection || offset.isNull()) return false;
    // A run of moves is applied to the mask the run started from, so an outline taken off the canvas and back is whole again.
    QImage base;
    QPoint total = offset;
    if (selectionMoveRun_ && selectionMoveRun_->resultKey == document_->selection->cacheKey()) {
        base = selectionMoveRun_->base;
        total = selectionMoveRun_->offset + offset;
    } else {
        base = document_->selection->convertToFormat(QImage::Format_Grayscale8);
    }
    if (!SelectionOps::hasCoverage(base)) return false;
    const QImage moved = SelectionOps::shifted(base, total);
    beginEdit(QStringLiteral("Move Selection")); document_->selection = moved; endEdit();
    selectionMoveRun_ = SelectionMoveRun{base, total, document_->selection->cacheKey()};
    return true;
}

bool EditorSession::nudgeSelectedPixels(const QPoint &offset)
{
    if (offset.isNull() || !beginSelectionTransform(false, QStringLiteral("Move Pixels"))) return false;
    Layer *floating = activeLayer();
    if (!floating) { cancelSelectionTransform(); return false; }
    floating->transform.origin += offset;
    return commitSelectionTransform();
}

static QImage combineSelection(const std::optional<QImage> &current, const QImage &shape, SelectionMode mode)
{
    if (mode == SelectionMode::Replace || !current) return shape;
    QImage result = current->convertToFormat(QImage::Format_Grayscale8);
    const QImage input = shape.convertToFormat(QImage::Format_Grayscale8);
    for (int y = 0; y < result.height(); ++y) {
        uchar *out = result.scanLine(y); const uchar *in = input.constScanLine(y);
        for (int x = 0; x < result.width(); ++x)
            out[x] = mode == SelectionMode::Add ? std::max(out[x], in[x]) : uchar((int(out[x]) * (255 - int(in[x])) + 127) / 255);
    }
    return result;
}

static QImage clippedPixels(const QImage &originalImage, const QImage &editedImage,
                            const Layer &layer, const std::optional<QImage> &selection)
{
    if (!selection) return editedImage;
    QImage original = originalImage.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const QImage edited = editedImage.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const QImage mask = selection->convertToFormat(QImage::Format_Grayscale8);
    QTransform toDocument;
    toDocument.translate(layer.transform.center().x(), layer.transform.center().y());
    toDocument.rotate(layer.transform.rotation);
    toDocument.scale(layer.transform.flipX ? -1 : 1, layer.transform.flipY ? -1 : 1);
    const double sx = layer.transform.size.width() / std::max(1, original.width());
    const double sy = layer.transform.size.height() / std::max(1, original.height());
    for (int y = 0; y < original.height(); ++y) {
        uchar *out = original.scanLine(y); const uchar *change = edited.constScanLine(y);
        for (int x = 0; x < original.width(); ++x) {
            const QPointF local((x + .5) * sx - layer.transform.size.width() / 2,
                                (y + .5) * sy - layer.transform.size.height() / 2);
            const QPointF mapped = toDocument.map(local);
            const QPoint point(qFloor(mapped.x()), qFloor(mapped.y()));
            const int coverage = QRect(QPoint(), mask.size()).contains(point) ? mask.constScanLine(point.y())[point.x()] : 0;
            for (int channel = 0; channel < 4; ++channel) {
                const int offset = x * 4 + channel;
                out[offset] = uchar((int(out[offset]) * (255 - coverage) + int(change[offset]) * coverage + 127) / 255);
            }
        }
    }
    return original;
}

// A drawn outline combined with the selection, as one undo step. A shape that encloses nothing deselects in New mode, as
// a lasso click does in Photoshop; subtracting from no selection selects nothing new, so nothing changes.
static bool commitShape(EditorSession &session, std::shared_ptr<Document> &document, const QImage &shape, SelectionMode mode, const QString &name)
{
    if (!document || (mode == SelectionMode::Subtract && !document->selection)) return false;
    const QImage result = combineSelection(document->selection, shape, mode);
    if (document->selection && *document->selection == result) return false;
    session.beginEdit(name); document->selection = result; session.endEdit();
    return true;
}

static bool encloses(const QRectF &bounds) { return bounds.width() > 0 && bounds.height() > 0; }

bool EditorSession::setRectangularSelection(const QRect &rect, SelectionMode mode)
{
    if (!document_) return false;
    if (rect.width() <= 0 || rect.height() <= 0) {
        if (mode != SelectionMode::Replace || !document_->selection) return false;
        deselect(); return true;
    }
    QImage shape(document_->canvasSize, QImage::Format_Grayscale8); shape.fill(0);
    QPainter painter(&shape); painter.fillRect(rect.intersected(QRect(QPoint(), document_->canvasSize)), Qt::white); painter.end();
    return commitShape(*this, document_, shape, mode, QStringLiteral("Rectangular Marquee"));
}

static bool paintedSelection(EditorSession &session, std::shared_ptr<Document> &document, const QPainterPath &path,
                             SelectionMode mode, const QString &name, bool antialiased)
{
    if (!document) return false;
    if (!encloses(path.boundingRect())) {
        if (mode != SelectionMode::Replace || !document->selection) return false;
        session.deselect(); return true;
    }
    QImage shape(document->canvasSize, QImage::Format_Grayscale8); shape.fill(0);
    QPainter painter(&shape); painter.setRenderHint(QPainter::Antialiasing, antialiased); painter.fillPath(path, Qt::white); painter.end();
    return commitShape(session, document, shape, mode, name);
}

bool EditorSession::setEllipticalSelection(const QRect &rect, SelectionMode mode, bool antialiased)
{
    QPainterPath path; path.addEllipse(QRectF(rect));
    return paintedSelection(*this, document_, path, mode, QStringLiteral("Elliptical Marquee"), antialiased);
}

bool EditorSession::setPolygonSelection(const QPolygonF &points, SelectionMode mode, bool antialiased, const QString &name)
{
    QPainterPath path;
    if (points.size() >= 3) { path.addPolygon(points); path.closeSubpath(); }
    return paintedSelection(*this, document_, path, mode, name, antialiased);
}

// What selection-from-image tools read at canvas size: every visible layer as shown, or just the active layer's own
// pixels without its mask, opacity, blend mode or effects, as Cmd-click selection reads them. A folder or blank layer
// reads as transparent (mac Magic Wand selectionSample).
static QImage selectionSampleImage(const Document &document, const Layer *active, bool sampleAllLayers)
{
    if (sampleAllLayers) return LayerRenderer::flattened(document);
    Document one = document; one.selection.reset(); one.layers.clear();
    if (active && !active->group && !active->image.isNull() && active->adjustment.isEmpty()) {
        Layer copy = *active;
        copy.parentId.reset(); copy.visible = true; copy.opacity = 1; copy.blendMode = BlendMode::Normal;
        copy.mask = {}; copy.maskPlacement.reset(); copy.maskSourceId.reset(); copy.effects.reset();
        one.layers = {copy};
    }
    return LayerRenderer::flattened(one);
}

bool EditorSession::magicWand(const QPoint &documentPoint, int tolerance, int sampleRadius,
                              bool contiguous, bool sampleAllLayers, SelectionMode mode, bool antialiased)
{
    Q_UNUSED(antialiased); // The traced outline lies on exact pixel edges, so smoothing changes nothing, as on the Mac.
    if (!document_ || !QRect(QPoint(), document_->canvasSize).contains(documentPoint)) return false;
    const QImage sample = selectionSampleImage(*document_, activeLayer(), sampleAllLayers);
    const auto shape = RasterOperations::magicWandMask(sample, documentPoint, tolerance, sampleRadius, contiguous);
    if (!shape) return false;
    // Nothing matched: New clears the selection, as a lasso click enclosing nothing does.
    if (!SelectionOps::hasCoverage(*shape)) {
        if (mode != SelectionMode::Replace || !document_->selection) return false;
        deselect(); return true;
    }
    return commitShape(*this, document_, *shape, mode, QStringLiteral("Magic Wand"));
}

EditorSession::SelectionSnapshot EditorSession::createSelectionSnapshot(bool sampleAllLayers, QString *error) const
{
    SelectionSnapshot snapshot;
    if (!document_) {
        if (error) *error = QObject::tr("No document is open.");
        snapshot.error = error ? *error : QObject::tr("No document is open.");
        return snapshot;
    }
    if (qint64(document_->canvasSize.width()) * document_->canvasSize.height() > 100000000LL ||
        document_->canvasSize.width() > 30000 || document_->canvasSize.height() > 30000) {
        if (error) *error = QObject::tr("Image dimensions exceed memory limits.");
        snapshot.error = error ? *error : QObject::tr("Image dimensions exceed memory limits.");
        return snapshot;
    }

    snapshot.documentId = document_->id;
    snapshot.documentGeneration = sessionRevision_;
    snapshot.canvasSize = document_->canvasSize;

    if (sampleAllLayers) {
        snapshot.sampleImage = LayerRenderer::flattened(*document_);
    } else if (const Layer *layer = activeLayer(); layer && !layer->group) {
        if (layer->image.isNull()) {
            if (error) *error = QObject::tr("The selected layer has no pixels.");
            snapshot.error = error ? *error : QObject::tr("The selected layer has no pixels.");
            return snapshot;
        }
        snapshot.sampleImage = selectionSampleImage(*document_, layer, false);
    } else {
        if (error) *error = QObject::tr("No active layer selected.");
        snapshot.error = error ? *error : QObject::tr("No active layer selected.");
        return snapshot;
    }

    snapshot.sampleImage.detach();
    snapshot.valid = true;
    return snapshot;
}

EditorSession::SelectionComputationResult EditorSession::computeSubjectSelection(
    const SelectionSnapshot &snapshot,
    quint64 requestId,
    std::atomic<bool> *cancelled)
{
    SelectionComputationResult result;
    result.documentId = snapshot.documentId;
    result.documentGeneration = snapshot.documentGeneration;
    result.requestId = requestId;

    if (!snapshot.valid || snapshot.sampleImage.isNull()) {
        result.error = snapshot.error.isEmpty() ? QObject::tr("Invalid selection snapshot.") : snapshot.error;
        return result;
    }
    if (cancelled && cancelled->load()) return result;

    QString error;
    const QImage raw = SubjectRemoval::rawMask(snapshot.sampleImage, &error, cancelled);
    if (cancelled && cancelled->load()) return result;
    if (raw.isNull() || !maskHasCoverage(raw)) {
        result.error = error.isEmpty() ? QObject::tr("No foreground subject was detected in this layer.") : error;
        return result;
    }

    const int width = raw.width(), height = raw.height();
    QImage binary(width, height, QImage::Format_Grayscale8);
    for (int y = 0; y < height; ++y) {
        const uchar *in = raw.constScanLine(y);
        uchar *out = binary.scanLine(y);
        for (int x = 0; x < width; ++x)
            out[x] = in[x] >= 128 ? 255 : 0;
    }

    if (!maskHasCoverage(binary)) {
        result.error = QObject::tr("No foreground subject was detected in this layer.");
        return result;
    }

    if (cancelled && cancelled->load()) return result;
    result.mask = SubjectRemoval::smoothBinaryMask(binary);
    if (cancelled && cancelled->load()) {
        result.mask = QImage();
        return result;
    }

    result.success = true;
    return result;
}

EditorSession::SelectionComputationResult EditorSession::computeObjectSelection(
    const SelectionSnapshot &snapshot,
    const QPoint &documentPoint,
    int edgeOffset,
    bool smoothEdges,
    quint64 requestId,
    std::atomic<bool> *cancelled)
{
    SelectionComputationResult result;
    result.documentId = snapshot.documentId;
    result.documentGeneration = snapshot.documentGeneration;
    result.requestId = requestId;

    if (!snapshot.valid || snapshot.sampleImage.isNull()) {
        result.error = snapshot.error.isEmpty() ? QObject::tr("Invalid selection snapshot.") : snapshot.error;
        return result;
    }
    if (!QRect(QPoint(), snapshot.canvasSize).contains(documentPoint)) {
        return result;
    }
    if (cancelled && cancelled->load()) return result;

    QString error;
    const QImage raw = SubjectRemoval::rawMask(snapshot.sampleImage, &error, cancelled);
    if (cancelled && cancelled->load()) return result;
    if (raw.isNull()) {
        result.error = error;
        return result;
    }

    QImage maskToTrace = raw;
    if (!snapshot.sampleImage.isNull()) {
        SubjectRemovalSettings refSettings;
        refSettings.advanced = true;
        refSettings.refineEdges = 2.0;
        maskToTrace = SubjectRemoval::refined(raw, snapshot.sampleImage, refSettings);
        if (maskToTrace.isNull()) maskToTrace = raw;
    }

    if (cancelled && cancelled->load()) return result;
    const QImage instance = SubjectRemoval::connectedInstance(maskToTrace, documentPoint, cancelled);
    if (cancelled && cancelled->load()) return result;
    if (instance.isNull() || !maskHasCoverage(instance)) {
        return result;
    }

    QImage adjusted = instance;
    if (edgeOffset != 0) {
        adjusted = SubjectRemoval::adjustEdgeOffset(adjusted, edgeOffset);
        if (cancelled && cancelled->load()) return result;
    }

    const QImage finalMask = smoothEdges ? SubjectRemoval::smoothBinaryMask(adjusted) : adjusted;
    if (cancelled && cancelled->load()) return result;

    result.mask = finalMask;
    result.success = true;
    return result;
}

bool EditorSession::applySelectionResult(
    const SelectionComputationResult &result,
    SelectionMode mode,
    const QString &historyName,
    QString *error)
{
    if (!document_) {
        if (error) *error = QObject::tr("No document is open.");
        return false;
    }
    if (document_->id != result.documentId) {
        if (error) *error = QObject::tr("Document identity mismatch.");
        return false;
    }
    if (sessionRevision_ != result.documentGeneration || document_->generation != result.documentGeneration) {
        if (error) *error = QObject::tr("Document generation mismatch.");
        return false;
    }
    if (mode == SelectionMode::Subtract && !document_->selection) {
        return false;
    }
    if (!result.success || result.mask.isNull() || !maskHasCoverage(result.mask)) {
        if (mode == SelectionMode::Replace && document_->selection.has_value()) {
            deselect();
        }
        if (error && !result.error.isEmpty()) *error = result.error;
        return false;
    }

    const QImage combined = combineSelection(document_->selection, result.mask, mode);
    beginEdit(historyName.isEmpty() ? QStringLiteral("Selection") : historyName);
    document_->selection = combined;
    endEdit();
    return true;
}

bool EditorSession::selectSubject(bool sampleAllLayers, SelectionMode mode, QString *error, std::atomic<bool> *cancelled)
{
    if (cancelled && cancelled->load()) return false;
    if (mode == SelectionMode::Subtract && (!document_ || !document_->selection)) return false;

    auto snapshot = createSelectionSnapshot(sampleAllLayers, error);
    if (!snapshot.valid) return false;
    if (cancelled && cancelled->load()) return false;

    auto compResult = computeSubjectSelection(snapshot, 0, cancelled);
    if (cancelled && cancelled->load()) return false;

    return applySelectionResult(compResult, mode, QStringLiteral("Select Subject"), error);
}

bool EditorSession::selectObject(const QPoint &documentPoint, int edgeOffset, bool smoothEdges,
                                bool sampleAllLayers, SelectionMode mode, QString *error,
                                std::atomic<bool> *cancelled)
{
    if (cancelled && cancelled->load()) return false;
    if (mode == SelectionMode::Subtract && (!document_ || !document_->selection)) return false;

    auto snapshot = createSelectionSnapshot(sampleAllLayers, error);
    if (!snapshot.valid) return false;
    if (cancelled && cancelled->load()) return false;

    auto compResult = computeObjectSelection(snapshot, documentPoint, edgeOffset, smoothEdges, 0, cancelled);
    if (cancelled && cancelled->load()) return false;

    return applySelectionResult(compResult, mode, QStringLiteral("Object Selection"), error);
}

static void rasterizeLayer(Layer &layer)
{
    layer.shape = {};
    layer.shapeStyle = std::nullopt;
    layer.text = std::nullopt;
}

bool EditorSession::invertActiveLayerPixels()
{
    const Layer *layer = activeLayer();
    if (!layer || layer->group || layer->image.isNull()) return false;
    const int index = indexOf(layer->id);
    const QImage result = clippedPixels(layer->image, RasterOperations::inverted(layer->image), *layer, document_->selection);
    if (result == layer->image) return false;
    beginEdit(QStringLiteral("Invert"));
    document_->layers[index].image = result;
    rasterizeLayer(document_->layers[index]);
    endEdit(); return true;
}

static QImage expandedMaskImage(const Layer &layer)
{
    if (layer.mask.size() != QSize(1, 1)) return layer.mask.convertToFormat(QImage::Format_Grayscale8);
    const QSize size = layer.image.isNull() ? layer.transform.size.toSize() : layer.image.size();
    QImage result(size.expandedTo(QSize(1, 1)), QImage::Format_Grayscale8);
    result.fill(qGray(layer.mask.pixel(0, 0)));
    return result;
}

static QImage clippedGrayscale(const QImage &originalImage, int value, const Layer &layer,
                               const std::optional<QImage> &selection)
{
    QImage result = originalImage.convertToFormat(QImage::Format_Grayscale8);
    if (!selection) { result.fill(value); return result; }
    const QImage mask = selection->convertToFormat(QImage::Format_Grayscale8);
    QTransform toDocument;
    toDocument.translate(layer.transform.center().x(), layer.transform.center().y());
    toDocument.rotate(layer.transform.rotation);
    toDocument.scale((layer.transform.flipX ? -1 : 1) * layer.transform.size.width() / result.width(),
                     (layer.transform.flipY ? -1 : 1) * layer.transform.size.height() / result.height());
    toDocument.translate(-result.width() / 2.0, -result.height() / 2.0);
    for (int y = 0; y < result.height(); ++y) {
        uchar *out = result.scanLine(y);
        for (int x = 0; x < result.width(); ++x) {
            const QPointF mapped = toDocument.map(QPointF(x + .5, y + .5));
            const QPoint point(qFloor(mapped.x()), qFloor(mapped.y()));
            const int coverage = QRect(QPoint(), mask.size()).contains(point) ? mask.constScanLine(point.y())[point.x()] : 0;
            out[x] = uchar((int(out[x]) * (255 - coverage) + value * coverage + 127) / 255);
        }
    }
    return result;
}

bool EditorSession::fillSelection(const QColor &color)
{
    Layer *layer = activeLayer();
    if (!document_ || !layer || layer->group || !color.isValid()) return false;
    // A text layer that is still text takes the color as its own, rather than being painted over: the letters change
    // color and stay editable (mac SelectionEdits.swift fillSelection).
    if (!maskSelected_ && !document_->selection && layer->text && recolorText(layer->id, color)) return true;
    const int index = indexOf(layer->id);
    if (maskSelected_) {
        if (layer->mask.isNull() || !layer->maskEnabled) return false;
        const QImage original = expandedMaskImage(*layer);
        const QImage result = clippedGrayscale(original, qGray(color.rgb()), *layer, document_->selection);
        if (result == original) return false;
        beginEdit(QStringLiteral("Fill Mask")); document_->layers[index].mask = result; endEdit(); return true;
    }
    QImage original = layer->image;
    if (original.isNull()) {
        const QSize size = layer->transform.size.toSize().expandedTo(QSize(1, 1));
        if (qint64(size.width()) * size.height() > 100000000LL) return false;
        original = QImage(size, QImage::Format_RGBA8888_Premultiplied); original.fill(Qt::transparent);
    }
    QImage painted(original.size(), QImage::Format_RGBA8888_Premultiplied); painted.fill(color);
    const QImage result = clippedPixels(original, painted, *layer, document_->selection);
    if (result == original) return false;
    beginEdit(QStringLiteral("Fill")); document_->layers[index].image = result; rasterizeLayer(document_->layers[index]); endEdit(); return true;
}

bool EditorSession::clearSelectedPixels()
{
    Layer *layer = activeLayer();
    if (!document_ || !document_->selection || !layer || layer->group) return false;
    if (maskSelected_) return fillSelection(Qt::white);
    if (layer->image.isNull()) return false;
    const int index = indexOf(layer->id);
    QImage cleared(layer->image.size(), QImage::Format_RGBA8888_Premultiplied); cleared.fill(Qt::transparent);
    const QImage result = clippedPixels(layer->image, cleared, *layer, document_->selection);
    if (result == layer->image) return false;
    beginEdit(QStringLiteral("Clear")); document_->layers[index].image = result; rasterizeLayer(document_->layers[index]); endEdit(); return true;
}

static QTransform pixelToDocument(const LayerTransform &placement, const QSize &pixels)
{
    QTransform transform;
    transform.translate(placement.center().x(), placement.center().y());
    transform.rotate(placement.rotation);
    transform.scale((placement.flipX ? -1.0 : 1.0) * placement.size.width() / pixels.width(),
                    (placement.flipY ? -1.0 : 1.0) * placement.size.height() / pixels.height());
    transform.translate(-pixels.width() / 2.0, -pixels.height() / 2.0);
    return transform;
}

bool EditorSession::beginSelectionTransform(bool duplicate, const QString &historyName)
{
    Layer *source = activeLayer();
    if (!document_ || floating_ || maskSelected_ || !document_->selection || !source || source->group || source->image.isNull()) return false;
    const auto copied = copiedPixels(false);
    if (!copied || copied->first.isNull()) return false;
    const int sourceIndex = indexOf(source->id);
    const Document before = *document_;
    const std::optional<QUuid> beforeActive = document_->activeLayerId;
    const QUuid sourceId = source->id;
    beginEdit(!historyName.isEmpty() ? historyName : duplicate ? QStringLiteral("Duplicate Pixels") : QStringLiteral("Transform Selection"));
    QImage cleared(source->image.size(), QImage::Format_RGBA8888_Premultiplied); cleared.fill(Qt::transparent);
    document_->layers[sourceIndex].image = clippedPixels(source->image, cleared, *source, document_->selection);
    if (!duplicate) rasterizeLayer(document_->layers[sourceIndex]);
    if (duplicate) document_->layers[sourceIndex].image = before.layers.at(sourceIndex).image;
    Layer layer;
    layer.id = QUuid::createUuid();
    layer.name = QStringLiteral("Floating Selection");
    layer.image = copied->first.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    layer.transform.origin = copied->second;
    layer.transform.size = layer.image.size();
    layer.parentId = source->parentId;
    layer.opacity = source->opacity;
    layer.blendMode = source->blendMode;
    document_->layers.insert(sourceIndex + 1, layer);
    document_->activeLayerId = layer.id;
    selectedLayerIds_ = {layer.id};
    maskSelected_ = false;
    floating_ = FloatingSelectionState{sourceId, layer.id, before, beforeActive, layer.transform, layer.image.size(), duplicate};
    return true;
}

bool EditorSession::commitSelectionTransform()
{
    if (!document_ || !floating_) return false;
    const FloatingSelectionState state = *floating_;
    const int floatingIndex = indexOf(state.layerId), sourceIndex = indexOf(state.sourceId);
    if (floatingIndex < 0 || sourceIndex < 0) { cancelSelectionTransform(); return false; }
    const Layer floating = document_->layers.at(floatingIndex);
    if (floating.transform == state.original) {
        *document_ = state.before;
        floating_.reset();
        selectedLayerIds_.clear();
        if (document_->activeLayerId) selectedLayerIds_.insert(*document_->activeLayerId);
        endEdit();
        return true;
    }
    Layer source = document_->layers.at(sourceIndex);
    bool invertible = false;
    const QTransform sourceToDocument = pixelToDocument(source.transform, source.image.size());
    const QTransform documentToSource = sourceToDocument.inverted(&invertible);
    if (!invertible) { cancelSelectionTransform(); return false; }
    const QTransform floatingToSource = documentToSource * pixelToDocument(floating.transform, floating.image.size());
    const QRectF movedBounds = floatingToSource.mapRect(QRectF(QPointF(), floating.image.size()));
    const QRectF originalBounds(QPointF(), source.image.size());
    const QRect extent = originalBounds.united(movedBounds).toAlignedRect();
    if (extent.width() < 1 || extent.height() < 1 || extent.width() > 30000 || extent.height() > 30000
        || qint64(extent.width()) * extent.height() > 100000000LL) { cancelSelectionTransform(); return false; }
    QImage merged(extent.size(), QImage::Format_RGBA8888_Premultiplied); merged.fill(Qt::transparent);
    QPainter painter(&merged);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, floating.transform.sampling != Sampling::Nearest);
    painter.drawImage(-extent.topLeft(), source.image);
    painter.translate(-extent.left(), -extent.top());
    painter.setTransform(floatingToSource, true);
    painter.drawImage(QPointF(), floating.image);
    painter.end();
    // QRect::center() is integer-valued and biases even-sized rasters by half a
    // pixel. Use the geometric center so a pure move never shifts the source.
    const QPointF newCenter = sourceToDocument.map(QRectF(extent).center());
    source.image = merged;
    rasterizeLayer(source);
    source.transform.size = QSizeF(extent.width() * source.transform.size.width() / originalBounds.width(),
                                   extent.height() * source.transform.size.height() / originalBounds.height());
    source.transform.origin = newCenter - QPointF(source.transform.size.width() / 2.0, source.transform.size.height() / 2.0);
    if (!source.mask.isNull() && !source.maskPlacement && source.mask.size() == originalBounds.size().toSize() && extent != originalBounds.toRect()) {
        QImage grown(extent.size(), QImage::Format_Grayscale8); grown.fill(255);
        QPainter maskPainter(&grown); maskPainter.drawImage(-extent.topLeft(), source.mask); maskPainter.end(); source.mask = grown;
    }
    const QTransform selectionMap = pixelToDocument(floating.transform, state.pixelSize)
        * pixelToDocument(state.original, state.pixelSize).inverted();
    if (document_->selection) {
        QImage moved(document_->canvasSize, QImage::Format_Grayscale8); moved.fill(0);
        QPainter selectionPainter(&moved); selectionPainter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        selectionPainter.setTransform(selectionMap); selectionPainter.drawImage(QPointF(), *document_->selection); selectionPainter.end();
        document_->selection = moved;
    }
    document_->layers.removeAt(floatingIndex);
    const int updatedSourceIndex = indexOf(state.sourceId);
    document_->layers[updatedSourceIndex] = source;
    document_->activeLayerId = state.sourceId;
    selectedLayerIds_ = {state.sourceId};
    floating_.reset();
    endEdit();
    return true;
}

bool EditorSession::cancelSelectionTransform()
{
    if (!document_ || !floating_) return false;
    const Document before = floating_->before;
    *document_ = before;
    selectedLayerIds_.clear();
    if (document_->activeLayerId) selectedLayerIds_.insert(*document_->activeLayerId);
    maskSelected_ = false;
    floating_.reset();
    endEdit();
    return true;
}

static std::optional<QPointF> documentToPixel(const Layer &layer, const QPointF &point, const QSize &size);
static QImage selectionCoverageForLayer(const Layer &layer, const QSize &size, const std::optional<QImage> &selection);

bool EditorSession::applyGradient(const QPointF &start, const QPointF &end, const QColor &foreground,
                                  const QColor &background, bool radial, bool foregroundTransparent,
                                  bool reversed, double opacity)
{
    Layer *layer = activeLayer();
    if (!document_ || !layer || layer->group || !foreground.isValid() || !background.isValid()
        || QLineF(start, end).length() < .5 || !std::isfinite(opacity) || opacity < .01 || opacity > 1) return false;
    const int index = indexOf(layer->id);
    const bool maskTarget = maskSelected_;
    if (maskTarget && layer->mask.isNull()) return false;
    QImage original = maskTarget ? expandedMaskImage(*layer) : layer->image;
    if (!maskTarget && original.isNull()) {
        const QSize size = layer->transform.size.toSize().expandedTo(QSize(1, 1));
        if (qint64(size.width()) * size.height() > 100000000LL) return false;
        original = QImage(size, QImage::Format_RGBA8888_Premultiplied); original.fill(Qt::transparent);
    }
    Layer target = *layer;
    if (maskTarget) target.transform = layer->maskPlacement.value_or(layer->transform);
    const auto first = documentToPixel(target, start, original.size()), last = documentToPixel(target, end, original.size());
    if (!first || !last) return false;
    QImage painted(original.size(), QImage::Format_RGBA8888_Premultiplied); painted.fill(Qt::transparent);
    QColor firstColor = foreground;
    QColor secondColor = foregroundTransparent ? foreground : background;
    firstColor.setAlphaF(opacity);
    secondColor.setAlphaF(foregroundTransparent ? 0 : opacity);
    if (maskTarget) {
        firstColor.setRgb(qGray(foreground.rgb()), qGray(foreground.rgb()), qGray(foreground.rgb()), qRound(opacity * 255));
        secondColor.setRgb(qGray((foregroundTransparent ? foreground : background).rgb()),
                           qGray((foregroundTransparent ? foreground : background).rgb()),
                           qGray((foregroundTransparent ? foreground : background).rgb()),
                           foregroundTransparent ? 0 : qRound(opacity * 255));
    }
    if (reversed) std::swap(firstColor, secondColor);
    QPainter painter(&painted);
    if (radial) { QRadialGradient gradient(*first, QLineF(*first, *last).length()); gradient.setColorAt(0, firstColor); gradient.setColorAt(1, secondColor); painter.fillRect(painted.rect(), gradient); }
    else { QLinearGradient gradient(*first, *last); gradient.setColorAt(0, firstColor); gradient.setColorAt(1, secondColor); painter.fillRect(painted.rect(), gradient); }
    painter.end();
    QImage base = maskTarget ? original.convertToFormat(QImage::Format_RGBA8888_Premultiplied)
                             : original.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (maskTarget) for (int y = 0; y < original.height(); ++y) {
        const uchar *gray = original.constScanLine(y); uchar *rgba = base.scanLine(y);
        for (int x = 0; x < original.width(); ++x) rgba[x * 4] = rgba[x * 4 + 1] = rgba[x * 4 + 2] = gray[x], rgba[x * 4 + 3] = 255;
    }
    QPainter composite(&base); composite.setCompositionMode(QPainter::CompositionMode_SourceOver); composite.drawImage(QPoint(), painted); composite.end();
    const QImage result = clippedPixels(maskTarget ? original.convertToFormat(QImage::Format_RGBA8888_Premultiplied) : original,
                                        base, target, document_->selection);
    if (maskTarget) {
        QImage gray(result.size(), QImage::Format_Grayscale8);
        for (int y = 0; y < gray.height(); ++y) { const uchar *rgba = result.constScanLine(y); uchar *out = gray.scanLine(y); for (int x = 0; x < gray.width(); ++x) out[x] = uchar(qGray(rgba[x*4], rgba[x*4+1], rgba[x*4+2])); }
        if (gray == original) return false;
        beginEdit(QStringLiteral("Gradient Mask")); document_->layers[index].mask = gray; endEdit(); return true;
    }
    if (result == original) return false;
    beginEdit(QStringLiteral("Gradient")); document_->layers[index].image = result; rasterizeLayer(document_->layers[index]); endEdit(); return true;
}

bool EditorSession::addShape(ShapeKind kind, const QRectF &input, const QColor &fill, const QColor &stroke,
                             double strokeWidth, double cornerRadius,
                             const std::optional<QPointF> &start,
                             const std::optional<QPointF> &end)
{
    if (!document_ || !fill.isValid() || !stroke.isValid() || !std::isfinite(strokeWidth) || strokeWidth < 0 || strokeWidth > 1000
        || !std::isfinite(cornerRadius) || cornerRadius < 0 || cornerRadius > 5000) return false;
    QRectF rect = input.normalized();
    if (rect.width() < 1 || rect.height() < 1 || rect.width() > 30000 || rect.height() > 30000) return false;

    QPointF startUnit(0.0, 0.0);
    QPointF endUnit(1.0, 1.0);
    QPointF origin;
    QSize size;

    if (kind == ShapeKind::Line) {
        const double thickness = std::max(1.0, strokeWidth > 0 ? strokeWidth : 2.0);
        QPointF from, to;
        if (start && end) {
            from = *start;
            to = *end;
        } else {
            from = rect.topLeft();
            to = rect.bottomRight();
        }
        const double minX = std::min(from.x(), to.x());
        const double minY = std::min(from.y(), to.y());
        const double spanW = std::abs(to.x() - from.x());
        const double spanH = std::abs(to.y() - from.y());
        const double pad = thickness / 2.0;
        rect = QRectF(minX - pad, minY - pad, std::max(1.0, spanW + thickness), std::max(1.0, spanH + thickness));
        startUnit = QPointF(rect.width() > 0 ? (from.x() - rect.left()) / rect.width() : 0.5,
                            rect.height() > 0 ? (from.y() - rect.top()) / rect.height() : 0.5);
        endUnit = QPointF(rect.width() > 0 ? (to.x() - rect.left()) / rect.width() : 0.5,
                          rect.height() > 0 ? (to.y() - rect.top()) / rect.height() : 0.5);
        size = QSize(qCeil(rect.width()), qCeil(rect.height()));
        origin = rect.topLeft();
    } else {
        const int margin = strokeWidth > 0 ? qCeil(strokeWidth / 2.0) + 2 : 0;
        size = QSize(qCeil(rect.width()) + margin * 2, qCeil(rect.height()) + margin * 2);
        origin = QPointF(std::floor(rect.left()) - margin, std::floor(rect.top()) - margin);
    }

    if (qint64(size.width()) * size.height() > 100000000LL) return false;
    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);

    if (kind == ShapeKind::Line) {
        const double thickness = std::max(1.0, strokeWidth > 0 ? strokeWidth : 2.0);
        const QColor color = (fill.isValid() && fill != Qt::transparent) ? fill : (stroke.isValid() ? stroke : Qt::black);
        QPen pen(color, thickness, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(pen);
        painter.drawLine(QPointF(startUnit.x() * size.width(), startUnit.y() * size.height()),
                         QPointF(endUnit.x() * size.width(), endUnit.y() * size.height()));
    } else {
        const int margin = strokeWidth > 0 ? qCeil(strokeWidth / 2.0) + 2 : 0;
        const QRectF bounds(margin, margin, rect.width(), rect.height());
        QPainterPath path;
        if (kind == ShapeKind::Ellipse) path.addEllipse(bounds);
        else if (cornerRadius > 0) {
            const double radius = std::min({cornerRadius, rect.width() / 2.0, rect.height() / 2.0});
            path.addRoundedRect(bounds, radius, radius);
        } else path.addRect(bounds);
        painter.fillPath(path, fill);
        if (strokeWidth > 0 && stroke.isValid() && stroke != Qt::transparent) {
            painter.strokePath(path, QPen(stroke, strokeWidth));
        }
    }
    painter.end();

    const QString kindStr = (kind == ShapeKind::Line) ? QStringLiteral("Line")
                          : (kind == ShapeKind::Ellipse) ? QStringLiteral("Ellipse")
                          : QStringLiteral("Rectangle");

    beginEdit(kindStr);
    if (!insertPixelLayer(image, origin, nextName(kindStr), kindStr, false)) { endEdit(); return false; }
    Layer *shapeLayer = activeLayer();
    if (shapeLayer) {
        LayerShapeStyle style;
        style.kind = kind;
        const QColor shapeColor = (kind == ShapeKind::Line && fill == Qt::transparent && stroke.isValid()) ? stroke : fill;
        style.red = shapeColor.redF();
        style.green = shapeColor.greenF();
        style.blue = shapeColor.blueF();
        style.cornerRadius = (kind == ShapeKind::Rectangle) ? cornerRadius : 0.0;
        if (kind == ShapeKind::Line) {
            style.lineWidth = strokeWidth > 0 ? strokeWidth : 2.0;
            style.start = startUnit;
            style.end = endUnit;
        }
        shapeLayer->shapeStyle = style;
        shapeLayer->shape.insert(QStringLiteral("kind"), kindStr);
        shapeLayer->shape.insert(QStringLiteral("fill"), fill.name(QColor::HexArgb));
        shapeLayer->shape.insert(QStringLiteral("red"), style.red);
        shapeLayer->shape.insert(QStringLiteral("green"), style.green);
        shapeLayer->shape.insert(QStringLiteral("blue"), style.blue);
        shapeLayer->shape.insert(QStringLiteral("stroke"), stroke.name(QColor::HexArgb));
        shapeLayer->shape.insert(QStringLiteral("strokeWidth"), strokeWidth);
        shapeLayer->shape.insert(QStringLiteral("cornerRadius"), style.cornerRadius);
        if (kind == ShapeKind::Line) {
            shapeLayer->shape.insert(QStringLiteral("lineWidth"), *style.lineWidth);
            shapeLayer->shape.insert(QStringLiteral("start"), QJsonArray{startUnit.x(), startUnit.y()});
            shapeLayer->shape.insert(QStringLiteral("end"), QJsonArray{endUnit.x(), endUnit.y()});
        }
    }
    endEdit();
    return true;
}

bool EditorSession::addShape(const QRectF &rect, const QColor &fill, const QColor &stroke, double strokeWidth, bool ellipse, double cornerRadius)
{
    return addShape(ellipse ? ShapeKind::Ellipse : ShapeKind::Rectangle, rect, fill, stroke, strokeWidth, cornerRadius);
}

namespace {
int textAlignmentIndex(TextAlignment alignment) { return alignment == TextAlignment::Center ? 1 : alignment == TextAlignment::Right ? 2 : 0; }

QFont textBaseFont(const TextStyle &style, int pixelSize, bool bold, bool italic, bool underline)
{
    QFont font(style.fontName); font.setPixelSize(pixelSize); font.setBold(bold); font.setItalic(italic); font.setUnderline(underline);
    return font;
}

QJsonObject textShapeMetadata(const TextStyle &style, int pixelSize, bool bold, bool italic, bool underline, bool areaText, const QSize &size)
{
    return {{QStringLiteral("kind"), QStringLiteral("Text")}, {QStringLiteral("text"), style.content},
        {QStringLiteral("fontFamily"), style.fontName}, {QStringLiteral("pixelSize"), pixelSize},
        {QStringLiteral("bold"), bold}, {QStringLiteral("italic"), italic}, {QStringLiteral("underline"), underline},
        {QStringLiteral("alignment"), textAlignmentIndex(style.alignment)}, {QStringLiteral("areaText"), areaText},
        {QStringLiteral("fill"), QColor::fromRgbF(style.red, style.green, style.blue).name(QColor::HexArgb)},
        {QStringLiteral("baseWidth"), size.width()}, {QStringLiteral("baseHeight"), size.height()}};
}

// Runs that do not fit the content are dropped rather than rendered or saved out of place.
void sanitizeTextRuns(TextStyle &style)
{
    if (!style.colorRunsAreValid()) style.colorRuns.reset();
    if (!style.fontRunsAreValid()) style.fontRuns.reset();
}

TextStyle flatTextStyle(const QString &text, const QString &fontFamily, int pixelSize, int alignment, const QColor &color,
                        double tracking, double leading)
{
    TextStyle ts;
    ts.content = text;
    ts.fontName = fontFamily;
    ts.fontSize = double(pixelSize);
    ts.red = color.redF();
    ts.green = color.greenF();
    ts.blue = color.blueF();
    ts.alignment = (alignment == 1 ? TextAlignment::Center : alignment == 2 ? TextAlignment::Right : TextAlignment::Left);
    ts.tracking = tracking;
    ts.leading = leading;
    return ts;
}
} // namespace

bool EditorSession::addText(const QString &text, const QRectF &input, const QString &fontFamily,
                            int pixelSize, bool bold, bool italic, bool underline, int alignment, const QColor &color, bool areaText,
                            double tracking, double leading)
{
    if (!color.isValid()) return false;
    return addText(flatTextStyle(text, QFont(fontFamily).family(), pixelSize, alignment, color, tracking, leading), input, bold, italic, underline, areaText);
}

bool EditorSession::addText(const TextStyle &input, const QRectF &boxInput, bool bold, bool italic, bool underline, bool areaText)
{
    TextStyle ts = input;
    sanitizeTextRuns(ts);
    const int pixelSize = qRound(ts.fontSize);
    const QRectF box = boxInput.normalized();
    if (!document_ || ts.content.trimmed().isEmpty() || !std::isfinite(box.x()) || !std::isfinite(box.y())
        || pixelSize < 4 || pixelSize > 1000) return false;
    const QImage image = renderText(ts, QSize(qCeil(box.width()), qCeil(box.height())), textBaseFont(ts, pixelSize, bold, italic, underline),
                                    textAlignmentIndex(ts.alignment), areaText);
    if (image.isNull()) return false;
    const QSize size = image.size();
    beginEdit(QStringLiteral("Text"));
    if (!insertPixelLayer(image, box.topLeft(), nextName(QStringLiteral("Text")), QStringLiteral("Text"), false)) { endEdit(); return false; }
    if (Layer *layer = activeLayer()) {
        layer->shape = textShapeMetadata(ts, pixelSize, bold, italic, underline, areaText, size);
        ts.fontSize = double(pixelSize);
        if (areaText) ts.boxSize = QSizeF(size.width(), size.height()); else ts.boxSize.reset();
        layer->text = ts;
        if (ts.requiredFormatVersion() > 1) document_->formatVersion = std::max(document_->formatVersion, ts.requiredFormatVersion());
    }
    endEdit();
    return true;
}

bool EditorSession::updateText(const QUuid &id, const QString &text, const QRectF &input, const QString &fontFamily,
                               int pixelSize, bool bold, bool italic, bool underline, int alignment, const QColor &color, bool areaText,
                               double tracking, double leading)
{
    if (!document_ || !color.isValid()) return false;
    const int index = indexOf(id);
    if (index < 0) return false;
    TextStyle ts = flatTextStyle(text, QFont(fontFamily).family(), pixelSize, alignment, color, tracking, leading);
    // The flat arguments describe one color and one face. Keep the layer's per-letter runs through the edit unless
    // those arguments changed the base color or face, which restyles the whole text.
    if (const std::optional<TextStyle> &old = document_->layers[index].text; old && (old->colorRuns || old->fontRuns)) {
        TextStyle kept = *old;
        int prefix = 0;
        const int limit = std::min<int>(old->content.size(), text.size());
        while (prefix < limit && old->content.at(prefix) == text.at(prefix)) ++prefix;
        int suffix = 0;
        while (suffix < limit - prefix && old->content.at(old->content.size() - 1 - suffix) == text.at(text.size() - 1 - suffix)) ++suffix;
        kept.replaceCharacters(prefix, int(old->content.size()) - prefix - suffix, int(text.size()) - prefix - suffix);
        kept.content = text;
        const QColor oldColor = QColor::fromRgbF(old->red, old->green, old->blue);
        if (oldColor != color) kept.setColor({color.redF(), color.greenF(), color.blueF()}, 0, 0);
        if (old->fontName != ts.fontName) kept.setFont(ts.fontName, 0, 0);
        kept.fontSize = ts.fontSize; kept.alignment = ts.alignment; kept.tracking = tracking; kept.leading = leading;
        ts = kept;
    }
    return updateText(id, ts, input, bold, italic, underline, areaText);
}

bool EditorSession::updateText(const QUuid &id, const TextStyle &input, const QRectF &boxInput, bool bold, bool italic, bool underline, bool areaText,
                               const QString &historyName)
{
    if (!document_) return false;
    const int index = indexOf(id);
    if (index < 0) return false;
    {
        const Layer &existing = document_->layers.at(index);
        if (existing.shape.value(QStringLiteral("kind")).toString() != QStringLiteral("Text") && !existing.text.has_value()) return false;
    }
    TextStyle ts = input;
    sanitizeTextRuns(ts);
    const int pixelSize = qRound(ts.fontSize);
    const QRectF box = boxInput.normalized();
    if (ts.content.trimmed().isEmpty() || box.width() < 1 || box.height() < 1 || pixelSize < 4 || pixelSize > 1000) return false;
    const QImage image = renderText(ts, QSize(qCeil(box.width()), qCeil(box.height())), textBaseFont(ts, pixelSize, bold, italic, underline),
                                    textAlignmentIndex(ts.alignment), areaText);
    if (image.isNull()) return false;
    const QSize size = image.size();
    beginEdit(historyName);
    // Taken after beginEdit: a reference from before it would write into the buffer the history snapshot shares.
    Layer &layer = document_->layers[index];
    if (!layer.mask.isNull() && !layer.maskPlacement) layer.maskPlacement = layer.transform;
    layer.image = image;
    layer.transform.origin = box.topLeft();
    layer.transform.size = size;
    layer.shape = textShapeMetadata(ts, pixelSize, bold, italic, underline, areaText, size);
    ts.fontSize = double(pixelSize);
    if (areaText) ts.boxSize = QSizeF(size.width(), size.height()); else ts.boxSize.reset();
    layer.text = ts;
    // Per-letter colors and faces need a project format that can carry them; undo restores the older version.
    if (ts.requiredFormatVersion() > 1) document_->formatVersion = std::max(document_->formatVersion, ts.requiredFormatVersion());
    endEdit();
    return true;
}

bool EditorSession::recolorText(const QUuid &id, const QColor &color)
{
    if (!document_ || !color.isValid()) return false;
    const int index = indexOf(id);
    if (index < 0) return false;
    const Layer &layer = document_->layers.at(index);
    if (layer.group || !layer.text || layer.image.isNull()) return false;
    TextStyle style = *layer.text;
    const TextStyle::Rgb target{color.redF(), color.greenF(), color.blueF()};
    if (style.red == target.r && style.green == target.g && style.blue == target.b && !style.colorRuns) return true;
    style.setColor(target, 0, 0);
    const QJsonObject shape = layer.shape;
    const QRectF box(layer.transform.origin, layer.transform.size);
    return updateText(id, style, box, shape.value(QStringLiteral("bold")).toBool(), shape.value(QStringLiteral("italic")).toBool(),
                      shape.value(QStringLiteral("underline")).toBool(), shape.value(QStringLiteral("areaText")).toBool(),
                      QStringLiteral("Fill Text"));
}

void EditorSession::redrawSelectedShapes()
{
    if (!document_) return;
    const QSet<QUuid> targets = selectedTransformLayerIds();
    for (Layer &layer : document_->layers) {
        if (!targets.contains(layer.id) || layer.group) continue;
        if (layer.shape.isEmpty() && !layer.shapeStyle && !layer.text) continue;
        const int width = std::max(1, qRound(layer.transform.size.width()));
        const int height = std::max(1, qRound(layer.transform.size.height()));
        if (width == layer.image.width() && height == layer.image.height()) continue;
        if (qint64(width) * height > 100000000LL) continue;
        const LayerTransform oldPlacement = layer.transform;

        const QString kindStr = layer.shapeStyle
            ? shapeKindToString(layer.shapeStyle->kind)
            : layer.shape.value(QStringLiteral("kind")).toString();
        const bool isText = (kindStr == QStringLiteral("Text") || layer.text.has_value());

        QImage image(QSize(width, height), QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);

        if (isText) {
            TextStyle style;
            style.content.clear();
            style.red = style.green = style.blue = 0.0;
            int pixelSize = 48;
            bool bold = false, italic = false, underline = false;
            int alignment = 0;
            bool areaText = false;

            if (layer.text) {
                style = *layer.text;
                pixelSize = qRound(layer.text->fontSize);
                alignment = textAlignmentIndex(layer.text->alignment);
                areaText = layer.text->boxSize.has_value();
            } else {
                style.content = layer.shape.value(QStringLiteral("text")).toString();
                style.fontName = layer.shape.value(QStringLiteral("fontFamily")).toString();
                pixelSize = layer.shape.value(QStringLiteral("pixelSize")).toInt(48);
                alignment = layer.shape.value(QStringLiteral("alignment")).toInt();
                areaText = layer.shape.value(QStringLiteral("areaText")).toBool();
                QColor textColor(layer.shape.value(QStringLiteral("fill")).toString());
                if (!textColor.isValid()) textColor = Qt::black;
                style.red = textColor.redF(); style.green = textColor.greenF(); style.blue = textColor.blueF();
            }
            bold = layer.shape.value(QStringLiteral("bold")).toBool();
            italic = layer.shape.value(QStringLiteral("italic")).toBool();
            underline = layer.shape.value(QStringLiteral("underline")).toBool();

            QFont font(style.fontName);
            font.setBold(bold); font.setItalic(italic); font.setUnderline(underline);

            if (areaText) {
                // Fixed paragraph box: reflows text within width and height WITHOUT scaling font size!
                font.setPixelSize(pixelSize);
                if (layer.text) layer.text->boxSize = QSizeF(width, height);
                layer.shape.insert(QStringLiteral("baseWidth"), width);
                layer.shape.insert(QStringLiteral("baseHeight"), height);
            } else {
                // Point text: scales with box
                const double baseWidth = std::max(1, layer.shape.value(QStringLiteral("baseWidth")).toInt(width));
                const double baseHeight = std::max(1, layer.shape.value(QStringLiteral("baseHeight")).toInt(height));
                const double scale = std::min(width / baseWidth, height / baseHeight);
                const int scaledSize = std::max(4, qRound(pixelSize * scale));
                font.setPixelSize(scaledSize);
            }

            painter.drawImage(QPoint(), renderText(style, QSize(width, height), font, alignment, areaText));
        } else if (kindStr == QStringLiteral("Line") || (layer.shapeStyle && layer.shapeStyle->kind == ShapeKind::Line)) {
            double thickness = 2.0;
            QPointF startUnit(0.0, 0.0);
            QPointF endUnit(1.0, 1.0);
            QColor lineColor = Qt::black;
            if (layer.shapeStyle) {
                thickness = layer.shapeStyle->lineWidth.value_or(2.0);
                startUnit = layer.shapeStyle->start.value_or(QPointF(0, 0));
                endUnit = layer.shapeStyle->end.value_or(QPointF(1, 1));
                lineColor = QColor::fromRgbF(layer.shapeStyle->red, layer.shapeStyle->green, layer.shapeStyle->blue);
            } else {
                thickness = layer.shape.value(QStringLiteral("lineWidth")).toDouble(layer.shape.value(QStringLiteral("strokeWidth")).toDouble(2.0));
                if (layer.shape.contains(QStringLiteral("start"))) {
                    const QJsonArray s = layer.shape.value(QStringLiteral("start")).toArray();
                    if (s.size() >= 2) startUnit = QPointF(s.at(0).toDouble(), s.at(1).toDouble());
                }
                if (layer.shape.contains(QStringLiteral("end"))) {
                    const QJsonArray e = layer.shape.value(QStringLiteral("end")).toArray();
                    if (e.size() >= 2) endUnit = QPointF(e.at(0).toDouble(), e.at(1).toDouble());
                }
                lineColor = QColor(layer.shape.value(QStringLiteral("stroke")).toString(layer.shape.value(QStringLiteral("fill")).toString()));
                if (!lineColor.isValid()) lineColor = Qt::black;
            }
            thickness = std::max(1.0, thickness);
            QPen pen(lineColor, thickness, Qt::SolidLine, Qt::RoundCap);
            painter.setPen(pen);
            painter.drawLine(QPointF(startUnit.x() * width, startUnit.y() * height),
                             QPointF(endUnit.x() * width, endUnit.y() * height));
        } else {
            const QColor fill = layer.shapeStyle
                ? QColor::fromRgbF(layer.shapeStyle->red, layer.shapeStyle->green, layer.shapeStyle->blue)
                : QColor(layer.shape.value(QStringLiteral("fill")).toString());
            const QColor stroke = layer.shape.contains(QStringLiteral("stroke"))
                ? QColor(layer.shape.value(QStringLiteral("stroke")).toString())
                : Qt::transparent;
            const double strokeWidth = layer.shape.value(QStringLiteral("strokeWidth")).toDouble(0.0);
            const double cornerRadius = layer.shapeStyle
                ? layer.shapeStyle->cornerRadius
                : layer.shape.value(QStringLiteral("cornerRadius")).toDouble(0.0);

            const double margin = strokeWidth > 0 ? std::min(double(qCeil(strokeWidth / 2.0) + 2), std::min(width, height) / 2.0) : 0;
            const QRectF bounds(margin, margin, std::max(0.0, width - margin * 2), std::max(0.0, height - margin * 2));
            QPainterPath path;
            if (kindStr == QStringLiteral("Ellipse") || (layer.shapeStyle && layer.shapeStyle->kind == ShapeKind::Ellipse)) {
                path.addEllipse(bounds);
            } else if (cornerRadius > 0) {
                const double radius = std::min({cornerRadius, bounds.width() / 2.0, bounds.height() / 2.0});
                path.addRoundedRect(bounds, radius, radius);
            } else {
                path.addRect(bounds);
            }
            painter.fillPath(path, fill);
            if (strokeWidth > 0 && stroke.isValid() && stroke != Qt::transparent) {
                painter.strokePath(path, QPen(stroke, strokeWidth));
            }
        }
        painter.end();
        if (!layer.mask.isNull() && !layer.maskPlacement) layer.maskPlacement = oldPlacement;
        layer.image = image;
    }
}

bool EditorSession::addNoiseToActiveLayer(float amount, bool gaussian, bool monochromatic, quint32 seed)
{
    const Layer *layer = activeLayer();
    if (!layer || layer->group || layer->image.isNull() || amount < 0 || amount > 400) return false;
    const int index = indexOf(layer->id);
    const QImage result = clippedPixels(layer->image, RasterOperations::addNoise(layer->image, amount, gaussian, monochromatic, seed), *layer, document_->selection);
    if (result == layer->image) return false;
    beginEdit(QStringLiteral("Add Noise"));
    document_->layers[index].image = result;
    rasterizeLayer(document_->layers[index]);
    endEdit(); return true;
}

bool EditorSession::distortActiveLayer(double amount)
{
    const Layer *layer = activeLayer();
    if (!layer || layer->group || layer->image.isNull() || !std::isfinite(amount) || amount < -100 || amount > 100) return false;
    const int index = indexOf(layer->id);
    const QImage result = clippedPixels(layer->image, RasterOperations::lensDistorted(layer->image, amount / 100.0 * .35), *layer, document_->selection);
    if (result == layer->image) return false;
    beginEdit(QStringLiteral("Lens Correction"));
    document_->layers[index].image = result;
    rasterizeLayer(document_->layers[index]);
    endEdit(); return true;
}

bool EditorSession::applyLevels(const LevelsSettings &settings)
{
    const Layer *layer = activeLayer(); if (!layer || layer->group || layer->image.isNull()) return false;
    const int index = indexOf(layer->id);
    const QImage result = clippedPixels(layer->image, RasterOperations::levels(layer->image, settings), *layer, document_->selection);
    if (result == layer->image) return false;
    beginEdit(QStringLiteral("Levels")); document_->layers[index].image = result; rasterizeLayer(document_->layers[index]); endEdit(); return true;
}

bool EditorSession::applyExposure(double stops, double offset, double gamma)
{
    const Layer *layer = activeLayer();
    if (!layer || layer->group || layer->image.isNull() || !std::isfinite(stops) || !std::isfinite(offset)
        || !std::isfinite(gamma) || gamma <= 0) return false;
    const int index = indexOf(layer->id);
    const QImage result = clippedPixels(layer->image, RasterOperations::exposure(layer->image, stops, offset, gamma), *layer, document_->selection);
    if (result == layer->image) return false;
    beginEdit(QStringLiteral("Exposure")); document_->layers[index].image = result; rasterizeLayer(document_->layers[index]); endEdit(); return true;
}

bool EditorSession::applyHueSaturation(const HueSaturationSettings &settings)
{
    const Layer *layer = activeLayer();
    if (!layer || layer->group || layer->image.isNull() || settings.isIdentity()) return false;
    const int index = indexOf(layer->id);
    const QImage result = clippedPixels(layer->image, RasterOperations::hueSaturation(layer->image, settings), *layer, document_->selection);
    if (result == layer->image) return false;
    beginEdit(QStringLiteral("Hue/Saturation")); document_->layers[index].image = result; rasterizeLayer(document_->layers[index]); endEdit(); return true;
}

bool EditorSession::applyCurves(const CurvesSettings &settings)
{
    const Layer *layer = activeLayer();
    if (!layer || layer->group || layer->image.isNull() || !settings.isValid()) return false;
    const int index = indexOf(layer->id);
    const QImage result = clippedPixels(layer->image, RasterOperations::curves(layer->image, settings), *layer, document_->selection);
    if (result == layer->image) return false;
    beginEdit(QStringLiteral("Curves")); document_->layers[index].image = result; rasterizeLayer(document_->layers[index]); endEdit(); return true;
}

bool EditorSession::applyGradientMap(const QColor &shadows, const QColor &highlights, bool reversed)
{
    const Layer *layer = activeLayer(); if (!layer || layer->group || layer->image.isNull() || !shadows.isValid() || !highlights.isValid()) return false;
    const int index = indexOf(layer->id);
    const QImage result = clippedPixels(layer->image, RasterOperations::gradientMap(layer->image, shadows, highlights, reversed), *layer, document_->selection);
    if (result == layer->image) return false;
    beginEdit(QStringLiteral("Gradient Map")); document_->layers[index].image = result; rasterizeLayer(document_->layers[index]); endEdit(); return true;
}

bool EditorSession::applyGrain(double amount, double size, double roughness, quint32 seed)
{
    const Layer *layer = activeLayer();
    if (!layer || layer->group || layer->image.isNull() || amount < 0 || amount > 100 || size < .5 || size > 20 || roughness < 0 || roughness > 100) return false;
    const int index = indexOf(layer->id);
    const double units = (layer->transform.size.width() / layer->image.width() + layer->transform.size.height() / layer->image.height()) / 2;
    const QImage adjusted = RasterOperations::grain(layer->image, amount, size, roughness, seed, layer->transform.origin, units);
    const QImage result = clippedPixels(layer->image, adjusted, *layer, document_->selection);
    if (result == layer->image) return false;
    beginEdit(QStringLiteral("Grain")); document_->layers[index].image = result; rasterizeLayer(document_->layers[index]); endEdit(); return true;
}

bool EditorSession::applyGaussianBlur(double radius)
{
    const Layer *layer = activeLayer(); if (!layer || layer->group || layer->image.isNull() || radius < .1 || radius > 250) return false;
    const int index = indexOf(layer->id);
    const QImage result = clippedPixels(layer->image, RasterOperations::gaussianBlur(layer->image, radius), *layer, document_->selection);
    if (result == layer->image) return false;
    beginEdit(QStringLiteral("Gaussian Blur")); document_->layers[index].image = result; rasterizeLayer(document_->layers[index]); endEdit(); return true;
}

bool EditorSession::applyMotionBlur(double angleDegrees, double distance)
{
    const Layer *layer = activeLayer(); if (!layer || layer->group || layer->image.isNull() || angleDegrees < -90 || angleDegrees > 90 || distance < 1 || distance > 2000) return false;
    const int index = indexOf(layer->id);
    const QImage result = clippedPixels(layer->image, RasterOperations::motionBlur(layer->image, angleDegrees, distance), *layer, document_->selection);
    if (result == layer->image) return false;
    beginEdit(QStringLiteral("Motion Blur")); document_->layers[index].image = result; rasterizeLayer(document_->layers[index]); endEdit(); return true;
}

bool EditorSession::applyBlackWhite(const float *weights, bool tint, double tintHue, double tintSaturation)
{
    const Layer *layer = activeLayer(); if (!layer || layer->group || layer->image.isNull()) return false;
    const int index = indexOf(layer->id);
    const QImage adjusted = RasterOperations::blackWhite(layer->image, weights, tint, tintHue, tintSaturation);
    const QImage result = clippedPixels(layer->image, adjusted, *layer, document_->selection);
    if (result == layer->image) return false;
    beginEdit(QStringLiteral("Black & White")); document_->layers[index].image = result; rasterizeLayer(document_->layers[index]); endEdit(); return true;
}

bool EditorSession::applyColorBalance(const float *shadows, const float *midtones, const float *highlights, bool preserveLuminosity)
{
    const Layer *layer = activeLayer(); if (!layer || layer->group || layer->image.isNull()) return false;
    const int index = indexOf(layer->id);
    const QImage adjusted = RasterOperations::colorBalance(layer->image, shadows, midtones, highlights, preserveLuminosity);
    const QImage result = clippedPixels(layer->image, adjusted, *layer, document_->selection);
    if (result == layer->image) return false;
    beginEdit(QStringLiteral("Color Balance")); document_->layers[index].image = result; rasterizeLayer(document_->layers[index]); endEdit(); return true;
}

bool EditorSession::applyVignette(double amount, const QColor &color, double midpoint, double roundness, double feather, double highlights)
{
    const Layer *layer = activeLayer();
    if (!layer || layer->group || amount <= 0) return false;
    const int index = indexOf(layer->id);
    if (index < 0) return false;

    if (layer->image.isNull()) {
        if (!layer->adjustment.isEmpty()) return false;
        const QSize canvasSize = document_->canvasSize;
        if (canvasSize.width() <= 0 || canvasSize.height() <= 0) return false;

        QImage base(canvasSize, QImage::Format_RGBA8888_Premultiplied);
        base.fill(0);
        const QRectF canvasFrame(0.0, 0.0, canvasSize.width(), canvasSize.height());
        const QImage adjusted = RasterOperations::vignette(base, amount, color, midpoint, roundness, feather, highlights, canvasFrame);

        Layer canvasLayer = *layer;
        canvasLayer.transform.origin = QPointF(0, 0);
        canvasLayer.transform.size = canvasSize;
        const QImage result = clippedPixels(base, adjusted, canvasLayer, document_->selection);
        if (result == base) return false;

        beginEdit(QStringLiteral("Vignette"));
        Layer &dst = document_->layers[index];
        dst.image = result;
        dst.transform.origin = QPointF(0, 0);
        dst.transform.size = canvasSize;
        rasterizeLayer(dst);
        endEdit();
        return true;
    }

    const QImage adjusted = RasterOperations::vignette(layer->image, amount, color, midpoint, roundness, feather, highlights);
    const QImage result = clippedPixels(layer->image, adjusted, *layer, document_->selection);
    if (result == layer->image) return false;
    beginEdit(QStringLiteral("Vignette"));
    document_->layers[index].image = result;
    rasterizeLayer(document_->layers[index]);
    endEdit();
    return true;
}

bool EditorSession::applyDither(const DitherSettings &settings)
{
    const Layer *layer = activeLayer();
    if (!layer || layer->group || layer->image.isNull()) return false;
    const int index = indexOf(layer->id);
    if (index < 0) return false;
    const QImage dithered = settings.apply(layer->image);
    if (dithered.isNull()) return false;
    const QImage result = clippedPixels(layer->image, dithered, *layer, document_->selection);
    if (result == layer->image) return false;
    beginEdit(QStringLiteral("Dither")); document_->layers[index].image = result; rasterizeLayer(document_->layers[index]); endEdit(); return true;
}

bool EditorSession::applyBloomGlow(double amount, double radius)
{
    const Layer *layer = activeLayer();
    if (!layer || layer->group || layer->image.isNull() || amount <= 0 || radius <= 0) return false;
    const int index = indexOf(layer->id);
    if (index < 0) return false;

    // 1. Calculate blur margin: radius * 3 + 2 matching macOS Filters.swift:377
    const double margin = std::ceil(radius * 3.0 + 2.0);
    const QRectF bounds(0, 0, layer->image.width(), layer->image.height());
    const QRectF extent = bounds.adjusted(-margin, -margin, margin, margin);
    const int grownWidth = qRound(extent.width());
    const int grownHeight = qRound(extent.height());

    if (grownWidth > 30000 || grownHeight > 30000 || qint64(grownWidth) * grownHeight > 100000000) {
        return false;
    }

    // 2. Pad original image into grown buffer
    QImage grown(grownWidth, grownHeight, QImage::Format_RGBA8888_Premultiplied);
    grown.fill(0);
    {
        QPainter p(&grown);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(QPoint(int(margin), int(margin)), layer->image);
    }

    // 3. Compute grown transform placing the larger grid at identical document coordinates
    LayerTransform grownTransform = layer->transform;
    grownTransform.size = QSizeF(extent.width() * layer->transform.size.width() / bounds.width(),
                                 extent.height() * layer->transform.size.height() / bounds.height());
    const QTransform toDoc = LayerRenderer::pixelToDocument(layer->transform, layer->image.size());
    const QPointF centerInDoc = toDoc.map(extent.center());
    grownTransform.origin = QPointF(centerInDoc.x() - grownTransform.size.width() / 2.0,
                                    centerInDoc.y() - grownTransform.size.height() / 2.0);

    // 4. Run Bloom / Glow on grown image
    const QImage bloomed = RasterOperations::bloomGlow(grown, amount, radius);

    // 5. Apply selection clipping
    Layer grownLayer = *layer;
    grownLayer.image = grown;
    grownLayer.transform = grownTransform;
    const QImage clipped = clippedPixels(grown, bloomed, grownLayer, document_->selection);

    // 6. Trimming: cut away transparent padding left empty (matching macOS PixelFilter.trimmed)
    size_t edges[4] = {0, 0, 0, 0};
    brush_alpha_bounds(clipped.constBits(), clipped.width(), clipped.height(), clipped.bytesPerLine(), edges);
    const int cropX = static_cast<int>(edges[0]);
    const int cropY = static_cast<int>(edges[1]);
    const int cropW = static_cast<int>(edges[2] - edges[0]);
    const int cropH = static_cast<int>(edges[3] - edges[1]);
    const QRect crop(cropX, cropY, cropW, cropH);

    QImage finalImage = clipped;
    LayerTransform finalTransform = grownTransform;

    if (crop.width() >= 1 && crop.height() >= 1 && crop != QRect(0, 0, clipped.width(), clipped.height())) {
        finalImage = clipped.copy(crop);
        finalTransform.size = QSizeF(crop.width() * grownTransform.size.width() / double(clipped.width()),
                                     crop.height() * grownTransform.size.height() / double(clipped.height()));
        const QTransform grownToDoc = LayerRenderer::pixelToDocument(grownTransform, clipped.size());
        const QPointF cropCenterDoc = grownToDoc.map(QRectF(crop).center());
        finalTransform.origin = QPointF(cropCenterDoc.x() - finalTransform.size.width() / 2.0,
                                        cropCenterDoc.y() - finalTransform.size.height() / 2.0);
    }

    // 7. Mask handling: carried onto new grid
    QImage finalMask = layer->mask;
    std::optional<LayerTransform> finalMaskPlacement = layer->maskPlacement;
    if (!layer->mask.isNull()) {
        if (!layer->maskPlacement) {
            Layer targetLayer = *layer;
            targetLayer.transform = finalTransform;
            finalMask = LayerRenderer::placedMask(*layer, targetLayer, finalImage.size());
            finalMaskPlacement.reset();
        }
    }

    if (finalImage == layer->image && finalTransform == layer->transform && finalMask == layer->mask) {
        return false;
    }

    // 8. Commit
    beginEdit(QStringLiteral("Bloom / Glow"));
    Layer &dst = document_->layers[index];
    dst.image = finalImage;
    dst.transform = finalTransform;
    if (!layer->mask.isNull()) {
        dst.mask = finalMask;
        dst.maskPlacement = finalMaskPlacement;
    }
    rasterizeLayer(dst);
    endEdit();
    return true;
}

bool EditorSession::applyTonalContrast(double amount, double radius, double shadows, double midtones, double highlights)
{
    const Layer *layer = activeLayer(); if (!layer || layer->group || layer->image.isNull() || amount <= 0) return false;
    const int index = indexOf(layer->id);
    const QImage adjusted = RasterOperations::tonalContrast(layer->image, amount, radius, shadows, midtones, highlights);
    const QImage result = clippedPixels(layer->image, adjusted, *layer, document_->selection);
    if (result == layer->image) return false;
    beginEdit(QStringLiteral("Tonal Contrast")); document_->layers[index].image = result; rasterizeLayer(document_->layers[index]); endEdit(); return true;
}

bool EditorSession::applyCameraRaw(const CameraRawSettings &settings)
{
    const Layer *layer = activeLayer();
    if (!layer) return false;
    return applyCameraRaw(layer->id, settings);
}

bool EditorSession::applyCameraRaw(const QUuid &id, const CameraRawSettings &settings)
{
    if (!document_) return false;
    const int index = indexOf(id);
    if (index < 0) return false;
    const Layer &layer = document_->layers.at(index);
    if (layer.group || layer.image.isNull()) return false;
    if (settings.isIdentity()) return false;
    const QImage graded = RasterOperations::cameraRaw(layer.image, settings);
    const QImage result = clippedPixels(layer.image, graded, layer, document_->selection);
    if (result == layer.image) return false;
    beginEdit(QStringLiteral("Camera Raw Filter"));
    document_->layers[index].image = result;
    rasterizeLayer(document_->layers[index]);
    endEdit();
    return true;
}

bool EditorSession::contentAwareFill()
{
    const Layer *layer = activeLayer();
    if (!document_ || maskSelected_ || !document_->selection || !SelectionOps::hasCoverage(*document_->selection)
        || !layer || layer->group || layer->image.isNull()) return false;
    const int index = indexOf(layer->id);
    QImage image = layer->image;
    LayerTransform placed = layer->transform;
    const QRect selected = SelectionOps::coverageBounds(*document_->selection);
    // The layer grows over any of the selection that lies on the canvas past its edge, so the fill reaches it (mac Filters.swift
    // FilterEdit(growingTo:)). The padding is transparent; selected pixels are filled whether or not anything was there.
    const QTransform toDocument = pixelToDocument(layer->transform, image.size());
    bool invertible = false;
    const QTransform toPixels = toDocument.inverted(&invertible);
    if (!invertible) return false;
    const QRectF wanted = toPixels.mapRect(QRectF(selected));
    const QRect bounds(QPoint(), image.size());
    const QRect extent = QRectF(bounds).united(wanted).toAlignedRect();
    if (extent != bounds) {
        if (extent.width() > 30000 || extent.height() > 30000 || qint64(extent.width()) * extent.height() > 100000000LL) return false;
        QImage grown(extent.size(), QImage::Format_RGBA8888_Premultiplied); grown.fill(Qt::transparent);
        QPainter painter(&grown); painter.drawImage(-extent.topLeft(), image); painter.end();
        image = grown;
        placed.size = QSizeF(extent.width() * layer->transform.size.width() / bounds.width(),
                             extent.height() * layer->transform.size.height() / bounds.height());
        const QPointF middle = toDocument.map(QRectF(extent).center());
        placed.origin = middle - QPointF(placed.size.width() / 2.0, placed.size.height() / 2.0);
    }
    Layer target = *layer; target.transform = placed;
    const QImage coverage = selectionCoverageForLayer(target, image.size(), document_->selection);
    const auto filled = RasterOperations::contentAwareFill(image, coverage);
    if (!filled) return false;
    // Soft selection edges blend the fill with what was there, as every filter does.
    QImage result = clippedPixels(image, *filled, target, document_->selection);
    if (result == layer->image) return false;
    beginEdit(QStringLiteral("Content-Aware Fill"));
    Layer &edited = document_->layers[index];
    if (extent != bounds && !edited.mask.isNull() && !edited.maskPlacement && edited.mask.size() == bounds.size()) {
        // The mask covering the old grid carries onto the new one, its edge tone continuing past the old edge.
        const QImage old = edited.mask.convertToFormat(QImage::Format_Grayscale8);
        QImage carried(extent.size(), QImage::Format_Grayscale8);
        for (int y = 0; y < carried.height(); ++y) {
            uchar *out = carried.scanLine(y);
            const uchar *row = old.constScanLine(std::clamp(y + extent.top(), 0, old.height() - 1));
            for (int x = 0; x < carried.width(); ++x) out[x] = row[std::clamp(x + extent.left(), 0, old.width() - 1)];
        }
        edited.mask = carried;
    }
    edited.image = result; edited.transform = placed;
    rasterizeLayer(edited);
    endEdit();
    return true;
}

static std::optional<QPointF> documentToPixel(const Layer &layer, const QPointF &point, const QSize &size)
{
    const int width = size.width(), height = size.height();
    if (width < 1 || height < 1 || layer.transform.size.width() <= 0 || layer.transform.size.height() <= 0) return std::nullopt;
    QTransform transform;
    transform.translate(layer.transform.center().x(), layer.transform.center().y());
    transform.rotate(layer.transform.rotation);
    transform.scale((layer.transform.flipX ? -1 : 1) * layer.transform.size.width() / width,
                    (layer.transform.flipY ? -1 : 1) * layer.transform.size.height() / height);
    transform.translate(-width / 2.0, -height / 2.0);
    bool invertible = false;
    const QTransform inverse = transform.inverted(&invertible);
    return invertible ? std::optional<QPointF>(inverse.map(point)) : std::nullopt;
}

static QImage selectionCoverageForLayer(const Layer &layer, const QSize &size, const std::optional<QImage> &selection)
{
    QImage local(size, QImage::Format_Grayscale8); local.fill(selection ? 0 : 255);
    if (!selection) return local;
    QTransform toDocument;
    toDocument.translate(layer.transform.center().x(), layer.transform.center().y());
    toDocument.rotate(layer.transform.rotation);
    toDocument.scale((layer.transform.flipX ? -1 : 1) * layer.transform.size.width() / size.width(),
                     (layer.transform.flipY ? -1 : 1) * layer.transform.size.height() / size.height());
    toDocument.translate(-size.width() / 2.0, -size.height() / 2.0);
    const QImage mask = selection->convertToFormat(QImage::Format_Grayscale8);
    for (int y = 0; y < size.height(); ++y) { uchar *out = local.scanLine(y); for (int x = 0; x < size.width(); ++x) {
        const QPointF mapped = toDocument.map(QPointF(x + .5, y + .5)); const QPoint point(qFloor(mapped.x()), qFloor(mapped.y()));
        out[x] = QRect(QPoint(), mask.size()).contains(point) ? mask.constScanLine(point.y())[point.x()] : 0;
    } }
    return local;
}

bool EditorSession::removeBackground(const SubjectRemovalSettings &settings, const QImage &preparedMask, QString *error)
{
    const Layer *layer = activeLayer();
    if (!document_ || !layer || layer->group || layer->image.isNull()
        || !std::isfinite(settings.refineEdges) || settings.refineEdges < 0 || settings.refineEdges > 40
        || !std::isfinite(settings.matteContrast) || settings.matteContrast < 0 || settings.matteContrast > 100
        || !std::isfinite(settings.shiftEdge) || settings.shiftEdge < -10 || settings.shiftEdge > 10) return false;
    const QUuid id = layer->id; const QSize size = layer->image.size();
    const QImage raw = preparedMask.isNull() ? SubjectRemoval::rawMask(layer->image, error) : preparedMask;
    if (raw.isNull()) return false;
    QImage subject = SubjectRemoval::refined(raw, layer->image, settings);
    if (subject.isNull()) return false;
    subject = subject.convertToFormat(QImage::Format_Grayscale8);

    QImage base(size, QImage::Format_Grayscale8); base.fill(255);
    const bool composableMask = !layer->mask.isNull() && !layer->maskPlacement && layer->mask.size() == size;
    if (composableMask) base = layer->mask.convertToFormat(QImage::Format_Grayscale8);
    QImage combined = subject;
    if (composableMask) for (int y = 0; y < size.height(); ++y) {
        uchar *out = combined.scanLine(y); const uchar *under = base.constScanLine(y);
        for (int x = 0; x < size.width(); ++x) out[x] = uchar((int(out[x]) * under[x] + 127) / 255);
    }
    if (document_->selection) {
        const QImage selection = selectionCoverageForLayer(*layer, size, document_->selection);
        for (int y = 0; y < size.height(); ++y) {
            uchar *out = combined.scanLine(y); const uchar *under = base.constScanLine(y), *clip = selection.constScanLine(y);
            for (int x = 0; x < size.width(); ++x) out[x] = uchar((int(out[x]) * clip[x] + int(under[x]) * (255 - clip[x]) + 127) / 255);
        }
    }
    const int index = indexOf(id); if (index < 0) return false;
    beginEdit(QStringLiteral("Remove Background"));
    document_->layers[index].mask = combined;
    document_->layers[index].maskEnabled = true;
    maskSelected_ = true;
    endEdit();
    return true;
}

LevelsHistogram EditorSession::levelsHistogram() const
{
    const Layer *layer=activeLayer();if(!document_||!layer)return {};
    if(!layer->adjustment.isEmpty()){Document below=*document_;const int index=indexOf(layer->id);below.layers=below.layers.mid(0,index);const QImage image=LayerRenderer::flattened(below);return RasterOperations::levelsHistogram(image,document_->selection?*document_->selection:QImage());}
    if(layer->image.isNull())return {};
    return RasterOperations::levelsHistogram(layer->image,selectionCoverageForLayer(*layer,layer->image.size(),document_->selection));
}

std::optional<QColor> EditorSession::levelsSampleAt(const QPointF &documentPoint) const
{
    const Layer *layer = activeLayer();
    if (!document_ || !layer || !QRectF(QPointF(), document_->canvasSize).contains(documentPoint)) return std::nullopt;
    if (!layer->adjustment.isEmpty()) {
        Document below = *document_;
        below.layers = below.layers.mid(0, indexOf(layer->id));
        const QImage image = LayerRenderer::flattened(below);
        const QPoint point(qFloor(documentPoint.x()), qFloor(documentPoint.y()));
        const QColor color = image.pixelColor(point);
        return color.alpha() ? std::optional<QColor>(color) : std::nullopt;
    }
    if (layer->image.isNull()) return std::nullopt;
    const auto pixel = documentToPixel(*layer, documentPoint, layer->image.size());
    if (!pixel) return std::nullopt;
    const QPoint point(qFloor(pixel->x()), qFloor(pixel->y()));
    if (!QRect(QPoint(), layer->image.size()).contains(point)) return std::nullopt;
    const QColor color = layer->image.pixelColor(point);
    return color.alpha() ? std::optional<QColor>(color) : std::nullopt;
}

static QImage selectionCoverageForLayer(const Layer &layer, const QSize &size, const std::optional<QImage> &selection);

QString EditorSession::paintRefusal() const
{
    if (!document_) return {};
    const Layer *layer = activeLayer();
    if (!layer) return {};
    const bool paintingMask = maskSelected_;
    if (document_->selection) {
        const QImage &sel = *document_->selection;
        const QImage gray = sel.format() == QImage::Format_Grayscale8 ? sel : sel.convertToFormat(QImage::Format_Grayscale8);
        bool any = false;
        for (int y = 0; y < gray.height() && !any; ++y) {
            const uchar *row = gray.constScanLine(y);
            for (int x = 0; x < gray.width(); ++x) if (row[x]) { any = true; break; }
        }
        if (!any) return QObject::tr("The selection is empty, so there is nowhere to paint. Deselect (Ctrl+D) to paint anywhere.");
    }
    if (selectedLayerIds_.size() > 1) return QObject::tr("Several layers are selected. Select one layer to paint on it.");
    if (!layer->visible) return QObject::tr("The layer is hidden. Show it to paint on it.");
    if (paintingMask) {
        if (layer->mask.isNull() || !layer->maskEnabled) return QObject::tr("The layer mask is turned off. Enable it to paint on it.");
        return {};
    }
    if (layer->group) return QObject::tr("A folder has no pixels to paint on. Select a layer inside it, or its mask.");
    if (!layer->adjustment.isEmpty()) return QObject::tr("An adjustment layer has no pixels to paint on. Paint on its mask instead.");
    return {};
}

bool EditorSession::beginBrushStroke(const QPointF &documentPoint, const QColor &color, double diameter,
                                     double hardness, double opacity, bool erasing, const std::optional<QPointF> &cloneSource,
                                     int healMode, quint32 effectSeed)
{
    const Layer *layer = activeLayer();
    const bool paintingMask = maskSelected_;
    if (brush_ || !layer || (layer->group && !paintingMask) || (paintingMask && (layer->mask.isNull() || !layer->maskEnabled))
        || !std::isfinite(diameter) || diameter < 1 || diameter > 2000
        || !std::isfinite(hardness) || hardness < 0 || hardness > 1 || opacity <= 0 || opacity > 1) return false;
    const QUuid id = layer->id;
    const int index = indexOf(id);
    if ((cloneSource || healMode >= 0) && paintingMask) return false;
    beginEdit(paintingMask ? QStringLiteral("Paint Mask") : cloneSource ? QStringLiteral("Clone Stamp") : healMode == 3 ? QStringLiteral("Blur")
                                                              : healMode >= 0 ? QStringLiteral("Spot Healing")
                                                              : erasing ? QStringLiteral("Erase") : QStringLiteral("Brush Stroke"));
    if (paintingMask && document_->layers.at(index).mask.size() == QSize(1, 1)) {
        const QSize size = (layer->image.isNull() ? layer->transform.size.toSize() : layer->image.size()).expandedTo(QSize(1, 1));
        const int value = qGray(layer->mask.pixel(0, 0)); document_->layers[index].mask = QImage(size, QImage::Format_Grayscale8); document_->layers[index].mask.fill(value);
    } else if (!paintingMask && document_->layers.at(index).image.isNull()) {
        const QSize size(std::max(1, qRound(layer->transform.size.width())), std::max(1, qRound(layer->transform.size.height())));
        if (qint64(size.width()) * size.height() > 100000000LL) { endEdit(); return false; }
        document_->layers[index].image = QImage(size, QImage::Format_RGBA8888_Premultiplied);
        document_->layers[index].image.fill(Qt::transparent);
    }
    Layer &editable = document_->layers[index];
    const QImage original = paintingMask ? editable.mask.convertToFormat(QImage::Format_Grayscale8)
                                         : editable.image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const auto pixel = documentToPixel(editable, documentPoint, original.size());
    if (!pixel) { endEdit(); return false; }
    const auto clonePixel = cloneSource ? documentToPixel(editable, *cloneSource, original.size()) : std::optional<QPointF>();
    if (cloneSource && !clonePixel) { endEdit(); return false; }
    QImage coverage(original.size(), QImage::Format_Grayscale8); coverage.fill(0);
    brush_ = BrushState{id, *pixel, color, diameter, hardness, opacity, erasing, paintingMask, cloneSource.has_value(),
                        paintingMask ? (erasing ? 255 : 0) : 0, clonePixel ? *clonePixel - *pixel : QPointF(),
                        healMode, effectSeed, original, original, coverage, coverage, {}, {}, {}, {}};
    if (document_->selection) brush_->selectionCoverage = selectionCoverageForLayer(editable, original.size(), document_->selection);

    // Audit against macOS EditorSession+Brush.swift:
    // Smoothing is enabled only for tool == .brush (which includes Paint and Erase).
    // Clone Stamp, Spot Healing, and Blur/Smear do NOT enable smoothing.
    const bool enablesSmoothing = (!cloneSource.has_value() && healMode < 0);
    brushSmoothingEnabled_ = enablesSmoothing && (brushSmoothing_ > 0.0);
    brushAnchor_ = documentPoint;
    brushPointer_ = documentPoint;

    continueBrushStrokeInternal(documentPoint);
    return true;
}

void EditorSession::setCloneSource(const QPointF &documentPoint)
{
    if (!std::isfinite(documentPoint.x()) || !std::isfinite(documentPoint.y())) return;
    cloneSource_ = documentPoint; cloneOffset_.reset();
}

bool EditorSession::beginCloneStroke(const QPointF &documentPoint, double diameter, double hardness, double opacity,
                                     bool aligned, bool sampleAllLayers)
{
    if (!cloneSource_) return false;
    const QPointF offset = aligned ? cloneOffset_.value_or(*cloneSource_ - documentPoint) : *cloneSource_ - documentPoint;
    if(aligned)cloneOffset_=offset;
    if(!beginBrushStroke(documentPoint,Qt::transparent,diameter,hardness,opacity,false,documentPoint+offset))return false;
    if(sampleAllLayers&&brush_&&document_){const Layer *layer=activeLayer();const QImage composite=LayerRenderer::flattened(*document_);QImage sample(brush_->original.size(),QImage::Format_RGBA8888_Premultiplied);sample.fill(Qt::transparent);QTransform map;map.translate(layer->transform.center().x(),layer->transform.center().y());map.rotate(layer->transform.rotation);map.scale((layer->transform.flipX?-1:1)*layer->transform.size.width()/sample.width(),(layer->transform.flipY?-1:1)*layer->transform.size.height()/sample.height());map.translate(-sample.width()/2.0,-sample.height()/2.0);for(int y=0;y<sample.height();++y){uchar *out=sample.scanLine(y);for(int x=0;x<sample.width();++x){const QPointF point=map.map(QPointF(x+.5,y+.5));const int sx=qFloor(point.x()),sy=qFloor(point.y());if(QRect(QPoint(),composite.size()).contains(sx,sy)){const uchar *source=composite.constScanLine(sy)+sx*4;std::copy_n(source,4,out+x*4);}}}brush_->cloneSample=sample;brush_->coverage.fill(0);brush_->settledCoverage.fill(0);brush_->samples.clear();brush_->tailBounds={};brush_->dirtyPixels={};document_->layers[indexOf(brush_->layerId)].image=brush_->original;continueBrushStroke(documentPoint);}
    return true;
}

bool EditorSession::beginHealingStroke(const QPointF &documentPoint, double diameter, double hardness,
                                       double opacity, int mode, quint32 seed)
{
    if (mode < 0 || mode > 2) return false;
    return beginBrushStroke(documentPoint, Qt::transparent, diameter, hardness, opacity, false, std::nullopt, mode, seed);
}

bool EditorSession::beginBlurStroke(const QPointF &documentPoint, double diameter, double hardness, double opacity, double radius)
{
    if (!std::isfinite(radius)) return false;
    if (!beginBrushStroke(documentPoint, Qt::transparent, diameter, hardness, opacity, false, std::nullopt, 3, 0)) return false;
    brush_->blurRadius = std::clamp(radius, .5, 50.0);
    return true;
}

bool EditorSession::beginWarpStroke(const QPointF &documentPoint, int mode, double diameter, double hardness, double strength)
{
    const Layer *layer = activeLayer();
    if (brush_ || warp_ || mode < 0 || mode > 1 || maskSelected_ || !layer || layer->group || layer->image.isNull()
        || diameter < 2 || diameter > 2000 || hardness < 0 || hardness > 1 || strength <= 0 || strength > 1) return false;
    const auto pixel = documentToPixel(*layer, documentPoint, layer->image.size()); if (!pixel) return false;
    beginEdit(mode == 0 ? QStringLiteral("Liquify") : QStringLiteral("Smudge"));
    QImage original = layer->image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    QImage coverage(original.size(), QImage::Format_Grayscale8); coverage.fill(0);
    warp_ = WarpState{layer->id, mode, diameter, std::min(.98, hardness), strength, *pixel, original, original, coverage, {}};
    if (mode == 1) {
        const double scale = std::max(1e-9, (layer->transform.size.width() / original.width() + layer->transform.size.height() / original.height()) / 2.0);
        const int radius = std::max(1, qCeil(diameter / scale / 2)); const int side = radius * 2 + 1;
        warp_->carried.fill(0, qint64(side) * side * 4); const int cx = qRound(pixel->x()), cy = qRound(pixel->y());
        for (int dy = -radius; dy <= radius; ++dy) for (int dx = -radius; dx <= radius; ++dx) {
            const int x = cx + dx, y = cy + dy; if (!QRect(QPoint(), original.size()).contains(x, y)) continue;
            const uchar *source = original.constScanLine(y) + x * 4; const qint64 target = (qint64(dy + radius) * side + dx + radius) * 4;
            for (int c = 0; c < 4; ++c) warp_->carried[target + c] = char(source[c]);
        }
    }
    return true;
}

static void stampBrushCoverage(QImage &coverage, const QPointF &center, double radius, double hardness)
{
    constexpr int lookupSize = 4096;
    struct FalloffLookup { double radius = -1, hardness = -1; std::array<uchar, lookupSize + 1> values{}; };
    static thread_local FalloffLookup lookup;
    if (lookup.radius != radius || lookup.hardness != hardness) {
        lookup.radius = radius; lookup.hardness = hardness;
        const double edge = std::exp(-2.5), normalization = 1 - edge;
        for (int i = 0; i <= lookupSize; ++i) {
            const double distance = std::sqrt(double(i) / lookupSize);
            double strength = 1;
            if (distance >= 1) strength = 0;
            else if (hardness < 1 && distance > hardness) {
                const double u = (distance - hardness) / (1 - hardness);
                strength = (std::exp(-2.5 * u * u) - edge) / normalization;
            }
            lookup.values[size_t(i)] = uchar(std::clamp(qRound(strength * 255), 0, 255));
        }
    }
    const int left = std::max(0, qFloor(center.x() - radius)), right = std::min(coverage.width() - 1, qCeil(center.x() + radius));
    const int top = std::max(0, qFloor(center.y() - radius)), bottom = std::min(coverage.height() - 1, qCeil(center.y() + radius));
    if (left > right || top > bottom) return;
    const double lookupScale = lookupSize / (radius * radius);
    for (int y = top; y <= bottom; ++y) { uchar *row = coverage.scanLine(y); for (int x = left; x <= right; ++x) {
        const double dx = x + .5 - center.x(), dy = y + .5 - center.y();
        const int sample = int((dx * dx + dy * dy) * lookupScale);
        if (sample >= lookupSize) continue;
        row[x] = std::max(row[x], lookup.values[size_t(std::max(0, sample))]);
    } }
}

static void stampCoverageLine(QImage &coverage, const QPointF &from, const QPointF &to, double radius, double hardness)
{
    const double length = QLineF(from, to).length();
    const int steps = std::max(1, qCeil(length / std::max(1.0, radius * .12)));
    for (int step = 0; step <= steps; ++step)
        stampBrushCoverage(coverage, from + (to - from) * (double(step) / steps), radius, hardness);
}

static QVector<QPointF> smoothBrushCurve(const QPointF &before, const QPointF &start,
                                         const QPointF &end, const QPointF &after)
{
    const auto knot = [](double t, const QPointF &a, const QPointF &b) { return t + std::max(.0001, std::sqrt(QLineF(a, b).length())); };
    const double t0 = 0, t1 = knot(t0, before, start), t2 = knot(t1, start, end), t3 = knot(t2, end, after);
    const auto mix = [](const QPointF &a, const QPointF &b, double ta, double tb, double t) { return a * ((tb - t) / (tb - ta)) + b * ((t - ta) / (tb - ta)); };
    const auto point = [&](double u) {
        if (u <= 0) return start;
        if (u >= 1) return end;
        const double t = t1 + (t2 - t1) * u;
        const QPointF a = mix(before, start, t0, t1, t), b = mix(start, end, t1, t2, t), c = mix(end, after, t2, t3, t);
        return mix(mix(a, b, t0, t2, t), mix(b, c, t1, t3, t), t1, t2, t);
    };
    QVector<QPointF> result{start};
    const auto subdivide = [&](auto &&self, const QPointF &a, const QPointF &b, double lo, double hi, int depth) -> void {
        const QPointF delta = b - a; const double lengthSquared = QPointF::dotProduct(delta, delta);
        const auto error = [&](const QPointF &p) { const double u = lengthSquared > 0 ? std::clamp(QPointF::dotProduct(p - a, delta) / lengthSquared, 0.0, 1.0) : 0; return QLineF(p, a + delta * u).length(); };
        const double mid = (lo + hi) / 2; const QPointF center = point(mid);
        const double deviation = std::max({error(center), error(point((lo + mid) / 2)), error(point((mid + hi) / 2))});
        if (deviation <= .2 || depth >= 10) { result.push_back(b); return; }
        self(self, a, center, lo, mid, depth + 1); self(self, center, b, mid, hi, depth + 1);
    };
    subdivide(subdivide, start, end, 0, 1, 0);
    return result;
}

static QRect brushPathBounds(const QPointF &from, const QPointF &to, double radius, const QSize &size)
{
    const double reach = radius + 1;
    return QRect(QPoint(qFloor(std::min(from.x(), to.x()) - reach), qFloor(std::min(from.y(), to.y()) - reach)),
                 QPoint(qCeil(std::max(from.x(), to.x()) + reach), qCeil(std::max(from.y(), to.y()) + reach)))
        .intersected(QRect(QPoint(), size));
}

static void restoreCoverage(QImage &destination, const QImage &settled, const QRect &rect)
{
    if (rect.isEmpty()) return;
    for (int y = rect.top(); y <= rect.bottom(); ++y)
        std::copy_n(settled.constScanLine(y) + rect.left(), rect.width(), destination.scanLine(y) + rect.left());
}

std::optional<QPointF> EditorSession::smoothedBrushPoint(const QPointF &point)
{
    if (!brushSmoothingEnabled_ || brushSmoothing_ <= 0.0 || !brushAnchor_) {
        return point;
    }
    const double radius = brushSmoothing_ / std::max(0.01, viewportZoom_);
    const QPointF delta = point - *brushAnchor_;
    const double distance = std::hypot(delta.x(), delta.y());
    if (distance <= radius) {
        return std::nullopt;
    }
    const double step = (distance - radius) / distance;
    const QPointF moved = *brushAnchor_ + delta * step;
    brushAnchor_ = moved;
    return moved;
}

void EditorSession::continueBrushStroke(const QPointF &documentPoint)
{
    if (warp_) {
        continueBrushStrokeInternal(documentPoint);
        return;
    }
    if (!brush_ || !document_) return;

    brushPointer_ = documentPoint;
    if (brushSmoothingEnabled_) {
        const auto painted = smoothedBrushPoint(documentPoint);
        if (!painted) {
            return;
        }
        continueBrushStrokeInternal(*painted);
    } else {
        brushAnchor_ = documentPoint;
        continueBrushStrokeInternal(documentPoint);
    }
}

void EditorSession::continueBrushStrokeInternal(const QPointF &documentPoint)
{
    if (warp_ && document_) {
        const int index = indexOf(warp_->layerId); if (index < 0) return;
        Layer &layer = document_->layers[index]; if (layer.image.format() != warp_->original.format() || layer.image.size() != warp_->original.size()) layer.image = warp_->original; QImage &working = layer.image; const auto target = documentToPixel(layer, documentPoint, warp_->original.size()); if (!target) return;
        const double scale = std::max(1e-9, (layer.transform.size.width() / warp_->original.width() + layer.transform.size.height() / warp_->original.height()) / 2.0);
        const double radiusValue = std::max(1.0, warp_->diameter / scale / 2.0); const int radius = qCeil(radiusValue);
        const double distance = QLineF(warp_->previousPixel, *target).length();
        // Smudge dabs come about a canvas pixel apart (a little more on a brush over 200 wide), so the steps run
        // together into one smear rather than a row of echoes; Liquify's come a twentieth of the radius apart.
        const double spacing = warp_->mode == 1 ? std::max(1.0, std::max(1.0, warp_->diameter * .005) / scale) : std::max(1.0, radiusValue * .05);
        if (distance < spacing) return;
        const int steps = qCeil(distance / spacing);
        QPointF previous = warp_->previousPixel;
        constexpr int tileSide = 64, falloffSize = 4096;
        const QImage &source = warp_->original;
        const QSize bounds = working.size();
        // The dab's falloff by squared distance from its centre, looked up rather than worked out per pixel.
        std::array<float, falloffSize + 1> falloff{};
        for (int i = 0; i <= falloffSize; ++i) {
            const double u = std::sqrt(double(i) / falloffSize);
            if (u >= 1) falloff[i] = 0; else if (u <= warp_->hardness) falloff[i] = 1;
            else { const double t = (1 - u) / (1 - warp_->hardness); falloff[i] = float(t * t * (3 - 2 * t)); }
        }
        const double falloffScale = falloffSize / (radiusValue * radiusValue);
        const auto span = [&](int dy) { return int(std::sqrt(std::max(0.0, radiusValue * radiusValue - double(dy) * dy))); };
        for (int step = 1; step <= steps; ++step) {
            const QPointF center = warp_->previousPixel + (*target - warp_->previousPixel) * (double(step) / steps);
            const int cx = qRound(center.x()), cy = qRound(center.y());
            if (warp_->mode == 1) {
                // What was under the brush at the last dab, laid down here at the smudge's strength, and carried on
                // in turn: nothing older, which would stamp a ghost copy of it at every dab.
                const qint64 side = radius * 2 + 1; const float strength = float(warp_->strength);
                for (int dy = std::max(-radius, -cy); dy <= radius && cy + dy < bounds.height(); ++dy) {
                    const int reach = span(dy); uchar *line = working.scanLine(cy + dy);
                    for (int dx = std::max(-reach, -cx); dx <= reach && cx + dx < bounds.width(); ++dx) {
                        const float w = falloff[std::min(falloffSize, int((dx * dx + dy * dy) * falloffScale))] * strength; if (w <= 0) continue;
                        uchar *out = line + (cx + dx) * 4; char *held = warp_->carried.data() + ((dy + radius) * side + dx + radius) * 4;
                        for (int c = 0; c < 4; ++c) { const float under = out[c], painted = under + (uchar(held[c]) - under) * w; out[c] = uchar(std::clamp(int(painted + .5f), 0, 255)); held[c] = char(out[c]); }
                    }
                }
            } else {
                // Each dab moves the offsets, and the pixels under it are drawn once from the untouched layer, so a
                // long stroke does not soften them a little with every dab.
                const QPointF movement = (center - previous) * warp_->strength;
                const int margin = qCeil(std::hypot(movement.x(), movement.y())) + 2;
                const QRect box = QRect(cx - radius, cy - radius, radius * 2 + 1, radius * 2 + 1).intersected(QRect(QPoint(), bounds));
                if (!box.isEmpty()) {
                    const QRect area = box.adjusted(-margin, -margin, margin, margin).intersected(QRect(QPoint(), bounds));
                    const int areaWidth = area.width();
                    QVector<float> old(qsizetype(areaWidth) * area.height() * 2, 0.0f);
                    for (int ty = area.top() / tileSide; ty <= area.bottom() / tileSide; ++ty) for (int tx = area.left() / tileSide; tx <= area.right() / tileSide; ++tx) {
                        const auto tile = warp_->offsets.constFind((qint64(ty) << 32) | tx); if (tile == warp_->offsets.constEnd()) continue;
                        const QRect part = QRect(tx * tileSide, ty * tileSide, tileSide, tileSide).intersected(area);
                        for (int y = part.top(); y <= part.bottom(); ++y)
                            std::copy_n(tile->constData() + ((y - ty * tileSide) * tileSide + part.left() - tx * tileSide) * 2, part.width() * 2,
                                        old.data() + (qsizetype(y - area.top()) * areaWidth + part.left() - area.left()) * 2);
                    }
                    for (int y = box.top(); y <= box.bottom(); ++y) {
                        const int dy = y - cy, reach = span(dy); float *store = nullptr; int storeTile = -1;
                        uchar *out = working.scanLine(y);
                        for (int x = std::max(box.left(), cx - reach); x <= std::min(box.right(), cx + reach); ++x) {
                            const double w = falloff[std::min(falloffSize, int(((x - cx) * (x - cx) + dy * dy) * falloffScale))]; if (w <= 0) continue;
                            const double qx = std::clamp(x - movement.x()*w, double(area.left()), double(area.right())), qy = std::clamp(y - movement.y()*w, double(area.top()), double(area.bottom()));
                            const int ax = qFloor(qx) - area.left(), ay = qFloor(qy) - area.top(), bx = std::min(ax + 1, areaWidth - 1), by = std::min(ay + 1, area.height() - 1); const double fx = qx - qFloor(qx), fy = qy - qFloor(qy);
                            const float *p00 = old.constData() + (qsizetype(ay) * areaWidth + ax) * 2, *p10 = old.constData() + (qsizetype(ay) * areaWidth + bx) * 2;
                            const float *p01 = old.constData() + (qsizetype(by) * areaWidth + ax) * 2, *p11 = old.constData() + (qsizetype(by) * areaWidth + bx) * 2;
                            double offset[2];
                            for (int k = 0; k < 2; ++k) offset[k] = (p00[k]*(1-fx) + p10[k]*fx)*(1-fy) + (p01[k]*(1-fx) + p11[k]*fx)*fy;
                            offset[0] -= movement.x()*w; offset[1] -= movement.y()*w;
                            if (x / tileSide != storeTile) {
                                storeTile = x / tileSide;
                                QVector<float> &tile = warp_->offsets[(qint64(y / tileSide) << 32) | storeTile];
                                if (tile.isEmpty()) tile.fill(0.0f, tileSide * tileSide * 2);
                                store = tile.data() + (y % tileSide) * tileSide * 2 - storeTile * tileSide * 2;
                            }
                            store[x * 2] = float(offset[0]); store[x * 2 + 1] = float(offset[1]);
                            const double sx = std::clamp(x + offset[0], 0.0, double(bounds.width()-1)), sy = std::clamp(y + offset[1], 0.0, double(bounds.height()-1));
                            const int x0=qFloor(sx), y0=qFloor(sy), x1=std::min(x0+1,bounds.width()-1), y1=std::min(y0+1,bounds.height()-1); const double gx=sx-x0, gy=sy-y0;
                            const uchar *a=source.constScanLine(y0)+x0*4,*b=source.constScanLine(y0)+x1*4,*c=source.constScanLine(y1)+x0*4,*d=source.constScanLine(y1)+x1*4;
                            for(int channel=0;channel<4;++channel) out[x*4+channel]=uchar(std::clamp(qRound((a[channel]*(1-gx)+b[channel]*gx)*(1-gy)+(c[channel]*(1-gx)+d[channel]*gx)*gy),0,255));
                        }
                    }
                }
            }
            stampBrushCoverage(warp_->coverage, center, radiusValue + 2, 1); previous = center;
        }
        warp_->previousPixel = *target; warp_->changed = true; return;
    }
    if (!brush_ || !document_) return;
    const int index = indexOf(brush_->layerId);
    if (index < 0) return;
    Layer &layer = document_->layers[index];
    const auto current = documentToPixel(layer, documentPoint, brush_->original.size());
    if (!current) return;
    const double scale = std::max(1e-9, (layer.transform.size.width() / brush_->original.width()
                                      + layer.transform.size.height() / brush_->original.height()) / 2.0);
    const double radius = std::max(.5, brush_->diameter / scale / 2.0);
    QRect dirty = brush_->dirtyPixels;
    brush_->dirtyPixels = {};
    if (brush_->samples.isEmpty()) {
        brush_->samples.push_back(*current);
        stampBrushCoverage(brush_->settledCoverage, *current, radius, brush_->hardness);
        stampBrushCoverage(brush_->coverage, *current, radius, brush_->hardness);
        dirty |= brushPathBounds(*current, *current, radius, brush_->coverage.size());
    } else if (brush_->samples.constLast() != *current) {
        dirty |= brush_->tailBounds;
        restoreCoverage(brush_->coverage, brush_->settledCoverage, brush_->tailBounds);
        brush_->tailBounds = {};
        brush_->samples.push_back(*current);
        const int count = brush_->samples.size();
        if (count >= 3) {
            const QPointF before = brush_->samples.at(std::max(0, count - 4));
            const QVector<QPointF> curve = smoothBrushCurve(before, brush_->samples.at(count - 3),
                                                            brush_->samples.at(count - 2), brush_->samples.at(count - 1));
            for (int i = 1; i < curve.size(); ++i) {
                stampCoverageLine(brush_->settledCoverage, curve.at(i - 1), curve.at(i), radius, brush_->hardness);
                stampCoverageLine(brush_->coverage, curve.at(i - 1), curve.at(i), radius, brush_->hardness);
                dirty |= brushPathBounds(curve.at(i - 1), curve.at(i), radius, brush_->coverage.size());
            }
        }
        const QPointF tailStart = brush_->samples.at(count - 2), tailEnd = brush_->samples.at(count - 1);
        stampCoverageLine(brush_->coverage, tailStart, tailEnd, radius, brush_->hardness);
        brush_->tailBounds = brushPathBounds(tailStart, tailEnd, radius, brush_->coverage.size());
        dirty |= brush_->tailBounds;
        if (brush_->samples.size() > 5) brush_->samples.removeFirst();
    }
    if (dirty.isEmpty()) return;
    const QImage &selection = brush_->selectionCoverage;
    bool changedPixels = false;
    if (brush_->mask) {
        QImage &result = layer.mask;
        for (int y = dirty.top(); y <= dirty.bottom(); ++y) { uchar *out = result.scanLine(y); const uchar *coverage = brush_->coverage.constScanLine(y), *clip = selection.isNull() ? nullptr : selection.constScanLine(y); for (int x = dirty.left(); x <= dirty.right(); ++x) {
            const int alpha = qRound(coverage[x] * brush_->opacity * (clip ? clip[x] : 255) / 255.0);
            const uchar value = uchar((int(brush_->original.constScanLine(y)[x]) * (255 - alpha) + brush_->maskValue * alpha + 127) / 255);
            changedPixels |= value != brush_->original.constScanLine(y)[x]; out[x] = value;
        } }
    } else if (brush_->healMode < 0) {
        QImage &result = layer.image;
        for (int y = dirty.top(); y <= dirty.bottom(); ++y) { uchar *out = result.scanLine(y); const uchar *original = brush_->original.constScanLine(y), *coverage = brush_->coverage.constScanLine(y), *clip = selection.isNull() ? nullptr : selection.constScanLine(y); for (int x = dirty.left(); x <= dirty.right(); ++x) {
            const int alpha = qRound(coverage[x] * brush_->opacity * (clip ? clip[x] : 255) / 255.0), inverse = 255 - alpha; uchar *pixel = out + x * 4; const uchar *base = original + x * 4;
            if (brush_->erasing) for (int c = 0; c < 4; ++c) pixel[c] = uchar((int(base[c]) * inverse + 127) / 255);
            else if (brush_->clone) {
                const int sx = qFloor(x + brush_->cloneOffsetPixel.x()), sy = qFloor(y + brush_->cloneOffsetPixel.y());
                if (sx < 0 || sx >= brush_->cloneSample.width() || sy < 0 || sy >= brush_->cloneSample.height()) { for (int c = 0; c < 4; ++c) pixel[c] = base[c]; continue; }
                const uchar *sample = brush_->cloneSample.constScanLine(sy) + sx * 4;
                const int sourceAlpha = (int(sample[3]) * alpha + 127) / 255, sourceInverse = 255 - sourceAlpha;
                for (int c = 0; c < 3; ++c) pixel[c] = uchar(std::min(255, (int(sample[c]) * alpha + 127) / 255 + (int(base[c]) * sourceInverse + 127) / 255));
                pixel[3] = uchar(std::min(255, sourceAlpha + (int(base[3]) * sourceInverse + 127) / 255));
            } else { pixel[0] = uchar(std::min(255, brush_->color.red() * alpha / 255 + (int(base[0]) * inverse + 127) / 255)); pixel[1] = uchar(std::min(255, brush_->color.green() * alpha / 255 + (int(base[1]) * inverse + 127) / 255)); pixel[2] = uchar(std::min(255, brush_->color.blue() * alpha / 255 + (int(base[2]) * inverse + 127) / 255)); pixel[3] = uchar(std::min(255, alpha + (int(base[3]) * inverse + 127) / 255)); }
            changedPixels |= !std::equal(pixel, pixel + 4, base);
        } }
        if (changedPixels) rasterizeLayer(layer);
    }
    brush_->previousPixel = *current;
    brush_->changed |= changedPixels;
}

bool EditorSession::endBrushStroke()
{
    if (warp_) {
        const int index = document_ ? indexOf(warp_->layerId) : -1;
        if (index >= 0) {
            Layer &layer = document_->layers[index]; const QImage selection = selectionCoverageForLayer(layer, warp_->original.size(), document_->selection); QImage result = warp_->original;
            for (int y=0;y<result.height();++y) { uchar *out=result.scanLine(y); const uchar *changed=layer.image.constScanLine(y),*coverage=warp_->coverage.constScanLine(y),*clip=selection.constScanLine(y); for(int x=0;x<result.width();++x) {
                const int amount=(int(coverage[x])*clip[x]+127)/255, inverse=255-amount; for(int c=0;c<4;++c) out[x*4+c]=uchar((int(out[x*4+c])*inverse+int(changed[x*4+c])*amount+127)/255);
            } }
            layer.image=result; warp_->changed=result!=warp_->original; if (warp_->changed) rasterizeLayer(layer);
        }
        const bool changed=warp_->changed; warp_.reset(); endEdit(); return changed;
    }
    if (!brush_) return false;

    // Smoothing leaves the brush short of the pointer; the stroke ends where the hand did.
    if (brushSmoothingEnabled_ && brushSmoothing_ > 0.0 && brushPointer_ && brushAnchor_ && *brushPointer_ != *brushAnchor_) {
        continueBrushStrokeInternal(*brushPointer_);
        brushAnchor_ = brushPointer_;
    }
    brushAnchor_.reset();
    brushPointer_.reset();
    brushSmoothingEnabled_ = false;

    if (document_ && brush_->samples.size() >= 2) {
        const int index = indexOf(brush_->layerId);
        if (index >= 0) {
            const Layer &layer = document_->layers.at(index);
            const double scale = std::max(1e-9, (layer.transform.size.width() / brush_->original.width()
                                              + layer.transform.size.height() / brush_->original.height()) / 2.0);
            const double radius = std::max(.5, brush_->diameter / scale / 2.0);
            const int count = brush_->samples.size();
            QRect dirty = brush_->tailBounds;
            restoreCoverage(brush_->coverage, brush_->settledCoverage, brush_->tailBounds);
            const QVector<QPointF> curve = smoothBrushCurve(brush_->samples.at(std::max(0, count - 3)),
                brush_->samples.at(count - 2), brush_->samples.at(count - 1), brush_->samples.at(count - 1));
            for (int i = 1; i < curve.size(); ++i) {
                stampCoverageLine(brush_->settledCoverage, curve.at(i - 1), curve.at(i), radius, brush_->hardness);
                stampCoverageLine(brush_->coverage, curve.at(i - 1), curve.at(i), radius, brush_->hardness);
                dirty |= brushPathBounds(curve.at(i - 1), curve.at(i), radius, brush_->coverage.size());
            }
            brush_->tailBounds = {};
            brush_->dirtyPixels |= dirty;
            const QPointF lastDocument = pixelToDocument(layer.transform, brush_->original.size()).map(brush_->samples.constLast());
            continueBrushStrokeInternal(lastDocument);
        }
    }
    if (brush_->healMode >= 0 && document_) {
        const int index = indexOf(brush_->layerId);
        if (index >= 0) {
            Layer &layer = document_->layers[index];
            QImage coverage = brush_->coverage;
            const QImage &selection = brush_->selectionCoverage;
            if (!selection.isNull()) for (int y = 0; y < coverage.height(); ++y) { uchar *out = coverage.scanLine(y); const uchar *clip = selection.constScanLine(y); for (int x = 0; x < coverage.width(); ++x) out[x] = uchar((int(out[x]) * clip[x] + 127) / 255); }
            if (brush_->healMode == 3) {
                // Only the pieces the brush reached are softened, at the layer's own resolution: the canvas radius
                // carried into its pixels, within a budget of several canvases (past it the sample is made coarser).
                QImage result = brush_->original;
                int left = coverage.width(), top = coverage.height(), right = -1, bottom = -1;
                for (int y = 0; y < coverage.height(); ++y) { const uchar *row = coverage.constScanLine(y); for (int x = 0; x < coverage.width(); ++x) if (row[x]) { left = std::min(left, x); right = std::max(right, x); top = std::min(top, y); bottom = y; } }
                if (right >= left) {
                    const double perPixel = std::max(1e-9, (layer.transform.size.width() / brush_->original.width() + layer.transform.size.height() / brush_->original.height()) / 2.0);
                    const double sigma = std::clamp(brush_->blurRadius, .5, 50.0) / perPixel;
                    const int margin = qCeil(3 * sigma);
                    const QRect region = QRect(QPoint(left, top), QPoint(right, bottom)).adjusted(-margin, -margin, margin, margin).intersected(QRect(QPoint(), brush_->original.size()));
                    const qint64 budget = std::max<qint64>(4 * qint64(document_->canvasSize.width()) * document_->canvasSize.height(), 4000000);
                    const double shrink = std::max(1.0, std::sqrt(double(qint64(region.width()) * region.height()) / budget));
                    QImage piece = brush_->original.copy(region), soft;
                    if (shrink > 1.0) {
                        const QSize coarse(std::max(1, qRound(region.width() / shrink)), std::max(1, qRound(region.height() / shrink)));
                        soft = RasterOperations::gaussianBlur(piece.scaled(coarse, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), sigma / shrink)
                                   .scaled(region.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    } else soft = RasterOperations::gaussianBlur(piece, sigma);
                    for (int y = top; y <= bottom; ++y) { uchar *out = result.scanLine(y); const uchar *base = brush_->original.constScanLine(y), *mask = coverage.constScanLine(y), *blurred = soft.constScanLine(y - region.top()); for (int x = left; x <= right; ++x) {
                        const int amount = qRound(mask[x] * brush_->opacity), inverse = 255 - amount; const uchar *sample = blurred + (x - region.left()) * 4;
                        for (int c = 0; c < 4; ++c) out[x * 4 + c] = uchar((int(base[x * 4 + c]) * inverse + int(sample[c]) * amount + 127) / 255);
                    } }
                }
                layer.image = result; brush_->changed = result != brush_->original; if (brush_->changed) rasterizeLayer(layer);
            } else {
                const auto healed = RasterOperations::spotHeal(brush_->original, coverage, brush_->opacity, brush_->healMode, brush_->effectSeed);
                if (healed) { layer.image = *healed; brush_->changed = layer.image != brush_->original; if (brush_->changed) rasterizeLayer(layer); }
            }
        }
    }
    const bool changed = brush_->changed;
    brush_.reset();
    endEdit();
    return changed;
}

bool EditorSession::cancelBrushStroke()
{
    brushAnchor_.reset();
    brushPointer_.reset();
    brushSmoothingEnabled_ = false;
    if (!document_ || (!brush_ && !warp_)) return false;
    if (brush_) {
        const int index = indexOf(brush_->layerId);
        if (index >= 0) {
            if (brush_->mask) document_->layers[index].mask = brush_->original;
            else document_->layers[index].image = brush_->original;
        }
        brush_.reset();
    }
    if (warp_) {
        const int index = indexOf(warp_->layerId);
        if (index >= 0) document_->layers[index].image = warp_->original;
        warp_.reset();
    }
    endEdit();
    return true;
}

bool EditorSession::addLayerMask(bool revealing, bool useSelection)
{
    const Layer *layer = activeLayer();
    if (!document_ || !layer || !layer->mask.isNull()) return false;
    const int index = indexOf(layer->id);
    QImage mask;
    const bool fromSelection = useSelection && document_->selection.has_value();
    if (fromSelection) {
        const int width = layer->image.isNull() ? std::max(1, qRound(layer->transform.size.width())) : layer->image.width();
        const int height = layer->image.isNull() ? std::max(1, qRound(layer->transform.size.height())) : layer->image.height();
        if (qint64(width) * height > 100000000LL) return false;
        mask = QImage(width, height, QImage::Format_Grayscale8);
        const QImage selection = document_->selection->convertToFormat(QImage::Format_Grayscale8);
        QTransform toDocument;
        toDocument.translate(layer->transform.center().x(), layer->transform.center().y());
        toDocument.rotate(layer->transform.rotation);
        toDocument.scale((layer->transform.flipX ? -1 : 1) * layer->transform.size.width() / width,
                         (layer->transform.flipY ? -1 : 1) * layer->transform.size.height() / height);
        toDocument.translate(-width / 2.0, -height / 2.0);
        for (int y = 0; y < height; ++y) {
            uchar *out = mask.scanLine(y);
            for (int x = 0; x < width; ++x) {
                const QPointF mapped = toDocument.map(QPointF(x + .5, y + .5));
                const QPoint point(qFloor(mapped.x()), qFloor(mapped.y()));
                const int coverage = QRect(QPoint(), selection.size()).contains(point) ? selection.constScanLine(point.y())[point.x()] : 0;
                out[x] = uchar(revealing ? coverage : 255 - coverage);   // mac b3419ab: the button reveals the selection
            }
        }
    } else { mask = QImage(1, 1, QImage::Format_Grayscale8); mask.fill(revealing ? 255 : 0); }
    beginEdit(fromSelection ? (revealing ? QStringLiteral("Reveal Selection") : QStringLiteral("Hide Selection"))
                            : revealing ? QStringLiteral("Add Reveal-All Mask") : QStringLiteral("Add Hide-All Mask"));
    document_->layers[index].mask = mask;
    if (fromSelection) document_->selection.reset();
    maskSelected_ = true;
    endEdit(); return true;
}

bool EditorSession::toggleLayerMask()
{
    const Layer *layer = activeLayer(); if (!layer || layer->mask.isNull()) return false;
    const int index = indexOf(layer->id);
    beginEdit(layer->maskEnabled ? QStringLiteral("Disable Layer Mask") : QStringLiteral("Enable Layer Mask"));
    document_->layers[index].maskEnabled = !document_->layers.at(index).maskEnabled;
    endEdit(); return true;
}

bool EditorSession::toggleMaskLink()
{
    const Layer *layer=activeLayer();if(!layer||layer->mask.isNull())return false;const int index=indexOf(layer->id);
    beginEdit(layer->maskLinked?QStringLiteral("Unlink Layer Mask"):QStringLiteral("Link Layer Mask"));document_->layers[index].maskLinked=!layer->maskLinked;endEdit();return true;
}

bool EditorSession::deleteLayerMask()
{
    const Layer *layer = activeLayer(); if (!layer || layer->mask.isNull()) return false;
    const int index = indexOf(layer->id);
    beginEdit(QStringLiteral("Delete Layer Mask")); document_->layers[index].mask = {}; document_->layers[index].maskPlacement.reset(); maskSelected_ = false; endEdit(); return true;
}

bool EditorSession::invertLayerMask()
{
    const Layer *layer = activeLayer(); if (!layer || layer->mask.isNull()) return false;
    const int index = indexOf(layer->id);
    QImage mask = layer->mask.convertToFormat(QImage::Format_Grayscale8);
    for (int y = 0; y < mask.height(); ++y) { uchar *row = mask.scanLine(y); for (int x = 0; x < mask.width(); ++x) row[x] = uchar(255 - row[x]); }
    beginEdit(QStringLiteral("Invert")); document_->layers[index].mask = mask; endEdit(); return true;
}

bool EditorSession::canCopyLayerMask(const QUuid &sourceId, const QUuid &targetId) const
{
    if (!document_ || sourceId == targetId || !canEditLayers()) return false;
    const int srcIdx = indexOf(sourceId);
    const int dstIdx = indexOf(targetId);
    if (srcIdx < 0 || dstIdx < 0) return false;
    const Layer &srcLayer = document_->layers.at(srcIdx);
    const Layer &dstLayer = document_->layers.at(dstIdx);
    return !srcLayer.mask.isNull() && !dstLayer.group;
}

bool EditorSession::copyLayerMask(const QUuid &sourceId, const QUuid &targetId)
{
    if (!canCopyLayerMask(sourceId, targetId)) return false;
    const int srcIdx = indexOf(sourceId);
    const int dstIdx = indexOf(targetId);
    if (srcIdx < 0 || dstIdx < 0) return false;
    const bool wasNull = document_->layers.at(dstIdx).mask.isNull();
    beginEdit(wasNull ? QStringLiteral("Copy Layer Mask") : QStringLiteral("Replace Layer Mask"));
    const Layer &srcLayer = document_->layers.at(srcIdx);
    Layer &dstLayer = document_->layers[dstIdx];
    dstLayer.mask = srcLayer.mask;
    dstLayer.maskPlacement = srcLayer.maskPlacement.value_or(srcLayer.transform);
    dstLayer.maskEnabled = srcLayer.maskEnabled;
    dstLayer.maskLinked = srcLayer.maskLinked;
    selectLayer(targetId);
    maskSelected_ = true;
    endEdit();
    return true;
}

static QImage placedBinarySelection(const QSize &canvasSize, const QImage &source, const LayerTransform &placement, bool selectDark)
{
    const QImage input = source.convertToFormat(selectDark ? QImage::Format_Grayscale8 : QImage::Format_RGBA8888_Premultiplied);
    QImage binary(input.size(), QImage::Format_Grayscale8); binary.fill(0);
    for (int y = 0; y < input.height(); ++y) {
        uchar *out = binary.scanLine(y); const uchar *row = input.constScanLine(y);
        for (int x = 0; x < input.width(); ++x) out[x] = selectDark ? (row[x] < 128 ? 255 : 0) : (row[x * 4 + 3] >= 128 ? 255 : 0);
    }
    QImage selection(canvasSize, QImage::Format_Grayscale8); selection.fill(0);
    QPainter painter(&selection); painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.translate(placement.center()); painter.rotate(placement.rotation);
    painter.scale(placement.flipX ? -1 : 1, placement.flipY ? -1 : 1);
    painter.drawImage(QRectF(-placement.size.width() / 2, -placement.size.height() / 2,
                            placement.size.width(), placement.size.height()), binary);
    painter.end();
    return selection;
}

bool EditorSession::loadMaskAsSelection(SelectionMode mode, const std::optional<QUuid> &id)
{
    if (!document_ || (mode == SelectionMode::Subtract && !document_->selection)) return false;
    const Layer *layer = id ? nullptr : activeLayer();
    if (id) for (const Layer &candidate : document_->layers) if (candidate.id == *id) { layer = &candidate; break; }
    if (!layer || layer->mask.isNull()) return false;
    const LayerTransform placement = layer->maskPlacement.value_or(layer->transform);
    const QImage traced = placedBinarySelection(document_->canvasSize, layer->mask, placement, true);
    if (!maskHasCoverage(traced)) return false;
    const QImage result = combineSelection(document_->selection, traced, mode);
    beginEdit(QStringLiteral("Load Mask Selection")); document_->selection = result; endEdit(); return true;
}

bool EditorSession::loadLayerAsSelection(SelectionMode mode, const std::optional<QUuid> &id)
{
    if (!document_ || (mode == SelectionMode::Subtract && !document_->selection)) return false;
    const Layer *layer = id ? nullptr : activeLayer();
    if (id) for (const Layer &candidate : document_->layers) if (candidate.id == *id) { layer = &candidate; break; }
    if (!layer || layer->group || layer->image.isNull()) return false;
    const QImage traced = placedBinarySelection(document_->canvasSize, layer->image, layer->transform, false);
    if (!maskHasCoverage(traced)) return false;
    const QImage result = combineSelection(document_->selection, traced, mode);
    beginEdit(QStringLiteral("Load Layer Selection")); document_->selection = result; endEdit(); return true;
}

void EditorSession::selectMaskTarget(bool mask)
{
    const Layer *layer = activeLayer(); maskSelected_ = mask && layer && !layer->mask.isNull();
}

std::optional<QUuid> EditorSession::maskAloneLayerId() const
{
    const Layer *layer = activeLayer();
    if (maskAlone_ && maskSelected_ && layer && layer->id == *maskAlone_ && !layer->mask.isNull()) return maskAlone_;
    return std::nullopt;
}

void EditorSession::toggleMaskAlone(const QUuid &id)
{
    const bool showing = maskAloneLayerId() == std::optional<QUuid>(id);
    selectLayer(id);
    selectMaskTarget(true);
    const Layer *layer = activeLayer();
    if (!layer || layer->id != id || !maskSelected_) { maskAlone_.reset(); return; }
    if (showing) maskAlone_.reset(); else maskAlone_ = id;
}

bool EditorSession::canLinkMask(const QUuid &source, const QUuid &target) const
{
    if (!document_ || source == target) return false;
    const Layer *sourceLayer = nullptr, *targetLayer = nullptr;
    for (const Layer &layer : document_->layers) {
        if (layer.id == source) sourceLayer = &layer;
        if (layer.id == target) targetLayer = &layer;
    }
    if (!sourceLayer || !targetLayer || sourceLayer->group || targetLayer->group || !sourceLayer->adjustment.isEmpty()) return false;
    QSet<QUuid> visited{target};
    const Layer *current = sourceLayer;
    while (current) {
        if (visited.contains(current->id)) return false;
        visited.insert(current->id);
        if (!current->maskSourceId) break;
        const QUuid next = *current->maskSourceId;
        current = nullptr;
        for (const Layer &layer : document_->layers) if (layer.id == next) { current = &layer; break; }
        if (!current) return false;
    }
    return true;
}

bool EditorSession::linkMask(const QUuid &source, const QUuid &target)
{
    if (!canLinkMask(source, target)) return false;
    const int index = indexOf(target); if (document_->layers.at(index).maskSourceId == source) return true;
    beginEdit(QStringLiteral("Create Clipping Mask")); document_->layers[index].maskSourceId = source; endEdit(); return true;
}

bool EditorSession::toggleClippingMask(const QUuid &target)
{
    if (!document_) return false;
    const int targetIndex = indexOf(target); if (targetIndex < 0 || document_->layers.at(targetIndex).group) return false;
    const Layer targetLayer = document_->layers.at(targetIndex);
    if (targetLayer.maskSourceId) {
        const QUuid source = *targetLayer.maskSourceId;
        beginEdit(QStringLiteral("Release Clipping Mask"));
        for (int i = targetIndex; i < document_->layers.size(); ++i) {
            Layer &layer = document_->layers[i];
            if (i != targetIndex && (layer.parentId != targetLayer.parentId || layer.maskSourceId != source)) break;
            layer.maskSourceId.reset();
        }
        endEdit(); return true;
    }
    int below = -1;
    for (int i = targetIndex - 1; i >= 0; --i) if (document_->layers.at(i).parentId == targetLayer.parentId) { below = i; break; }
    if (below < 0 || document_->layers.at(below).group) return false;
    const QUuid source = document_->layers.at(below).maskSourceId.value_or(document_->layers.at(below).id);
    return linkMask(source, target);
}

static bool validDocumentSize(const QSize &size)
{
    return size.width() >= 1 && size.width() <= 30000 && size.height() >= 1 && size.height() <= 30000;
}

void EditorSession::translateCanvas(const QSize &newSize, const QPointF &offset)
{
    if (!document_) return;
    for (Layer &layer : document_->layers) {
        layer.transform.origin += offset;
        if (layer.maskPlacement) {
            layer.maskPlacement->origin += offset;
        }
    }
    for (CanvasGuide &guide : document_->guides) {
        guide = guide.offset(offset.x(), offset.y());
    }
    if (document_->selection) {
        QImage moved(newSize, QImage::Format_Grayscale8);
        moved.fill(0);
        QPainter painter(&moved);
        painter.drawImage(offset, *document_->selection);
        painter.end();
        document_->selection = moved;
    }
    document_->canvasSize = newSize;
}

bool EditorSession::resizeCanvas(const QSize &size, int anchor, const std::optional<QColor> &extension)
{
    if (!document_ || !validDocumentSize(size) || anchor < 0 || anchor > 8) return false;
    const QSize old = document_->canvasSize;
    const QPointF offset(std::floor(double(size.width() - old.width()) * (anchor % 3) / 2.0),
                         std::floor(double(size.height() - old.height()) * (anchor / 3) / 2.0));
    if (size == old && offset.isNull()) return false;
    beginEdit(QStringLiteral("Canvas Size"));
    translateCanvas(size, offset);
    if (extension && (size.width() > old.width() || size.height() > old.height())) {
        QImage image(size, QImage::Format_RGBA8888_Premultiplied); image.fill(*extension);
        QPainter painter(&image); painter.setCompositionMode(QPainter::CompositionMode_Clear);
        painter.fillRect(QRectF(offset, old), Qt::transparent); painter.end();
        Layer layer; layer.id = QUuid::createUuid(); layer.name = QStringLiteral("Canvas Extension");
        layer.image = image; layer.transform.size = size;
        document_->layers.prepend(layer);
    }
    endEdit(); return true;
}

bool EditorSession::crop(const QRect &documentRect)
{
    if (!document_ || !validDocumentSize(documentRect.size()) || std::abs(documentRect.x()) > 1000000
        || std::abs(documentRect.y()) > 1000000) return false;
    const QPointF offset(-documentRect.x(), -documentRect.y());
    beginEdit(QStringLiteral("Crop"));
    translateCanvas(documentRect.size(), offset);
    endEdit(); return true;
}

bool EditorSession::trim(const TrimOptions &options)
{
    if (!document_ || !options.trimsAny()) return false;
    const QImage composite = LayerRenderer::flattened(*document_);
    const auto rectOpt = ImageTrim::calculateTrimRect(composite, options);
    if (!rectOpt) return false;
    const QRect rect = *rectOpt;
    if (rect.x() == 0 && rect.y() == 0 && rect.size() == document_->canvasSize) {
        return false;
    }

    if (!validDocumentSize(rect.size()) || std::abs(rect.x()) > 1000000 || std::abs(rect.y()) > 1000000) {
        return false;
    }

    const QPointF offset(-rect.x(), -rect.y());
    beginEdit(QStringLiteral("Trim"));
    translateCanvas(rect.size(), offset);
    endEdit();
    return true;
}

std::optional<QRect> EditorSession::selectionBounds() const
{
    if (!document_ || !document_->selection) return std::nullopt;
    const QImage mask = document_->selection->convertToFormat(QImage::Format_Grayscale8);
    int left = mask.width(), top = mask.height(), right = -1, bottom = -1;
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *row = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            if (row[x] > 0) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    if (right < left) return std::nullopt;
    return QRect(left, top, right - left + 1, bottom - top + 1);
}

static std::array<QPointF, 4> layerCorners(const LayerTransform &value)
{
    QTransform transform; transform.translate(value.center().x(), value.center().y()); transform.rotate(value.rotation);
    const double w = value.size.width() / 2, h = value.size.height() / 2;
    return {transform.map(QPointF(-w, -h)), transform.map(QPointF(w, -h)),
            transform.map(QPointF(w, h)), transform.map(QPointF(-w, h))};
}

QSet<QUuid> EditorSession::selectedTransformLayerIds() const
{
    QSet<QUuid> result;
    if (!document_) return result;
    for (const Layer &layer : document_->layers) {
        if (layer.group || !layer.transform.isValid()) continue;
        bool visible = layer.visible, selected = selectedLayerIds_.contains(layer.id);
        std::optional<QUuid> parent = layer.parentId;
        QSet<QUuid> visited;
        while (parent && !visited.contains(*parent)) {
            visited.insert(*parent);
            const auto it = std::find_if(document_->layers.cbegin(), document_->layers.cend(), [&](const Layer &candidate) { return candidate.id == *parent; });
            if (it == document_->layers.cend()) { visible = false; break; }
            visible = visible && it->visible;
            selected = selected || selectedLayerIds_.contains(it->id);
            parent = it->parentId;
        }
        if (visible && selected) result.insert(layer.id);
    }
    return result;
}

QRectF EditorSession::selectedLayersBounds() const
{
    QRectF bounds; bool first = true;
    if (!document_) return bounds;
    const QSet<QUuid> targets = selectedTransformLayerIds();
    for (const Layer &layer : document_->layers) {
        if (!targets.contains(layer.id)) continue;
        const auto corners = layerCorners(layer.transform);
        double left = corners[0].x(), right = left, top = corners[0].y(), bottom = top;
        for (const QPointF &point : corners) { left = std::min(left, point.x()); right = std::max(right, point.x()); top = std::min(top, point.y()); bottom = std::max(bottom, point.y()); }
        const QRectF layerBounds(QPointF(left, top), QPointF(right, bottom));
        bounds = first ? layerBounds : bounds.united(layerBounds); first = false;
    }
    return bounds;
}

bool EditorSession::transformSelectedLayers(const QRectF &input, double rotationDegrees)
{
    if (!document_ || !std::isfinite(rotationDegrees)) return false;
    const Layer *active = activeLayer();
    const bool groupTransform = selectedLayerIds_.size() > 1 || (active && active->group);
    const QSet<QUuid> targets = selectedTransformLayerIds();
    if (!groupTransform || targets.isEmpty()) return false;
    const QRectF originalBounds = selectedLayersBounds(); const QRectF bounds = input.normalized();
    if (originalBounds.width() <= 0 || originalBounds.height() <= 0 || bounds.width() < 1 || bounds.height() < 1
        || bounds.width() > 300000 || bounds.height() > 300000) return false;
    const double sx = bounds.width() / originalBounds.width(), sy = bounds.height() / originalBounds.height();
    if (bounds == originalBounds && qFuzzyIsNull(rotationDegrees)) return false;
    beginEdit(QStringLiteral("Transform Layers"));
    for (Layer &layer : document_->layers) {
        if (!targets.contains(layer.id)) continue;
        const LayerTransform old = layer.transform; const std::optional<LayerTransform> oldMask = layer.maskPlacement;
        const QPointF relative = old.center() - originalBounds.topLeft();
        QPointF center(bounds.left() + relative.x() * sx, bounds.top() + relative.y() * sy);
        if (!qFuzzyIsNull(rotationDegrees)) {
            QTransform rotation; rotation.translate(bounds.center().x(), bounds.center().y()); rotation.rotate(rotationDegrees); rotation.translate(-bounds.center().x(), -bounds.center().y());
            center = rotation.map(center);
        }
        layer.transform.size = QSizeF(std::max(1.0, old.size.width() * sx), std::max(1.0, old.size.height() * sy));
        layer.transform.origin = center - QPointF(layer.transform.size.width() / 2, layer.transform.size.height() / 2);
        layer.transform.rotation = old.rotation + rotationDegrees;
        if (!layer.mask.isNull() && layer.mask.size() != QSize(1, 1) && layer.maskLinked) {
            if (oldMask) layer.maskPlacement = oldMask->following(old, layer.transform); else layer.maskPlacement.reset();
        }
    }
    redrawSelectedShapes(); endEdit(); return true;
}

static bool usableDistortion(const std::array<QPointF, 4> &corners)
{
    double sign = 0;
    for (int i = 0; i < 4; ++i) {
        const QPointF &a = corners[size_t(i)], &b = corners[size_t((i + 1) % 4)], &c = corners[size_t((i + 2) % 4)];
        if (!std::isfinite(a.x()) || !std::isfinite(a.y()) || std::abs(a.x()) > 1000000 || std::abs(a.y()) > 1000000) return false;
        const double cross = (b.x() - a.x()) * (c.y() - b.y()) - (b.y() - a.y()) * (c.x() - b.x());
        if (std::abs(cross) <= .01) return false;
        if (sign == 0) sign = cross; else if ((cross < 0) != (sign < 0)) return false;
    }
    return true;
}

static QRect alphaBounds(const QImage &image);

struct WarpedRaster { QImage image; LayerTransform placement; QRect crop; };

static std::optional<WarpedRaster> warpRaster(const QImage &source, const LayerTransform &placement,
                                               const std::array<QPointF, 4> &corners, bool mask, int background = 0,
                                               const std::optional<QRect> &forcedCrop = std::nullopt)
{
    if (source.isNull() || !usableDistortion(corners)) return std::nullopt;
    double left = corners[0].x(), right = left, top = corners[0].y(), bottom = top;
    for (const QPointF &point : corners) { left = std::min(left, point.x()); right = std::max(right, point.x()); top = std::min(top, point.y()); bottom = std::max(bottom, point.y()); }
    const QPointF origin(std::floor(left), std::floor(top));
    const QSize size(qCeil(right) - int(origin.x()), qCeil(bottom) - int(origin.y()));
    if (size.width() < 1 || size.height() < 1 || size.width() > 30000 || size.height() > 30000
        || qint64(size.width()) * size.height() > 100000000LL) return std::nullopt;
    const int order[4] = {0, 1, 3, 2};
    QPolygonF target;
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
        const int u = placement.flipX ? 1 - x : x, v = placement.flipY ? 1 - y : y;
        target << corners[size_t(order[v * 2 + u])] - origin;
    }
    // QTransform expects polygon order around the perimeter, not scan-line order.
    std::swap(target[2], target[3]);
    const QPolygonF sourceQuad{QPointF(0, 0), QPointF(source.width(), 0),
                               QPointF(source.width(), source.height()), QPointF(0, source.height())};
    QTransform mapping;
    if (!QTransform::quadToQuad(sourceQuad, target, mapping)) return std::nullopt;
    QImage output(size, mask ? QImage::Format_Grayscale8 : QImage::Format_RGBA8888_Premultiplied);
    output.fill(mask ? background : 0);
    QPainter painter(&output); painter.setRenderHint(QPainter::SmoothPixmapTransform, placement.sampling != Sampling::Nearest);
    painter.setTransform(mapping); painter.drawImage(QPointF(0, 0), source); painter.end();
    QRect crop = forcedCrop.value_or(mask ? output.rect() : alphaBounds(output));
    crop = crop.intersected(output.rect());
    if (crop.isEmpty()) crop = output.rect();
    WarpedRaster result{crop == output.rect() ? output : output.copy(crop), {}, crop};
    result.placement.origin = origin + crop.topLeft(); result.placement.size = crop.size(); result.placement.sampling = placement.sampling;
    return result;
}

bool EditorSession::distortSelectedLayers(const std::array<QPointF, 4> &corners, bool maskOnly)
{
    if (!document_ || !usableDistortion(corners)) return false;
    Layer *active = activeLayer();
    if (!active || active->group || (maskOnly && active->mask.isNull())) return false;
    QSet<QUuid> ids = maskOnly ? QSet<QUuid>{active->id} : selectedLayerIds_;
    if (ids.isEmpty()) ids.insert(active->id);
    QRectF groupBounds = selectedLayersBounds();
    if (maskOnly) {
        const auto maskCorners = layerCorners(active->maskPlacement.value_or(active->transform));
        double left = maskCorners[0].x(), right = left, top = maskCorners[0].y(), bottom = top;
        for (const QPointF &point : maskCorners) { left = std::min(left, point.x()); right = std::max(right, point.x()); top = std::min(top, point.y()); bottom = std::max(bottom, point.y()); }
        groupBounds = QRectF(QPointF(left, top), QPointF(right, bottom));
    }
    if (groupBounds.width() <= 0 || groupBounds.height() <= 0) return false;
    QTransform groupWarp;
    const QPolygonF groupSource{groupBounds.topLeft(), groupBounds.topRight(), groupBounds.bottomRight(), groupBounds.bottomLeft()};
    const QPolygonF groupTarget{corners[0], corners[1], corners[2], corners[3]};
    if (!QTransform::quadToQuad(groupSource, groupTarget, groupWarp)) return false;

    struct Result { int index; QImage image; LayerTransform transform; QImage mask; std::optional<LayerTransform> maskPlacement; bool maskOnly; };
    QVector<Result> results;
    for (int index = 0; index < document_->layers.size(); ++index) {
        const Layer &layer = document_->layers.at(index);
        if (!ids.contains(layer.id) || layer.group) continue;
        if (maskOnly) {
            const LayerTransform old = layer.maskPlacement.value_or(layer.transform);
            const auto oldCorners = layerCorners(old);
            std::array<QPointF, 4> moved; for (int i = 0; i < 4; ++i) moved[size_t(i)] = groupWarp.map(oldCorners[size_t(i)]);
            int background = 0; if (!layer.mask.isNull()) { int total = 0, count = 0; for (int x = 0; x < layer.mask.width(); ++x) { total += qGray(layer.mask.pixel(x, 0)); total += qGray(layer.mask.pixel(x, layer.mask.height() - 1)); count += 2; } background = count && total * 2 >= count * 255 ? 255 : 0; }
            const auto warped = warpRaster(layer.mask, old, moved, true, background); if (!warped) return false;
            results.push_back({index, {}, {}, warped->image, warped->placement, true});
            continue;
        }
        if (layer.image.isNull()) continue;
        const auto oldCorners = layerCorners(layer.transform);
        std::array<QPointF, 4> moved; for (int i = 0; i < 4; ++i) moved[size_t(i)] = groupWarp.map(oldCorners[size_t(i)]);
        const auto warped = warpRaster(layer.image, layer.transform, moved, false); if (!warped) return false;
        QImage newMask = layer.mask; std::optional<LayerTransform> newMaskPlacement = layer.maskPlacement;
        if (!layer.mask.isNull() && layer.maskLinked) {
            if (!layer.maskPlacement) {
                const auto warpedMask = warpRaster(layer.mask, layer.transform, moved, true, 0, warped->crop);
                if (!warpedMask) return false;
                newMask = warpedMask->image; newMaskPlacement.reset();
            } else {
                const auto maskCorners = layerCorners(*layer.maskPlacement); std::array<QPointF, 4> movedMask;
                for (int i = 0; i < 4; ++i) movedMask[size_t(i)] = groupWarp.map(maskCorners[size_t(i)]);
                const auto warpedMask = warpRaster(layer.mask, *layer.maskPlacement, movedMask, true, 255);
                if (!warpedMask) return false;
                newMask = warpedMask->image; newMaskPlacement = warpedMask->placement;
            }
        } else if (!layer.mask.isNull() && !layer.maskPlacement) newMaskPlacement = layer.transform;
        results.push_back({index, warped->image, warped->placement, newMask, newMaskPlacement, false});
    }
    if (results.isEmpty()) return false;
    beginEdit(maskOnly ? QStringLiteral("Distort Layer Mask") : results.size() > 1 ? QStringLiteral("Distort Layers") : QStringLiteral("Distort"));
    for (const Result &result : results) {
        Layer &layer = document_->layers[result.index];
        if (result.maskOnly) { layer.mask = result.mask; layer.maskPlacement = result.maskPlacement; }
        else { layer.image = result.image; layer.transform = result.transform; layer.mask = result.mask; layer.maskPlacement = result.maskPlacement; rasterizeLayer(layer); }
    }
    endEdit(); return true;
}

static QImage rasterizedScaled(const QImage &source, const LayerTransform &transform, double sx, double sy,
                               const QRectF &bounds, Sampling sampling, bool grayscale)
{
    QImage output(qMax(1, qCeil(bounds.width())), qMax(1, qCeil(bounds.height())),
                  grayscale ? QImage::Format_Grayscale8 : QImage::Format_RGBA8888_Premultiplied);
    output.fill(grayscale ? Qt::black : Qt::transparent);
    QPainter painter(&output);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, sampling != Sampling::Nearest);
    painter.translate(-bounds.left(), -bounds.top()); painter.scale(sx, sy);
    painter.translate(transform.center()); painter.rotate(transform.rotation);
    painter.scale(transform.flipX ? -1 : 1, transform.flipY ? -1 : 1);
    painter.drawImage(QRectF(-transform.size.width() / 2, -transform.size.height() / 2,
                            transform.size.width(), transform.size.height()), source);
    return output;
}

bool EditorSession::resizeImage(const QSize &size, double resolution, Sampling sampling)
{
    if (!document_ || !validDocumentSize(size) || !std::isfinite(resolution) || resolution < 1 || resolution > 9600
        || qint64(size.width()) * size.height() > 100000000LL) return false;
    const QSize old = document_->canvasSize;
    if (size == old && qFuzzyCompare(resolution, document_->resolution)) return false;
    const double sx = double(size.width()) / old.width(), sy = double(size.height()) / old.height();
    beginEdit(QStringLiteral("Image Size"));
    if (size != old) for (Layer &layer : document_->layers) {
        const auto corners = layerCorners(layer.transform);
        double left = corners[0].x() * sx, right = left, top = corners[0].y() * sy, bottom = top;
        for (const QPointF &p : corners) { left = std::min(left, p.x() * sx); right = std::max(right, p.x() * sx); top = std::min(top, p.y() * sy); bottom = std::max(bottom, p.y() * sy); }
        const QRectF bounds(QPointF(std::floor(left), std::floor(top)), QPointF(std::ceil(right), std::ceil(bottom)));
        if (!layer.image.isNull()) layer.image = rasterizedScaled(layer.image, layer.transform, sx, sy, bounds, sampling, false);
        if (!layer.mask.isNull() && layer.mask.size() != QSize(1, 1)) layer.mask = rasterizedScaled(layer.mask, layer.transform, sx, sy, bounds, sampling, true);
        layer.transform.origin = bounds.topLeft(); layer.transform.size = bounds.size(); layer.transform.rotation = 0;
        layer.transform.flipX = false; layer.transform.flipY = false; layer.transform.sampling = sampling;
    }
    if (document_->selection && size != old) document_->selection = document_->selection->scaled(size, Qt::IgnoreAspectRatio,
        sampling == Sampling::Nearest ? Qt::FastTransformation : Qt::SmoothTransformation);
    if (size != old) {
        for (CanvasGuide &guide : document_->guides) guide = guide.scaled(sx, sy);
    }
    document_->canvasSize = size; document_->resolution = resolution;
    endEdit(); return true;
}

static LayerTransform mirroredTransform(LayerTransform value, bool horizontally, double axis)
{
    if (horizontally) {
        value.flipX = !value.flipX;
        value.origin.setX(2 * axis - value.center().x() - value.size.width() / 2);
    } else {
        value.flipY = !value.flipY;
        value.origin.setY(2 * axis - value.center().y() - value.size.height() / 2);
    }
    value.rotation = -value.rotation;
    return value;
}

bool EditorSession::flipLayers(bool horizontally)
{
    if (!document_ || !activeLayer()) return false;
    QSet<QUuid> ids = selectedLayerIds_;
    if (ids.isEmpty()) ids.insert(activeLayer()->id);
    for (const QUuid &id : QSet<QUuid>(ids)) ids.unite(descendantIds(id));
    QVector<int> indices;
    QRectF bounds;
    for (int i = 0; i < document_->layers.size(); ++i) {
        const Layer &layer = document_->layers.at(i);
        if (!ids.contains(layer.id) || layer.group) continue;
        indices.push_back(i);
        for (const QPointF &corner : layerCorners(layer.transform)) bounds |= QRectF(corner, QSizeF(0, 0));
    }
    if (indices.isEmpty()) return false;
    const bool group = indices.size() > 1 || activeLayer()->group;
    const double axis = group ? (horizontally ? bounds.center().x() : bounds.center().y())
                              : (horizontally ? document_->layers.at(indices.front()).transform.center().x()
                                              : document_->layers.at(indices.front()).transform.center().y());
    beginEdit(horizontally ? QStringLiteral("Flip Horizontal") : QStringLiteral("Flip Vertical"));
    for (int index : indices) {Layer &layer=document_->layers[index];const LayerTransform old=layer.transform;const std::optional<LayerTransform> mask=layer.maskPlacement;layer.transform=mirroredTransform(old,horizontally,axis);if(!layer.mask.isNull()&&layer.mask.size()!=QSize(1,1)){if(layer.maskLinked){if(mask)layer.maskPlacement=mask->following(old,layer.transform);else layer.maskPlacement.reset();}else layer.maskPlacement=mask.value_or(old);}}
    endEdit(); return true;
}

bool EditorSession::flipCanvas(bool horizontally)
{
    if (!document_) return false;
    const double axis = horizontally ? document_->canvasSize.width() / 2.0 : document_->canvasSize.height() / 2.0;
    beginEdit(horizontally ? QStringLiteral("Flip Canvas Horizontal") : QStringLiteral("Flip Canvas Vertical"));
    for (Layer &layer : document_->layers) layer.transform = mirroredTransform(layer.transform, horizontally, axis);
    for (CanvasGuide &guide : document_->guides) guide = guide.mirrored(horizontally, axis);
    if (document_->selection) document_->selection = document_->selection->flipped(
        horizontally ? Qt::Horizontal : Qt::Vertical);
    endEdit(); return true;
}

static QRect alphaBounds(const QImage &image)
{
    int left = image.width(), top = image.height(), right = -1, bottom = -1;
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) if (image.pixelColor(x, y).alpha()) {
        left = std::min(left, x); top = std::min(top, y); right = std::max(right, x); bottom = std::max(bottom, y);
    }
    return right < left ? QRect() : QRect(QPoint(left, top), QPoint(right, bottom));
}

bool EditorSession::canMergeLayers() const
{
    const Layer *active = activeLayer();
    if (!document_ || !active) return false;
    if (selectedLayerIds_.size() > 1) {
        QSet<QUuid> rendered = selectedLayerIds_;
        for (const QUuid &id : selectedLayerIds_) rendered.unite(descendantIds(id));
        return std::any_of(document_->layers.cbegin(), document_->layers.cend(), [&rendered](const Layer &layer) { return rendered.contains(layer.id) && !layer.group; });
    }
    if (active->group) {
        const QSet<QUuid> rendered = descendantIds(active->id);
        return std::any_of(document_->layers.cbegin(), document_->layers.cend(), [&rendered](const Layer &layer) { return rendered.contains(layer.id) && !layer.group; });
    }
    const int activeIndex = indexOf(active->id);
    for (int i = activeIndex - 1; i >= 0; --i) if (document_->layers.at(i).parentId == active->parentId) return !document_->layers.at(i).group;
    return false;
}

QString EditorSession::mergeTitle() const
{
    const Layer *active = activeLayer();
    if (selectedLayerIds_.size() > 1) return QStringLiteral("Merge Layers");
    if (active && active->group) return QStringLiteral("Merge Group");
    return QStringLiteral("Merge Down");
}

bool EditorSession::mergeLayers()
{
    Layer *active = activeLayer();
    if (!document_ || !active) return false;
    const QVector<Layer> original = document_->layers;
    QSet<QUuid> rendered, removed;
    QString name, action;
    std::optional<QUuid> parent;
    QUuid anchor;
    if (selectedLayerIds_.size() > 1) {
        rendered = selectedLayerIds_;
        for (const QUuid &id : selectedLayerIds_) rendered.unite(descendantIds(id));
        removed = rendered; name = active->name; parent = active->parentId; anchor = active->id; action = QStringLiteral("Merge Layers");
    } else if (active->group) {
        rendered = descendantIds(active->id); removed = rendered; removed.insert(active->id);
        name = active->name; parent = active->parentId; anchor = active->id; action = QStringLiteral("Merge Group");
    } else {
        const int activeIndex = indexOf(active->id); int below = -1;
        for (int i = activeIndex - 1; i >= 0; --i) if (original.at(i).parentId == active->parentId) { below = i; break; }
        if (below < 0 || original.at(below).group) return false;
        rendered = {original.at(below).id, active->id}; removed = rendered;
        name = original.at(below).name; parent = active->parentId; anchor = active->id; action = QStringLiteral("Merge Down");
    }
    bool hasPixels = false;
    for (const Layer &layer : original) if (rendered.contains(layer.id) && !layer.group) { hasPixels = true; break; }
    if (!hasPixels) return false;
    Document subset = *document_; subset.layers.clear(); subset.selection.reset();
    for (Layer layer : original) if (rendered.contains(layer.id)) {
        if (layer.parentId && !rendered.contains(*layer.parentId)) layer.parentId.reset();
        if (layer.maskSourceId && !rendered.contains(*layer.maskSourceId)) layer.maskSourceId.reset();
        subset.layers.push_back(std::move(layer));
    }
    const QImage full = LayerRenderer::flattened(subset);
    const QRect trim = alphaBounds(full);
    if (trim.isEmpty()) return false;
    Layer merged; merged.id = QUuid::createUuid(); merged.name = name; merged.parentId = parent;
    merged.image = full.copy(trim); merged.transform.origin = trim.topLeft(); merged.transform.size = trim.size();
    int slot = 0;
    for (; slot < original.size() && original.at(slot).id != anchor; ++slot) {}
    int insertion = slot;
    for (int i = 0; i < slot; ++i) if (removed.contains(original.at(i).id)) --insertion;
    beginEdit(action);
    QVector<Layer> next;
    for (Layer layer : original) if (!removed.contains(layer.id)) {
        if (layer.maskSourceId && removed.contains(*layer.maskSourceId)) layer.maskSourceId = merged.id;
        next.push_back(std::move(layer));
    }
    next.insert(std::clamp(insertion, 0, int(next.size())), merged);
    document_->layers = std::move(next); document_->activeLayerId = merged.id;
    selectedLayerIds_ = {merged.id}; maskSelected_ = false;
    endEdit(); return true;
}

bool EditorSession::flattenImage()
{
    if (!document_ || document_->layers.isEmpty()) return false;
    const QImage full = LayerRenderer::flattened(*document_);
    Layer merged; merged.id = QUuid::createUuid(); merged.name = QStringLiteral("Background");
    merged.image = full; merged.transform.size = document_->canvasSize;
    beginEdit(QStringLiteral("Flatten Image"));
    document_->layers = {merged}; document_->activeLayerId = merged.id;
    selectedLayerIds_ = {merged.id}; maskSelected_ = false;
    endEdit(); return true;
}

void EditorSession::selectLayer(const std::optional<QUuid> &id)
{
    if (!document_) return;
    if (id && indexOf(*id) < 0) return;
    if (document_->activeLayerId != id) maskSelected_ = false;
    document_->activeLayerId = id;
    selectedLayerIds_.clear();
    if (id) selectedLayerIds_.insert(*id);
}

void EditorSession::selectLayers(const QSet<QUuid> &ids, const std::optional<QUuid> &primary)
{
    if (!document_) return;
    QSet<QUuid> valid;
    for (const Layer &layer : document_->layers) if (ids.contains(layer.id)) valid.insert(layer.id);
    selectedLayerIds_ = valid;
    const std::optional<QUuid> next = primary && valid.contains(*primary) ? primary
        : valid.isEmpty() ? std::nullopt : std::optional<QUuid>(*valid.cbegin());
    if (document_->activeLayerId != next || valid.size() != 1) maskSelected_ = false;
    document_->activeLayerId = next;
}

QString EditorSession::nextName(const QString &base) const
{
    QSet<QString> names;
    if (document_) for (const Layer &layer : document_->layers) names.insert(layer.name);
    int number = 1;
    while (names.contains(QStringLiteral("%1 %2").arg(base).arg(number))) ++number;
    return QStringLiteral("%1 %2").arg(base).arg(number);
}

void EditorSession::addBlankLayer()
{
    if (!document_ || document_->layers.size() >= 10000) return;
    Layer layer;
    layer.id = QUuid::createUuid();
    layer.name = nextName(QStringLiteral("Layer"));
    layer.transform.size = document_->canvasSize;
    if (const Layer *active = activeLayer()) layer.parentId = active->group ? document_->activeLayerId : active->parentId;
    int insertion = document_->activeLayerId ? indexOf(*document_->activeLayerId) + 1 : document_->layers.size();
    if (const Layer *active = activeLayer(); active && active->group) {
        const QSet<QUuid> descendants = descendantIds(active->id);
        for (int i = document_->layers.size() - 1; i >= 0; --i) if (descendants.contains(document_->layers.at(i).id)) { insertion = std::max(insertion, i + 1); break; }
    }
    beginEdit(QStringLiteral("New Blank Layer"));
    document_->layers.insert(insertion, layer);
    selectLayer(layer.id);
    endEdit();
}

void EditorSession::addGroup()
{
    if (!document_ || document_->layers.size() >= 10000) return;
    Layer group;
    group.id = QUuid::createUuid();
    group.name = nextName(QStringLiteral("Folder"));
    group.group = true;
    group.transform.size = document_->canvasSize;
    if (const Layer *active = activeLayer()) group.parentId = active->group ? document_->activeLayerId : active->parentId;
    const int insertion = document_->activeLayerId ? indexOf(*document_->activeLayerId) + 1 : document_->layers.size();
    beginEdit(QStringLiteral("New Folder"));
    document_->layers.insert(insertion, group);
    selectLayer(group.id);
    endEdit();
}

bool EditorSession::canSaveAdjustment(AdjustmentKind kind) const
{
    Q_UNUSED(kind);
    return bool(document_);
}

bool EditorSession::canSaveAdjustment(const QString &kind) const
{
    const auto optKind = adjustmentKindFromString(kind);
    return optKind.has_value() && canSaveAdjustment(*optKind);
}

bool EditorSession::addAdjustment(const QString &kind, const QJsonObject &settings)
{
    const auto optKind = adjustmentKindFromString(kind);
    if (!document_ || document_->layers.size() >= 10000 || !optKind) return false;
    if (!canSaveAdjustment(*optKind)) return false;

    Layer layer; layer.id = QUuid::createUuid(); layer.name = kind; layer.transform.size = document_->canvasSize;
    layer.adjustment = settings;
    if (kind == QStringLiteral("Grain") && !layer.adjustment.contains(QStringLiteral("grainSettings"))) {
        layer.adjustment.insert(QStringLiteral("grainSettings"), QJsonObject{{QStringLiteral("amount"), 25.0},
            {QStringLiteral("size"), 1.5}, {QStringLiteral("roughness"), 50.0},
            {QStringLiteral("seed"), double(QRandomGenerator::global()->generate())}});
    } else if (kind == QStringLiteral("Gradient Map") && !layer.adjustment.contains(QStringLiteral("gradientMapSettings"))) {
        layer.adjustment.insert(QStringLiteral("gradientMapSettings"), QJsonObject{
            {QStringLiteral("shadows"), QJsonObject{{QStringLiteral("red"), 0.0}, {QStringLiteral("green"), 0.0}, {QStringLiteral("blue"), 0.0}}},
            {QStringLiteral("highlights"), QJsonObject{{QStringLiteral("red"), 1.0}, {QStringLiteral("green"), 1.0}, {QStringLiteral("blue"), 1.0}}},
            {QStringLiteral("reversed"), false}
        });
    } else if (kind == QStringLiteral("Black & White") && !layer.adjustment.contains(QStringLiteral("blackWhiteSettings"))) {
        layer.adjustment.insert(QStringLiteral("blackWhiteSettings"), QJsonObject{
            {QStringLiteral("reds"), 40.0}, {QStringLiteral("yellows"), 60.0}, {QStringLiteral("greens"), 40.0},
            {QStringLiteral("cyans"), 60.0}, {QStringLiteral("blues"), 20.0}, {QStringLiteral("magentas"), 80.0},
            {QStringLiteral("tint"), false}, {QStringLiteral("tintHue"), 40.0}, {QStringLiteral("tintSaturation"), 20.0}
        });
    } else if (kind == QStringLiteral("Color Balance") && !layer.adjustment.contains(QStringLiteral("colorBalanceSettings"))) {
        layer.adjustment.insert(QStringLiteral("colorBalanceSettings"), QJsonObject{
            {QStringLiteral("shadowCyanRed"), 0.0}, {QStringLiteral("shadowMagentaGreen"), 0.0}, {QStringLiteral("shadowYellowBlue"), 0.0},
            {QStringLiteral("midCyanRed"), 0.0}, {QStringLiteral("midMagentaGreen"), 0.0}, {QStringLiteral("midYellowBlue"), 0.0},
            {QStringLiteral("highlightCyanRed"), 0.0}, {QStringLiteral("highlightMagentaGreen"), 0.0}, {QStringLiteral("highlightYellowBlue"), 0.0},
            {QStringLiteral("preserveLuminosity"), true}
        });
    } else if (kind == QStringLiteral("Gaussian Blur") && !layer.adjustment.contains(QStringLiteral("blurRadius"))) {
        layer.adjustment.insert(QStringLiteral("blurRadius"), 10.0);
    } else if (kind == QStringLiteral("Motion Blur")) {
        if (!layer.adjustment.contains(QStringLiteral("motionAngle"))) layer.adjustment.insert(QStringLiteral("motionAngle"), 0.0);
        if (!layer.adjustment.contains(QStringLiteral("motionDistance"))) layer.adjustment.insert(QStringLiteral("motionDistance"), 10.0);
    } else if (kind == QStringLiteral("Add Noise")) {
        if (!layer.adjustment.contains(QStringLiteral("noiseAmount"))) layer.adjustment.insert(QStringLiteral("noiseAmount"), 10.0);
        if (!layer.adjustment.contains(QStringLiteral("noiseGaussian"))) layer.adjustment.insert(QStringLiteral("noiseGaussian"), false);
        if (!layer.adjustment.contains(QStringLiteral("noiseMonochromatic"))) layer.adjustment.insert(QStringLiteral("noiseMonochromatic"), false);
        if (!layer.adjustment.contains(QStringLiteral("noiseSeed"))) layer.adjustment.insert(QStringLiteral("noiseSeed"), double(QRandomGenerator::global()->generate()));
    }
    layer.adjustment.insert(QStringLiteral("kind"), kind);
    if (const Layer *active = activeLayer()) layer.parentId = active->group ? document_->activeLayerId : active->parentId;
    const int insertion = document_->activeLayerId ? indexOf(*document_->activeLayerId) + 1 : document_->layers.size();
    beginEdit(QStringLiteral("New %1 Adjustment").arg(kind));
    if (isVersion9Adjustment(*optKind)) {
        document_->formatVersion = std::max(document_->formatVersion, 9);
    }
    document_->layers.insert(insertion, layer);
    selectLayer(layer.id);
    endEdit();
    return true;
}

bool EditorSession::updateAdjustment(const QUuid &id, const QJsonObject &settings, const QString &historyName)
{
    const int index = indexOf(id); if (index < 0 || document_->layers.at(index).adjustment.isEmpty() || settings.value(QStringLiteral("kind")).toString().isEmpty()) return false;
    if (document_->layers.at(index).adjustment == settings) return true;
    beginEdit(historyName);
    const auto optKind = adjustmentKindFromString(settings.value(QStringLiteral("kind")).toString());
    if (optKind && isVersion9Adjustment(*optKind)) {
        document_->formatVersion = std::max(document_->formatVersion, 9);
    }
    document_->layers[index].adjustment = settings;
    endEdit();
    return true;
}

bool EditorSession::previewAdjustment(const QUuid &id, const QJsonObject &settings)
{
    if (!document_) return false;
    const int index = indexOf(id);
    if (index < 0 || document_->layers.at(index).adjustment.isEmpty() || settings.value(QStringLiteral("kind")).toString().isEmpty()) return false;
    document_->layers[index].adjustment = settings;
    advanceRevision();
    return true;
}

bool EditorSession::previewLayerEffects(const QUuid &id, const std::optional<LayerEffects> &effects)
{
    if (!document_) return false;
    const int index = indexOf(id);
    if (index < 0) return false;
    document_->layers[index].effects = effects;
    advanceRevision();
    return true;
}

bool EditorSession::previewLayerImage(const QUuid &id, const QImage &image)
{
    if (!document_) return false;
    const int index = indexOf(id);
    if (index < 0) return false;
    document_->layers[index].image = image;
    advanceRevision();
    return true;
}

bool EditorSession::rollbackLayer(const QUuid &id, const Layer &originalLayer)
{
    if (!document_) return false;
    const int index = indexOf(id);
    if (index < 0) return false;
    document_->layers[index] = originalLayer;
    advanceRevision();
    return true;
}

bool EditorSession::rollbackLayerImage(const QUuid &id, const QImage &originalImage)
{
    if (!document_) return false;
    const int index = indexOf(id);
    if (index < 0) return false;
    document_->layers[index].image = originalImage;
    advanceRevision();
    return true;
}

bool EditorSession::previewCameraRaw(const QUuid &id, const CameraRawSettings &settings,
                                    CameraRawClipping clipping)
{
    if (!document_) return false;
    const int index = indexOf(id);
    if (index < 0) return false;
    const Layer &layer = document_->layers.at(index);
    if (layer.group || layer.image.isNull()) return false;
    const QImage graded = RasterOperations::cameraRaw(layer.image, settings, clipping);
    return previewLayerImage(id, graded);
}

QSet<QUuid> EditorSession::descendantIds(const QUuid &id) const

{
    QSet<QUuid> result;
    if (!document_) return result;
    QVector<QUuid> pending{id};
    while (!pending.isEmpty()) {
        const QUuid parent = pending.takeLast();
        for (const Layer &layer : document_->layers) if (layer.parentId == parent && !result.contains(layer.id)) {
            result.insert(layer.id);
            pending.push_back(layer.id);
        }
    }
    return result;
}

void EditorSession::deleteIds(const QSet<QUuid> &ids, const QString &historyName)
{
    if (!document_ || ids.isEmpty()) return;
    QSet<QUuid> removed = ids;
    for (const QUuid &id : ids) removed.unite(descendantIds(id));
    int reference = document_->activeLayerId ? indexOf(*document_->activeLayerId) : -1;
    beginEdit(historyName);
    for (int i = document_->layers.size() - 1; i >= 0; --i) if (removed.contains(document_->layers.at(i).id)) document_->layers.removeAt(i);
    for (Layer &layer : document_->layers) if (layer.maskSourceId && removed.contains(*layer.maskSourceId)) layer.maskSourceId.reset();
    selectedLayerIds_.clear();
    if (document_->layers.isEmpty()) document_->activeLayerId.reset();
    else {
        reference = std::clamp(reference, 0, int(document_->layers.size()) - 1);
        document_->activeLayerId = document_->layers.at(reference).id;
        selectedLayerIds_.insert(*document_->activeLayerId);
    }
    endEdit();
}

void EditorSession::deleteActiveLayer()
{
    if (document_ && document_->activeLayerId) deleteIds({*document_->activeLayerId}, QStringLiteral("Delete Layer"));
}

void EditorSession::deleteSelectedLayers()
{
    deleteSelectedLayers(false);
}

QSet<QUuid> EditorSession::selectedDeletionLiveMaskDependents() const
{
    QSet<QUuid> result; if (!document_) return result;
    QSet<QUuid> removed = selectedLayerIds_; if (removed.isEmpty() && document_->activeLayerId) removed.insert(*document_->activeLayerId);
    const QSet<QUuid> roots = removed; for (const QUuid &id : roots) removed.unite(descendantIds(id));
    for (const Layer &layer : document_->layers) if (!removed.contains(layer.id) && layer.maskSourceId && removed.contains(*layer.maskSourceId)) result.insert(layer.id);
    return result;
}

void EditorSession::deleteSelectedLayers(bool bakeLiveMasks)
{
    if (bakeLiveMasks && document_) {
        const QSet<QUuid> targets = selectedDeletionLiveMaskDependents();
        if (!targets.isEmpty()) {
            beginEdit(QStringLiteral("Bake and Delete Layers"));
            for (Layer &layer : document_->layers) if (targets.contains(layer.id)) {
                if (layer.image.isNull() || !layer.adjustment.isEmpty()) { layer.maskSourceId.reset(); continue; }
                layer.image = LayerRenderer::bakeLiveMask(*document_, layer);
                layer.transform.origin = {}; layer.transform.size = document_->canvasSize; layer.transform.rotation = 0; layer.transform.flipX = layer.transform.flipY = false;
                layer.mask = {}; layer.maskPlacement.reset(); layer.maskSourceId.reset();
            }
            if (selectedLayerIds_.size() <= 1) deleteActiveLayer(); else deleteIds(selectedLayerIds_, QStringLiteral("Delete Layers"));
            endEdit(); return;
        }
    }
    if (selectedLayerIds_.size() <= 1) deleteActiveLayer();
    else deleteIds(selectedLayerIds_, QStringLiteral("Delete Layers"));
}

void EditorSession::renameLayer(const QUuid &id, const QString &name)
{
    const QString trimmed = name.trimmed();
    const int index = indexOf(id);
    if (index < 0 || trimmed.isEmpty() || trimmed.toUtf8().size() > 16384 || document_->layers.at(index).name == trimmed) return;
    beginEdit(QStringLiteral("Rename Layer"));
    document_->layers[index].name = trimmed;
    endEdit();
}

void EditorSession::toggleLayerVisibility(const QUuid &id)
{
    const int index = indexOf(id);
    if (index < 0) return;
    beginEdit(document_->layers.at(index).visible ? QStringLiteral("Hide Layer") : QStringLiteral("Show Layer"));
    document_->layers[index].visible = !document_->layers.at(index).visible;
    endEdit();
}

void EditorSession::setLayerOpacity(const QUuid &id, double opacity)
{
    const int index = indexOf(id);
    opacity = std::clamp(opacity, 0.0, 1.0);
    if (index < 0 || qFuzzyCompare(document_->layers.at(index).opacity, opacity)) return;
    beginEdit(QStringLiteral("Layer Opacity"));
    if (document_->layers.at(index).group && opacity < 0.999999) {
        document_->formatVersion = std::max(document_->formatVersion, 8);
    }
    document_->layers[index].opacity = opacity;
    endEdit();
}

void EditorSession::setSelectedLayersOpacity(double opacity)
{
    if (!document_ || !std::isfinite(opacity)) return;
    opacity = std::clamp(opacity, 0.0, 1.0);
    QSet<QUuid> ids = selectedLayerIds_;
    if (ids.isEmpty() && document_->activeLayerId) ids.insert(*document_->activeLayerId);
    QVector<int> changed;
    for (int i = 0; i < document_->layers.size(); ++i) {
        const Layer &layer = document_->layers.at(i);
        if (ids.contains(layer.id) && !layer.group && !qFuzzyCompare(layer.opacity, opacity)) changed.push_back(i);
    }
    if (changed.isEmpty()) return;
    beginEdit(QStringLiteral("Layer Opacity"));
    for (int index : changed) document_->layers[index].opacity = opacity;
    endEdit();
}

void EditorSession::setLayerBlendMode(const QUuid &id, BlendMode mode)
{
    const int index = indexOf(id);
    if (index < 0 || document_->layers.at(index).blendMode == mode) return;
    beginEdit(QStringLiteral("Layer Blend Mode"));
    document_->layers[index].blendMode = mode;
    endEdit();
}

bool EditorSession::canMoveActiveLayer(int offset) const
{
    const Layer *active = activeLayer();
    if (!active || !document_) return false;
    QVector<QUuid> siblings;
    for (const Layer &layer : document_->layers) if (layer.parentId == active->parentId) siblings.push_back(layer.id);
    const int index = siblings.indexOf(active->id);
    return index >= 0 && index + offset >= 0 && index + offset < siblings.size();
}

void EditorSession::moveActiveLayer(int offset)
{
    if (!canMoveActiveLayer(offset) || !document_->activeLayerId) return;
    const Layer *active = activeLayer();
    QVector<QUuid> siblings;
    for (const Layer &layer : document_->layers) if (layer.parentId == active->parentId) siblings.push_back(layer.id);
    const int siblingIndex = siblings.indexOf(active->id);
    const int a = indexOf(active->id), b = indexOf(siblings.at(siblingIndex + offset));
    beginEdit(QStringLiteral("Reorder Layers"));
    document_->layers.swapItemsAt(a, b);
    endEdit();
}

bool EditorSession::reorderLayers(const QVector<int> &topFirstRows, int destination)
{
    if (!document_ || topFirstRows.isEmpty() || destination < 0 || destination > document_->layers.size()) return false;
    QVector<Layer> topFirst(document_->layers.crbegin(), document_->layers.crend());
    QVector<int> rows = topFirstRows;
    std::sort(rows.begin(), rows.end());
    for (int row : rows) if (row < 0 || row >= topFirst.size()) return false;
    QVector<Layer> moved;
    for (int i = rows.size() - 1; i >= 0; --i) moved.prepend(topFirst.takeAt(rows.at(i)));
    int adjusted = destination;
    for (int row : rows) if (row < destination) --adjusted;
    adjusted = std::clamp(adjusted, 0, int(topFirst.size()));
    for (int i = 0; i < moved.size(); ++i) topFirst.insert(adjusted + i, moved.at(i));
    QVector<Layer> bottomFirst(topFirst.crbegin(), topFirst.crend());
    if (bottomFirst == document_->layers) return false;
    beginEdit(QStringLiteral("Reorder Layers"));
    document_->layers = std::move(bottomFirst);
    endEdit();
    return true;
}

QVector<QUuid> EditorSession::copyableLayerIds() const
{
    if (!document_) return {};
    QSet<QUuid> selected = selectedLayerIds_;
    if (document_->activeLayerId) selected.insert(*document_->activeLayerId);
    QSet<QUuid> nested;
    for (const QUuid &id : std::as_const(selected)) nested |= descendantIds(id);
    QVector<QUuid> ids;
    for (const Layer &layer : document_->layers) if (selected.contains(layer.id) && !nested.contains(layer.id)) ids << layer.id;
    return ids;
}

bool EditorSession::duplicateLayers(const QVector<QUuid> &ids, const QString &historyName)
{
    if (!document_ || ids.isEmpty()) return false;
    // Roots in document order; each copy block goes right after its original's subtree. Later blocks first keeps indices valid.
    QVector<QUuid> roots;
    for (const Layer &layer : document_->layers) if (ids.contains(layer.id)) roots << layer.id;
    if (roots.isEmpty()) return false;
    qint64 added = 0, existing = 0;
    for (const Layer &layer : std::as_const(document_->layers)) existing += qint64(layer.image.width()) * layer.image.height();
    beginEdit(historyName);
    QVector<QUuid> copies;
    std::optional<QUuid> activeCopy;
    const std::optional<QUuid> active = document_->activeLayerId;
    for (int r = roots.size() - 1; r >= 0; --r) {
        const int start = indexOf(roots[r]);
        const QSet<QUuid> included = descendantIds(roots[r]) | QSet<QUuid>{roots[r]};
        QVector<Layer> block;
        int end = start;
        for (int i = start; i < document_->layers.size(); ++i) if (included.contains(document_->layers.at(i).id)) { block << document_->layers.at(i); end = i; }
        QHash<QUuid, QUuid> mapping;
        for (const Layer &layer : std::as_const(block)) { mapping.insert(layer.id, QUuid::createUuid()); added += qint64(layer.image.width()) * layer.image.height(); }
        if (existing + added > 100000000LL) { added = 0; continue; }
        for (Layer &layer : block) {
            const QUuid old = layer.id; layer.id = mapping.value(old);
            if (old == roots[r]) layer.name += QStringLiteral(" copy");
            if (layer.parentId && mapping.contains(*layer.parentId)) layer.parentId = mapping.value(*layer.parentId);
            if (layer.maskSourceId && mapping.contains(*layer.maskSourceId)) layer.maskSourceId = mapping.value(*layer.maskSourceId);
        }
        copies << mapping.value(roots[r]);
        if (active && *active == roots[r]) activeCopy = mapping.value(roots[r]);
        for (int i = 0; i < block.size(); ++i) document_->layers.insert(end + 1 + i, block.at(i));
    }
    if (copies.isEmpty()) { endEdit(); return false; }
    selectLayers(QSet<QUuid>(copies.cbegin(), copies.cend()), activeCopy ? activeCopy : std::optional<QUuid>(copies.constLast()));
    endEdit();
    return true;
}

void EditorSession::duplicateActiveLayer()
{
    const Layer *active = activeLayer();
    if (!active || active->group) return;
    const int index = indexOf(active->id);
    Layer copy = *active;
    copy.id = QUuid::createUuid();
    copy.name += QStringLiteral(" copy");
    beginEdit(QStringLiteral("Duplicate Layer"));
    document_->layers.insert(index + 1, copy);
    selectLayer(copy.id);
    endEdit();
}

bool EditorSession::canPlaceLayer(const QUuid &id, const std::optional<QUuid> &parent) const
{
    if (indexOf(id) < 0) return false;
    if (!parent) return true;
    const int parentIndex = indexOf(*parent);
    return *parent != id && parentIndex >= 0 && document_->layers.at(parentIndex).group && !descendantIds(id).contains(*parent);
}

bool EditorSession::placeLayer(const QUuid &id, const std::optional<QUuid> &parent,
                               const std::optional<QUuid> &above, bool atBottom)
{
    QSet<QUuid> carried = descendantIds(id); carried.insert(id);
    if (!canPlaceLayer(id, parent) || (above && carried.contains(*above))) return false;
    QVector<Layer> layers, block;
    for (Layer layer : std::as_const(document_->layers)) {
        if (carried.contains(layer.id)) { if (layer.id == id) layer.parentId = parent; block.push_back(std::move(layer)); }
        else layers.push_back(std::move(layer));
    }
    int insertion = atBottom ? 0 : layers.size();
    if (above) {
        insertion = -1;
        for (int i = 0; i < layers.size(); ++i) if (layers.at(i).id == *above && layers.at(i).parentId == parent) { insertion = i + 1; break; }
        if (insertion < 0) return false;
    }
    for (int i = 0; i < block.size(); ++i) layers.insert(insertion + i, block.at(i));
    if (layers == document_->layers) return false;
    beginEdit(QStringLiteral("Move Layer"));
    document_->layers = std::move(layers);
    selectLayer(id);
    endEdit();
    return true;
}

bool EditorSession::duplicateLayer(const QUuid &id, const std::optional<QUuid> &parent,
                                   const std::optional<QUuid> &above, bool atBottom)
{
    const int sourceIndex = indexOf(id);
    if (!document_ || sourceIndex < 0 || document_->layers.at(sourceIndex).group || !canPlaceLayer(id, parent)) return false;
    Layer copy = document_->layers.at(sourceIndex); copy.id = QUuid::createUuid(); copy.name += QStringLiteral(" copy"); copy.parentId = parent;
    QVector<Layer> layers = document_->layers;
    int insertion = atBottom ? 0 : layers.size();
    if (above) {
        insertion = -1;
        for (int i = 0; i < layers.size(); ++i) if (layers.at(i).id == *above && layers.at(i).parentId == parent) { insertion = i + 1; break; }
        if (insertion < 0) return false;
    }
    beginEdit(QStringLiteral("Duplicate Layer")); layers.insert(insertion, copy); document_->layers = std::move(layers); selectLayer(copy.id); endEdit(); return true;
}

bool EditorSession::moveActiveLayerOutOfGroup()
{
    const Layer *layer = activeLayer(); if (!document_ || !layer || !layer->parentId) return false;
    const QUuid id = layer->id, parentId = *layer->parentId;
    const Layer *group = nullptr; for (const Layer &candidate : document_->layers) if (candidate.id == parentId) { group = &candidate; break; }
    if (!group) return false;
    return placeLayer(id, group->parentId, group->id);
}

void EditorSession::groupSelectedLayers()
{
    if (!document_ || selectedLayerIds_.isEmpty() || document_->layers.size() >= 10000) return;
    Layer group;
    group.id = QUuid::createUuid();
    group.name = nextName(QStringLiteral("Folder"));
    group.group = true;
    group.transform.size = document_->canvasSize;
    int highest = -1;
    for (int i = 0; i < document_->layers.size(); ++i) if (selectedLayerIds_.contains(document_->layers.at(i).id)) highest = i;
    if (highest < 0) return;
    beginEdit(QStringLiteral("Group Layers"));
    document_->layers.insert(highest + 1, group);
    for (Layer &layer : document_->layers) if (selectedLayerIds_.contains(layer.id)) layer.parentId = group.id;
    selectLayer(group.id);
    endEdit();
}

bool EditorSession::canUngroupLayers() const
{
    const Layer *layer = activeLayer();
    return document_ && layer && layer->group;
}

// Reverses Group Layers (mac 1318f1e): the folder's direct children take its place among its own siblings, in the
// order they had inside it, and the folder goes. Its own opacity, blend mode, mask and effects go with it, as
// Photoshop's Ungroup does. Clipping between two of its own layers survives; clipping that no longer makes sense
// (a clipped layer whose base is no longer its sibling right below it) is released.
void EditorSession::ungroupLayers()
{
    if (!canUngroupLayers()) return;
    const QUuid groupId = *document_->activeLayerId;
    const int groupIndex = indexOf(groupId);
    if (groupIndex < 0) return;
    const std::optional<QUuid> newParent = document_->layers.at(groupIndex).parentId;
    QVector<Layer> layers;
    QSet<QUuid> childIds;
    std::optional<QUuid> firstChild;
    QVector<Layer> children;
    for (const Layer &layer : document_->layers) if (layer.parentId == groupId) children.append(layer);
    for (Layer &child : children) { child.parentId = newParent; childIds.insert(child.id); if (!firstChild) firstChild = child.id; }
    for (const Layer &layer : document_->layers) {
        if (layer.id == groupId) layers += children;           // spliced in where the folder sat
        else if (!childIds.contains(layer.id)) layers.append(layer);
    }
    // Release clipping that no longer makes sense: a clipped layer stays clipped only when every sibling between it and
    // its base (in the same folder) is clipped to that base too.
    for (int i = 0; i < layers.size(); ++i) {
        Layer &layer = layers[i];
        if (!layer.maskSourceId) continue;
        bool keep = false;
        for (int j = i - 1; j >= 0; --j) {
            if (layers.at(j).parentId != layer.parentId) continue;
            if (layers.at(j).id == *layer.maskSourceId) { keep = !layers.at(j).maskSourceId; break; }
            if (layers.at(j).maskSourceId != layer.maskSourceId) break;
        }
        if (!keep) layer.maskSourceId.reset();
    }
    beginEdit(QStringLiteral("Ungroup Layers"));
    document_->layers = layers;
    selectLayers(childIds, firstChild);
    endEdit();
}

bool EditorSession::canEditEffects() const
{
    if (!document_ || !document_->activeLayerId) return false;
    const int idx = indexOf(*document_->activeLayerId);
    if (idx < 0 || idx >= document_->layers.size()) return false;
    const Layer &l = document_->layers[idx];
    return !l.group && l.adjustment.isEmpty() && !l.image.isNull();
}

std::optional<LayerEffects> EditorSession::activeLayerEffects() const
{
    if (!document_ || !document_->activeLayerId) return std::nullopt;
    return layerEffects(*document_->activeLayerId);
}

std::optional<LayerEffects> EditorSession::layerEffects(const QUuid &id) const
{
    if (!document_) return std::nullopt;
    const int idx = indexOf(id);
    if (idx < 0 || idx >= document_->layers.size()) return std::nullopt;
    return document_->layers[idx].effects;
}

bool EditorSession::setLayerEffects(const QUuid &id, const LayerEffects &effects, const QString &historyName)
{
    if (!document_ || !effects.isValid()) return false;
    const int idx = indexOf(id);
    if (idx < 0 || idx >= document_->layers.size()) return false;
    const Layer &layer = document_->layers.at(idx);
    if (layer.group || !layer.adjustment.isEmpty() || layer.image.isNull()) return false;

    const std::optional<LayerEffects> newEffects = effects.isEmpty() ? std::nullopt : std::optional<LayerEffects>(effects);
    if (layer.effects == newEffects) return true;

    beginEdit(historyName.isEmpty() ? QStringLiteral("Layer Effects") : historyName);
    document_->layers[idx].effects = newEffects;
    endEdit();
    return true;
}

bool EditorSession::addLayerEffect(const QUuid &id, LayerEffectKind kind)
{
    if (!document_ || !canEditEffects()) return false;
    const int idx = indexOf(id);
    if (idx < 0 || idx >= document_->layers.size()) return false;
    LayerEffects eff = document_->layers[idx].effects.value_or(LayerEffects());
    eff.setEnabled(kind, true);
    switch (kind) {
    case LayerEffectKind::Stroke:
        if (!eff.stroke) {
            StrokeEffect s;
            s.enabled = true;
            s.size = 3.0;
            s.inside = false;
            eff.stroke = s;
        }
        break;
    case LayerEffectKind::DropShadow:
        if (!eff.shadow) {
            ShadowEffect s;
            s.enabled = true;
            eff.shadow = s;
        }
        break;
    case LayerEffectKind::ColorOverlay:
        if (!eff.colorOverlay) {
            ColorOverlayEffect s;
            s.enabled = true;
            eff.colorOverlay = s;
        }
        break;
    case LayerEffectKind::InnerShadow:
        if (!eff.innerShadow) {
            InnerShadowEffect s;
            s.enabled = true;
            eff.innerShadow = s;
        }
        break;
    case LayerEffectKind::OuterGlow:
        if (!eff.outerGlow) {
            OuterGlowEffect s;
            s.enabled = true;
            eff.outerGlow = s;
        }
        break;
    case LayerEffectKind::InnerGlow:
        if (!eff.innerGlow) {
            InnerGlowEffect s;
            s.enabled = true;
            eff.innerGlow = s;
        }
        break;
    }
    const QString name = QStringLiteral("Add ") + layerEffectKindToString(kind);
    return setLayerEffects(id, eff, name);
}

bool EditorSession::toggleLayerEffect(const QUuid &id, LayerEffectKind kind)
{
    if (!document_) return false;
    const int idx = indexOf(id);
    if (idx < 0 || idx >= document_->layers.size()) return false;
    Layer &layer = document_->layers[idx];
    if (!layer.effects) return false;
    LayerEffects eff = *layer.effects;
    const bool enabled = eff.isEnabled(kind);
    eff.setEnabled(kind, !enabled);
    const QString name = (enabled ? QStringLiteral("Hide ") : QStringLiteral("Show ")) + layerEffectKindToString(kind);
    return setLayerEffects(id, eff, name);
}

bool EditorSession::removeLayerEffect(const QUuid &id, LayerEffectKind kind)
{
    if (!document_) return false;
    const int idx = indexOf(id);
    if (idx < 0 || idx >= document_->layers.size()) return false;
    Layer &layer = document_->layers[idx];
    if (!layer.effects) return false;
    LayerEffects eff = *layer.effects;
    eff.remove(kind);
    const QString name = QStringLiteral("Remove ") + layerEffectKindToString(kind);
    return setLayerEffects(id, eff, name);
}

bool EditorSession::canCopyLayerEffect(LayerEffectKind kind, const QUuid &sourceId, const QUuid &targetId) const
{
    if (!document_ || sourceId == targetId || !canEditLayers()) return false;
    const int srcIdx = indexOf(sourceId);
    const int dstIdx = indexOf(targetId);
    if (srcIdx < 0 || dstIdx < 0) return false;
    const Layer &srcLayer = document_->layers.at(srcIdx);
    const Layer &dstLayer = document_->layers.at(dstIdx);
    if (!srcLayer.effects || !srcLayer.effects->contains(kind)) return false;
    if (dstLayer.group || !dstLayer.adjustment.isEmpty() || (dstLayer.image.isNull() && !dstLayer.text && dstLayer.shape.isEmpty())) return false;
    return true;
}

bool EditorSession::copyLayerEffect(LayerEffectKind kind, const QUuid &sourceId, const QUuid &targetId)
{
    if (!canCopyLayerEffect(kind, sourceId, targetId)) return false;
    const int srcIdx = indexOf(sourceId);
    const int dstIdx = indexOf(targetId);
    if (srcIdx < 0 || dstIdx < 0) return false;
    const Layer &srcLayer = document_->layers.at(srcIdx);
    const Layer &dstLayer = document_->layers.at(dstIdx);
    LayerEffects dstEffects = dstLayer.effects.value_or(LayerEffects());
    switch (kind) {
    case LayerEffectKind::Stroke: dstEffects.stroke = srcLayer.effects->stroke; break;
    case LayerEffectKind::DropShadow: dstEffects.shadow = srcLayer.effects->shadow; break;
    case LayerEffectKind::ColorOverlay: dstEffects.colorOverlay = srcLayer.effects->colorOverlay; break;
    case LayerEffectKind::InnerShadow: dstEffects.innerShadow = srcLayer.effects->innerShadow; break;
    case LayerEffectKind::OuterGlow: dstEffects.outerGlow = srcLayer.effects->outerGlow; break;
    case LayerEffectKind::InnerGlow: dstEffects.innerGlow = srcLayer.effects->innerGlow; break;
    }
    return setLayerEffects(targetId, dstEffects, QStringLiteral("Copy ") + layerEffectKindToString(kind));
}

bool EditorSession::copyAllLayerEffects(const QUuid &sourceId, const QUuid &targetId)
{
    if (!document_ || sourceId == targetId) return false;
    const int srcIdx = indexOf(sourceId);
    const int dstIdx = indexOf(targetId);
    if (srcIdx < 0 || dstIdx < 0) return false;
    const Layer &srcLayer = document_->layers[srcIdx];
    Layer &dstLayer = document_->layers[dstIdx];
    if (!srcLayer.effects) return false;
    if (dstLayer.group || !dstLayer.adjustment.isEmpty() || dstLayer.image.isNull()) return false;
    return setLayerEffects(targetId, *srcLayer.effects, QStringLiteral("Copy Layer Effects"));
}

bool EditorSession::clearLayerEffects(const QUuid &id)
{
    return setLayerEffects(id, LayerEffects(), QStringLiteral("Clear Layer Effects"));
}

void EditorSession::restore(const DocumentHistory::Snapshot &snapshot)
{
    const bool keepMaskTarget = maskSelected_ && document_ && document_->activeLayerId == snapshot.activeLayerId;
    document_ = snapshot.document ? std::make_shared<Document>(*snapshot.document) : nullptr;
    advanceRevision();
    selectedLayerIds_.clear();
    if (document_ && snapshot.activeLayerId) {
        document_->activeLayerId = snapshot.activeLayerId;
        selectedLayerIds_.insert(*snapshot.activeLayerId);
    }
    const Layer *layer = activeLayer();
    maskSelected_ = keepMaskTarget && layer && !layer->mask.isNull();
}

void EditorSession::undo()
{
    if (const auto value = history_.undo()) restore(*value);
}

void EditorSession::redo()
{
    if (const auto value = history_.redo()) restore(*value);
}

bool EditorSession::canClearGuides() const
{
    return document_ && !document_->guides.isEmpty();
}

bool EditorSession::canEditGuides() const
{
    return document_ != nullptr && !locksGuides_;
}

QVector<CanvasGuide> EditorSession::displayedGuides() const
{
    QVector<CanvasGuide> guides = document_ ? document_->guides : QVector<CanvasGuide>{};
    if (!guideDrag_) return guides;
    CanvasGuide current;
    current.id = guideDrag_->id;
    current.axis = guideDrag_->axis;
    current.position = guideDrag_->position;
    bool found = false;
    for (int i = 0; i < guides.size(); ++i) {
        if (guides[i].id == guideDrag_->id) {
            guides[i] = current;
            found = true;
            break;
        }
    }
    if (!found && guideDrag_->isNew) {
        guides.append(current);
    }
    return guides;
}

std::optional<CanvasGuide> EditorSession::hitGuide(const QPointF &docPoint, double tolerance) const
{
    if (!showsGuides_ || locksGuides_ || !document_) return std::nullopt;
    std::optional<CanvasGuide> best;
    double bestDist = tolerance + 1.0;
    for (const CanvasGuide &guide : displayedGuides()) {
        const double dist = (guide.axis == CanvasGuide::Axis::Vertical)
            ? std::abs(docPoint.x() - guide.position)
            : std::abs(docPoint.y() - guide.position);
        if (dist <= tolerance && dist < bestDist) {
            best = guide;
            bestDist = dist;
        }
    }
    return best;
}

void EditorSession::beginGuideCreation(CanvasGuide::Axis axis, double position)
{
    if (!canEditGuides()) return;
    showsGuides_ = true;
    GuideDrag drag;
    drag.id = QUuid::createUuid();
    drag.axis = axis;
    drag.position = snappedGuidePosition(position, axis, std::nullopt);
    drag.isNew = true;
    drag.original = std::nullopt;
    guideDrag_ = drag;
}

void EditorSession::beginGuideMove(const CanvasGuide &guide)
{
    if (!canEditGuides()) return;
    GuideDrag drag;
    drag.id = guide.id;
    drag.axis = guide.axis;
    drag.position = guide.position;
    drag.isNew = false;
    drag.original = guide.position;
    guideDrag_ = drag;
}

void EditorSession::moveGuideDrag(double position)
{
    if (!guideDrag_) return;
    guideDrag_->position = snappedGuidePosition(position, guideDrag_->axis, guideDrag_->id);
}

void EditorSession::finishGuideDrag(bool deleteGuide)
{
    if (!guideDrag_ || !document_) {
        guideDrag_ = std::nullopt;
        return;
    }
    const GuideDrag drag = *guideDrag_;
    guideDrag_ = std::nullopt;

    if (deleteGuide) {
        if (drag.isNew) return;
        beginEdit(QStringLiteral("Delete Guide"));
        for (auto it = document_->guides.begin(); it != document_->guides.end(); ) {
            if (it->id == drag.id) it = document_->guides.erase(it);
            else ++it;
        }
        endEdit();
        return;
    }

    if (drag.isNew) {
        beginEdit(QStringLiteral("New Guide"));
        if (document_->formatVersion < 8) document_->formatVersion = 8;
        CanvasGuide guide;
        guide.id = drag.id;
        guide.axis = drag.axis;
        guide.position = drag.position;
        document_->guides.append(guide);
        endEdit();
    } else if (!drag.original || *drag.original != drag.position) {
        beginEdit(QStringLiteral("Move Guide"));
        for (auto &g : document_->guides) {
            if (g.id == drag.id) {
                g.position = drag.position;
                break;
            }
        }
        endEdit();
    }
}

void EditorSession::cancelGuideDrag()
{
    guideDrag_ = std::nullopt;
}

void EditorSession::clearGuides()
{
    if (!canClearGuides() || !document_) return;
    beginEdit(QStringLiteral("Clear Guides"));
    document_->guides.clear();
    endEdit();
}

void EditorSession::addGuide(const CanvasGuide &guide)
{
    if (!canEditGuides() || !document_) return;
    showsGuides_ = true;
    beginEdit(QStringLiteral("New Guide"));
    if (document_->formatVersion < 8) document_->formatVersion = 8;
    document_->guides.append(guide);
    endEdit();
}

double EditorSession::snappedGuidePosition(double position, CanvasGuide::Axis axis,
                                          const std::optional<QUuid> &excluding,
                                          double tolerance) const
{
    if (!snapEnabled_ || !document_) return position;
    QVector<double> targets;
    const double length = (axis == CanvasGuide::Axis::Vertical)
        ? double(document_->canvasSize.width())
        : double(document_->canvasSize.height());

    if (snapToGrid_ && showsGrid_) {
        targets += layoutGrid_.lines(length);
    }
    if (snapToGuides_ && showsGuides_) {
        for (const CanvasGuide &g : displayedGuides()) {
            if (g.axis == axis && (!excluding || g.id != *excluding)) {
                targets.append(g.position);
            }
        }
    }
    if (snapToDocumentBounds_) {
        targets.append(0.0);
        targets.append(length / 2.0);
        targets.append(length);
    }
    if (snapToLayers_) {
        for (const Layer &layer : document_->layers) {
            if (layer.group || layer.image.isNull() || !layer.transform.isValid()) continue;
            const auto corners = layerCorners(layer.transform);
            double minV = (axis == CanvasGuide::Axis::Vertical) ? corners[0].x() : corners[0].y();
            double maxV = minV;
            for (int i = 1; i < 4; ++i) {
                const double v = (axis == CanvasGuide::Axis::Vertical) ? corners[i].x() : corners[i].y();
                minV = std::min(minV, v);
                maxV = std::max(maxV, v);
            }
            targets.append(std::round(minV));
            targets.append(std::round((minV + maxV) / 2.0));
            targets.append(std::round(maxV));
        }
    }
    std::optional<double> best;
    double bestDiff = tolerance + 1.0;
    for (double target : targets) {
        const double diff = std::abs(target - position);
        if (diff <= tolerance) {
            if (!best || diff < bestDiff) {
                best = target;
                bestDiff = diff;
            }
        }
    }
    return best.value_or(position);
}

static std::optional<double> nearestTarget(double value, const QVector<double> &lines, double tolerance)
{
    std::optional<double> best;
    for (const double line : lines)
        if (std::abs(line - value) <= tolerance && (!best || std::abs(line - value) < std::abs(*best - value))) best = line;
    return best;
}

QPointF EditorSession::snappedResizePoint(const QPointF &point, const LayerTransform &start, const QPointF &startPoint,
                                          const QPoint &sign, bool fromCenter, bool lockRatio, const QSet<QUuid> &moving,
                                          double tolerance, std::optional<double> *guideX, std::optional<double> *guideY) const
{
    if (guideX) guideX->reset();
    if (guideY) guideY->reset();
    if (!snapEnabled_ || !document_ || start.rotation != 0.0) return point;
    const SnapTargets targets = transformSnapTargets(moving);
    const QPointF handlePoint = start.center() + QPointF(sign.x() * start.size.width() / 2.0, sign.y() * start.size.height() / 2.0);
    const QPointF at = handlePoint + point - startPoint;   // where the dragged handle is, to tell its edge from the far one
    const auto edge = [&](const LayerTransform &t, bool horizontal) {
        const QRectF box(t.origin, t.size);
        return horizontal ? (std::abs(box.left() - at.x()) <= std::abs(box.right() - at.x()) ? box.left() : box.right())
                          : (std::abs(box.top() - at.y()) <= std::abs(box.bottom() - at.y()) ? box.top() : box.bottom());
    };
    const auto update = [&](const QPointF &p) { return resizedByHandle(start, sign, startPoint, p, fromCenter, lockRatio); };
    const LayerTransform draft = update(point);
    struct Snap { bool horizontal; double target; };
    QVector<Snap> snaps;
    if (sign.x() != 0) if (const auto x = nearestTarget(edge(draft, true), targets.xs, tolerance)) snaps.append({true, *x});
    if (sign.y() != 0) if (const auto y = nearestTarget(edge(draft, false), targets.ys, tolerance)) snaps.append({false, *y});
    if (lockRatio && snaps.size() == 2) {
        const Snap nearer = std::abs(snaps[0].target - edge(draft, snaps[0].horizontal)) <= std::abs(snaps[1].target - edge(draft, snaps[1].horizontal)) ? snaps[0] : snaps[1];
        snaps = {nearer};
    }
    // An edge follows the pointer in a straight line along each axis, so one step measured across a pixel lands it.
    QPointF result = point;
    for (const Snap &snap : snaps) {
        const double before = edge(update(result), snap.horizontal);
        QPointF nudged = result;
        (snap.horizontal ? nudged.rx() : nudged.ry()) += 1.0;
        const double perPixel = edge(update(nudged), snap.horizontal) - before;
        if (std::abs(perPixel) <= 0.01) continue;
        (snap.horizontal ? result.rx() : result.ry()) += (snap.target - before) / perPixel;
        if (snap.horizontal) { if (guideX) *guideX = snap.target; } else if (guideY) *guideY = snap.target;
    }
    return result;
}

QPointF EditorSession::snappedPoint(const QPointF &point, double tolerance, std::optional<double> *guideX, std::optional<double> *guideY) const
{
    if (guideX) guideX->reset();
    if (guideY) guideY->reset();
    if (!snapEnabled_ || !document_) return point;
    const SnapTargets targets = cropSnapTargets();
    const auto x = nearestTarget(point.x(), targets.xs, tolerance), y = nearestTarget(point.y(), targets.ys, tolerance);
    if (guideX) *guideX = x;
    if (guideY) *guideY = y;
    return QPointF(x.value_or(point.x()), y.value_or(point.y()));
}

QPointF EditorSession::snappedSelectionOffset(const QRectF &box, const QPointF &offset, double tolerance, bool horizontal, bool vertical,
                                              std::optional<double> *guideX, std::optional<double> *guideY) const
{
    if (guideX) guideX->reset();
    if (guideY) guideY->reset();
    const QPointF rounded(std::round(offset.x()), std::round(offset.y()));
    if (!snapEnabled_ || !document_) return rounded;
    const SnapTargets targets = cropSnapTargets();
    const QRectF moved = box.translated(rounded);
    QPointF result = rounded;
    const auto snapAxis = [&](const QVector<double> &lines, double a, double mid, double b, double &out, std::optional<double> *guide) {
        std::optional<double> best; double delta = 0;
        for (const double edge : {a, mid, b}) {
            const auto t = nearestTarget(edge, lines, tolerance);
            if (t && (!best || std::abs(*t - edge) < std::abs(delta))) { best = t; delta = *t - edge; }
        }
        if (best) { out += delta; if (guide) *guide = best; }
    };
    if (horizontal) snapAxis(targets.xs, moved.left(), moved.center().x(), moved.right(), result.rx(), guideX);
    if (vertical) snapAxis(targets.ys, moved.top(), moved.center().y(), moved.bottom(), result.ry(), guideY);
    return result;
}

EditorSession::SnapTargets EditorSession::alignmentSnapTargets(const QSet<QUuid> &excludingLayers, bool includeCenters) const
{
    if (!snapEnabled_ || !document_) return {};
    QVector<double> xs;
    QVector<double> ys;

    if (snapToDocumentBounds_) {
        xs.append(0.0);
        xs.append(double(document_->canvasSize.width()));
        ys.append(0.0);
        ys.append(double(document_->canvasSize.height()));
        if (includeCenters) {
            xs.append(double(document_->canvasSize.width()) / 2.0);
            ys.append(double(document_->canvasSize.height()) / 2.0);
        }
    }

    if (snapToLayers_) {
        for (const Layer &layer : document_->layers) {
            if (layer.group || layer.image.isNull() || !layer.transform.isValid() || excludingLayers.contains(layer.id)) continue;
            const auto corners = layerCorners(layer.transform);
            double minX = corners[0].x(), maxX = corners[0].x();
            double minY = corners[0].y(), maxY = corners[0].y();
            for (int i = 1; i < 4; ++i) {
                minX = std::min(minX, corners[i].x());
                maxX = std::max(maxX, corners[i].x());
                minY = std::min(minY, corners[i].y());
                maxY = std::max(maxY, corners[i].y());
            }
            if (includeCenters) {
                xs.append(std::round(minX));
                xs.append(std::round((minX + maxX) / 2.0));
                xs.append(std::round(maxX));
                ys.append(std::round(minY));
                ys.append(std::round((minY + maxY) / 2.0));
                ys.append(std::round(maxY));
            } else {
                xs.append(std::round(minX));
                xs.append(std::round(maxX));
                ys.append(std::round(minY));
                ys.append(std::round(maxY));
            }
        }
    }

    // Hidden extras do not snap, matching Photoshop.
    if (snapToGrid_ && showsGrid_) {
        xs += layoutGrid_.lines(document_->canvasSize.width());
        ys += layoutGrid_.lines(document_->canvasSize.height());
    }

    if (snapToGuides_ && showsGuides_) {
        for (const CanvasGuide &guide : displayedGuides()) {
            if (guide.axis == CanvasGuide::Axis::Vertical) {
                xs.append(guide.position);
            } else {
                ys.append(guide.position);
            }
        }
    }

    return {xs, ys};
}

QVector<double> EditorSession::cropSnapTargetsX() const
{
    return alignmentSnapTargets({}, false).xs;
}

QVector<double> EditorSession::cropSnapTargetsY() const
{
    return alignmentSnapTargets({}, false).ys;
}

} // namespace compositor
