#pragma once

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QVector>

namespace compositor {

// Reusable power-of-two reductions.  Keeping the final resample below 2x
// avoids the blur and shimmer produced by reducing a large image in one pass.
class DownsampleCache final {
public:
    static DownsampleCache &shared();
    [[nodiscard]] static int levelForFactor(double factor);
    [[nodiscard]] QImage image(const QImage &source, double factor);
    [[nodiscard]] QImage image(const QImage &source, int wantedLevel, int *appliedLevel = nullptr);
    void clear();

    static constexpr int maxLevel = 6;
    static constexpr qsizetype pixelBudget = 200'000'000;   // one surface (mac DocumentLimits.maxSurfacePixels)

private:
    struct Entry {
        QSize sourceSize;
        QImage::Format format = QImage::Format_Invalid;
        QVector<QImage> levels;
        quint64 lastUse = 0;
        [[nodiscard]] qsizetype pixels() const;
    };

    void evict(quint64 keeping);

    QMutex mutex_;
    QHash<quint64, Entry> entries_;
    quint64 clock_ = 0;
};

} // namespace compositor
