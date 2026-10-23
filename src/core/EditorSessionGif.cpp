#include "core/EditorSession.h"
#include "io/GifIO.h"
#include <QFileInfo>
#include <QPainter>

namespace compositor {
bool EditorSession::insertGif(const QString &path, const std::optional<QPointF> &center, QString *error)
{
    auto animation = GifIO::readFile(path, error);
    if (!animation) return false;
    const QImage image = animation->frame(0);
    if (image.isNull()) return false;
    beginEdit(QStringLiteral("Import GIF"));
    const bool inserted = insertImage(image, QFileInfo(path).completeBaseName(), center);
    if (inserted) { activeLayer()->gif = *animation; document_->formatVersion = std::max(document_->formatVersion, 17); }
    endEdit(); return inserted;
}
bool EditorSession::selectGifFrame(int index)
{
    Layer *layer = activeLayer();
    if (!layer || !layer->gif || editDepth_ || brush_) return false;
    if (layer->gif->selected == index) return true;
    const QImage image = layer->gif->frame(index);
    if (image.isNull()) return false;
    layer->gif->selected = index; layer->image = image;
    advanceRevision(); return true;
}
bool EditorSession::duplicateGifFrame(std::optional<int> index)
{
    if (index && !selectGifFrame(*index)) return false;
    Layer *layer = activeLayer(); if (!layer || !layer->gif || layer->gif->frames.size() >= 4096) return false;
    const auto before = *layer->gif;
    const auto &a = before;
    int identity = a.source->delays.size(), position = -1;
    for (int i = 0; i < a.frames.size(); ++i) { identity = std::max(identity, a.frames[i].index + 1); if (a.frames[i].index == a.selected) position = i; }
    if (identity >= 8192 || position < 0) return false;
    beginEdit(QStringLiteral("Duplicate GIF Frame"));
    layer = activeLayer();
    auto &edited = *layer->gif;
    edited.frames.insert(position + 1, {identity, a.frames[position].sourceIndex});
    // The copy keeps the frame's own delay override, if it has one, as on macOS; "Same for all" still covers it.
    edited.pixels.insert(identity, layer->image); if (a.delays.contains(a.selected)) edited.delays.insert(identity, a.delays.value(a.selected));
    edited.selected = identity;
    endEdit(); return true;
}
bool EditorSession::deleteGifFrame(std::optional<int> index)
{
    if (index && !selectGifFrame(*index)) return false;
    Layer *layer = activeLayer(); if (!layer || !layer->gif || layer->gif->frames.size() <= 1) return false;
    auto &a = *layer->gif;
    const auto it = std::find_if(a.frames.begin(), a.frames.end(), [&](const auto &f) { return f.index == a.selected; });
    if (it == a.frames.end()) return false;
    const int position = int(it - a.frames.begin());
    const int next = a.frames[position + 1 < a.frames.size() ? position + 1 : position - 1].index;
    const QImage image = a.frame(next); if (image.isNull()) return false;
    beginEdit(QStringLiteral("Delete GIF Frame"));
    layer = activeLayer();
    auto &edited = *layer->gif;
    edited.pixels.remove(edited.selected); edited.delays.remove(edited.selected); edited.frames.removeAt(position);
    edited.selected = next; layer->image = image;
    endEdit(); return true;
}
bool EditorSession::setGifDelay(int milliseconds, bool sameForAll, bool original)
{
    Layer *layer = activeLayer();
    if (!layer || !layer->gif || milliseconds < 20 || milliseconds > 655350) return false;
    beginEdit(QStringLiteral("GIF Frame Delay"));
    layer = activeLayer();
    auto &a = *layer->gif;
    a.sameDelay = sameForAll;
    if (sameForAll) a.uniformDelay = milliseconds;
    else if (original) a.delays.remove(a.selected);
    else a.delays.insert(a.selected, milliseconds);
    endEdit(); return true;
}
// The timeline's delay menu: every frame for the same time, or (nullopt) each for its own, the GIF's original.
bool EditorSession::setGifTiming(std::optional<int> uniformMilliseconds)
{
    Layer *layer = activeLayer();
    if (!layer || !layer->gif || (uniformMilliseconds && (*uniformMilliseconds < 20 || *uniformMilliseconds > 655350))) return false;
    const auto &a = *layer->gif;
    if (uniformMilliseconds ? a.sameDelay && a.uniformDelay == *uniformMilliseconds : !a.sameDelay) return true;
    beginEdit(QStringLiteral("Change GIF Delays"));
    layer = activeLayer();
    layer->gif->sameDelay = bool(uniformMilliseconds);
    if (uniformMilliseconds) layer->gif->uniformDelay = *uniformMilliseconds;
    endEdit(); return true;
}
void EditorSession::captureGifEdits()
{
    gifBefore_.clear();
    if (!document_) return;
    gifCanvasBefore_ = document_->canvasSize;
    for (const auto &layer : document_->layers) if (layer.gif) gifBefore_.insert(layer.id, layer);
}
void EditorSession::commitGifEdits()
{
    if (!document_) return;
    for (auto &layer : document_->layers) {
        if (!layer.gif || !gifBefore_.contains(layer.id)) continue;
        const auto &before = gifBefore_[layer.id];
        if (before.gif->selected != layer.gif->selected || before.image == layer.image) continue;
        auto &a = *layer.gif;
        if (layer.image.size() != before.image.size()) {
            // Keep every frame on the same pixel grid when a stroke grows it or Image Size resamples it.
            QTransform canvasScale;
            canvasScale.scale(double(document_->canvasSize.width()) / gifCanvasBefore_.width(), double(document_->canvasSize.height()) / gifCanvasBefore_.height());
            const QTransform map = before.transform.unitToDocument() * canvasScale * layer.transform.unitToDocument().inverted();
            QTransform original;
            if (!a.geometry.isEmpty()) original = QTransform(a.geometry.value("a").toDouble(), a.geometry.value("b").toDouble(), a.geometry.value("c").toDouble(), a.geometry.value("d").toDouble(), a.geometry.value("tx").toDouble(), a.geometry.value("ty").toDouble());
            const QTransform geometry = original * map;
            a.geometry = {{"width", layer.image.width()}, {"height", layer.image.height()}, {"a", geometry.m11()}, {"b", geometry.m12()},
                {"c", geometry.m21()}, {"d", geometry.m22()}, {"tx", geometry.dx()}, {"ty", geometry.dy()}, {"sampling", samplingToString(layer.transform.sampling)}};
            for (auto it = a.pixels.begin(); it != a.pixels.end(); ++it) {
                if (it.key() == a.selected) continue;
                QImage result(layer.image.size(), it.value().format()); result.fill(Qt::transparent);
                QPainter p(&result); p.setRenderHint(QPainter::SmoothPixmapTransform);
                p.scale(result.width(), result.height()); p.setTransform(map, true); p.drawImage(QRectF(0, 0, 1, 1), it.value()); p.end(); it.value() = result;
            }
        }
        if (!layer.image.isNull()) a.pixels.insert(a.selected, layer.image);
    }
    gifBefore_.clear();
}
}
