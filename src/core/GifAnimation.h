#pragma once
#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QMap>
#include <QVector>
#include <memory>
#include <mutex>

namespace compositor {
struct GifFrameReader;
struct GifSource {
    QByteArray data;
    QSize size;
    QVector<int> delays; // milliseconds, minimum 20
    QVector<QImage> thumbnails;
    // One reader kept open for every frame read (macOS GIFSource.withReader). A GIF frame is often only the part that
    // changed, so a new reader rebuilds it from the first frame on each read, too slow to play, while this one carries
    // on from the frame it read last.
    mutable std::shared_ptr<GifFrameReader> reader;
    mutable std::mutex readerLock;
};
struct GifFrame {
    int index = 0, sourceIndex = 0;
    bool operator==(const GifFrame &) const = default;
};
struct GifAnimation {
    std::shared_ptr<const GifSource> source;
    QVector<GifFrame> frames;
    int selected = 0;
    QMap<int, QImage> pixels;
    QMap<int, int> delays;
    int uniformDelay = 100;
    bool sameDelay = false;
    QJsonObject geometry;
    int delay(int identity) const;
    QImage frame(int identity) const;
    bool valid() const;
    bool operator==(const GifAnimation &) const = default;
};
}
