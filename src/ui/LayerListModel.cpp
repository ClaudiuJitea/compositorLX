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

QVector<LayerListModel::Item> LayerListModel::visibleItems() const
{
    QVector<Item> result;
    if (!document_) return result;
    const QVector<int> indexes = visibleLayerIndexes();
    for (int i : indexes) {
        const Layer &layer = document_->layers.at(i);
        result.push_back({false, i, layer.id, LayerEffectKind::Stroke});
        if (layer.effects.has_value() && !layer.effects->isEmpty()) {
            if (!effectsCollapsed_.contains(layer.id)) {
                for (LayerEffectKind kind : layer.effects->kinds()) {
                    result.push_back({true, i, layer.id, kind});
                }
            }
        }
    }
    return result;
}

int LayerListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() || !document_ ? 0 : visibleItems().size();
}

int LayerListModel::layerIndex(int row) const
{
    const QVector<Item> items = visibleItems();
    return row >= 0 && row < items.size() ? items.at(row).layerIndex : -1;
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
    if (!document_ || !index.isValid()) return std::nullopt;
    const QVector<Item> items = visibleItems();
    return index.row() >= 0 && index.row() < items.size() ? std::optional<QUuid>(items.at(index.row()).layerId) : std::nullopt;
}

bool LayerListModel::isEffect(const QModelIndex &index) const
{
    if (!document_ || !index.isValid()) return false;
    const QVector<Item> items = visibleItems();
    return index.row() >= 0 && index.row() < items.size() ? items.at(index.row()).isEffect : false;
}

std::optional<LayerEffectKind> LayerListModel::effectKind(const QModelIndex &index) const
{
    if (!document_ || !index.isValid()) return std::nullopt;
    const QVector<Item> items = visibleItems();
    if (index.row() >= 0 && index.row() < items.size() && items.at(index.row()).isEffect) {
        return items.at(index.row()).effectKind;
    }
    return std::nullopt;
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

void LayerListModel::toggleEffectsExpanded(const QUuid &id)
{
    if (!document_) return;
    beginResetModel();
    if (effectsCollapsed_.contains(id)) effectsCollapsed_.remove(id); else effectsCollapsed_.insert(id);
    endResetModel();
}

QVariant LayerListModel::data(const QModelIndex &index, int role) const
{
    if (!document_ || !index.isValid()) return {};
    const QVector<Item> items = visibleItems();
    if (index.row() < 0 || index.row() >= items.size()) return {};
    const Item &item = items.at(index.row());
    if (item.layerIndex < 0 || item.layerIndex >= document_->layers.size()) return {};
    const Layer &layer = document_->layers.at(item.layerIndex);

    if (role == IsEffectRole) return item.isEffect;
    if (role == EffectKindRole) return static_cast<int>(item.effectKind);
    if (role == HasEffectsRole) return layer.effects.has_value() && !layer.effects->isEmpty();
    if (role == EffectsExpandedRole) return !effectsCollapsed_.contains(layer.id);

    if (item.isEffect) {
        const bool enabled = layer.effects.has_value() && layer.effects->isEnabled(item.effectKind);
        if (role == Qt::DisplayRole) return layerEffectKindToString(item.effectKind);
        if (role == Qt::CheckStateRole) return enabled ? Qt::Checked : Qt::Unchecked;
        if (role == Qt::ToolTipRole) {
            return QStringLiteral("%1 (%2)").arg(layerEffectKindToString(item.effectKind),
                                                 enabled ? tr("Enabled") : tr("Disabled"));
        }
        if (role == Qt::UserRole) return tr("Effect");
        if (role == Qt::UserRole + 1) return depthFor(layer);
        return {};
    }

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
        if (!layer.image.isNull()) {
            QString text = QStringLiteral("%1 × %2 px").arg(layer.image.width()).arg(layer.image.height());
            // A layer keeps its full pixels however small it is scaled, so a scaled one says so: "100 × 100 px · 5%" rather
            // than looking as if it had been resampled (mac 67cc31e).
            const double scale = layer.transform.size.width() / std::max(1, layer.image.width());
            if (layer.transform.size.width() > 0 && std::abs(scale - 1.0) > 0.005) text += QStringLiteral(" · %1%").arg(qRound(scale * 100));
            return text;
        }
        return QStringLiteral("%1 × %2 px").arg(qRound(layer.transform.size.width())).arg(qRound(layer.transform.size.height()));
    }
    if (role == Qt::UserRole + 1) return depthFor(layer);
    if (role == Qt::UserRole + 2) return layer.group;
    if (role == Qt::UserRole + 3 && !layer.mask.isNull()) return maskThumbnail(layer, document_->canvasSize);
    if (role == Qt::UserRole + 4) return layer.maskSourceId.has_value();
    if (role == Qt::UserRole + 5) return !collapsed_.contains(layer.id);
    if (role == Qt::UserRole + 6) return maskAlone_ == std::optional<QUuid>(layer.id);
    if (role == Qt::UserRole + 7) return !layer.mask.isNull() && !layer.maskEnabled;                       // disabled-mask mark
    if (role == Qt::UserRole + 8) return !layer.mask.isNull() && !layer.group && layer.adjustment.isEmpty(); // link chain shown
    if (role == Qt::UserRole + 9) return layer.maskLinked;
    if (role == Qt::UserRole + 20) return layer.text.has_value();                                 // editable-text badge
    return {};
}

