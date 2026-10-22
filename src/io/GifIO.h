#pragma once
#include "core/Document.h"
namespace compositor {
class GifIO {
public:
    static std::optional<GifAnimation> read(const QByteArray &data, QString *error = nullptr);
    static std::optional<GifAnimation> readFile(const QString &path, QString *error = nullptr);
    static QJsonObject metadata(const GifAnimation &animation, const QString &id);
    static bool restoreMetadata(GifAnimation &animation, const QJsonObject &metadata, QString *error);
    static bool write(const Document &document, const QString &path, const QSize &size, bool loop, bool transparency, QString *error = nullptr);
};
}
