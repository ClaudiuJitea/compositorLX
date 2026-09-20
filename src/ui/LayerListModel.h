#pragma once

#include "core/Document.h"

#include <QAbstractListModel>
#include <QSet>

#include <memory>

namespace compositor {

class LayerListModel final : public QAbstractListModel {
    Q_OBJECT
public:
    explicit LayerListModel(QObject *parent = nullptr);

    void setDocument(std::shared_ptr<Document> document, bool reset = true);
    [[nodiscard]] std::optional<QUuid> layerId(const QModelIndex &index) const;
    void toggleExpanded(const QUuid &id);
    [[nodiscard]] bool isExpanded(const QUuid &id) const { return !collapsed_.contains(id); }
    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex &index) const override;
    [[nodiscard]] QStringList mimeTypes() const override;
    [[nodiscard]] QMimeData *mimeData(const QModelIndexList &indexes) const override;
    [[nodiscard]] bool canDropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                                       const QModelIndex &parent) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                      const QModelIndex &parent) override;
    [[nodiscard]] Qt::DropActions supportedDropActions() const override { return Qt::MoveAction | Qt::CopyAction; }

signals:
    void visibilityToggleRequested(const QUuid &id);
    void renameRequested(const QUuid &id, const QString &name);
    void layersDropRequested(const QVector<QUuid> &ids, const QUuid &parent, const QUuid &above,
                             bool atBottom, bool copy);

private:
    [[nodiscard]] int layerIndex(int row) const;
    [[nodiscard]] QVector<int> visibleLayerIndexes() const;
    [[nodiscard]] int depthFor(const Layer &layer) const;
    std::shared_ptr<Document> document_;
    QSet<QUuid> collapsed_;
};

} // namespace compositor
