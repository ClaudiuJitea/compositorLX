#pragma once

#include "core/Document.h"
#include "io/ProjectDigest.h"

#include <QObject>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class QFileSystemWatcher;
class QTimer;

namespace compositor {

class ProjectWatcher final : public QObject {
    Q_OBJECT

public:
    enum class ChangeType {
        Removed,
        Incomplete,
        Unchanged,
        Changed
    };
    Q_ENUM(ChangeType)

    struct InspectionResult {
        uint64_t requestId = 0;
        QString packagePath;
        std::optional<ProjectDigest> expectedKnownDigest;
        ChangeType type = ChangeType::Unchanged;
        std::optional<ProjectDigest> newDigest;
        std::shared_ptr<Document> document;
    };

    using InspectionHook = std::function<void(const QString &packagePath)>;
    static void setInspectionHook(InspectionHook hook);

    explicit ProjectWatcher(const QString &packagePath, QObject *parent = nullptr);
    ~ProjectWatcher() override;

    [[nodiscard]] QString packagePath() const { return packagePath_; }
    [[nodiscard]] QString projectDirectory() const { return packagePath_; }

    void setKnownDigest(const ProjectDigest &digest);
    void clearKnownDigest();
    [[nodiscard]] std::optional<ProjectDigest> knownDigest() const { return knownDigest_; }

    void setSaving(bool saving);
    [[nodiscard]] bool isSaving() const { return saving_; }

    void setCoalescingInterval(int ms);
    [[nodiscard]] int coalescingInterval() const { return coalescingIntervalMs_; }

    void stop();
    void arm();

    /// Asynchronously evaluates current disk state off the GUI thread
    void checkAsync();

    /// Synchronously evaluates current disk state against known digest
    ChangeType checkNow();

    [[nodiscard]] bool isInspecting() const { return inspecting_; }
    [[nodiscard]] uint64_t currentRequestId() const { return currentRequestId_; }

    static InspectionResult inspectPackage(uint64_t reqId, const QString &packagePath, const std::optional<ProjectDigest> &known);

signals:
    void packageChangedExternally(const QString &packagePath, const ProjectDigest &newDigest);
    void packageRemovedExternally(const QString &packagePath);
    void packageValidatedExternally(uint64_t requestId, const QString &packagePath,
                                   const std::optional<ProjectDigest> &expectedKnownDigest,
                                   const ProjectDigest &newDigest,
                                   const std::shared_ptr<Document> &loadedDocument);
    void changeEvaluated(ChangeType type);

private slots:
    void onFileSystemChanged(const QString &path);
    void onRearmTick();
    void onCoalesceTimeout();

private:
    QString packagePath_;
    QString parentPath_;
    QString manifestPath_;
    QString imagesPath_;

    QFileSystemWatcher *watcher_ = nullptr;
    QTimer *coalescingTimer_ = nullptr;
    QTimer *rearmTimer_ = nullptr;

    int rearmAttempts_ = 0;
    int coalescingIntervalMs_ = 300;

    std::optional<ProjectDigest> knownDigest_;
    bool saving_ = false;

    uint64_t currentRequestId_ = 0;
    std::atomic<int> inFlightInspections_{0};
    std::atomic<bool> inspecting_{false};

    static InspectionHook sInspectionHook;
};

} // namespace compositor
