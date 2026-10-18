#include "io/RawImporter.h"
#include "io/LensCorrection.h"
#include <QTransform>
#include "rendering/DeepImage.h"

#include <QFileInfo>
#include <QImage>
#include <algorithm>
#include <cmath>

#if defined(COMPOSITOR_HAVE_LIBRAW)
#include <libraw/libraw.h>
#endif

namespace compositor {

QStringList RawImporter::supportedExtensions() {
    return {
        QStringLiteral("cr2"), QStringLiteral("cr3"), QStringLiteral("nef"), QStringLiteral("nrw"),
        QStringLiteral("arw"), QStringLiteral("srf"), QStringLiteral("sr2"), QStringLiteral("dng"),
        QStringLiteral("orf"), QStringLiteral("rw2"), QStringLiteral("pef"), QStringLiteral("ptx"),
        QStringLiteral("raf"), QStringLiteral("3fr"), QStringLiteral("fff"), QStringLiteral("dcr"),
        QStringLiteral("kdc"), QStringLiteral("k25"), QStringLiteral("mef"), QStringLiteral("mos"),
        QStringLiteral("mrw"), QStringLiteral("erf"), QStringLiteral("x3f"), QStringLiteral("raw"),
        QStringLiteral("rwl"), QStringLiteral("iiq"), QStringLiteral("srw"), QStringLiteral("bay")
    };
}

bool RawImporter::matches(const QString &filePath) {
    const QString ext = QFileInfo(filePath).suffix().toLower();
    return supportedExtensions().contains(ext);
}

bool RawImporter::probeDimensions(const QString &filePath, RawDimensions &dims) {
    dims = RawDimensions{};
    const QFileInfo fi(filePath);
    if (!fi.exists() || !fi.isFile() || fi.size() <= 0 || fi.size() > MaxFileBytes) {
        return false;
    }
#if defined(COMPOSITOR_HAVE_LIBRAW)
    libraw_data_t *lr = libraw_init(0);
    if (!lr) return false;
    if (libraw_open_file(lr, filePath.toUtf8().constData()) != LIBRAW_SUCCESS) {
        libraw_close(lr);
        return false;
    }
    dims.rawWidth = lr->sizes.raw_width;
    dims.rawHeight = lr->sizes.raw_height;
    dims.visibleWidth = lr->sizes.width;
    dims.visibleHeight = lr->sizes.height;
    dims.pixelAspect = lr->sizes.pixel_aspect > 0.001 ? lr->sizes.pixel_aspect : 1.0;
    dims.flip = lr->sizes.flip;

    libraw_adjust_sizes_info_only(lr);
    dims.outputWidth = lr->sizes.iwidth > 0 ? int(lr->sizes.iwidth) : dims.visibleWidth;
    dims.outputHeight = lr->sizes.iheight > 0 ? int(lr->sizes.iheight) : dims.visibleHeight;

    libraw_close(lr);
    return true;
#else
    return false;
#endif
}

bool RawImporter::pixelSize(const QString &filePath, int &width, int &height) {
    width = 0; height = 0;
    RawDimensions dims;
    if (!probeDimensions(filePath, dims)) return false;

    const double a = dims.pixelAspect > 0.001 ? dims.pixelAspect : 1.0;
    const int calcW = int(a > 1.0 ? std::ceil(std::max(dims.visibleWidth, dims.outputWidth) * a) : std::max(dims.visibleWidth, dims.outputWidth));
    const int calcH = int(a < 1.0 ? std::ceil(std::max(dims.visibleHeight, dims.outputHeight) / a) : std::max(dims.visibleHeight, dims.outputHeight));
    const int worstW = std::max({dims.rawWidth, dims.visibleWidth, dims.outputWidth, calcW});
    const int worstH = std::max({dims.rawHeight, dims.visibleHeight, dims.outputHeight, calcH});
    const int worstDim = std::max(worstW, worstH);
    const qint64 worstPixels = std::max({qint64(dims.rawWidth) * dims.rawHeight,
                                         qint64(dims.visibleWidth) * dims.visibleHeight,
                                         qint64(dims.outputWidth) * dims.outputHeight,
                                         qint64(calcW) * calcH});

    if (worstDim <= 0 || worstDim > MaxDimension || worstPixels > MaxDecodedPixels) {
        return false;
    }
    width = dims.outputWidth;
    height = dims.outputHeight;
    return true;
}

std::optional<RawDevelopSettings> RawImporter::asShot(const QString &filePath) {
    const QFileInfo fi(filePath);
    if (!fi.exists() || !fi.isFile() || fi.size() <= 0 || fi.size() > MaxFileBytes) {
        return std::nullopt;
    }
#if defined(COMPOSITOR_HAVE_LIBRAW)
    libraw_data_t *lr = libraw_init(0);
    if (!lr) return std::nullopt;
    if (libraw_open_file(lr, filePath.toUtf8().constData()) != LIBRAW_SUCCESS) {
        libraw_close(lr);
        return std::nullopt;
    }
    const int rawW = lr->sizes.raw_width;
    const int rawH = lr->sizes.raw_height;
    int visW = lr->sizes.width;
    int visH = lr->sizes.height;
    if (lr->sizes.flip == 5 || lr->sizes.flip == 6) {
        std::swap(visW, visH);
    }
    const double a = lr->sizes.pixel_aspect > 0.001 ? lr->sizes.pixel_aspect : 1.0;
    const int procW = int(a > 1.0 ? std::ceil(visW * a) : visW);
    const int procH = int(a < 1.0 ? std::ceil(visH / a) : visH);
    const int worstDim = std::max({rawW, rawH, visW, visH, procW, procH});
    const qint64 worstPixels = std::max({qint64(rawW) * rawH,
                                         qint64(visW) * visH,
                                         qint64(procW) * procH});
    if (worstDim <= 0 || worstDim > MaxDimension || worstPixels > MaxDecodedPixels) {
        libraw_close(lr);
        return std::nullopt;
    }
    RawDevelopSettings settings;
    float r = lr->color.cam_mul[0];
    float g = lr->color.cam_mul[1];
    float b = lr->color.cam_mul[2];
    if (g <= 0.0f) g = 1.0f;
    r /= g;
    b /= g;

    float temp = 5000.0f;
    if (r > 0.1f && b > 0.1f) {
        temp = std::clamp(5500.0f * std::pow(b / r, 0.6f), 2000.0f, 12000.0f);
    }
    float tint = 0.0f;
    settings.temperature = temp;
    settings.asShotTemperature = temp;
    settings.tint = tint;
    settings.asShotTint = tint;
    settings.exposure = 0.0f;
    settings.boost = 1.0f;

    libraw_close(lr);
    return settings;
#else
    return std::nullopt;
#endif
}

#if defined(COMPOSITOR_HAVE_LIBRAW)
static LensMetadata lensMetadata(const libraw_data_t &raw)
{
    return {QString::fromUtf8(raw.idata.make).trimmed(), QString::fromUtf8(raw.idata.model).trimmed(),
            QString::fromUtf8(raw.lens.LensMake).trimmed(), QString::fromUtf8(raw.lens.Lens).trimmed(),
            raw.other.focal_len, raw.other.aperture, 1000};
}
#endif
QString RawImporter::lensProfile(const QString &filePath)
{
#if defined(COMPOSITOR_HAVE_LIBRAW)
    const QFileInfo info(filePath);
    if (!info.isFile() || info.size() <= 0 || info.size() > MaxFileBytes) return {};
    libraw_data_t *raw = libraw_init(0);
    if (!raw) return {};
    const QString name = libraw_open_file(raw, filePath.toUtf8().constData()) == LIBRAW_SUCCESS
        ? LensCorrection::profileName(lensMetadata(*raw)) : QString();
    libraw_close(raw);
    return name;
#else
    Q_UNUSED(filePath);
    return {};
#endif
}

QImage RawImporter::develop(const QString &filePath, const RawDevelopSettings &settings, int limit,
                            std::atomic<bool> *cancelled, qint64 pixelBudget) {
    if (cancelled && cancelled->load()) return {};
    const QFileInfo fi(filePath);
    if (!fi.exists() || !fi.isFile() || fi.size() <= 0 || fi.size() > MaxFileBytes) {
        return {};
    }
#if defined(COMPOSITOR_HAVE_LIBRAW)
    if (cancelled && cancelled->load()) return {};
    libraw_data_t *lr = libraw_init(0);
    if (!lr) return {};
    if (cancelled && cancelled->load()) {
        libraw_close(lr);
        return {};
    }
    if (libraw_open_file(lr, filePath.toUtf8().constData()) != LIBRAW_SUCCESS) {
        libraw_close(lr);
        return {};
    }
    if (cancelled && cancelled->load()) {
        libraw_close(lr);
        return {};
    }

    const int rawW = lr->sizes.raw_width;
    const int rawH = lr->sizes.raw_height;
    int visW = lr->sizes.width;
    int visH = lr->sizes.height;
    if (lr->sizes.flip == 5 || lr->sizes.flip == 6) {
        std::swap(visW, visH);
    }
    const double aspect = lr->sizes.pixel_aspect > 0.001 ? lr->sizes.pixel_aspect : 1.0;
    const int procW = int(aspect > 1.0 ? std::ceil(visW * aspect) : visW);
    const int procH = int(aspect < 1.0 ? std::ceil(visH / aspect) : visH);

    const int worstW = std::max({rawW, visW, procW});
    const int worstH = std::max({rawH, visH, procH});
    const int worstDim = std::max(worstW, worstH);
    const qint64 worstPixels = std::max({qint64(rawW) * rawH,
                                         qint64(visW) * visH,
                                         qint64(procW) * procH});

    if (worstDim <= 0 || worstDim > MaxDimension || worstPixels > MaxDecodedPixels) {
        libraw_close(lr);
        return {};
    }

    const qint64 effectiveBudget = (pixelBudget > 0) ? std::min(pixelBudget, MaxOutputPixels) : MaxOutputPixels;
    if (limit <= 0 && worstPixels > effectiveBudget) {
        libraw_close(lr);
        return {};
    }

    if (cancelled && cancelled->load()) {
        libraw_close(lr);
        return {};
    }
    if (libraw_unpack(lr) != LIBRAW_SUCCESS) {
        libraw_close(lr);
        return {};
    }
    if (cancelled && cancelled->load()) {
        libraw_close(lr);
        return {};
    }

    const LensMetadata lens = lensMetadata(*lr);
    const bool correctLens = settings.lensCorrection && !LensCorrection::profileName(lens).isEmpty();
    const int flip = lr->sizes.flip;
    if (correctLens) lr->params.user_flip = 0;
    lr->params.output_bps = 16;
    lr->params.threshold = std::clamp(settings.noiseReduction,0.0f,1.0f)*1000;
    lr->params.highlight = settings.highlightRecovery ? 3 : 0;
    lr->params.output_color = 1; // sRGB
    lr->params.exp_correc = 1;
    lr->params.exp_shift = std::pow(2.0f, settings.exposure);
    lr->params.exp_preser = 0.5f;

    if (limit > 0) {
        const int longest = std::max(lr->sizes.width, lr->sizes.height);
        if (longest > limit) {
            lr->params.half_size = 1;
        }
    }

    if (settings.isAsShot()) {
        lr->params.use_camera_wb = 1;
    } else {
        const double T = std::clamp(double(settings.temperature), 2000.0, 12000.0);
        double rMul = 1.0, gMul = 1.0, bMul = 1.0;
        if (T <= 6600.0) {
            rMul = 1.0;
            bMul = std::clamp(std::log(std::max(10.0, T - 1000.0)) * 0.35 - 1.8, 0.2, 2.5);
        } else {
            rMul = std::clamp(std::pow(T / 6600.0, -0.6), 0.3, 1.0);
            bMul = 1.0;
        }
        const double magenta = settings.tint / 100.0;
        gMul = std::clamp(1.0 - 0.3 * magenta, 0.2, 2.0);
        rMul = std::clamp(rMul * (1.0 + 0.15 * magenta), 0.2, 3.0);
        bMul = std::clamp(bMul * (1.0 + 0.15 * magenta), 0.2, 3.0);

        lr->params.user_mul[0] = static_cast<float>(rMul);
        lr->params.user_mul[1] = static_cast<float>(gMul);
        lr->params.user_mul[2] = static_cast<float>(bMul);
        lr->params.user_mul[3] = static_cast<float>(gMul);
    }

    if (cancelled && cancelled->load()) {
        libraw_close(lr);
        return {};
    }

    int ret = libraw_dcraw_process(lr);
    if (ret != LIBRAW_SUCCESS) {
        libraw_close(lr);
        return {};
    }

    if (cancelled && cancelled->load()) {
        libraw_close(lr);
        return {};
    }

    libraw_processed_image_t *img = libraw_dcraw_make_mem_image(lr, &ret);
    if (!img || img->type != LIBRAW_IMAGE_BITMAP || img->colors != 3 || img->bits != 16) {
        if (img) libraw_dcraw_clear_mem(img);
        libraw_close(lr);
        return {};
    }

    if (img->width <= 0 || img->height <= 0 || img->width > MaxDimension || img->height > MaxDimension ||
        (qint64(img->width) * img->height) > effectiveBudget) {
        libraw_dcraw_clear_mem(img);
        libraw_close(lr);
        return {};
    }

    if (cancelled && cancelled->load()) {
        libraw_dcraw_clear_mem(img);
        libraw_close(lr);
        return {};
    }

    QImage wide(img->width,img->height,QImage::Format_RGBA64_Premultiplied);
    const auto *samples=reinterpret_cast<const uint16_t *>(img->data);
    for(int y=0;y<wide.height();++y){auto *out=reinterpret_cast<QRgba64 *>(wide.scanLine(y));for(int x=0;x<wide.width();++x){const size_t i=(size_t(y)*wide.width()+x)*3;out[x]=QRgba64::fromRgba64(samples[i],samples[i+1],samples[i+2],65535);}}
    libraw_dcraw_clear_mem(img);
    libraw_close(lr);
    if (cancelled && cancelled->load()) return {};
    if (correctLens) {
        wide = LensCorrection::apply(wide, lens, cancelled);
        const int angle = flip == 3 ? 180 : flip == 5 ? -90 : flip == 6 ? 90 : 0;
        if (angle) wide = wide.transformed(QTransform().rotate(angle));
        else if (flip == 1 || flip == 2) wide = wide.mirrored(flip == 1,flip == 2);
        else if (flip == 4 || flip == 7) wide = wide.transformed(QTransform(0,flip == 4 ? 1 : -1,flip == 4 ? 1 : -1,0,0,0));
    }
    // Finish the tone curve and preview resampling before the single final quantization.
    if (std::abs(settings.boost - 1.0f) > .01f) {
        const float boost = std::clamp(settings.boost, 0.0f, 2.0f);
        for (int y = 0; y < wide.height(); ++y) {
            if (cancelled && cancelled->load()) return {};
            auto *row = reinterpret_cast<QRgba64 *>(wide.scanLine(y));
            const auto tone = [boost](quint16 value) {
                const double v = value / 65535.0;
                const double result = v < .5 ? .5 * std::pow(2*v, boost) : 1 - .5 * std::pow(2*(1-v), boost);
                return quint16(std::clamp(qRound(result * 65535), 0, 65535));
            };
            for (int x = 0; x < wide.width(); ++x)
                row[x] = QRgba64::fromRgba64(tone(row[x].red()), tone(row[x].green()), tone(row[x].blue()), row[x].alpha());
        }
    }
    if (limit > 0 && std::max(wide.width(), wide.height()) > limit)
        wide = wide.scaled(limit, limit, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QImage copied = DeepImage::dithered(wide).convertToFormat(QImage::Format_ARGB32_Premultiplied);

    if (cancelled && cancelled->load()) return {};

    return copied;
#else
    Q_UNUSED(settings);
    Q_UNUSED(limit);
    Q_UNUSED(cancelled);
    return {};
#endif
}

} // namespace compositor

