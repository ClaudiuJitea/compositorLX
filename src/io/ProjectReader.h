#pragma once

#include "core/Document.h"

#include <QString>

#include <stdexcept>

namespace compositor {

class ProjectError final : public std::runtime_error {
public:
    explicit ProjectError(const QString &message);
    [[nodiscard]] QString message() const;

private:
    QString message_;
};

class ProjectReader final {
public:
    [[nodiscard]] static Document load(const QString &projectDirectory);
};

} // namespace compositor

