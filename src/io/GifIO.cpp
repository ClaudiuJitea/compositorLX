#include "io/GifIO.h"
#include "rendering/LayerRenderer.h"
#include "rendering/DeepImage.h"
#include <QBuffer>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QPainter>
#include <QSaveFile>
#include <QSet>
#include <gif_lib.h>
#include <algorithm>
#include <cmath>

namespace compositor {
int GifAnimation::delay(int identity) const
{
    if (sameDelay) return uniformDelay;
    if (delays.contains(identity)) return delays.value(identity);
    for (const auto &f : frames) if (f.index == identity && source && f.sourceIndex < source->delays.size()) return source->delays[f.sourceIndex];
    return 100;
}
QImage GifAnimation::frame(int identity) const
{
    if (pixels.contains(identity)) return pixels.value(identity);
    if (!source) return {};
    const auto it = std::find_if(frames.cbegin(), frames.cend(), [identity](const auto &f) { return f.index == identity; });
    if (it == frames.cend()) return {};
    QBuffer buffer; buffer.setData(source->data); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "gif");
    QImage result;
    for (int i = 0; i <= it->sourceIndex; ++i) { result = reader.read(); if (result.isNull()) return {}; }
    if (!geometry.isEmpty()) {
        QImage transformed(QSize(geometry.value("width").toInt(), geometry.value("height").toInt()), QImage::Format_RGBA8888_Premultiplied);
        if (transformed.isNull()) return {};
        transformed.fill(Qt::transparent);
        QTransform map(geometry.value("a").toDouble(1), geometry.value("b").toDouble(), geometry.value("c").toDouble(),
                       geometry.value("d").toDouble(1), geometry.value("tx").toDouble(), geometry.value("ty").toDouble());
        QPainter p(&transformed);
        p.setRenderHint(QPainter::SmoothPixmapTransform, geometry.value("sampling").toString() != "nearest");
        p.scale(transformed.width(), transformed.height()); p.setTransform(map, true);
        p.drawImage(QRectF(0, 0, 1, 1), result); p.end(); result = transformed;
    }
    return result.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
}
bool GifAnimation::valid() const
{
    if (!source || frames.isEmpty() || frames.size() > 4096 || uniformDelay < 20 || uniformDelay > 655350) return false;
    QSet<int> ids;
    for (const auto &f : frames) {
        if (f.index < 0 || f.index >= 8192 || f.sourceIndex < 0 || f.sourceIndex >= source->delays.size() || ids.contains(f.index)
            || (f.index < source->delays.size() && f.index != f.sourceIndex)) return false;
        ids.insert(f.index);
    }
    if (!ids.contains(selected)) return false;
    for (auto it = pixels.begin(); it != pixels.end(); ++it) if (!ids.contains(it.key()) || it.value().isNull()) return false;
    for (auto it = delays.begin(); it != delays.end(); ++it) if (!ids.contains(it.key()) || it.value() < 20 || it.value() > 655350) return false;
    if (!geometry.isEmpty()) {
        const double w = geometry.value("width").toDouble(), h = geometry.value("height").toDouble();
        if (w < 1 || h < 1 || w != std::floor(w) || h != std::floor(h) || w > DocumentLimits::maxSide || h > DocumentLimits::maxSide || w * h > DocumentLimits::maxSurfacePixels) return false;
        for (const char *key : {"a", "b", "c", "d", "tx", "ty"}) {
            const auto v = geometry.value(QLatin1String(key));
            if (!v.isDouble() || !std::isfinite(v.toDouble()) || std::abs(v.toDouble()) > 1000000) return false;
        }
        if (std::abs(geometry.value("a").toDouble() * geometry.value("d").toDouble() - geometry.value("b").toDouble() * geometry.value("c").toDouble()) <= 1e-15) return false;
    }
    return true;
}
std::optional<GifAnimation> GifIO::read(const QByteArray &data, QString *error)
{
    const auto fail = [&](const QString &message) -> std::optional<GifAnimation> { if (error) *error = message; return {}; };
    if (data.size() > 512LL * 1024 * 1024 || (!data.startsWith("GIF87a") && !data.startsWith("GIF89a"))) return fail(QStringLiteral("Invalid or oversized GIF."));
    QBuffer buffer; buffer.setData(data); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "gif");
    const QSize size = reader.size();
    if (size.isEmpty() || size.width() > DocumentLimits::maxSide || size.height() > DocumentLimits::maxSide || qint64(size.width()) * size.height() > DocumentLimits::maxSurfacePixels)
        return fail(QStringLiteral("GIF dimensions exceed the image limit."));
    auto source = std::make_shared<GifSource>(); source->data = data; source->size = size;
    GifAnimation animation; animation.source = source;
    // Timeline thumbnails up to 96 px, smaller for long GIFs so they stay within 16 MP together (macOS GIFDecoder).
    const int count = std::max(1, reader.imageCount());
    const int thumbnailSide = std::clamp(int(std::sqrt(16'000'000.0 / count)), 1, 96);
    while (reader.canRead()) {
        if (animation.frames.size() >= 4096) return fail(QStringLiteral("A GIF may contain at most 4096 frames."));
        const QImage image = reader.read();
        if (image.isNull() || image.size() != size) return fail(QStringLiteral("A GIF frame could not be decoded."));
        const int index = animation.frames.size();
        animation.frames.append({index, index});
        source->delays.append(std::clamp(reader.nextImageDelay(), 20, 655350));
        source->thumbnails.append(image.scaled(thumbnailSide, thumbnailSide, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    if (animation.frames.isEmpty()) return fail(QStringLiteral("The GIF has no readable frames."));
    return animation;
}
std::optional<GifAnimation> GifIO::readFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 512LL * 1024 * 1024) { if (error) *error = QStringLiteral("The GIF could not be read or is too large."); return {}; }
    return read(file.readAll(), error);
}
QJsonObject GifIO::metadata(const GifAnimation &a, const QString &id)
{
    QJsonObject obj{{"sourceFile", id + ".gif"}, {"selectedFrameIndex", a.selected}};
    QJsonArray order, deleted, edits, delays;
    QSet<int> originals;
    for (const auto &f : a.frames) { order.append(QJsonObject{{"index", f.index}, {"sourceIndex", f.sourceIndex}}); if (f.index < a.source->delays.size()) originals.insert(f.index); }
    for (int i = 0; i < a.source->delays.size(); ++i) if (!originals.contains(i)) deleted.append(i);
    for (auto it = a.pixels.begin(); it != a.pixels.end(); ++it) edits.append(QJsonObject{{"index", it.key()}, {"imageFile", id + QStringLiteral(".frame-%1.png").arg(it.key())}});
    for (auto it = a.delays.begin(); it != a.delays.end(); ++it) delays.append(QJsonObject{{"index", it.key()}, {"duration", it.value() / 1000.0}});
    obj.insert("frameOrder", order); obj.insert("deletedFrameIndices", deleted); obj.insert("framePixels", edits);
    obj.insert("timing", QJsonObject{{"mode", a.sameDelay ? "Same for all" : "Frame delay"}, {"uniformDelay", a.uniformDelay / 1000.0}, {"frameDelays", delays}});
    if (!a.geometry.isEmpty()) obj.insert("geometry", a.geometry);
    return obj;
}
bool GifIO::restoreMetadata(GifAnimation &a, const QJsonObject &obj, QString *error)
{
    const auto fail = [&] { if (error) *error = QStringLiteral("Invalid GIF timeline metadata."); return false; };
    const auto integer = [](const QJsonValue &v) { return v.isDouble() && v.toDouble() == v.toInt(-1) ? v.toInt(-1) : -1; };
    a.selected = obj.contains("selectedFrameIndex") ? integer(obj.value("selectedFrameIndex")) : 0;
    if (obj.contains("geometry")) { if (!obj.value("geometry").isObject()) return fail(); a.geometry = obj.value("geometry").toObject(); }
    QSet<int> deleted;
    if (obj.contains("deletedFrameIndices") && !obj.value("deletedFrameIndices").isArray()) return fail();
    for (const auto &v : obj.value("deletedFrameIndices").toArray()) {
        const int index = integer(v);
        if (index < 0 || index >= a.source->delays.size() || deleted.contains(index)) return fail();
        deleted.insert(index);
    }
    if (obj.contains("frameOrder")) {
        if (!obj.value("frameOrder").isArray()) return fail();
        a.frames.clear();
        for (const auto &v : obj.value("frameOrder").toArray()) a.frames.append({integer(v.toObject().value("index")), integer(v.toObject().value("sourceIndex"))});
        QSet<int> kept;
        for (const auto &f : a.frames) if (f.index < a.source->delays.size()) kept.insert(f.index);
        for (int i = 0; i < a.source->delays.size(); ++i) if (kept.contains(i) == deleted.contains(i)) return fail();
    } else a.frames.erase(std::remove_if(a.frames.begin(), a.frames.end(), [&](const auto &f) { return deleted.contains(f.index); }), a.frames.end());
    if (obj.contains("timing")) {
        if (!obj.value("timing").isObject()) return fail();
        const auto t = obj.value("timing").toObject();
        const QString mode = t.value("mode").toString("Frame delay");
        if (mode != "Frame delay" && mode != "Same for all") return fail();
        a.sameDelay = mode == "Same for all";
        const auto duration = [](const QJsonValue &v) { const double d = v.toDouble(-1); return std::isfinite(d) && d >= .02 && d <= 655.35 ? qRound(d * 1000) : -1; };
        a.uniformDelay = t.contains("uniformDelay") ? duration(t.value("uniformDelay")) : 100;
        if (t.contains("frameDelays") && !t.value("frameDelays").isArray()) return fail();
        for (const auto &v : t.value("frameDelays").toArray()) {
            const auto entry = v.toObject(); const int index = integer(entry.value("index"));
            if (a.delays.contains(index)) return fail();
            a.delays.insert(index, duration(entry.value("duration")));
        }
    }
    return a.valid() || fail();
}

