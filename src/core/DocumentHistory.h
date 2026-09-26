#pragma once

#include "core/Document.h"

#include <optional>
#include <memory>

namespace compositor {

class DocumentHistory final {
public:
    struct Snapshot {
        std::optional<Document> document;
        std::optional<QUuid> activeLayerId;
        QUuid revision;
    };

    explicit DocumentHistory(int entryLimit = 100, qsizetype retainedByteLimit = 256 * 1024 * 1024);

    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    [[nodiscard]] QString undoName() const;
    [[nodiscard]] QString redoName() const;
    [[nodiscard]] bool isModified() const;
    [[nodiscard]] int undoCount() const;
    [[nodiscard]] QUuid currentRevision() const { return revision_; }
    void markSaved();
    void markSaved(const QUuid &revision);
    void markModified();
    void reset();
    void begin(const QString &name, const std::shared_ptr<Document> &document);
    void end(const std::shared_ptr<Document> &document);
    [[nodiscard]] std::optional<Snapshot> undo();
    [[nodiscard]] std::optional<Snapshot> redo();
    [[nodiscard]] qsizetype retainedBytes(const std::shared_ptr<Document> &current) const;

private:
    struct Entry {
        QString name;
        Snapshot before;
        Snapshot after;
    };

    static Snapshot snapshot(const std::shared_ptr<Document> &document, const QUuid &revision);
    void trim(const std::shared_ptr<Document> &current);

    QVector<Entry> past_;
    QVector<Entry> future_;
    QUuid revision_ = QUuid::createUuid();
    std::optional<QUuid> savedRevision_ = revision_;
    std::optional<Snapshot> pending_;
    QString pendingName_ = QStringLiteral("Edit");
    int depth_ = 0;
    int entryLimit_;
    qsizetype retainedByteLimit_;
};

} // namespace compositor
