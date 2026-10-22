#pragma once
#include <QImage>
#include <QSize>
#include <QString>
#include <QVector>
#include "core/DocumentLimits.h"
namespace compositor {
class PdfImporter {
public:
    static QVector<QImage> read(const QString &path, const QSize &fit = {}, QString *error = nullptr,
                               qint64 pixelBudget = DocumentLimits::documentPixelBudget());
};
}