bool GifIO::write(const Document &document, const QString &path, const QSize &size, bool loop, bool transparency, QString *error)
{
    const auto fail = [&](const QString &s) { if (error) *error = s; return false; };
    if (size.isEmpty() || size.width() > DocumentLimits::maxSide || size.height() > DocumentLimits::maxSide || qint64(size.width()) * size.height() > DocumentLimits::maxSurfacePixels) return fail(QStringLiteral("Invalid GIF export size."));
    struct Track { int layer; QVector<int> starts; int end = 0; };
    QVector<Track> tracks; QSet<int> boundaries{0}; int end = 0;
    for (int i = 0; i < document.layers.size(); ++i) {
        const auto &layer = document.layers[i];
        if (!layer.gif || !LayerRenderer::effectivelyVisible(document, layer)) continue;
        if (!layer.gif->valid()) return fail(QStringLiteral("Invalid GIF timeline."));
        Track track{i, {}, 0};
        for (const auto &f : layer.gif->frames) { track.starts.append(track.end); boundaries.insert(track.end); track.end += std::max(2, qRound(layer.gif->delay(f.index) / 10.0)); }
        end = std::max(end, track.end); tracks.append(track);
    }
    if (tracks.isEmpty()) end = 10;
    boundaries.insert(end);
    QList<int> sorted = boundaries.values(); std::sort(sorted.begin(), sorted.end());
    QVector<int> starts{0};
    for (int b : sorted) if (b < end && b - starts.last() >= 2 && end - b >= 2) starts.append(b);
    // GIF's 16-bit delay field also limits a long hold after merging different timelines.
    for (int i = 0; i < starts.size(); ++i) {
        const int next = i + 1 < starts.size() ? starts[i + 1] : end;
        if (next - starts[i] > 65535) starts.insert(i + 1, starts[i] + 65533);
        if (starts.size() > 4096) return fail(QStringLiteral("The composed animation exceeds 4096 frames."));
    }
    QSaveFile file(path); if (!file.open(QIODevice::WriteOnly)) return fail(file.errorString());
    int code = 0;
    auto *gif = EGifOpen(&file, [](GifFileType *g, const GifByteType *data, int count) -> int { return int(static_cast<QSaveFile *>(g->UserData)->write(reinterpret_cast<const char *>(data), count)); }, &code);
    if (!gif) return fail(QStringLiteral("Could not initialize GIF encoder."));
    bool ok = true;
    EGifSetGifVersion(gif, true);
    ok = EGifPutScreenDesc(gif, size.width(), size.height(), 8, 0, nullptr) != GIF_ERROR;
    if (loop && ok) {
        const unsigned char loops[] = {1, 0, 0};
        ok = EGifPutExtensionLeader(gif, APPLICATION_EXT_FUNC_CODE) != GIF_ERROR &&
             EGifPutExtensionBlock(gif, 11, "NETSCAPE2.0") != GIF_ERROR && EGifPutExtensionBlock(gif, 3, loops) != GIF_ERROR && EGifPutExtensionTrailer(gif) != GIF_ERROR;
    }
    for (int frame = 0; ok && frame < starts.size(); ++frame) {
        Document current = document;
        const int sample = frame + 1 == starts.size() ? end - 1 : starts[frame];
        for (const auto &track : tracks) {
            auto &layer = current.layers[track.layer];
            const int pos = std::clamp(int(std::upper_bound(track.starts.begin(), track.starts.end(), sample) - track.starts.begin()) - 1, 0, int(track.starts.size()) - 1);
            layer.image = layer.gif->frame(layer.gif->frames[pos].index);
            if (layer.image.isNull()) { ok = false; break; }
        }
        if (!ok) break;
        QImage image = DeepImage::dithered(LayerRenderer::flattened(current)).scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (!transparency) { QImage flat(size, QImage::Format_RGB32); flat.fill(Qt::white); QPainter p(&flat); p.drawImage(0, 0, image); p.end(); image = flat; }
        image = image.convertToFormat(QImage::Format_RGBA8888);
        const int count = size.width() * size.height();
        std::vector<GifByteType> r(count), g(count), b(count), indices(count);
        for (int y = 0; y < size.height(); ++y) for (int x = 0; x < size.width(); ++x) {
            const auto *p = image.constScanLine(y) + x * 4; const int i = y * size.width() + x;
            r[i] = p[0]; g[i] = p[1]; b[i] = p[2];
        }
        // Reserve one palette entry for transparent pixels; full frames dispose to background.
        GifColorType colors[256]{}; int colorCount = transparency ? 128 : 256;
        if (GifQuantizeBuffer(size.width(), size.height(), &colorCount, r.data(), g.data(), b.data(), indices.data(), colors) == GIF_ERROR) { ok = false; break; }
        const int transparent = transparency ? 255 : NO_TRANSPARENT_COLOR;
        if (transparency) for (int y = 0; y < size.height(); ++y) for (int x = 0; x < size.width(); ++x)
            if (image.constScanLine(y)[x * 4 + 3] < 128) indices[y * size.width() + x] = 255;
        ColorMapObject *map = GifMakeMapObject(256, colors);
        const int next = frame + 1 < starts.size() ? starts[frame + 1] : end;
        GraphicsControlBlock control{DISPOSE_BACKGROUND, false, next - starts[frame], transparent};
        GifByteType extension[4]; EGifGCBToExtension(&control, extension);
        ok = map && EGifPutExtension(gif, GRAPHICS_EXT_FUNC_CODE, 4, extension) != GIF_ERROR && EGifPutImageDesc(gif, 0, 0, size.width(), size.height(), false, map) != GIF_ERROR;
        for (int y = 0; ok && y < size.height(); ++y) ok = EGifPutLine(gif, indices.data() + y * size.width(), size.width()) != GIF_ERROR;
        GifFreeMapObject(map);
    }
    if (EGifCloseFile(gif, &code) == GIF_ERROR) ok = false;
    if (!ok) { file.cancelWriting(); return fail(QStringLiteral("GIF encoding failed.")); }
    return file.commit() || fail(file.errorString());
}
}
