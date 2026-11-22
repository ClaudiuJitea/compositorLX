// The Layers panel's right-click menu and whole-layer Copy / Paste (mac NativeLayerList.contextMenu,
// SelectionClipboard.copySelection / paste and ProjectWorkspace.pasteCopiedLayer).
#include "ui/MainWindow.h"
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>
#include <QFileDialog>
#include "io/ImageExporter.h"

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
    add(QStringLiteral("layerMenuClipping"), session_.selectedClippingTitle(), session_.canToggleSelectedClipping(),
        [this] { session_.toggleSelectedClipping(); });
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
    // Whichever would change a selected layer, both for a mix; shown, greyed, for layers neither applies to (mac de0a43d).
    const bool sixteen = session_.canConvertToSixteenBit(), eight = session_.canConvertToEightBit();
    if (sixteen || !eight) add(QStringLiteral("layerMenuConvert16"), tr("Convert to 16-Bit"), sixteen, [this] { session_.convertLayers(true); });
    if (eight) add(QStringLiteral("layerMenuConvert8"), tr("Convert to 8-Bit"), true, [this] { session_.convertLayers(false); });
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
    add(QStringLiteral("layerMenuVisibility"), session_.visibilityTitle(), editable, [this] { session_.toggleSelectedLayersVisibility(); });
    menu.addSeparator();
    add(QStringLiteral("layerMenuExportPng"), selectedCount > 1 ? tr("Export Layers as PNGs…") : tr("Export Layer as PNG…"), document_ && active,
        [this] { exportLayersAsPng(); });

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
        // Where the pixels came from, so Paste puts them back in place (mac PixelClipboard.origin).
        mime->setData(QStringLiteral("application/x-compositor-pixel-origin"),
                      QByteArray::number(clipboardOrigin_.x()) + ',' + QByteArray::number(clipboardOrigin_.y()));
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


// Defined in MainWindow.cpp, with its dialog hook for tests.
QMessageBox::StandardButton appMessage(QWidget *parent, const QString &title, const QString &text, const QString &detail,
                                        QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton,
                                        const QHash<int, QString> &labels);

void MainWindow::exportLayersAsPng(const QString &destination)
{
    const QString start = document_ && !document_->projectPath.isEmpty() ? QFileInfo(document_->projectPath).absolutePath() : QDir::homePath();
    if (!document_) return;
    QSet<QUuid> selected = session_.selectedLayerIds();
    if (selected.isEmpty() && document_->activeLayerId) selected.insert(*document_->activeLayerId);
    // Top first, as the panel lists them, so the upper of two layers of the same name keeps it.
    QVector<const Layer *> layers;
    for (auto it = document_->layers.crbegin(); it != document_->layers.crend(); ++it) if (selected.contains(it->id)) layers.push_back(&*it);
    if (layers.isEmpty()) return;
    QSet<QString> used;
    QStringList names;
    for (const Layer *layer : layers) {
        const QString base = ImageExporter::layerFileName(layer->name);
        QString name = base;
        for (int number = 2; used.contains(name.toLower()); ++number) name = QStringLiteral("%1 %2").arg(base).arg(number);
        used.insert(name.toLower());
        names << name + QStringLiteral(".png");
    }
    QStringList paths;
    if (layers.size() == 1) {
        const QString path = !destination.isEmpty() ? destination
            : QFileDialog::getSaveFileName(this, tr("Export Layer as PNG"), QDir(start).filePath(names.first()), tr("PNG images (*.png)"));
        if (path.isEmpty()) return;
        paths << path;
    } else {
        const QString folder = !destination.isEmpty() ? destination
            : QFileDialog::getExistingDirectory(this, tr("Export %1 Layers as PNGs").arg(layers.size()), start);
        if (folder.isEmpty()) return;
        for (const QString &name : names) paths << QDir(folder).filePath(name);
        QStringList existing;
        for (const QString &path : paths) if (QFileInfo::exists(path)) existing << path;
        if (!existing.isEmpty()) {
            const QString text = existing.size() == 1 ? tr("“%1” already exists. Replace it?").arg(QFileInfo(existing.first()).fileName())
                : tr("%1 of these files already exist in “%2”. Replace them?").arg(existing.size()).arg(QFileInfo(folder).fileName());
            if (appMessage(this, tr("Export Layers as PNGs"), text, {}, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes,
                            {{int(QMessageBox::Yes), tr("Replace")}}) != QMessageBox::Yes) return;
        }
    }
    QStringList empty;
    for (int i = 0; i < layers.size(); ++i) {
        QString error;
        const std::optional<QByteArray> data = ImageExporter::layerPng(*document_, layers[i]->id, &error);
        if (!data) { if (error.isEmpty()) empty << layers[i]->name; else { appMessage(this, tr("Couldn’t export layers"), error, {}, QMessageBox::Ok, QMessageBox::NoButton, {}); return; } continue; }
        if (!ImageExporter::writeAtomically(*data, paths[i], &error)) { appMessage(this, tr("Couldn’t export layers"), error, {}, QMessageBox::Ok, QMessageBox::NoButton, {}); return; }
    }
    if (!empty.isEmpty())
        appMessage(this, empty.size() == 1 ? tr("“%1” has nothing to export").arg(empty.first()) : tr("%1 layers have nothing to export").arg(empty.size()),
                    tr("A layer with no visible pixels, such as an adjustment layer, isn’t saved as a PNG."), {}, QMessageBox::Ok, QMessageBox::NoButton, {});
}

} // namespace compositor
