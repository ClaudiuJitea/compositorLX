#include "rendering/DownsampleCache.h"

#include <QMutexLocker>

#include <algorithm>
#include <cmath>
#include <limits>

namespace compositor {

DownsampleCache &DownsampleCache::shared()
{
    static DownsampleCache cache;
    return cache;
}

int DownsampleCache::levelForFactor(double factor)
{
    if (!std::isfinite(factor) || factor <= 0 || factor >= .5) return 0;
    return std::min(maxLevel, int(std::floor(std::log2(1.0 / factor))));
}

qsizetype DownsampleCache::Entry::pixels() const
{
    qsizetype total = 0;
    for (const QImage &image : levels) total += qsizetype(image.width()) * image.height();
    return total;
}

QImage DownsampleCache::image(const QImage &source, double factor)
{
    return image(source, levelForFactor(factor));
}

QImage DownsampleCache::image(const QImage &source, int wantedLevel, int *appliedLevel)
{
    if (appliedLevel) *appliedLevel = 0;
    if (source.isNull() || wantedLevel < 1 || (source.width() <= 1 && source.height() <= 1)) return source;
    wantedLevel = std::min(wantedLevel, maxLevel);
    const quint64 key = source.cacheKey();
    QVector<QImage> levels;
    {
        QMutexLocker lock(&mutex_);
        ++clock_;
        const auto found = entries_.constFind(key);
        if (found != entries_.cend() && found->sourceSize == source.size() && found->format == source.format()) levels = found->levels;
    }
    while (levels.size() < wantedLevel) {
        const QImage &previous = levels.isEmpty() ? source : levels.constLast();
        if (previous.width() <= 1 && previous.height() <= 1) break;
        const QSize size(std::max(1, (previous.width() + 1) / 2), std::max(1, (previous.height() + 1) / 2));
        QImage next = previous.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (next.isNull()) break;
        // Scaling should retain grayscale masks, but explicitly restore their
        // one-channel representation on Qt backends which promote it.
        if (source.format() == QImage::Format_Grayscale8 && next.format() != QImage::Format_Grayscale8)
            next = next.convertToFormat(QImage::Format_Grayscale8);
        levels.push_back(std::move(next));
    }
    if (levels.isEmpty()) return source;
    {
        QMutexLocker lock(&mutex_);
        Entry &entry = entries_[key];
        entry.sourceSize = source.size();
        entry.format = source.format();
        if (entry.levels.size() < levels.size()) entry.levels = levels;
        entry.lastUse = clock_;
        evict(key);
    }
    const int applied = std::min(wantedLevel, int(levels.size()));
    if (appliedLevel) *appliedLevel = applied;
    return levels.at(applied - 1);
}

void DownsampleCache::evict(quint64 keeping)
{
    qsizetype total = 0;
    for (const Entry &entry : std::as_const(entries_)) total += entry.pixels();
    while (total > pixelBudget && entries_.size() > 1) {
        auto oldest = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (it.key() != keeping && (oldest == entries_.end() || it->lastUse < oldest->lastUse)) oldest = it;
        if (oldest == entries_.end()) break;
        total -= oldest->pixels();
        entries_.erase(oldest);
    }
}

void DownsampleCache::clear()
{
    QMutexLocker lock(&mutex_);
    entries_.clear();
}

} // namespace compositor
