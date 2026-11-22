#include "core/LayerAlign.h"
#include "core/EditorSession.h"
#include "rendering/LayerRenderer.h"

#include <QHash>
#include <QTransform>
#include <algorithm>
#include <numeric>

namespace compositor {

QString layerAlignmentName(LayerAlignment alignment)
{
    switch (alignment) {
    case LayerAlignment::Left: return QStringLiteral("Left Edges");
    case LayerAlignment::HorizontalCenter: return QStringLiteral("Horizontal Centers");
    case LayerAlignment::Right: return QStringLiteral("Right Edges");
    case LayerAlignment::Top: return QStringLiteral("Top Edges");
    case LayerAlignment::VerticalCenter: return QStringLiteral("Vertical Centers");
    case LayerAlignment::Bottom: return QStringLiteral("Bottom Edges");
    }
    return {};
}

QString layerDistributionName(LayerDistribution distribution)
{
    switch (distribution) {
    case LayerDistribution::HorizontalCenters: return QStringLiteral("Horizontal Centers");
    case LayerDistribution::VerticalCenters: return QStringLiteral("Vertical Centers");
    case LayerDistribution::HorizontalSpacing: return QStringLiteral("Horizontal Spacing");
    case LayerDistribution::VerticalSpacing: return QStringLiteral("Vertical Spacing");
    }
    return {};
}

QSizeF alignmentOffset(LayerAlignment alignment, const QRectF &box, const QRectF &target)
{
    switch (alignment) {
    case LayerAlignment::Left: return {target.left() - box.left(), 0};
    case LayerAlignment::HorizontalCenter: return {target.center().x() - box.center().x(), 0};
    case LayerAlignment::Right: return {target.right() - box.right(), 0};
    case LayerAlignment::Top: return {0, target.top() - box.top()};
    case LayerAlignment::VerticalCenter: return {0, target.center().y() - box.center().y()};
    case LayerAlignment::Bottom: return {0, target.bottom() - box.bottom()};
    }
    return {};
}

QVector<QSizeF> distributionOffsets(LayerDistribution distribution, const QVector<QRectF> &boxes)
{
    QVector<QSizeF> offsets(boxes.size(), QSizeF(0, 0));
    if (boxes.size() <= 2) return offsets;
    const bool horizontal = distribution == LayerDistribution::HorizontalCenters || distribution == LayerDistribution::HorizontalSpacing;
    const auto start = [horizontal](const QRectF &box) { return horizontal ? box.left() : box.top(); };
    const auto length = [horizontal](const QRectF &box) { return horizontal ? box.width() : box.height(); };
    QVector<int> order(boxes.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return start(boxes[a]) + length(boxes[a]) / 2 < start(boxes[b]) + length(boxes[b]) / 2;
    });
    const QRectF first = boxes[order.first()], last = boxes[order.last()];
    const double steps = double(order.size() - 1);
    const auto offset = [horizontal](double delta) { return horizontal ? QSizeF(delta, 0) : QSizeF(0, delta); };
    if (distribution == LayerDistribution::HorizontalCenters || distribution == LayerDistribution::VerticalCenters) {
        const double from = start(first) + length(first) / 2, to = start(last) + length(last) / 2;
        for (int rank = 0; rank < order.size(); ++rank) {
            const int index = order[rank];
            offsets[index] = offset(from + (to - from) * rank / steps - (start(boxes[index]) + length(boxes[index]) / 2));
        }
    } else {
        const double span = start(last) + length(last) - start(first);
        double used = 0;
        for (int index : order) used += length(boxes[index]);
        const double gap = (span - used) / steps;
        double position = start(first);
        for (int index : order) {
            offsets[index] = offset(position - start(boxes[index]));
            position += length(boxes[index]) + gap;
        }
    }
    return offsets;
}

// The upright box around a layer's turned transform box, as snapping places layers.
static QRectF uprightBox(const LayerTransform &value)
{
    QTransform transform; transform.translate(value.center().x(), value.center().y()); transform.rotate(value.rotation);
    const double w = value.size.width() / 2, h = value.size.height() / 2;
    return transform.mapRect(QRectF(-w, -h, w * 2, h * 2));
}

