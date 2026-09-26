#include "io/ProjectDigest.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace compositor {

std::optional<ProjectDigest> ProjectDigest::compute(const QString &packagePath)
{
    const QFileInfo pkgInfo(packagePath);
    if (!pkgInfo.exists() || !pkgInfo.isDir()) {
        return std::nullopt;
    }

    const QString manifestPath = pkgInfo.filePath() + QStringLiteral("/manifest.json");
    QFile manifestFile(manifestPath);
    if (!manifestFile.exists() || !manifestFile.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray manifestBytes = manifestFile.readAll();
    manifestFile.close();
    if (manifestBytes.isEmpty()) {
        return std::nullopt;
    }

    QCryptographicHash hasher(QCryptographicHash::Sha256);
    hasher.addData(manifestBytes);

    const QString imagesPath = pkgInfo.filePath() + QStringLiteral("/images");
    const QDir imagesDir(imagesPath);
    if (!imagesDir.exists()) {
        return std::nullopt;
    }

    const QStringList files = imagesDir.entryList(QDir::Files, QDir::Name);
    for (const QString &fileName : files) {
        hasher.addData(fileName.toUtf8());
        const QFileInfo fileInfo(imagesDir.filePath(fileName));
        const quint64 size = static_cast<quint64>(fileInfo.size());
        hasher.addData(QByteArrayView(reinterpret_cast<const char *>(&size), sizeof(size)));

        QFile imageFile(fileInfo.filePath());
        if (!imageFile.open(QIODevice::ReadOnly)) {
            return std::nullopt;
        }
        QCryptographicHash imageHasher(QCryptographicHash::Sha256);
        if (!imageHasher.addData(&imageFile)) {
            return std::nullopt;
        }
        hasher.addData(imageHasher.result());
    }

    return ProjectDigest(hasher.result());
}

} // namespace compositor
