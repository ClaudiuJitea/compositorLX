#include "io/CMYK.h"
#include <lcms2.h>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QSaveFile>
#include <algorithm>

namespace compositor
{
std::optional<CMYKProfile> CMYKProfile::fromData(const QByteArray &data, const QString &name)
{
    if (data.isEmpty() || data.size() > 16 * 1024 * 1024)
        return std::nullopt;
    cmsHPROFILE profile = cmsOpenProfileFromMem(data.constData(), cmsUInt32Number(data.size()));
    if (!profile)
        return std::nullopt;
    const bool valid = cmsGetColorSpace(profile) == cmsSigCmykData;
    char description[512]{};
    cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US", description, sizeof description);
    cmsCloseProfile(profile);
    if (!valid)
        return std::nullopt;
    return CMYKProfile{name.isEmpty() ? QString::fromUtf8(description) : name, data};
}
namespace CMYK
{
QVector<CMYKProfile> installed()
{
    QVector<CMYKProfile> out;
    QStringList roots{
        QStringLiteral("/usr/share/color/icc"), QStringLiteral("/usr/local/share/color/icc"),
        QDir::homePath() + QStringLiteral("/.color/icc"),
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/color/icc"),
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/CMYK Profiles")};
    for (const QString &root : roots) {
        QDirIterator it(root, {QStringLiteral("*.icc"), QStringLiteral("*.icm")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QFile file(it.next());
            if (file.size() > 16 * 1024 * 1024 || !file.open(QIODevice::ReadOnly))
                continue;
            const auto p = CMYKProfile::fromData(file.readAll(), QFileInfo(file).completeBaseName());
            if (p && std::none_of(out.cbegin(), out.cend(), [&](const auto &v) { return v.name == p->name; }))
                out.push_back(*p);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const auto &a, const auto &b) { return a.name.localeAwareCompare(b.name) < 0; });
    return out;
}
std::optional<CMYKProfile> chosen()
{
    QSettings settings;
    const auto data = settings.value(QStringLiteral("cmykProfileData")).toByteArray();
    if (const auto p =
            CMYKProfile::fromData(data, settings.value(QStringLiteral("cmykProfileName")).toString()))
        return p;
    const auto profiles = installed();
    return profiles.isEmpty() ? std::nullopt : std::optional<CMYKProfile>(profiles.first());
}
bool choose(const CMYKProfile &p)
{
    if (!CMYKProfile::fromData(p.data, p.name))
        return false;
    QSettings s;
    s.setValue(QStringLiteral("cmykProfileData"), p.data);
    s.setValue(QStringLiteral("cmykProfileName"), p.name);
    return true;
}
std::optional<CMYKProfile> load(const QString &path)
{
    QFile file(path);
    if (file.size() > 16 * 1024 * 1024 || !file.open(QIODevice::ReadOnly))
        return std::nullopt;
    auto p = CMYKProfile::fromData(file.readAll(), QFileInfo(path).completeBaseName());
    if (!p)
        return std::nullopt;
    const QString root =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/CMYK Profiles");
    QDir().mkpath(root);
    QSaveFile out(root + QLatin1Char('/') + QFileInfo(path).fileName());
    if (!out.open(QIODevice::WriteOnly) || out.write(p->data) != p->data.size() || !out.commit())
        return std::nullopt;
    choose(*p);
    return p;
}
QByteArray inks(const QImage &source, const CMYKProfile &p, const QColor &matte)
{
    if (source.isNull())
        return {};
    cmsHPROFILE rgb = cmsCreate_sRGBProfile(),
                cmyk = cmsOpenProfileFromMem(p.data.constData(), cmsUInt32Number(p.data.size()));
    if (!cmyk) {
        cmsCloseProfile(rgb);
        return {};
    }
    cmsHTRANSFORM transform = cmsCreateTransform(
        rgb, TYPE_RGB_8, cmyk, TYPE_CMYK_8, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION);
    cmsCloseProfile(rgb);
    cmsCloseProfile(cmyk);
    if (!transform)
        return {};
    QImage opaque(source.size(), QImage::Format_RGB888);
    opaque.fill(matte);
    {
        QPainter painter(&opaque);
        painter.drawImage(0, 0, source);
    }
    QByteArray out(qsizetype(source.width()) * source.height() * 4, Qt::Uninitialized);
    for (int y = 0; y < source.height(); ++y)
        cmsDoTransform(transform, opaque.constScanLine(y), out.data() + qsizetype(y) * source.width() * 4,
                       source.width());
    cmsDeleteTransform(transform);
    return out;
}
QImage fromInks(const QByteArray &data, const QSize &size, const CMYKProfile &p)
{
    if (size.width() < 1 || size.height() < 1 || data.size() != qint64(size.width()) * size.height() * 4)
        return {};
    cmsHPROFILE rgb = cmsCreate_sRGBProfile(),
                cmyk = cmsOpenProfileFromMem(p.data.constData(), cmsUInt32Number(p.data.size()));
    if (!cmyk) {
        cmsCloseProfile(rgb);
        return {};
    }
    cmsHTRANSFORM transform = cmsCreateTransform(
        cmyk, TYPE_CMYK_8, rgb, TYPE_RGB_8, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION);
    cmsCloseProfile(rgb);
    cmsCloseProfile(cmyk);
    if (!transform)
        return {};
    QImage out(size, QImage::Format_RGB888);
    for (int y = 0; y < size.height(); ++y)
        cmsDoTransform(transform, data.constData() + qsizetype(y) * size.width() * 4, out.scanLine(y),
                       size.width());
    cmsDeleteTransform(transform);
    return out;
}
QImage proof(const QImage &source, const CMYKProfile &p, bool gamutWarning, bool showProof)
{
    QImage straight = source.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < straight.height(); ++y) {
        auto *row = straight.scanLine(y);
        for (int x = 0; x < straight.width(); ++x)
            row[x * 4 + 3] = 255;
    }
    const QByteArray data = inks(straight, p);
    QImage out = fromInks(data, source.size(), p).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (out.isNull())
        return source;
    const QImage alpha = source.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < out.height(); ++y) {
        auto *dst = out.scanLine(y);
        const auto *src = alpha.constScanLine(y);
        for (int x = 0; x < out.width(); ++x) {
            bool outside = false;
            for (int c = 0; c < 3; ++c)
                outside |= std::abs(int(dst[x * 4 + c]) - int(straight.constScanLine(y)[x * 4 + c])) > 10;
            for (int c = 0; c < 3; ++c) {
                const int value = gamutWarning && outside ? 128
                                  : showProof             ? dst[x * 4 + c]
                                                          : straight.constScanLine(y)[x * 4 + c];
                dst[x * 4 + c] = uchar(value * src[x * 4 + 3] / 255);
            }
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }
    return out;
}
} // namespace CMYK
} // namespace compositor
