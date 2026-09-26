#include "ImageTrim.h"
#include <cmath>
#include <algorithm>

namespace compositor {

std::optional<QRect> ImageTrim::calculateTrimRect(const QImage &inputImage, const TrimOptions &options)
{
    if (!options.trimsAny() || inputImage.isNull() || inputImage.width() <= 0 || inputImage.height() <= 0) {
        return std::nullopt;
    }

    const QImage image = (inputImage.format() == QImage::Format_RGBA8888 ||
                          inputImage.format() == QImage::Format_RGBA8888_Premultiplied)
        ? inputImage
        : inputImage.convertToFormat(QImage::Format_RGBA8888_Premultiplied);

    const int width = image.width();
    const int height = image.height();

    if (options.basedOn == TrimBasedOn::TransparentPixels) {
        int left = width, right = -1, top = height, bottom = -1;
        for (int y = 0; y < height; ++y) {
            const uchar *line = image.constScanLine(y);
            for (int x = 0; x < width; ++x) {
                const uchar a = line[x * 4 + 3];
                if (a > 0) {
                    left = std::min(left, x);
                    right = std::max(right, x);
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
            }
        }

        // If right < 0, the entire image is transparent (alpha == 0)
        if (right < 0) {
            return std::nullopt;
        }

        const int minX = options.left ? left : 0;
        const int minY = options.top ? top : 0;
        const int maxX = options.right ? (right + 1) : width;
        const int maxY = options.bottom ? (bottom + 1) : height;

        if (maxX <= minX || maxY <= minY) {
            return std::nullopt;
        }
        return QRect(minX, minY, maxX - minX, maxY - minY);
    }

    // TopLeftPixelColor or BottomRightPixelColor
    const QPoint samplePoint = (options.basedOn == TrimBasedOn::TopLeftPixelColor)
        ? QPoint(0, 0)
        : QPoint(width - 1, height - 1);

    const uchar *sampleLine = image.constScanLine(samplePoint.y());
    const int targetR = sampleLine[samplePoint.x() * 4 + 0];
    const int targetG = sampleLine[samplePoint.x() * 4 + 1];
    const int targetB = sampleLine[samplePoint.x() * 4 + 2];
    const int targetA = sampleLine[samplePoint.x() * 4 + 3];
    const int tol = static_cast<int>(options.tolerance);

    const auto pixelMatches = [&](int x, int y) -> bool {
        const uchar *p = image.constScanLine(y) + x * 4;
        return std::abs(int(p[0]) - targetR) <= tol &&
               std::abs(int(p[1]) - targetG) <= tol &&
               std::abs(int(p[2]) - targetB) <= tol &&
               std::abs(int(p[3]) - targetA) <= tol;
    };

    int left = width, right = 0, top = height, bottom = 0;
    for (int y = 0; y < height; ++y) {
        int first = 0;
        while (first < width && pixelMatches(first, y)) {
            first++;
        }
        if (first == width) continue;

        int last = width;
        while (last > first && pixelMatches(last - 1, y)) {
            last--;
        }
        if (first < left) left = first;
        if (last > right) right = last;
        if (y < top) top = y;
        bottom = y + 1;
    }

    // If right == 0, every pixel matched the sample color
    if (right <= 0) {
        return std::nullopt;
    }

    const int minX = options.left ? left : 0;
    const int minY = options.top ? top : 0;
    const int maxX = options.right ? right : width;
    const int maxY = options.bottom ? bottom : height;

    if (maxX <= minX || maxY <= minY) {
        return std::nullopt;
    }
    return QRect(minX, minY, maxX - minX, maxY - minY);
}

std::optional<QImage> ImageTrim::trimImage(const QImage &image, const TrimOptions &options)
{
    const auto rect = calculateTrimRect(image, options);
    if (!rect) return std::nullopt;
    return image.copy(*rect);
}

} // namespace compositor
