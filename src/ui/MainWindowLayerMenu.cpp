// The Layers panel's right-click menu and whole-layer Copy / Paste (mac NativeLayerList.contextMenu,
// SelectionClipboard.copySelection / paste and ProjectWorkspace.pasteCopiedLayer).
#include "ui/MainWindow.h"

#include "core/EditorSession.h"
#include "io/ProjectWriter.h"
#include "ui/LayerListModel.h"

#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMimeData>

namespace compositor {

namespace {
const QString kCopiedLayerMime = QStringLiteral("application/x-compositor-copied-layer");
}

void MainWindow::selectRowForContextMenu(const QModelIndex &index)
{
    if (!document_ || !index.isValid() || layerModel_->isEffect(index)) return;
    const auto id = layerModel_->layerId(index);
    if (!id) return;
    if (layerView_->selectionModel()->isSelected(index)) {
        // Inside a multi-selection the selection stays; the clicked row becomes the primary layer.
        session_.selectLayers(session_.selectedLayerIds(), id);
        updateInspector();
    } else {
        layerView_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
}

void MainWindow::populateLayerContextMenu(QMenu &menu)
{
    const Layer *active = session_.activeLayer();
    const bool editable = document_ && active;
    const int selectedCount = session_.selectedLayerIds().size();
    const bool maskTargeted = session_.isMaskSelected() && active && !active->mask.isNull();
    const auto add = [&](const QString &name, const QString &text, bool enabled, auto &&slot) {
        QAction *action = menu.addAction(text);
        action->setObjectName(name);
        action->setEnabled(enabled);
        connect(action, &QAction::triggered, this, [this, slot] { slot(); syncDocumentViews(); });
        return action;
    };

    add(QStringLiteral("layerMenuDuplicate"), tr("Duplicate Layer"), editable, [this] { session_.duplicateActiveLayer(); });
    add(QStringLiteral("layerMenuRename"), tr("Rename…"), editable && selectedCount <= 1, [this] {
        if (layerView_->currentIndex().isValid()) layerView_->edit(layerView_->currentIndex());
    });
    add(QStringLiteral("layerMenuDelete"),
        maskTargeted ? tr("Delete Mask") : selectedCount > 1 ? tr("Delete Selected Layers") : tr("Delete Layer"),
        editable, [this] { deleteLayersWithMaskChoice(); });
    menu.addSeparator();
    add(QStringLiteral("layerMenuClipping"),
        active && active->maskSourceId ? tr("Release Clipping Mask") : tr("Create Clipping Mask"),
        active && session_.canToggleClippingMask(active->id), [this] { if (const Layer *layer = session_.activeLayer()) session_.toggleClippingMask(layer->id); });
    add(QStringLiteral("layerMenuGroup"), tr("Group Selected Layers"),
        document_ && document_->layers.size() < 10000 && selectedCount > 0, [this] { session_.groupSelectedLayers(); });
    if (active && active->group)
        add(QStringLiteral("layerMenuUngroup"), tr("Ungroup Layers"), session_.canUngroupLayers(), [this] {
            if (transformOriginalDocument_) finishPersistentTransform(true);
            session_.ungroupLayers();
        });
    add(QStringLiteral("layerMenuMoveOut"), tr("Move Out of Folder"), active && active->parentId.has_value(),
        [this] { session_.moveActiveLayerOutOfGroup(); });
    add(QStringLiteral("layerMenuMerge"), session_.mergeTitle(), session_.canMergeLayers(), [this] { session_.mergeLayers(); });
    menu.addSeparator();

    const bool canAddMask = session_.canEditMask() && active && active->mask.isNull();
    auto *maskMenu = menu.addMenu(tr("Add Mask"));
    maskMenu->menuAction()->setObjectName(QStringLiteral("layerMenuAddMask"));
    maskMenu->setEnabled(canAddMask);
    const auto addMask = [&](const QString &name, const QString &text, bool revealing) {
        QAction *action = maskMenu->addAction(text);
        action->setObjectName(name);
        action->setEnabled(canAddMask);
        connect(action, &QAction::triggered, this, [this, revealing] { session_.addLayerMask(revealing, true); syncDocumentViews(); });
    };
    addMask(QStringLiteral("layerMenuRevealAll"), tr("Reveal All (White)"), true);
    addMask(QStringLiteral("layerMenuHideAll"), tr("Hide All (Black)"), false);
    const bool hasMask = session_.canEditMask() && active && !active->mask.isNull();
    add(QStringLiteral("layerMenuToggleMask"), active && !active->mask.isNull() && !active->maskEnabled ? tr("Enable Mask") : tr("Disable Mask"),
        hasMask, [this] { session_.toggleLayerMask(); });
    add(QStringLiteral("layerMenuDeleteMask"), tr("Delete Mask"), hasMask, [this] { session_.deleteLayerMask(); });
    add(QStringLiteral("layerMenuLinkMask"), active && !active->mask.isNull() && !active->maskLinked ? tr("Link Mask") : tr("Unlink Mask"),
        document_ && active && !active->mask.isNull() && !active->group && active->adjustment.isEmpty(), [this] { session_.toggleMaskLink(); });
    menu.addSeparator();
    add(QStringLiteral("layerMenuVisibility"), active && !active->visible ? tr("Show Layer") : tr("Hide Layer"), editable, [this] {
        if (const Layer *layer = session_.activeLayer()) session_.toggleLayerVisibility(layer->id);
    });

    if (session_.canEditEffects()) {
        menu.addSeparator();
        auto *effectsAct = menu.addAction(tr("Layer Effects…"));
        effectsAct->setObjectName(QStringLiteral("layerMenuEffects"));
        connect(effectsAct, &QAction::triggered, this, [this]() { layerEffectsDialog(); });
        auto *addMenu = menu.addMenu(tr("Add Effect"));
        for (LayerEffectKind kind : {LayerEffectKind::Stroke, LayerEffectKind::DropShadow, LayerEffectKind::ColorOverlay,
                                     LayerEffectKind::InnerShadow, LayerEffectKind::OuterGlow, LayerEffectKind::InnerGlow}) {
            auto *act = addMenu->addAction(layerEffectKindToString(kind) + QStringLiteral("…"));
            connect(act, &QAction::triggered, this, [this, kind]() {
                if (!document_ || !document_->activeLayerId) return;
                const QUuid target = *document_->activeLayerId;
                session_.addLayerEffect(target, kind);
                layerEffectsDialog(target, kind);
            });
        }
    }
}

// Cmd-C with no selection copies the layer itself (a folder with all it holds, an adjustment, editable text...) for
// Paste here or in another project. The layers' ids travel on the clipboard beside the picture, so anything copied
// since (here or in another app) replaces them (mac copiedLayer).
void MainWindow::copyWholeLayers()
{
    const QVector<QUuid> ids = session_.copiedLayerIds();
    if (ids.isEmpty()) return;
    auto *mime = new QMimeData;
    if (const auto copied = session_.copiedPixels(false)) {
        clipboardImage_ = copied->first; clipboardOrigin_ = copied->second;
        mime->setImageData(clipboardImage_);
    } else {
        clipboardImage_ = QImage();
    }
    QByteArray encoded;
    for (const QUuid &id : ids) { if (!encoded.isEmpty()) encoded += '\n'; encoded += id.toString(QUuid::WithoutBraces).toUtf8(); }
    mime->setData(kCopiedLayerMime, encoded);
    QGuiApplication::clipboard()->setMimeData(mime);
    statusHint_->setText(ids.size() > 1 ? tr("Copied layers") : tr("Copied layer"));
}

bool MainWindow::pasteWholeLayers()
{
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasFormat(kCopiedLayerMime)) return false;
    QVector<QUuid> ids;
    for (const QByteArray &part : mime->data(kCopiedLayerMime).split('\n')) {
        const QUuid id(QString::fromUtf8(part).trimmed());
        if (!id.isNull() && !ids.contains(id)) ids.push_back(id);
    }
    if (ids.isEmpty()) return false;
    const auto holds = [&ids](const Document *doc) {
        return doc && std::any_of(doc->layers.cbegin(), doc->layers.cend(), [&ids](const Layer &layer) { return ids.contains(layer.id); });
    };
    if (holds(document_.get())) {
        QVector<QUuid> present;
        for (const QUuid &id : ids) if (std::any_of(document_->layers.cbegin(), document_->layers.cend(), [&id](const Layer &l) { return l.id == id; })) present.push_back(id);
        if (!session_.duplicateLayers(present, tr("Paste"))) return false;
        syncDocumentViews();
        return true;
    }
    stashCurrentTab();
    for (int i = 0; i < int(workspaceTabs_.size()); ++i) {
        if (i == currentTab_ || !holds(workspaceTabs_.at(i).document().get())) continue;
        return copyLayersToTab(ids, currentTab_, false, std::nullopt, tr("Paste"));
    }
    return false;
}

} // namespace compositor
