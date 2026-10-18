#pragma once
#include <QByteArray>
#include <QImage>
#include <QString>
#include <QVector>
#include <optional>
namespace compositor
{
struct CMYKProfile {
    QString name;
    QByteArray data;
    static std::optional<CMYKProfile> fromData(const QByteArray &data, const QString &name = {});
    bool operator==(const CMYKProfile &) const = default;
};
namespace CMYK
{
QVector<CMYKProfile> installed();
std::optional<CMYKProfile> chosen();
bool choose(const CMYKProfile &profile);
std::optional<CMYKProfile> load(const QString &path);
QByteArray inks(const QImage &source, const CMYKProfile &profile, const QColor &matte = Qt::white);
QImage fromInks(const QByteArray &inks, const QSize &size, const CMYKProfile &profile);
QImage proof(const QImage &source, const CMYKProfile &profile, bool gamutWarning = false,
             bool showProof = true);
} // namespace CMYK
} // namespace compositor
