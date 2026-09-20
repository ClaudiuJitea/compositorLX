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
    return result;
}

std::optional<DocumentHistory::Snapshot> DocumentHistory::redo()
{
    if (!canRedo()) return std::nullopt;
    Entry entry = future_.takeLast();
    const Snapshot result = entry.after;
    past_.push_back(std::move(entry));
    revision_ = result.revision;
    return result;
}

qsizetype DocumentHistory::retainedBytes(const std::shared_ptr<Document> &current) const
{
    QSet<qint64> live;
    if (current) for (const Layer &layer : current->layers) {
        if (!layer.image.isNull()) live.insert(layer.image.cacheKey());
        if (!layer.mask.isNull()) live.insert(layer.mask.cacheKey());
    }
    QSet<qint64> seen = live;
    qsizetype bytes = 0;
    const auto count = [&](const Snapshot &value) {
        if (!value.document) return;
        for (const Layer &layer : value.document->layers) for (const QImage *image : {&layer.image, &layer.mask}) {
            if (!image->isNull() && !seen.contains(image->cacheKey())) {
                seen.insert(image->cacheKey());
                bytes += image->sizeInBytes();
            }
        }
    };
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
