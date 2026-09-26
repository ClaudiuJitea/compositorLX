#pragma once

#include <QByteArray>
#include <QString>
#include <optional>

namespace compositor {

class ProjectDigest final {
public:
    ProjectDigest() = default;
    explicit ProjectDigest(QByteArray hash) : hash_(std::move(hash)) {}

    [[nodiscard]] bool isValid() const { return !hash_.isEmpty(); }
    [[nodiscard]] const QByteArray &hash() const { return hash_; }
    [[nodiscard]] QString toHex() const { return QString::fromLatin1(hash_.toHex()); }

    bool operator==(const ProjectDigest &other) const { return hash_ == other.hash_; }
    bool operator!=(const ProjectDigest &other) const { return hash_ != other.hash_; }

    /// Computes a content digest of the project package:
    /// - manifest.json byte-for-byte
    /// - sorted entries in images/ (file name, file size, and file content SHA-256)
    /// Returns std::nullopt if the package is missing, incomplete, or unreadable.
    static std::optional<ProjectDigest> compute(const QString &packagePath);

private:
    QByteArray hash_;
};

} // namespace compositor