bool LayerListModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!document_ || !index.isValid()) return false;
    const QVector<Item> items = visibleItems();
    if (index.row() < 0 || index.row() >= items.size()) return false;
    const Item &item = items.at(index.row());
    if (item.layerIndex < 0 || item.layerIndex >= document_->layers.size()) return false;
    const Layer &layer = document_->layers.at(item.layerIndex);

    if (item.isEffect) {
        if (role == Qt::CheckStateRole) {
            emit effectVisibilityToggleRequested(layer.id, item.effectKind);
            return true;
        }
        return false;
    }

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
    const QVector<Item> items = visibleItems();
    if (index.row() >= 0 && index.row() < items.size() && items.at(index.row()).isEffect) {
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
    }
    return QAbstractListModel::flags(index) | Qt::ItemIsEditable | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable | Qt::ItemIsEnabled
        | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
}

QStringList LayerListModel::mimeTypes() const
{
    return {
        QStringLiteral("application/x-compositor-layers"),
        QStringLiteral("com.compositor.layer-row"),
        QStringLiteral("application/x-compositor-layer-mask"),
        QStringLiteral("com.compositor.layer-mask"),
        QStringLiteral("application/x-compositor-layer-effect"),
        QStringLiteral("com.compositor.layer-effect")
    };
}

QMimeData *LayerListModel::mimeData(const QModelIndexList &indexes) const
{
    auto *mime = new QMimeData;
    if (!document_ || indexes.isEmpty()) return mime;

    if (indexes.size() == 1 && isEffect(indexes.first())) {
        const QModelIndex &idx = indexes.first();
        const auto id = layerId(idx);
        const auto kind = effectKind(idx);
        if (id && kind) {
            const QString effectStr = id->toString(QUuid::WithoutBraces) + QStringLiteral(":") + layerEffectKindToString(*kind);
            const QByteArray data = effectStr.toUtf8();
            mime->setData(QStringLiteral("application/x-compositor-layer-effect"), data);
            mime->setData(QStringLiteral("com.compositor.layer-effect"), data);
            return mime;
        }
    }

    QVector<int> rows;
    for (const QModelIndex &index : indexes) {
        if (index.isValid() && !rows.contains(index.row()) && !isEffect(index)) {
            rows.push_back(index.row());
        }
    }
    std::sort(rows.begin(), rows.end());
    QSet<QUuid> selected;
    for (int row : rows) {
        const int i = layerIndex(row);
        if (i >= 0 && i < document_->layers.size()) {
            selected.insert(document_->layers.at(i).id);
        }
    }
    QByteArray encoded;
    for (int row : rows) {
        const int i = layerIndex(row);
        if (i < 0 || i >= document_->layers.size()) continue;
        const Layer &layer = document_->layers.at(i);
        bool carried = false;
        std::optional<QUuid> parent = layer.parentId;
        while (parent) {
            if (selected.contains(*parent)) { carried = true; break; }
            const auto it = std::find_if(document_->layers.cbegin(), document_->layers.cend(),
                                         [&](const Layer &cand) { return cand.id == *parent; });
            parent = it == document_->layers.cend() ? std::nullopt : it->parentId;
        }
        if (carried) continue;
        if (!encoded.isEmpty()) encoded += '\n';
        encoded += layer.id.toString(QUuid::WithoutBraces).toUtf8();
    }
    mime->setData(QStringLiteral("application/x-compositor-layers"), encoded);
    mime->setData(QStringLiteral("com.compositor.layer-row"), encoded);
    return mime;
}

