#pragma once
#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QMap>
#include <QVector>
#include <memory>

namespace compositor {
struct GifSource {
    QByteArray data;
    QSize size;
    QVector<int> delays; // milliseconds, minimum 20
    QVector<QImage> thumbnails;
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
