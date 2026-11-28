#include "io/MovieDecoder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTransform>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace compositor {

namespace {

QString averror(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return QString::fromUtf8(text);
}

// How the track's display matrix turns its pictures upright, as FFmpeg's own autorotate does (fftools get_rotation and
// the transpose and flip it inserts): a rotation in quarter turns, then flips.
struct Orientation {
    int quarterTurnsClockwise = 0;
    bool flipHorizontal = false, flipVertical = false;
    [[nodiscard]] bool swapsSides() const { return quarterTurnsClockwise % 2 != 0; }
};

Orientation orientation(const int32_t *matrix)
{
    Orientation result;
    if (!matrix) return result;
    double theta = -std::round(av_display_rotation_get(matrix));
    theta -= 360 * std::floor(theta / 360 + 0.9 / 360);
    if (std::abs(theta - 90) < 1.0) {
        // transpose=clock, or cclock_flip when the matrix also mirrors.
        if (matrix[3] > 0) { result.quarterTurnsClockwise = 3; result.flipVertical = true; }
        else result.quarterTurnsClockwise = 1;
    } else if (std::abs(theta - 180) < 1.0) {
        result.flipHorizontal = matrix[0] < 0;
        result.flipVertical = matrix[4] < 0;
    } else if (std::abs(theta - 270) < 1.0) {
        // transpose=cclock, or clock_flip when the matrix also mirrors.
        if (matrix[3] < 0) { result.quarterTurnsClockwise = 1; result.flipVertical = true; }
        else result.quarterTurnsClockwise = 3;
    } else if (std::abs(theta) < 1.0) {
        result.flipVertical = matrix[4] < 0;
    }
    return result;
}

QImage upright(QImage image, const Orientation &o)
{
    if (o.quarterTurnsClockwise) image = image.transformed(QTransform().rotate(90.0 * o.quarterTurnsClockwise));
    if (o.flipHorizontal || o.flipVertical) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        image = image.flipped((o.flipHorizontal ? Qt::Horizontal : Qt::Orientations()) | (o.flipVertical ? Qt::Vertical : Qt::Orientations()));
#else
        image = image.mirrored(o.flipHorizontal, o.flipVertical);
#endif
    }
    return image;
}

int swsColorspace(AVColorSpace space, int height)
{
    switch (space) {
    case AVCOL_SPC_BT709: return SWS_CS_ITU709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
    case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
    case AVCOL_SPC_FCC: return SWS_CS_FCC;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return SWS_CS_BT2020;
    default:
        // Untagged: HD and larger are BT.709, smaller BT.601, as players and AVFoundation assume.
        return height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    }
}

bool isHdrTransfer(AVColorTransferCharacteristic transfer)
{
    return transfer == AVCOL_TRC_SMPTE2084 || transfer == AVCOL_TRC_ARIB_STD_B67;
}

} // namespace

struct MovieDecoder::Private {
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVStream *stream = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *decoded = nullptr;
    AVFrame *held = nullptr;   // the last frame decoded, for a later request at or after it
    SwsContext *sws = nullptr;
    int streamIndex = -1;
    int64_t start = 0, end = 0;   // the track's time range, in its time base
    Orientation orientation;
    bool heldValid = false, drained = false;

    ~Private()
    {
        sws_freeContext(sws);
        av_frame_free(&held);
        av_frame_free(&decoded);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
    }

    [[nodiscard]] double seconds(int64_t pts) const { return double(pts - start) * av_q2d(stream->time_base); }
    [[nodiscard]] int64_t pts(double seconds) const { return start + int64_t(std::llround(seconds / av_q2d(stream->time_base))); }
    [[nodiscard]] int64_t timestamp(const AVFrame *frame) const
    {
        return frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts != AV_NOPTS_VALUE ? frame->pts : start;
    }