bool LayerListModel::canCopyMask(const QUuid &sourceId, const QUuid &targetId) const
{
    if (!document_ || sourceId == targetId) return false;
    const Layer *src = nullptr;
    const Layer *dst = nullptr;
    for (const Layer &l : document_->layers) {
        if (l.id == sourceId) src = &l;
        if (l.id == targetId) dst = &l;
    }
    if (!src || !dst) return false;
    return !src->mask.isNull() && !dst->group;
}

bool LayerListModel::canCopyEffect(LayerEffectKind kind, const QUuid &sourceId, const QUuid &targetId) const
{
    if (!document_ || sourceId == targetId) return false;
    const Layer *src = nullptr;
    const Layer *dst = nullptr;
    for (const Layer &l : document_->layers) {
        if (l.id == sourceId) src = &l;
        if (l.id == targetId) dst = &l;
    }
    if (!src || !dst) return false;
    if (!src->effects || !src->effects->contains(kind)) return false;
    if (dst->group || !dst->adjustment.isEmpty() || (dst->image.isNull() && !dst->text && dst->shape.isEmpty())) return false;
    return true;
}

bool LayerListModel::canDropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                                     const QModelIndex &parent) const
{
    Q_UNUSED(column)
    if (!document_ || !data) return false;

    if (data->hasFormat(QStringLiteral("application/x-compositor-layer-mask"))
        || data->hasFormat(QStringLiteral("com.compositor.layer-mask"))) {
        const int targetRow = parent.isValid() ? parent.row() : row;
        if (targetRow < 0 || targetRow >= rowCount()) return false;
        const auto target = layerId(index(targetRow, 0));
        if (!target) return false;
        const QByteArray raw = data->hasFormat(QStringLiteral("application/x-compositor-layer-mask"))
            ? data->data(QStringLiteral("application/x-compositor-layer-mask"))
            : data->data(QStringLiteral("com.compositor.layer-mask"));
        const QUuid sourceId(QString::fromUtf8(raw).trimmed());
        if (sourceId.isNull()) return false;
        return canCopyMask(sourceId, *target);
    }

    if (data->hasFormat(QStringLiteral("application/x-compositor-layer-effect"))
        || data->hasFormat(QStringLiteral("com.compositor.layer-effect"))) {
        const int targetRow = parent.isValid() ? parent.row() : row;
        if (targetRow < 0 || targetRow >= rowCount()) return false;
        const auto target = layerId(index(targetRow, 0));
        if (!target) return false;
        const QByteArray raw = data->hasFormat(QStringLiteral("application/x-compositor-layer-effect"))
            ? data->data(QStringLiteral("application/x-compositor-layer-effect"))
            : data->data(QStringLiteral("com.compositor.layer-effect"));
        const QString str = QString::fromUtf8(raw).trimmed();
        const int colon = str.indexOf(QLatin1Char(':'));
        if (colon <= 0) return false;
        const QUuid sourceId(str.left(colon));
        const auto kind = layerEffectKindFromString(str.mid(colon + 1));
        if (sourceId.isNull() || !kind) return false;
        return canCopyEffect(*kind, sourceId, *target);
    }

    if (data->hasFormat(QStringLiteral("application/x-compositor-layers"))
        || data->hasFormat(QStringLiteral("com.compositor.layer-row"))) {
        return (action == Qt::MoveAction || action == Qt::CopyAction)
            && row <= rowCount() && (!parent.isValid() || parent.row() < rowCount());
    }

    return false;
}

