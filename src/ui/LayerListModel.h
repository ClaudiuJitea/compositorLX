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

    enum CustomRoles {
        IsEffectRole = Qt::UserRole + 10,
        EffectKindRole = Qt::UserRole + 11,
        HasEffectsRole = Qt::UserRole + 12,
        EffectsExpandedRole = Qt::UserRole + 13
    };

    struct Item {
        bool isEffect = false;
        int layerIndex = -1;
        QUuid layerId;
        LayerEffectKind effectKind = LayerEffectKind::Stroke;
    };

    void setDocument(std::shared_ptr<Document> document, bool reset = true);
    // The layer whose mask is shown alone on the canvas; its mask thumbnail is outlined in white.
    void setMaskAlone(const std::optional<QUuid> &id);
    [[nodiscard]] std::optional<QUuid> layerId(const QModelIndex &index) const;
    [[nodiscard]] bool isEffect(const QModelIndex &index) const;
    [[nodiscard]] std::optional<LayerEffectKind> effectKind(const QModelIndex &index) const;
    void toggleExpanded(const QUuid &id);
    [[nodiscard]] bool isExpanded(const QUuid &id) const { return !collapsed_.contains(id); }
    void toggleEffectsExpanded(const QUuid &id);
    [[nodiscard]] bool isEffectsExpanded(const QUuid &id) const { return !effectsCollapsed_.contains(id); }
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
    [[nodiscard]] Qt::DropActions supportedDragActions() const override { return Qt::MoveAction | Qt::CopyAction; }

    [[nodiscard]] bool canCopyMask(const QUuid &sourceId, const QUuid &targetId) const;
    [[nodiscard]] bool canCopyEffect(LayerEffectKind kind, const QUuid &sourceId, const QUuid &targetId) const;

signals:
    void visibilityToggleRequested(const QUuid &id);
    void effectVisibilityToggleRequested(const QUuid &id, LayerEffectKind kind);
    void renameRequested(const QUuid &id, const QString &name);
    void layersDropRequested(const QVector<QUuid> &ids, const QUuid &parent, const QUuid &above,
                             bool atBottom, bool copy);
    void maskDropRequested(const QUuid &sourceId, const QUuid &targetId);
    void effectDropRequested(const QUuid &sourceId, LayerEffectKind kind, const QUuid &targetId);

private:
    std::optional<QUuid> maskAlone_;
    [[nodiscard]] int layerIndex(int row) const;
    [[nodiscard]] QVector<int> visibleLayerIndexes() const;
    [[nodiscard]] QVector<Item> visibleItems() const;
    [[nodiscard]] int depthFor(const Layer &layer) const;
    std::shared_ptr<Document> document_;
    QSet<QUuid> collapsed_;
    QSet<QUuid> effectsCollapsed_;
};

} // namespace compositor
