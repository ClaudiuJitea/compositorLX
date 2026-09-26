#include "io/ProjectWatcher.h"
#include "io/ProjectReader.h"

#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QTimer>
#include <QtConcurrent>

namespace compositor {

ProjectWatcher::InspectionHook ProjectWatcher::sInspectionHook = nullptr;

void ProjectWatcher::setInspectionHook(InspectionHook hook)
{
    sInspectionHook = std::move(hook);
}

ProjectWatcher::ProjectWatcher(const QString &packagePath, QObject *parent)
    : QObject(parent)
    , packagePath_(QFileInfo(packagePath).absoluteFilePath())
    , parentPath_(QFileInfo(packagePath_).absoluteDir().absolutePath())
    , manifestPath_(packagePath_ + QStringLiteral("/manifest.json"))
    , imagesPath_(packagePath_ + QStringLiteral("/images"))
    , watcher_(new QFileSystemWatcher(this))
    , coalescingTimer_(new QTimer(this))
    , rearmTimer_(new QTimer(this))
{
    coalescingTimer_->setSingleShot(true);
    connect(coalescingTimer_, &QTimer::timeout, this, &ProjectWatcher::onCoalesceTimeout);

    connect(rearmTimer_, &QTimer::timeout, this, &ProjectWatcher::onRearmTick);

    connect(watcher_, &QFileSystemWatcher::fileChanged, this, &ProjectWatcher::onFileSystemChanged);
    connect(watcher_, &QFileSystemWatcher::directoryChanged, this, &ProjectWatcher::onFileSystemChanged);

    arm();
}

ProjectWatcher::~ProjectWatcher()
{
    stop();
}

void ProjectWatcher::setKnownDigest(const ProjectDigest &digest)
{
    knownDigest_ = digest;
}

void ProjectWatcher::clearKnownDigest()
{
    knownDigest_.reset();
}

void ProjectWatcher::setSaving(bool saving)
{
    saving_ = saving;
}

void ProjectWatcher::setCoalescingInterval(int ms)
{
    coalescingIntervalMs_ = std::max(10, ms);
}

void ProjectWatcher::stop()
{
    ++currentRequestId_;
    inFlightInspections_ = 0;
    inspecting_ = false;
    if (coalescingTimer_) coalescingTimer_->stop();
    if (rearmTimer_) rearmTimer_->stop();
    if (watcher_) {
        const QStringList files = watcher_->files();
        if (!files.isEmpty()) watcher_->removePaths(files);
        const QStringList dirs = watcher_->directories();
        if (!dirs.isEmpty()) watcher_->removePaths(dirs);
    }
}

void ProjectWatcher::arm()
{
    if (!watcher_) return;

    const QStringList existingFiles = watcher_->files();
    if (!existingFiles.isEmpty()) watcher_->removePaths(existingFiles);
    const QStringList existingDirs = watcher_->directories();
    if (!existingDirs.isEmpty()) watcher_->removePaths(existingDirs);

    if (QFileInfo::exists(parentPath_)) {
        watcher_->addPath(parentPath_);
    }
    if (QFileInfo::exists(packagePath_)) {
        watcher_->addPath(packagePath_);
    }
    if (QFileInfo::exists(manifestPath_)) {
        watcher_->addPath(manifestPath_);
    }
    if (QFileInfo::exists(imagesPath_)) {
        watcher_->addPath(imagesPath_);
    }
}

void ProjectWatcher::onFileSystemChanged(const QString &/*path*/)
{
    rearmAttempts_ = 0;
    rearmTimer_->start(100);
    coalescingTimer_->start(coalescingIntervalMs_);
}

void ProjectWatcher::onRearmTick()
{
    ++rearmAttempts_;
    arm();
    const bool allWatched = watcher_->directories().contains(packagePath_) &&
                            watcher_->directories().contains(imagesPath_) &&
                            watcher_->files().contains(manifestPath_);
    if (allWatched || rearmAttempts_ >= 20) {
        rearmTimer_->stop();
    }
}

void ProjectWatcher::onCoalesceTimeout()
{
    checkAsync();
}

ProjectWatcher::InspectionResult ProjectWatcher::inspectPackage(uint64_t reqId, const QString &packagePath, const std::optional<ProjectDigest> &known)
{
    if (sInspectionHook) {
        sInspectionHook(packagePath);
    }

    InspectionResult res;
    res.requestId = reqId;
    res.packagePath = packagePath;
    res.expectedKnownDigest = known;

    if (!QFileInfo::exists(packagePath)) {
        res.type = ChangeType::Removed;
        return res;
    }

    const auto digest = ProjectDigest::compute(packagePath);
    if (!digest) {
        res.type = ChangeType::Incomplete;
        return res;
    }

    if (known && *digest == *known) {
        res.type = ChangeType::Unchanged;
        res.newDigest = *digest;
        return res;
    }

    // Fully validate the candidate package before notifying or replacing
    try {
        auto doc = std::make_shared<Document>(ProjectReader::load(packagePath));
        doc->projectPath = packagePath;
        res.type = ChangeType::Changed;
        res.newDigest = *digest;
        res.document = doc;
        return res;
    } catch (...) {
        res.type = ChangeType::Incomplete;
        return res;
    }
}

void ProjectWatcher::checkAsync()
{
    if (saving_) {
        emit changeEvaluated(ChangeType::Unchanged);
        return;
    }

    if (!QFileInfo::exists(packagePath_)) {
        emit changeEvaluated(ChangeType::Removed);
        emit packageRemovedExternally(packagePath_);
        return;
    }

    const uint64_t reqId = ++currentRequestId_;
    ++inFlightInspections_;
    inspecting_ = true;
    const QString pkgPath = packagePath_;
    const std::optional<ProjectDigest> known = knownDigest_;

    auto future = QtConcurrent::run([reqId, pkgPath, known]() -> InspectionResult {
        return ProjectWatcher::inspectPackage(reqId, pkgPath, known);
    });

    auto *watcher = new QFutureWatcher<InspectionResult>(this);
    connect(watcher, &QFutureWatcher<InspectionResult>::finished, this, [this, watcher, reqId]() {
        const InspectionResult res = watcher->result();
        watcher->deleteLater();

        --inFlightInspections_;
        if (inFlightInspections_ <= 0) {
            inFlightInspections_ = 0;
            inspecting_ = false;
        }

        if (currentRequestId_ != reqId) {
            return;
        }

        emit changeEvaluated(res.type);

        if (res.type == ChangeType::Removed) {
            emit packageRemovedExternally(res.packagePath);
        } else if (res.type == ChangeType::Changed && res.newDigest.has_value()) {
            emit packageValidatedExternally(res.requestId, res.packagePath, res.expectedKnownDigest, *res.newDigest, res.document);
            emit packageChangedExternally(res.packagePath, *res.newDigest);
        }
    });

    watcher->setFuture(future);
}

ProjectWatcher::ChangeType ProjectWatcher::checkNow()
{
    if (saving_) {
        emit changeEvaluated(ChangeType::Unchanged);
        return ChangeType::Unchanged;
    }

    if (!QFileInfo::exists(packagePath_)) {
        emit changeEvaluated(ChangeType::Removed);
        emit packageRemovedExternally(packagePath_);
        return ChangeType::Removed;
    }

    const uint64_t reqId = ++currentRequestId_;
    const InspectionResult res = inspectPackage(reqId, packagePath_, knownDigest_);

    emit changeEvaluated(res.type);
    if (res.type == ChangeType::Removed) {
        emit packageRemovedExternally(res.packagePath);
    } else if (res.type == ChangeType::Changed && res.newDigest.has_value()) {
        emit packageValidatedExternally(res.requestId, res.packagePath, res.expectedKnownDigest, *res.newDigest, res.document);
        emit packageChangedExternally(res.packagePath, *res.newDigest);
    }
    return res.type;
}

} // namespace compositor
