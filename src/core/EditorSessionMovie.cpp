#include "core/EditorSession.h"
#include "core/DocumentLimits.h"
#include "io/MovieDecoder.h"
#include "rendering/DeepImage.h"

#include <QCoreApplication>
#include <QFileInfo>

#include <cmath>

namespace compositor {

namespace {

// The document's pixels so far, which the budget limits (mac usedPixels: each layer's image and a GIF's edited frames).
qint64 usedPixels(const Document &document)
{
    qint64 used = 0;
    for (const Layer &layer : document.layers) {
        used += qint64(layer.image.width()) * layer.image.height();
        if (layer.gif) for (const QImage &frame : layer.gif->pixels) used += qint64(frame.width()) * frame.height();
    }
    return used;
}

QString tooLarge()
{
    return QCoreApplication::translate("EditorSession", "The movie is too large for this document.");
}

// The movie's first frame and its link, checked against what the document can still hold (mac MovieFramePicker.open).
bool openMovie(const QString &path, qint64 remaining, MovieLink *link, QImage *frame, QString *error)
{
    std::unique_ptr<MovieDecoder> decoder = MovieDecoder::open(path, error);
    if (!decoder) return false;
    const QSize size = decoder->info().size;
    if (std::max(size.width(), size.height()) > DocumentLimits::maxSide || qint64(size.width()) * size.height() > remaining) {
        if (error) *error = tooLarge();
        return false;
    }
    *frame = decoder->frame(0, error);
    if (frame->isNull()) return false;
    if (std::max(frame->width(), frame->height()) > DocumentLimits::maxSide || qint64(frame->width()) * frame->height() > remaining) {
        if (error) *error = tooLarge();
        return false;
    }
    *link = MovieLink{MovieBookmark::make(path), decoder->info().duration, 0};
    return true;
}

} // namespace

QString EditorSession::moviePath(const MovieLink &link)
{
    return MovieBookmark::path(link.bookmark);
}

bool EditorSession::insertMovie(const QString &path, const std::optional<QPointF> &center, QString *error)
{
    const qint64 remaining = DocumentLimits::documentPixelBudget() - (document_ ? usedPixels(*document_) : 0);
    MovieLink link;
    QImage frame;
    if (!openMovie(path, remaining, &link, &frame, error)) return false;
    beginEdit(QStringLiteral("Import Movie"));
    const bool inserted = insertImage(frame, QFileInfo(path).completeBaseName(), center);
    if (inserted) {
        activeLayer()->movie = link;
        document_->formatVersion = std::max(document_->formatVersion, 23);
    } else if (error) *error = tooLarge();
    endEdit();
    return inserted;
}

bool EditorSession::setMovieFrame(const QUuid &layerId, const QImage &frame, double seconds)
{
    // Only while the movie layer is still the one being edited (mac: seek's guard on the active layer).
    const int index = indexOf(layerId);
    if (!document_ || index < 0 || frame.isNull() || document_->activeLayerId != layerId) return false;
    const Layer &layer = document_->layers.at(index);
    if (!layer.movie || brush_) return false;
    beginEdit(QStringLiteral("Select Movie Frame"));
    Layer &edited = document_->layers[index];
    edited.image = DeepImage::canonical(frame);
    edited.movie->seconds = std::clamp(std::isfinite(seconds) ? seconds : 0.0, 0.0, edited.movie->duration);
    endEdit();
    return true;
}

bool EditorSession::grabMovieFrame(const QUuid &layerId, QString *error)
{
    const int index = indexOf(layerId);
    if (!document_ || index < 0) return false;
    const Layer &movie = document_->layers.at(index);
    if (!movie.movie || movie.image.isNull() || brush_) return false;
    const qint64 pixels = qint64(movie.image.width()) * movie.image.height();
    if (document_->layers.size() >= 10000 || pixels > DocumentLimits::documentPixelBudget() - usedPixels(*document_)) {
        if (error) *error = QCoreApplication::translate("EditorSession", "The image is too large for this document.");
        return false;
    }
    // The still sits just above the movie, where it is and as large, named for the frame's time; the movie stays active.
    Layer still;
    still.id = QUuid::createUuid();
    still.name = QStringLiteral("%1 · %2").arg(movie.name, formatMovieTime(movie.movie->seconds));
    still.image = movie.image;
    still.transform = movie.transform;
    still.parentId = movie.parentId;
    beginEdit(QStringLiteral("Grab Movie Frame"));
    document_->layers.insert(index + 1, still);
    endEdit();
    return true;
}

bool EditorSession::relinkMovie(const QUuid &layerId, const QString &path, QString *error)
{
    const int index = indexOf(layerId);
    if (!document_ || index < 0 || !document_->layers.at(index).movie) return false;
    MovieLink link;
    QImage frame;
    if (!openMovie(path, DocumentLimits::documentPixelBudget(), &link, &frame, error)) return false;
    beginEdit(QStringLiteral("Relink Movie"));
    Layer &layer = document_->layers[index];
    layer.movie = link;
    layer.image = DeepImage::canonical(frame);
    endEdit();
    return true;
}

} // namespace compositor
