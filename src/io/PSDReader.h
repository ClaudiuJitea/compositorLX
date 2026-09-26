#pragma once

#include "core/Document.h"

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QUuid>

namespace compositor {

struct PSDConversion {
    QUuid id;
    QString layerName;
    QString message;

    bool operator==(const PSDConversion &) const = default;
};

struct PSDImportResult {
    Document document;
    QVector<PSDConversion> conversions;
};

enum class PSDErrorCode {
    NoError,
    Truncated,
    UnsupportedVersion,
    UnsupportedColorMode,
    UnsupportedDepth,
    UnsupportedCompression,
    TooLarge,
    Unreadable
};

QString psdErrorMessage(PSDErrorCode code);

class PSDReader {
public:
    static bool matches(const QString &path);
    static bool matches(const QByteArray &data);

    static bool read(const QString &path, PSDImportResult &result, QString *error = nullptr, qint64 remainingPixels = 100000000LL);
    static bool read(const QByteArray &data, PSDImportResult &result, QString *error = nullptr, qint64 remainingPixels = 100000000LL);

    static std::optional<BlendMode> blendModeFromPSD(const QString &key);
};

} // namespace compositor
