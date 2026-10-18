#pragma once
#include <QImage>
#include <QString>
#include <QByteArray>
#include <optional>
namespace compositor::HdrImage
{
QImage read(const QString &path, QString *error = nullptr);
QImage readRadiance(const QString &path, QString *error = nullptr);
std::optional<QByteArray> encode(const QImage &image, QString *error = nullptr);
} // namespace compositor::HdrImage
