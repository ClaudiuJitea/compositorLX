#pragma once

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>

#include <memory>
#include <optional>

namespace compositor {

// Movie layers (mac b469ec1 MovieFramePicker): the movie's video track read through FFmpeg, as AVFoundation reads it
// on macOS. A frame is the one shown at a time from the track's start, decoded exactly (AVAssetImageGenerator with no
// tolerance), turned upright by the track's display matrix (appliesPreferredTrackTransform) and converted to sRGB; an
// HDR track (PQ or HLG) is tone-mapped to SDR, so a paused frame and a playing one look the same (mac 590c386, c0d4446).
class MovieDecoder {
public:
    struct Info {
        QSize size;          // upright, after the track's rotation
        double duration = 0; // the video track's length in seconds
        double frameRate = 0;
        bool hasAudio = false;
        bool hdr = false;
    };

    ~MovieDecoder();
    MovieDecoder(const MovieDecoder &) = delete;
    MovieDecoder &operator=(const MovieDecoder &) = delete;

    // Opens `path`'s first video track; nullptr, with `error` in words for the user, when there is none to read.
    static std::unique_ptr<MovieDecoder> open(const QString &path, QString *error = nullptr);
    // Whether `path` names a movie the importer opens as a movie layer (mac: QuickTime and MPEG-4 movies).
    static bool isMovieFile(const QString &path);

    [[nodiscard]] const Info &info() const { return info_; }

    // The frame shown `seconds` after the track's start. The track's end is exclusive: asking for it, or past it, gives
    // the last frame (mac MovieFramePicker.frameTime). `maximum`, when set, is the largest size wanted (the frame is
    // reduced to fit, keeping its shape), for previews. A null image, with `error` set, when it can't be decoded.
    QImage frame(double seconds, QString *error = nullptr, QSize maximum = {});

    // Where the last frame `frame` returned starts, in seconds from the track's start.
    [[nodiscard]] double lastFrameTime() const { return lastFrameTime_; }

    // A decoded picture in the movie's own pixels as 8-bit sRGB, upright: what `frame` returns, exposed for tests of the
    // HDR conversion. `pq`/`hlg` choose the transfer of a 16-bit BT.2020 RGB input.
    static QImage toneMapBt2020(const quint16 *rgb48, int width, int height, qsizetype stride, bool hlg);

private:
    MovieDecoder();
    struct Private;
    std::unique_ptr<Private> d_;
    Info info_;
    double lastFrameTime_ = 0;
};

// Where a movie layer's movie is (mac MovieLink.bookmark). macOS stores a security-scoped bookmark; it's read here for
// the path it names. LX writes a bookmark in the same "book" layout naming the file, which macOS, unable to open it
// with a sandbox scope, treats as a movie to relink.
namespace MovieBookmark {
[[nodiscard]] QByteArray make(const QString &path);
// The absolute path the bookmark names, or empty when it names none.
[[nodiscard]] QString path(const QByteArray &bookmark);
} // namespace MovieBookmark

// "1:05.25", as macOS formats the playhead (Duration .time(pattern: .minuteSecond(padMinuteToLength: 1,
// fractionalSecondsLength: 2))).
[[nodiscard]] QString formatMovieTime(double seconds);

} // namespace compositor
