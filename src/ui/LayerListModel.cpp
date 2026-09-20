#include "ui/LayerListModel.h"

#include <QIcon>
#include <QMimeData>
#include <QPainter>
#include <QSet>

#include <algorithm>

namespace compositor {

static QSize thumbnailSize(const QSize &canvas, int box = 36)
{
    if (canvas.isEmpty()) return {box, box};
    const double scale = double(box) / std::max(canvas.width(), canvas.height());
    return {std::max(1, qRound(canvas.width() * scale)), std::max(1, qRound(canvas.height() * scale))};
}

static void placeThumbnail(QPainter &painter, const QImage &image, const LayerTransform &placement,
                           const QSize &canvas, const QSize &thumbnail)
{
    if (image.isNull() || canvas.isEmpty()) return;
    const double scale = double(thumbnail.width()) / canvas.width();
    painter.save();
    painter.translate(placement.center().x() * scale, placement.center().y() * scale);
    painter.rotate(placement.rotation);
    painter.scale(placement.flipX ? -1 : 1, placement.flipY ? -1 : 1);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(QRectF(-placement.size.width() * scale / 2, -placement.size.height() * scale / 2,
                             placement.size.width() * scale, placement.size.height() * scale), image);
    painter.restore();
}

static QImage layerThumbnail(const Layer &layer, const QSize &canvas)
{
    const QSize size = thumbnailSize(canvas);
    QImage result(size, QImage::Format_ARGB32_Premultiplied); result.fill(QColor(56, 56, 56));
    QPainter painter(&result);
    constexpr int tile = 3;
    painter.setPen(Qt::NoPen); painter.setBrush(QColor(82, 82, 82));
    for (int y = 0; y < size.height(); y += tile) for (int x = 0; x < size.width(); x += tile)
        if (((x / tile) + (y / tile)) % 2 == 0) painter.drawRect(x, y, tile, tile);
    placeThumbnail(painter, layer.image, layer.transform, canvas, size);
    return result;
}

static QImage maskThumbnail(const Layer &layer, const QSize &canvas)
{
    const QImage mask = layer.mask.convertToFormat(QImage::Format_Grayscale8);
    qint64 sum = 0, count = 0;
    for (int y = 0; y < mask.height(); ++y) for (int x = 0; x < mask.width(); ++x)
        if (y == 0 || y + 1 == mask.height() || x == 0 || x + 1 == mask.width()) { sum += mask.constScanLine(y)[x]; ++count; }
    const int edge = count ? int(sum / count) : 255;
    const QSize size = thumbnailSize(canvas);
    QImage result(size, QImage::Format_Grayscale8); result.fill(edge);
    QPainter painter(&result);
    placeThumbnail(painter, mask, layer.maskPlacement.value_or(layer.transform), canvas, size);
    return result;
}

LayerListModel::LayerListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void LayerListModel::setDocument(std::shared_ptr<Document> document, bool reset)
{
    if (document_ == document && !reset) {
        if (rowCount() > 0) emit dataChanged(index(0, 0), index(rowCount() - 1, 0));
        return;
    }
    beginResetModel();
    document_ = std::move(document);
    endResetModel();
}

int LayerListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() || !document_ ? 0 : visibleLayerIndexes().size();
}

int LayerListModel::layerIndex(int row) const
{
    const QVector<int> indexes = visibleLayerIndexes();
    return row >= 0 && row < indexes.size() ? indexes.at(row) : -1;
}

QVector<int> LayerListModel::visibleLayerIndexes() const
{
    QVector<int> result;
    if (!document_) return result;
    QHash<QUuid, const Layer *> byId;
    for (const Layer &layer : document_->layers) byId.insert(layer.id, &layer);
    for (int i = document_->layers.size() - 1; i >= 0; --i) {
        bool hidden = false;
        std::optional<QUuid> parent = document_->layers.at(i).parentId;
        QSet<QUuid> visited;
        while (parent && !visited.contains(*parent)) {
            visited.insert(*parent);
            if (collapsed_.contains(*parent)) { hidden = true; break; }
            const Layer *owner = byId.value(*parent, nullptr);
            parent = owner ? owner->parentId : std::nullopt;
        }
        if (!hidden) result.push_back(i);
    }
    return result;
}

int LayerListModel::depthFor(const Layer &layer) const
{
    if (!document_) return 0;
    int depth = 0; std::optional<QUuid> parent = layer.parentId; QSet<QUuid> visited;
    while (parent && depth < 32 && !visited.contains(*parent)) {
        visited.insert(*parent); ++depth;
        const auto it = std::find_if(document_->layers.cbegin(), document_->layers.cend(), [&](const Layer &candidate) { return candidate.id == *parent; });
        parent = it == document_->layers.cend() ? std::nullopt : it->parentId;
    }
    return depth;
}

std::optional<QUuid> LayerListModel::layerId(const QModelIndex &index) const
{
    const int i = index.isValid() ? layerIndex(index.row()) : -1;
    return document_ && i >= 0 && i < document_->layers.size() ? std::optional<QUuid>(document_->layers.at(i).id) : std::nullopt;
}

void LayerListModel::toggleExpanded(const QUuid &id)
{
    if (!document_) return;
    const auto it = std::find_if(document_->layers.cbegin(), document_->layers.cend(), [&](const Layer &layer) { return layer.id == id && layer.group; });
    if (it == document_->layers.cend()) return;
    beginResetModel();
    if (collapsed_.contains(id)) collapsed_.remove(id); else collapsed_.insert(id);
    endResetModel();
}