    // The next decoded frame into `decoded`; false at the end of the track or on an error.
    bool next(int *error)
    {
        *error = 0;
        for (;;) {
            const int received = avcodec_receive_frame(codec, decoded);
            if (received == 0) return true;
            if (received == AVERROR_EOF) return false;
            if (received != AVERROR(EAGAIN)) { *error = received; return false; }
            if (drained) return false;
            const int read = av_read_frame(format, packet);
            if (read == AVERROR_EOF) { drained = true; avcodec_send_packet(codec, nullptr); continue; }
            if (read < 0) { *error = read; return false; }
            if (packet->stream_index == streamIndex) {
                const int sent = avcodec_send_packet(codec, packet);
                av_packet_unref(packet);
                if (sent < 0 && sent != AVERROR(EAGAIN)) { *error = sent; return false; }
            } else {
                av_packet_unref(packet);
            }
        }
    }

    void seek(int64_t target)
    {
        av_seek_frame(format, streamIndex, target, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codec);
        drained = false;
        heldValid = false;
    }

    QImage convert(const AVFrame *frame, QSize maximum)
    {
        const int width = frame->width, height = frame->height;
        if (width <= 0 || height <= 0) return {};
        QSize outSize(width, height);
        // A preview is decoded no larger than asked for, measured upright.
        if (maximum.isValid()) {
            const QSize uprightSize = orientation.swapsSides() ? QSize(height, width) : QSize(width, height);
            if (uprightSize.width() > maximum.width() || uprightSize.height() > maximum.height()) {
                const QSize fitted = uprightSize.scaled(maximum, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
                outSize = orientation.swapsSides() ? fitted.transposed() : fitted;
            }
        }
        const auto source = AVPixelFormat(frame->format);
        const bool hdr = isHdrTransfer(frame->color_trc);
        const AVPixelFormat target = hdr ? AV_PIX_FMT_RGB48LE : AV_PIX_FMT_RGBA;
        sws = sws_getCachedContext(sws, width, height, source, outSize.width(), outSize.height(), target,
                                   SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, nullptr, nullptr, nullptr);
        if (!sws) return {};
        // The matrix and range the track declares, read into full-range RGB.
        const int *coefficients = sws_getCoefficients(swsColorspace(frame->colorspace, height));
        const bool full = frame->color_range == AVCOL_RANGE_JPEG
                          || source == AV_PIX_FMT_YUVJ420P || source == AV_PIX_FMT_YUVJ422P || source == AV_PIX_FMT_YUVJ444P;
        sws_setColorspaceDetails(sws, coefficients, full ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
        QImage image;
        if (!hdr) {
            image = QImage(outSize, QImage::Format_RGBA8888);
            uint8_t *planes[4] = {image.bits(), nullptr, nullptr, nullptr};
            int strides[4] = {int(image.bytesPerLine()), 0, 0, 0};
            sws_scale(sws, frame->data, frame->linesize, 0, height, planes, strides);
            // Movies are opaque.
            image = std::move(image).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        } else {
            std::vector<quint16> rgb(size_t(outSize.width()) * outSize.height() * 3);
            uint8_t *planes[4] = {reinterpret_cast<uint8_t *>(rgb.data()), nullptr, nullptr, nullptr};
            int strides[4] = {outSize.width() * 6, 0, 0, 0};
            sws_scale(sws, frame->data, frame->linesize, 0, height, planes, strides);
            image = MovieDecoder::toneMapBt2020(rgb.data(), outSize.width(), outSize.height(), outSize.width() * 3,
                                                frame->color_trc == AVCOL_TRC_ARIB_STD_B67);
        }
        return upright(std::move(image), orientation);
    }
};

MovieDecoder::MovieDecoder() : d_(std::make_unique<Private>()) {}
MovieDecoder::~MovieDecoder() = default;

bool MovieDecoder::isMovieFile(const QString &path)
{
    static const QStringList extensions{QStringLiteral("mov"), QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("qt")};
    return extensions.contains(QFileInfo(path).suffix().toLower());
}

std::unique_ptr<MovieDecoder> MovieDecoder::open(const QString &path, QString *error)
{
    const auto fail = [&](const QString &message) { if (error) *error = message; return std::unique_ptr<MovieDecoder>(); };
    std::unique_ptr<MovieDecoder> decoder(new MovieDecoder);
    Private &d = *decoder->d_;
    const QByteArray file = QFile::encodeName(path);
    int status = avformat_open_input(&d.format, file.constData(), nullptr, nullptr);
    if (status < 0) return fail(QObject::tr("The movie could not be opened (%1).").arg(averror(status)));
    status = avformat_find_stream_info(d.format, nullptr);
    if (status < 0) return fail(QObject::tr("The movie could not be read (%1).").arg(averror(status)));
    const AVCodec *codec = nullptr;
    d.streamIndex = av_find_best_stream(d.format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (d.streamIndex < 0 || !codec) return fail(QObject::tr("The file has no video that can be played."));
    d.stream = d.format->streams[d.streamIndex];
    d.codec = avcodec_alloc_context3(codec);
    if (!d.codec || avcodec_parameters_to_context(d.codec, d.stream->codecpar) < 0) return fail(QObject::tr("The movie's video could not be decoded."));
    d.codec->thread_count = 0;
    d.codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    status = avcodec_open2(d.codec, codec, nullptr);
    if (status < 0) return fail(QObject::tr("The movie's video could not be decoded (%1).").arg(averror(status)));
    d.packet = av_packet_alloc(); d.decoded = av_frame_alloc(); d.held = av_frame_alloc();
    if (!d.packet || !d.decoded || !d.held) return fail(QObject::tr("Not enough memory to read the movie."));

    // The track's time range (mac: the video track's timeRange): its own start and duration, else the container's.
    const double base = av_q2d(d.stream->time_base);
    d.start = d.stream->start_time != AV_NOPTS_VALUE ? d.stream->start_time : 0;
    int64_t length = d.stream->duration;
    if (length == AV_NOPTS_VALUE || length <= 0) {
        if (d.format->duration > 0) length = int64_t(std::llround(d.format->duration / double(AV_TIME_BASE) / base));
    }
    if (length == AV_NOPTS_VALUE || length <= 0) return fail(QObject::tr("The movie's length could not be read."));
    d.end = d.start + length;
    const AVPacketSideData *side = av_packet_side_data_get(d.stream->codecpar->coded_side_data, d.stream->codecpar->nb_coded_side_data,
                                                           AV_PKT_DATA_DISPLAYMATRIX);
    d.orientation = orientation(side && side->size >= 9 * int(sizeof(int32_t)) ? reinterpret_cast<const int32_t *>(side->data) : nullptr);

    Info &info = decoder->info_;
    const QSize coded(d.codec->width, d.codec->height);
    info.size = d.orientation.swapsSides() ? coded.transposed() : coded;
    info.duration = double(length) * base;
    const AVRational rate = d.stream->avg_frame_rate.num > 0 ? d.stream->avg_frame_rate : d.stream->r_frame_rate;
    info.frameRate = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 0;
    info.hasAudio = av_find_best_stream(d.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) >= 0;
    info.hdr = isHdrTransfer(d.codec->color_trc);
    if (info.size.isEmpty() || !std::isfinite(info.duration) || info.duration <= 0) return fail(QObject::tr("The movie's video could not be read."));
    return decoder;
}

QImage MovieDecoder::frame(double seconds, QString *error, QSize maximum)
{
    Private &d = *d_;
    if (error) error->clear();
    // The end of the track is exclusive: all the way right is its last frame (mac frameTime: end − 1/max(timescale,
    // 60000) seconds).
    const double base = av_q2d(d.stream->time_base);
    const int64_t lastTick = std::max<int64_t>(1, int64_t(std::ceil((1.0 / std::max(1.0 / base, 60000.0)) / base)));
    const int64_t target = std::clamp(d.pts(std::isfinite(seconds) ? seconds : 0), d.start, d.end - lastTick);
    // Decoding on from the frame last shown, when the one wanted is a little way ahead (playback, a scrub to the right);
    // otherwise from the keyframe before it.
    const bool ahead = d.heldValid && d.timestamp(d.held) <= target && double(target - d.timestamp(d.held)) * base < 1.0;
    if (!ahead) d.seek(target);
    for (;;) {
        int status = 0;
        if (!d.next(&status)) {
            if (status < 0 && !d.heldValid) {
                if (error) *error = QObject::tr("The movie frame could not be decoded (%1).").arg(averror(status));
                return {};
            }
            break;   // the end of the track: the last frame decoded is the one shown
        }
        if (d.timestamp(d.decoded) > target && d.heldValid) {
            // Keep it for the next request; the one before is shown now.
            QImage shown = d.convert(d.held, maximum);
            lastFrameTime_ = d.seconds(d.timestamp(d.held));
            av_frame_unref(d.held); av_frame_move_ref(d.held, d.decoded);
            if (shown.isNull() && error) *error = QObject::tr("The movie frame could not be converted.");
            return shown;
        }
        av_frame_unref(d.held); av_frame_move_ref(d.held, d.decoded); d.heldValid = true;
        if (d.timestamp(d.held) > target) break;   // the first frame starts after the time asked for: it's the one shown
    }
    if (!d.heldValid) { if (error) *error = QObject::tr("The movie has no frame at that time."); return {}; }
    lastFrameTime_ = d.seconds(d.timestamp(d.held));
    QImage shown = d.convert(d.held, maximum);
    // What was held has been shown and the decoder may have reached the end: start the next request afresh.
    d.heldValid = false;
    d.seek(target);
    if (shown.isNull() && error) *error = QObject::tr("The movie frame could not be converted.");
    return shown;
}

// HDR (BT.2100 PQ or HLG) to SDR sRGB, as macOS shows an HDR movie's frames in an SDR document: to linear light, into
// BT.709's primaries, reference white (203 nits) at 1, highlights rolled off smoothly above it (a Reinhard shoulder),
// then the sRGB curve.
QImage MovieDecoder::toneMapBt2020(const quint16 *rgb48, int width, int height, qsizetype stride, bool hlg)
{
    QImage image(width, height, QImage::Format_RGBA8888_Premultiplied);
    if (image.isNull()) return image;
    constexpr double referenceWhite = 203.0;
    const auto pq = [](double e) {
        constexpr double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
        const double p = std::pow(std::max(e, 0.0), 1 / m2);
        return 10000.0 * std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1 / m1);   // nits
    };
    const auto hlgInverse = [](double e) {
        constexpr double a = 0.17883277, b = 0.28466892, c = 0.55991073;
        return e <= .5 ? e * e / 3 : (std::exp((e - c) / a) + b) / 12;   // scene light, 0–1
    };
    // Each 16-bit code's light, worked out once.
    std::vector<float> light(65536);
    for (int code = 0; code < 65536; ++code) {
        const double e = code / 65535.0;
        // HLG shown on a 1000-nit display (BT.2100's OOTF, system gamma 1.2); PQ is absolute.
        light[size_t(code)] = float(hlg ? 1000.0 * std::pow(hlgInverse(e), 1.2) / referenceWhite : pq(e) / referenceWhite);
    }
    // BT.2020 to BT.709 primaries, in linear light.
    constexpr double m[3][3] = {{1.6605, -0.5876, -0.0728}, {-0.1246, 1.1329, -0.0083}, {-0.0182, -0.1006, 1.1187}};
    std::vector<uchar> encode(4096);
    for (int i = 0; i < 4096; ++i) {
        const double v = i / 4095.0;
        encode[size_t(i)] = uchar(std::lround(255.0 * (v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - .055)));
    }
#pragma omp parallel for schedule(static)
    for (int y = 0; y < height; ++y) {
        const quint16 *in = rgb48 + qsizetype(y) * stride;
        uchar *out = image.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const double r = light[in[x * 3]], g = light[in[x * 3 + 1]], b = light[in[x * 3 + 2]];
            double c[3] = {m[0][0] * r + m[0][1] * g + m[0][2] * b, m[1][0] * r + m[1][1] * g + m[1][2] * b, m[2][0] * r + m[2][1] * g + m[2][2] * b};
            // A shoulder on the brightness, so colors keep their hue as they roll off.
            const double peak = std::max({c[0], c[1], c[2], 0.0});
            const double scale = peak > .8 ? (.8 + (peak - .8) / (1 + (peak - .8) / .2)) / peak : 1.0;
            for (int k = 0; k < 3; ++k) {
                const double v = std::clamp(c[k] * scale, 0.0, 1.0);
                out[x * 4 + k] = encode[size_t(std::lround(v * 4095))];
            }
            out[x * 4 + 3] = 255;
        }
    }
    return image;
}

namespace MovieBookmark {

namespace {
constexpr quint32 headerSize = 48;
constexpr quint32 typeString = 0x0101, typeArray = 0x0601;
constexpr quint32 keyPath = 0x1004;

void put32(QByteArray &data, quint32 value) { const quint32 le = qToLittleEndian(value); data.append(reinterpret_cast<const char *>(&le), 4); }
quint32 get32(const QByteArray &data, qsizetype offset) { return offset >= 0 && offset + 4 <= data.size() ? qFromLittleEndian<quint32>(data.constData() + offset) : 0; }
void align(QByteArray &data) { while (data.size() % 4) data.append('\0'); }
} // namespace

QByteArray make(const QString &path)
{
    const QStringList components = QDir::cleanPath(QFileInfo(path).absoluteFilePath()).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    // The body, offsets measured from its start: where the table of contents is, the path's components, the array
    // naming them, then the table itself.
    QByteArray body;
    put32(body, 0);
    QVector<quint32> offsets;
    for (const QString &component : components) {
        const QByteArray utf8 = component.toUtf8();
        offsets.push_back(quint32(body.size()));
        put32(body, quint32(utf8.size())); put32(body, typeString); body.append(utf8); align(body);
    }
    const quint32 array = quint32(body.size());
    put32(body, quint32(offsets.size() * 4)); put32(body, typeArray);
    for (quint32 offset : offsets) put32(body, offset);
    const quint32 toc = quint32(body.size());
    put32(body, 20 + 12 - 8); put32(body, 0xfffffffeu); put32(body, 1); put32(body, 0); put32(body, 1);
    put32(body, keyPath); put32(body, array); put32(body, 0);
    const quint32 tocLE = qToLittleEndian(toc);
    std::memcpy(body.data(), &tocLE, 4);
    QByteArray result("book", 4);
    put32(result, headerSize + quint32(body.size()));
    put32(result, 0x10040000);
    put32(result, headerSize);
    result.append(QByteArray(int(headerSize) - result.size(), '\0'));
    result.append(body);
    return result;
}

QString path(const QByteArray &bookmark)
{
    if (bookmark.size() < int(headerSize) + 4 || !bookmark.startsWith("book")) return {};
    const quint32 header = get32(bookmark, 12);
    if (header < 16 || header >= quint32(bookmark.size())) return {};
    const auto item = [&](quint32 offset, quint32 *type, QByteArray *payload) {
        const qsizetype at = qsizetype(header) + offset;
        const quint32 length = get32(bookmark, at);
        *type = get32(bookmark, at + 4);
        if (at < 0 || at + 8 + qsizetype(length) > bookmark.size()) return false;
        *payload = bookmark.mid(at + 8, length);
        return true;
    };
    // Each table of contents in turn, looking for the path's components.
    quint32 toc = get32(bookmark, header);
    for (int guard = 0; toc && guard < 16; ++guard) {
        const qsizetype base = qsizetype(header) + toc;
        if (get32(bookmark, base + 4) != 0xfffffffeu) return {};
        const quint32 count = get32(bookmark, base + 16);
        for (quint32 i = 0; i < count && i < 4096; ++i) {
            const qsizetype entry = base + 20 + qsizetype(i) * 12;
            if (get32(bookmark, entry) != keyPath) continue;
            quint32 type = 0; QByteArray list;
            if (!item(get32(bookmark, entry + 4), &type, &list) || type != typeArray) return {};
            QStringList components;
            for (qsizetype at = 0; at + 4 <= list.size(); at += 4) {
                QByteArray text;
                if (!item(get32(list, at), &type, &text) || (type & 0xff00) != 0x0100) return {};
                components.push_back(QString::fromUtf8(text));
            }
            return components.isEmpty() ? QString() : QLatin1Char('/') + components.join(QLatin1Char('/'));
        }
        toc = get32(bookmark, base + 12);
    }
    return {};
}

} // namespace MovieBookmark

QString formatMovieTime(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0) seconds = 0;
    // Hundredths, rounded half to even as Duration's formatter rounds.
    const double hundredths = std::nearbyint(seconds * 100);
    const qint64 total = qint64(hundredths);
    const qint64 minutes = total / 6000, rest = total % 6000;
    return QStringLiteral("%1:%2.%3").arg(minutes).arg(rest / 100, 2, 10, QLatin1Char('0')).arg(rest % 100, 2, 10, QLatin1Char('0'));
}

} // namespace compositor
