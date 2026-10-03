// Layer-stack operations ported from the macOS LayerGroups.swift, LiveLayerMask.swift (clipping rules) and
// SelectionClipboard.swift (Duplicate Layer): grouping, duplicating folders and several layers at once, placing a layer
// with Photoshop's clipping-group rules.
#include "core/EditorSession.h"

#include <QHash>

#include <algorithm>

namespace compositor {

namespace {

// Every layer and folder in the order LayerOrder.resolve lists them: each folder before what is inside it.
QVector<QUuid> hierarchyOrder(const QVector<Layer> &layers, bool topFirst = false)
{
    QHash<QUuid, QVector<int>> children; // keyed by parent; a null id is the root
    for (int i = 0; i < layers.size(); ++i) children[layers.at(i).parentId.value_or(QUuid())].push_back(i);
    QVector<QUuid> order;
    const auto visit = [&](const auto &self, const QUuid &parent, int depth) -> void {
        if (depth > 64) return;
        QVector<int> siblings = children.value(parent);
        if (topFirst) std::reverse(siblings.begin(), siblings.end());
        for (int index : siblings) {
            order.push_back(layers.at(index).id);
            if (layers.at(index).group) self(self, layers.at(index).id, depth + 1);
        }
    };
    visit(visit, QUuid(), 0);
    return order;
}

// A layer dropped into the middle of a clipping group joins it, as in Photoshop (mac adoptClipping).
void adoptClipping(const QUuid &id, QVector<Layer> &layers)
{
    int position = -1;
    for (int i = 0; i < layers.size(); ++i) if (layers.at(i).id == id) { position = i; break; }
    if (position < 0 || layers.at(position).group) return;
    QVector<int> siblings;
    for (int i = 0; i < layers.size(); ++i) if (layers.at(i).parentId == layers.at(position).parentId) siblings.push_back(i);
    const int index = siblings.indexOf(position);
    if (index <= 0 || index + 1 >= siblings.size()) return;
    const auto source = layers.at(siblings.at(index + 1)).maskSourceId;
    if (!source || *source == id) return;
    const Layer &below = layers.at(siblings.at(index - 1));
    if (below.id == *source || below.maskSourceId == source) layers[position].maskSourceId = source;
}

// A moved layer stops clipping when it no longer belongs to the contiguous stack above its base (mac
// releaseDetachedClipping).
void releaseDetachedClipping(QVector<Layer> &layers)
{
    QHash<QUuid, QVector<int>> siblings;
    for (int i = 0; i < layers.size(); ++i) siblings[layers.at(i).parentId.value_or(QUuid())].push_back(i);
    QVector<int> release;
    for (const QVector<int> &stack : std::as_const(siblings)) {
        std::optional<QUuid> base;
        for (int index : stack) {
            const Layer &layer = layers.at(index);
            if (layer.maskSourceId) {
                if (layer.maskSourceId != base) { release.push_back(index); base = layer.id; }
            } else base = layer.group ? std::nullopt : std::optional<QUuid>(layer.id);
        }
    }
    for (int index : release) layers[index].maskSourceId.reset();
}

} // namespace

QVector<QUuid> EditorSession::copiedLayerIds() const
{
    if (!document_) return {};
    QSet<QUuid> selected = selectedLayerIds_;
    if (document_->activeLayerId) selected.insert(*document_->activeLayerId);
    QSet<QUuid> nested;
    for (const QUuid &id : std::as_const(selected)) nested.unite(descendantIds(id));
    QVector<QUuid> result;
    for (const Layer &layer : document_->layers)
        if (selected.contains(layer.id) && !nested.contains(layer.id)) result.push_back(layer.id);
    return result;
}

std::optional<QUuid> EditorSession::insertCopyOf(const QUuid &id)
{
    const int index = indexOf(id);
    if (!document_ || index < 0) return std::nullopt;
    QSet<QUuid> included = descendantIds(id);
    included.insert(id);
    QVector<Layer> originals;
    for (const Layer &layer : std::as_const(document_->layers)) if (included.contains(layer.id)) originals.push_back(layer);
    if (document_->layers.size() + originals.size() > 10000) return std::nullopt;
    QHash<QUuid, QUuid> mapping;
    for (const Layer &layer : originals) mapping.insert(layer.id, QUuid::createUuid());
    QVector<Layer> copies;
    for (Layer layer : originals) {
        const bool root = layer.id == id;
        layer.id = mapping.value(layer.id);
        if (root) layer.name += QStringLiteral(" copy");
        if (layer.parentId && mapping.contains(*layer.parentId)) layer.parentId = mapping.value(*layer.parentId);
        if (layer.maskSourceId && mapping.contains(*layer.maskSourceId)) layer.maskSourceId = mapping.value(*layer.maskSourceId);
        copies.push_back(std::move(layer));
    }
    for (int i = 0; i < copies.size(); ++i) document_->layers.insert(index + 1 + i, copies.at(i));
    return mapping.value(id);
}

// A copy of each layer (a folder with all it holds), as one undo step: Duplicate Layer, and Paste of layers Copy took
// whole. One copy sits just above its original; several stack together, in their order, above the topmost original,
// as Photoshop's do. The copies end up selected (mac duplicateLayers).
bool EditorSession::duplicateLayers(const QVector<QUuid> &ids, const QString &editName)
{
    if (!document_ || ids.isEmpty()) return false;
    const std::optional<QUuid> active = document_->activeLayerId;
    beginEdit(editName);
    QHash<QUuid, QUuid> copiesOf;
    for (const QUuid &id : ids) if (const auto copy = insertCopyOf(id)) copiesOf.insert(id, *copy);
    if (copiesOf.isEmpty()) { endEdit(); return false; }
    if (copiesOf.size() > 1) {
        // Panel order, top first, so layers in different folders compare as they are seen.
        const QVector<QUuid> panel = hierarchyOrder(document_->layers, true);
        QVector<QUuid> originals;
        for (const QUuid &id : std::as_const(panel)) if (copiesOf.contains(id)) originals.push_back(id);
        if (!originals.isEmpty()) {
            const int topIndex = indexOf(originals.constFirst());
            const std::optional<QUuid> parent = topIndex >= 0 ? document_->layers.at(topIndex).parentId : std::nullopt;
            QUuid below = originals.constFirst();
            for (int i = originals.size() - 1; i >= 0; --i) {
                const QUuid copy = copiesOf.value(originals.at(i));
                if (placeLayer(copy, parent, below)) below = copy;
            }
        }
    }
    selectedLayerIds_.clear();
    for (const QUuid &copy : std::as_const(copiesOf)) selectedLayerIds_.insert(copy);
    std::optional<QUuid> primary;
    if (active && copiesOf.contains(*active)) primary = copiesOf.value(*active);
    else if (copiesOf.contains(ids.constFirst())) primary = copiesOf.value(ids.constFirst());
    else primary = *selectedLayerIds_.cbegin();
    document_->activeLayerId = primary;
    maskSelected_ = false;
    endEdit();
    return true;
}

void EditorSession::duplicateActiveLayer()
{
    duplicateLayers(copiedLayerIds());
}

bool EditorSession::duplicateLayer(const QUuid &id, const std::optional<QUuid> &parent,
                                   const std::optional<QUuid> &above, bool atBottom)
{
    if (!document_ || !canPlaceLayer(id, parent)) return false;
    beginEdit(QStringLiteral("Duplicate Layer"));
    selectLayer(id);
    const QSet<QUuid> before = [this] { QSet<QUuid> set; for (const Layer &layer : std::as_const(document_->layers)) set.insert(layer.id); return set; }();
    duplicateActiveLayer();
    const std::optional<QUuid> copy = document_->activeLayerId;
    const bool made = copy && !before.contains(*copy);
    if (made) placeLayer(*copy, parent, above, atBottom);
    endEdit();
    return made;
}

bool EditorSession::placeLayer(const QUuid &id, const std::optional<QUuid> &parent,
                               const std::optional<QUuid> &above, bool atBottom)
{
    QSet<QUuid> carried = descendantIds(id);
    carried.insert(id);
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
    adoptClipping(id, layers);
    releaseDetachedClipping(layers);
    if (layers == document_->layers) return false;
    beginEdit(QStringLiteral("Move Layer"));
    document_->layers = std::move(layers);
    selectLayer(id);
    endEdit();
    return true;
}

// Group Selected Layers (mac groupSelectedLayers): a folder wraps the selection, a selected folder carrying its
// subtree and never pulling its selected descendants out of it; layers from different folders go under their closest
// common folder; with nothing selected it makes an empty folder.
void EditorSession::groupSelectedLayers()
{
    if (!document_ || document_->layers.size() >= 10000) return;
    const QVector<Layer> &all = document_->layers;
    QHash<QUuid, const Layer *> byId;
    for (const Layer &layer : all) byId.insert(layer.id, &layer);
    QSet<QUuid> selected;
    for (const QUuid &id : selectedLayerIds_) if (byId.contains(id)) selected.insert(id);
    const auto ancestors = [&](const QUuid &id) {
        QVector<std::optional<QUuid>> result;
        std::optional<QUuid> parent = byId.value(id)->parentId;
        for (int depth = 0; parent && depth < 64; ++depth) {
            result.push_back(parent);
            const Layer *node = byId.value(*parent);
            parent = node ? node->parentId : std::nullopt;
        }
        result.push_back(std::nullopt);
        return result;
    };
    QSet<QUuid> rootIds;
    for (const QUuid &id : std::as_const(selected)) {
        bool nested = false;
        for (const auto &ancestor : ancestors(id)) if (ancestor && selected.contains(*ancestor)) { nested = true; break; }
        if (!nested) rootIds.insert(id);
    }
    QVector<QUuid> ordered;
    for (const QUuid &id : hierarchyOrder(all)) if (rootIds.contains(id)) ordered.push_back(id);
    std::optional<QUuid> parent;
    if (!ordered.isEmpty()) {
        for (const auto &candidate : ancestors(ordered.constFirst())) {
            const bool common = std::all_of(ordered.cbegin(), ordered.cend(), [&](const QUuid &id) { return ancestors(id).contains(candidate); });
            if (common) { parent = candidate; break; }
        }
    }
    Layer group;
    group.id = QUuid::createUuid();
    group.name = nextName(QStringLiteral("Folder"));
    group.group = true;
    group.transform.size = document_->canvasSize;
    group.parentId = parent;
    // The wrapper goes at the topmost selected branch in the common parent.
    QSet<QUuid> branches;
    for (const QUuid &id : std::as_const(ordered)) {
        QUuid branch = id;
        while (byId.value(branch)->parentId && byId.value(branch)->parentId != parent) branch = *byId.value(branch)->parentId;
        branches.insert(branch);
    }
    int highest = -1;
    for (int i = 0; i < all.size(); ++i) if (branches.contains(all.at(i).id)) highest = i;
    int insertion = all.size() - int(std::count_if(all.cbegin(), all.cend(), [&](const Layer &l) { return rootIds.contains(l.id); }));
    if (highest >= 0) {
        insertion = 0;
        for (int i = 0; i <= highest; ++i) if (!rootIds.contains(all.at(i).id)) ++insertion;
    }
    QVector<Layer> layers;
    for (const Layer &layer : all) if (!rootIds.contains(layer.id)) layers.push_back(layer);
    layers.insert(std::min(insertion, int(layers.size())), group);
    for (const QUuid &id : std::as_const(ordered)) {
        Layer child = *byId.value(id);
        child.parentId = group.id;
        layers.push_back(std::move(child));
    }
    beginEdit(QStringLiteral("Group Layers"));
    document_->layers = std::move(layers);
    selectLayer(group.id);
    endEdit();
}

bool EditorSession::canEditMask() const
{
    return document_ && activeLayer() && selectedLayerIds_.size() <= 1;
}

bool EditorSession::canToggleClippingMask(const QUuid &id) const
{
    const int index = indexOf(id);
    if (!document_ || index < 0 || document_->layers.at(index).group) return false;
    if (document_->layers.at(index).maskSourceId) return true;
    int below = -1;
    for (int i = index - 1; i >= 0; --i) if (document_->layers.at(i).parentId == document_->layers.at(index).parentId) { below = i; break; }
    if (below < 0 || document_->layers.at(below).group) return false;
    return canLinkMask(document_->layers.at(below).maskSourceId.value_or(document_->layers.at(below).id), id);
}

// The trash button and Delete without a selection: with one layer's mask thumbnail targeted only the mask goes;
// otherwise every selected layer does, in one undo step (mac deleteLayerOrMask).
void EditorSession::deleteLayerOrMask()
{
    const Layer *layer = activeLayer();
    if (layer && maskSelected_ && !layer->mask.isNull() && selectedLayerIds_.size() <= 1) deleteLayerMask();
    else deleteSelectedLayers();
}

// Shift-+ / Shift-minus: the active layer's blend mode steps through the blend menu's order, wrapping around, as one
// undo step (mac cycleBlendMode). Folders and multiple selections have no blend mode to step.
bool EditorSession::cycleBlendMode(bool forward)
{
    const Layer *layer = activeLayer();
    if (!document_ || !layer || layer->group || selectedLayerIds_.size() > 1) return false;
    constexpr int count = int(BlendMode::Luminosity) + 1;
    const int current = int(layer->blendMode);
    setLayerBlendMode(layer->id, BlendMode((current + (forward ? 1 : count - 1)) % count));
    return true;
}

} // namespace compositor