QVector<EditorSession::AlignmentItem> EditorSession::alignmentItems() const
{
    QVector<AlignmentItem> items;
    if (!document_) return items;
    QHash<QUuid, std::optional<QUuid>> parents;
    for (const Layer &layer : document_->layers) parents.insert(layer.id, layer.parentId);
    const auto selectedAncestor = [&](const QUuid &id) -> std::optional<QUuid> {
        std::optional<QUuid> current = parents.value(id);
        for (int depth = 0; depth < 64 && current; ++depth) {
            if (selectedLayerIds_.contains(*current)) return current;
            current = parents.value(*current);
        }
        return std::nullopt;
    };
    // Each moving layer under the outermost selected folder above it, or itself; a folder moves as one.
    QHash<QUuid, int> owners;
    for (const Layer &layer : document_->layers) {
        if (layer.image.isNull() || layer.group || !LayerRenderer::effectivelyVisible(*document_, layer)) continue;
        std::optional<QUuid> owner = selectedLayerIds_.contains(layer.id) ? std::optional<QUuid>(layer.id) : std::nullopt;
        for (auto ancestor = selectedAncestor(layer.id); ancestor; ancestor = selectedAncestor(*ancestor)) owner = ancestor;
        if (!owner) continue;
        if (!owners.contains(*owner)) { owners.insert(*owner, int(items.size())); items.push_back({}); }
        AlignmentItem &item = items[owners.value(*owner)];
        item.members.push_back(layer.id);
        item.box = item.box.isNull() ? uprightBox(layer.transform) : item.box.united(uprightBox(layer.transform));
    }
    return items;
}

bool EditorSession::canAlignLayers() const { return document_ && !selectedLayerIds_.isEmpty() && !brush_ && !warp_; }
bool EditorSession::canDistributeLayers() const { return canAlignLayers() && selectedLayerIds_.size() > 2; }

bool EditorSession::alignLayers(LayerAlignment alignment)
{
    const QVector<AlignmentItem> items = alignmentItems();
    if (!document_ || items.isEmpty()) return false;
    // With the selection when there is one, the canvas when one thing is selected, else the box around them all.
    QRectF target;
    if (const auto bounds = selectionBounds(); bounds && !bounds->isEmpty()) target = QRectF(*bounds);
    else if (items.size() == 1) target = QRectF(QPointF(), QSizeF(document_->canvasSize));
    else for (const AlignmentItem &item : items) target = target.isNull() ? item.box : target.united(item.box);
    QVector<QSizeF> offsets;
    for (const AlignmentItem &item : items) offsets.push_back(alignmentOffset(alignment, item.box, target));
    return moveAlignmentItems(items, offsets, QStringLiteral("Align ") + layerAlignmentName(alignment));
}

bool EditorSession::distributeLayers(LayerDistribution distribution)
{
    const QVector<AlignmentItem> items = alignmentItems();
    if (items.size() <= 2) return false;
    QVector<QRectF> boxes;
    for (const AlignmentItem &item : items) boxes.push_back(item.box);
    return moveAlignmentItems(items, distributionOffsets(distribution, boxes), QStringLiteral("Distribute ") + layerDistributionName(distribution));
}

bool EditorSession::moveAlignmentItems(const QVector<AlignmentItem> &items, const QVector<QSizeF> &offsets, const QString &name)
{
    // Whole pixels, so nothing lands between two and has to be resampled, as one undo step; masks follow their link.
    QHash<QUuid, QPointF> moves;
    for (int i = 0; i < items.size() && i < offsets.size(); ++i) {
        const QPointF whole(std::round(offsets[i].width()), std::round(offsets[i].height()));
        if (whole.isNull()) continue;
        for (const QUuid &id : items[i].members) moves.insert(id, whole);
    }
    if (moves.isEmpty() || !document_) return false;
    beginEdit(name);
    for (Layer &layer : document_->layers) {
        if (!moves.contains(layer.id)) continue;
        const LayerTransform old = layer.transform;
        layer.transform.origin += moves.value(layer.id);
        if (!layer.mask.isNull() && layer.mask.size() != QSize(1, 1)) {
            if (layer.maskLinked) { if (layer.maskPlacement) layer.maskPlacement = layer.maskPlacement->following(old, layer.transform); }
            else layer.maskPlacement = layer.maskPlacement.value_or(old);
        }
    }
    endEdit();
    return true;
}
} // namespace compositor
