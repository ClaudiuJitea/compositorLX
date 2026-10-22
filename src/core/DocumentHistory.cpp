#include "core/DocumentHistory.h"

#include <QSet>

#include <algorithm>

namespace compositor {

DocumentHistory::DocumentHistory(int entryLimit, qsizetype retainedByteLimit)
    : entryLimit_(std::max(0, entryLimit)), retainedByteLimit_(std::max<qsizetype>(0, retainedByteLimit))
{
}

bool DocumentHistory::canUndo() const { return depth_ == 0 && !past_.isEmpty(); }
bool DocumentHistory::canRedo() const { return depth_ == 0 && !future_.isEmpty(); }
QString DocumentHistory::undoName() const { return past_.isEmpty() ? QString() : past_.constLast().name; }
QString DocumentHistory::redoName() const { return future_.isEmpty() ? QString() : future_.constLast().name; }
bool DocumentHistory::isModified() const { return !savedRevision_ || revision_ != *savedRevision_; }
int DocumentHistory::undoCount() const { return past_.size(); }
void DocumentHistory::markSaved() { savedRevision_ = revision_; }
void DocumentHistory::markSaved(const QUuid &revision) { savedRevision_ = revision; }
void DocumentHistory::markModified() { savedRevision_.reset(); }

DocumentHistory::Snapshot DocumentHistory::snapshot(const std::shared_ptr<Document> &document, const QUuid &revision)
{
    return {document ? std::optional<Document>(*document) : std::nullopt,
            document ? document->activeLayerId : std::nullopt, revision};
}

void DocumentHistory::reset()
{
    past_.clear();
    future_.clear();
    pending_.reset();
    depth_ = 0;
    revision_ = QUuid::createUuid();
    savedRevision_ = revision_;
}

void DocumentHistory::begin(const QString &name, const std::shared_ptr<Document> &document)
{
    if (depth_ == 0) {
        pending_ = snapshot(document, revision_);
        pendingName_ = name;
    }
    ++depth_;
}

void DocumentHistory::end(const std::shared_ptr<Document> &document)
{
    if (depth_ <= 0) return;
    --depth_;
    if (depth_ != 0 || !pending_) return;
    const Snapshot before = std::move(*pending_);
    pending_.reset();
    const std::optional<Document> afterDocument = document ? std::optional<Document>(*document) : std::nullopt;
    if (before.document == afterDocument) return;
    revision_ = QUuid::createUuid();
    past_.push_back({pendingName_, before, snapshot(document, revision_)});
    future_.clear();
    trim(document);
}

std::optional<DocumentHistory::Snapshot> DocumentHistory::undo()
{
    if (!canUndo()) return std::nullopt;
    Entry entry = past_.takeLast();
    const Snapshot result = entry.before;
    future_.push_back(std::move(entry));
    revision_ = result.revision;
    // The document the undo lands on decides which pixels only history still holds (mac DocumentHistory.undo).
    trim(result.document ? std::make_shared<Document>(*result.document) : nullptr);
    return result;
}

std::optional<DocumentHistory::Snapshot> DocumentHistory::redo()
{
    if (!canRedo()) return std::nullopt;
    Entry entry = future_.takeLast();
    const Snapshot result = entry.after;
    past_.push_back(std::move(entry));
    revision_ = result.revision;
    trim(result.document ? std::make_shared<Document>(*result.document) : nullptr);
    return result;
}

qsizetype DocumentHistory::retainedBytes(const std::shared_ptr<Document> &current) const
{
    QSet<qint64> seen;
    QSet<const GifSource *> sources;
    qsizetype bytes = 0;
    const auto countImage = [&](const QImage &image, bool live) {
        if (!image.isNull() && !seen.contains(image.cacheKey())) { seen.insert(image.cacheKey()); if (!live) bytes += image.sizeInBytes(); }
    };
    const auto countDocument = [&](const Document &doc, bool live) {
        for (const Layer &layer : doc.layers) {
            countImage(layer.image, live); countImage(layer.mask, live);
            if (layer.gif) {
                for (const auto &image : layer.gif->pixels) countImage(image, live);
                if (layer.gif->source && !sources.contains(layer.gif->source.get())) {
                    sources.insert(layer.gif->source.get());
                    if (!live) bytes += layer.gif->source->data.size();
                    for (const auto &image : layer.gif->source->thumbnails) countImage(image, live);
                }
            }
        }
    };
    if (current) countDocument(*current, true);
    const auto count = [&](const Snapshot &value) { if (value.document) countDocument(*value.document, false); };
    for (const Entry &entry : past_) { count(entry.before); count(entry.after); }
    for (const Entry &entry : future_) { count(entry.before); count(entry.after); }
    return bytes;
}

void DocumentHistory::trim(const std::shared_ptr<Document> &current)
{
    while (past_.size() + future_.size() > entryLimit_ || retainedBytes(current) > retainedByteLimit_) {
        if (!past_.isEmpty()) past_.removeFirst();
        else if (!future_.isEmpty()) future_.removeFirst();
        else break;
    }
}

} // namespace compositor
