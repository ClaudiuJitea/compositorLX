#pragma once

#include "core/Document.h"

#include <stdexcept>

#include "io/ProjectDigest.h"

namespace compositor {

class ProjectWriteError final : public std::runtime_error {
public:
    explicit ProjectWriteError(const QString &message, bool isConflict = false);
    [[nodiscard]] QString message() const { return message_; }
    [[nodiscard]] bool isConflict() const { return isConflict_; }
private:
    QString message_;
    bool isConflict_ = false;
};

enum class SaveFaultInjection {
    None,
    FailAfterBackupBeforeInstall,
    FailInstallStaged,
    FailRollbackRestore,
    CrashAfterBackupBeforeInstall
};

struct SaveResult final {
    enum class Status {
        Success,
        Conflict,
        IoError
    };
    Status status = Status::Success;
    QString errorMessage;
    std::optional<ProjectDigest> installedDigest;
};

struct ExpectedDestinationState final {
    enum class Mode {
        Any,
        MustBeAbsent,
        MustMatchDigest
    };
    Mode mode = Mode::Any;
    ProjectDigest digest;

    static ExpectedDestinationState any() {
        return {Mode::Any, {}};
    }
    static ExpectedDestinationState mustBeAbsent() {
        return {Mode::MustBeAbsent, {}};
    }
    static ExpectedDestinationState mustMatch(const ProjectDigest &d) {
        return {Mode::MustMatchDigest, d};
    }
};

class ProjectWriter final {
public:
    using SaveResult = compositor::SaveResult;
    using ExpectedDestinationState = compositor::ExpectedDestinationState;
    using WorkerDelayHook = std::function<void(const QString &stage, const QString &destination)>;

    static void setWorkerDelayHook(WorkerDelayHook hook);

    static void save(const Document &document, const QString &projectDirectory,
                     SaveFaultInjection fault = SaveFaultInjection::None);
    static SaveResult saveAtomicChecked(const Document &document,
                                       const QString &projectDirectory,
                                       const ExpectedDestinationState &expectedState,
                                       SaveFaultInjection fault = SaveFaultInjection::None);
    static SaveResult saveAtomicChecked(const Document &document,
                                       const QString &projectDirectory,
                                       const std::optional<ProjectDigest> &expectedDigest,
                                       bool checkConflict = true,
                                       SaveFaultInjection fault = SaveFaultInjection::None);
    static bool recoverInterruptedPackage(const QString &projectDirectory);
    [[nodiscard]] static int computeTargetVersion(const Document &document);
};

} // namespace compositor
