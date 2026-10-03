#pragma once

#include "core/Document.h"

#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QRectF>
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

    // Exposed for tests: Photoshop's 'levl' / 'hue2' adjustment blocks, and a mask patch placed on a layer's grid.
    static std::optional<QJsonObject> levelsAdjustment(const QByteArray &data);
    static std::optional<QJsonObject> hueSaturationAdjustment(const QByteArray &data);
    static QImage maskOnLayerGrid(const QImage &patch, const QRectF &maskBounds, quint8 maskDefault,
                                  const QRectF &layerPlacement, const QSize &layerGrid);
};

} // namespace compositor