QVariant LayerListModel::data(const QModelIndex &index, int role) const
{
    if (!document_ || !index.isValid() || index.row() < 0 || index.row() >= document_->layers.size()) return {};
    const Layer &layer = document_->layers.at(layerIndex(index.row()));
    if (role == Qt::DisplayRole) return layer.name;
    if (role == Qt::CheckStateRole) return layer.visible ? Qt::Checked : Qt::Unchecked;
    if (role == Qt::DecorationRole) {
        if (layer.group) return QIcon::fromTheme(QStringLiteral("folder"));
        if (!layer.adjustment.isEmpty()) return QIcon::fromTheme(QStringLiteral("color-management"));
        if (!layer.image.isNull()) return QIcon(QPixmap::fromImage(layerThumbnail(layer, document_->canvasSize)));
    }
    if (role == Qt::ToolTipRole) {
        return QStringLiteral("%1%2").arg(layer.name, layer.group ? QStringLiteral(" (group)") : QString());
    }
    if (role == Qt::UserRole) {
        if (layer.group) return tr("Group");
        if (!layer.adjustment.isEmpty()) return tr("Adjustment · %1").arg(layer.adjustment.value(QStringLiteral("kind")).toString());
        if (!layer.image.isNull()) return QStringLiteral("%1 × %2 px").arg(layer.image.width()).arg(layer.image.height());
        return QStringLiteral("%1 × %2 px").arg(qRound(layer.transform.size.width())).arg(qRound(layer.transform.size.height()));
    }
    if (role == Qt::UserRole + 1) return depthFor(layer);
    if (role == Qt::UserRole + 2) return layer.group;
    if (role == Qt::UserRole + 3 && !layer.mask.isNull()) return maskThumbnail(layer, document_->canvasSize);
    if (role == Qt::UserRole + 4) return layer.maskSourceId.has_value();
    if (role == Qt::UserRole + 5) return !collapsed_.contains(layer.id);
    return {};
}

bool LayerListModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!document_ || !index.isValid()) return false;
    const Layer &layer = document_->layers.at(layerIndex(index.row()));
    if (role == Qt::EditRole) {
        const QString name = value.toString().trimmed();
        if (name.isEmpty() || name == layer.name) return false;
        emit renameRequested(layer.id, name);
        return true;
    }
    if (role != Qt::CheckStateRole) return false;
    if (layer.visible == (value.toInt() == Qt::Checked)) return false;
    emit visibilityToggleRequested(layer.id);
    return true;
}

Qt::ItemFlags LayerListModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) return Qt::ItemIsDropEnabled;
    return QAbstractListModel::flags(index) | Qt::ItemIsEditable | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable | Qt::ItemIsEnabled
        | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
}

QStringList LayerListModel::mimeTypes() const { return {QStringLiteral("application/x-compositor-layers")}; }

QMimeData *LayerListModel::mimeData(const QModelIndexList &indexes) const
{
    auto *mime = new QMimeData;
    if (!document_) return mime;
    QVector<int> rows; for (const QModelIndex &index : indexes) if (index.isValid() && !rows.contains(index.row())) rows.push_back(index.row());
    std::sort(rows.begin(), rows.end());
    QSet<QUuid> selected; for (int row : rows) { const int i=layerIndex(row); if(i>=0) selected.insert(document_->layers.at(i).id); }
    QByteArray encoded;
    for (int row : rows) {
        const Layer &layer = document_->layers.at(layerIndex(row)); bool carried = false; std::optional<QUuid> parent = layer.parentId;
        while (parent) {
            if (selected.contains(*parent)) { carried = true; break; }
            const auto it = std::find_if(document_->layers.cbegin(), document_->layers.cend(), [&](const Layer &candidate) { return candidate.id == *parent; });
            parent = it == document_->layers.cend() ? std::nullopt : it->parentId;
        }
        if (carried) continue;
        if (!encoded.isEmpty()) encoded += '\n';
        encoded += layer.id.toString(QUuid::WithoutBraces).toUtf8();
    }
    mime->setData(QStringLiteral("application/x-compositor-layers"), encoded);
    return mime;
}

bool LayerListModel::canDropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                                     const QModelIndex &parent) const
{
    Q_UNUSED(column)
    return document_ && data && data->hasFormat(QStringLiteral("application/x-compositor-layers"))
        && (action == Qt::MoveAction || action == Qt::CopyAction) && row <= rowCount() && (!parent.isValid() || parent.row() < rowCount());
}

bool LayerListModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                                  const QModelIndex &parent)
{
    if (!canDropMimeData(data, action, row, column, parent)) return false;
    QVector<QUuid> ids;
    for (const QByteArray &part : data->data(QStringLiteral("application/x-compositor-layers")).split('\n')) {
        const QUuid id(QString::fromUtf8(part)); if (!id.isNull() && !ids.contains(id)) ids.push_back(id);
    }
    if (ids.isEmpty()) return false;
    QUuid parentId, aboveId; bool atBottom = false;
    if (parent.isValid()) {
        const Layer &target = document_->layers.at(layerIndex(parent.row()));
        if (target.group) parentId = target.id;
        else { parentId = target.parentId.value_or(QUuid()); aboveId = target.id; }
    } else if (row >= rowCount()) atBottom = true;
    else if (row >= 0) {
        const Layer &target = document_->layers.at(layerIndex(row)); parentId = target.parentId.value_or(QUuid()); aboveId = target.id;
    }
    emit layersDropRequested(ids, parentId, aboveId, atBottom, action == Qt::CopyAction);
    return true;
}

} // namespace compositor