bool LayerListModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                                  const QModelIndex &parent)
{
    if (!canDropMimeData(data, action, row, column, parent)) return false;

    if (data->hasFormat(QStringLiteral("application/x-compositor-layer-mask"))
        || data->hasFormat(QStringLiteral("com.compositor.layer-mask"))) {
        const int targetRow = parent.isValid() ? parent.row() : row;
        if (targetRow < 0 || targetRow >= rowCount()) return false;
        const auto target = layerId(index(targetRow, 0));
        if (!target) return false;
        const QByteArray raw = data->hasFormat(QStringLiteral("application/x-compositor-layer-mask"))
            ? data->data(QStringLiteral("application/x-compositor-layer-mask"))
            : data->data(QStringLiteral("com.compositor.layer-mask"));
        const QUuid sourceId(QString::fromUtf8(raw).trimmed());
        if (sourceId.isNull()) return false;
        emit maskDropRequested(sourceId, *target);
        return true;
    }

    if (data->hasFormat(QStringLiteral("application/x-compositor-layer-effect"))
        || data->hasFormat(QStringLiteral("com.compositor.layer-effect"))) {
        const int targetRow = parent.isValid() ? parent.row() : row;
        if (targetRow < 0 || targetRow >= rowCount()) return false;
        const auto target = layerId(index(targetRow, 0));
        if (!target) return false;
        const QByteArray raw = data->hasFormat(QStringLiteral("application/x-compositor-layer-effect"))
            ? data->data(QStringLiteral("application/x-compositor-layer-effect"))
            : data->data(QStringLiteral("com.compositor.layer-effect"));
        const QString str = QString::fromUtf8(raw).trimmed();
        const int colon = str.indexOf(QLatin1Char(':'));
        if (colon <= 0) return false;
        const QUuid sourceId(str.left(colon));
        const auto kind = layerEffectKindFromString(str.mid(colon + 1));
        if (sourceId.isNull() || !kind) return false;
        emit effectDropRequested(sourceId, *kind, *target);
        return true;
    }

    if (data->hasFormat(QStringLiteral("application/x-compositor-layers"))
        || data->hasFormat(QStringLiteral("com.compositor.layer-row"))) {
        const QByteArray raw = data->hasFormat(QStringLiteral("application/x-compositor-layers"))
            ? data->data(QStringLiteral("application/x-compositor-layers"))
            : data->data(QStringLiteral("com.compositor.layer-row"));
        QVector<QUuid> ids;
        for (const QByteArray &part : raw.split('\n')) {
            const QUuid id(QString::fromUtf8(part).trimmed());
            if (!id.isNull() && !ids.contains(id)) ids.push_back(id);
        }
        if (ids.isEmpty()) return false;
        QUuid parentId, aboveId; bool atBottom = false;
        if (parent.isValid()) {
            const int targetIdx = layerIndex(parent.row());
            if (targetIdx >= 0 && targetIdx < document_->layers.size()) {
                const Layer &target = document_->layers.at(targetIdx);
                if (target.group) parentId = target.id;
                else { parentId = target.parentId.value_or(QUuid()); aboveId = target.id; }
            }
        } else if (row >= rowCount()) {
            atBottom = true;
        } else if (row >= 0) {
            const int targetIdx = layerIndex(row);
            if (targetIdx >= 0 && targetIdx < document_->layers.size()) {
                const Layer &target = document_->layers.at(targetIdx);
                parentId = target.parentId.value_or(QUuid()); aboveId = target.id;
            }
        }
        emit layersDropRequested(ids, parentId, aboveId, atBottom, action == Qt::CopyAction);
        return true;
    }

    return false;
}

void LayerListModel::setMaskAlone(const std::optional<QUuid> &id)
{
    if (maskAlone_ == id) return;
    maskAlone_ = id;
    if (rowCount() > 0) emit dataChanged(index(0), index(rowCount() - 1));
}

} // namespace compositor
