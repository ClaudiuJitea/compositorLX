#pragma once

#include "core/Document.h"

#include <stdexcept>

namespace compositor {

class ProjectWriteError final : public std::runtime_error {
public:
    explicit ProjectWriteError(const QString &message);
    [[nodiscard]] QString message() const { return message_; }
private:
    QString message_;
};

class ProjectWriter final {
public:
    static void save(const Document &document, const QString &projectDirectory);
};

} // namespace compositor
