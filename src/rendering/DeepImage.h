#pragma once
#include <QImage>
#include <functional>

namespace compositor::DeepImage
{
bool isDeep(const QImage &image);
bool isHDR(const QImage &image);
QImage canonical(const QImage &image);
QImage floating(const QImage &image);
QImage dithered(const QImage &image);
QImage through(const QImage &image, const std::function<QImage(const QImage &)> &operation,
               bool mixes = false);
QImage lookup(const QImage &image, const std::function<QImage(const QImage &)> &operation);
QImage takePainted(const QImage &deep, const QImage &painted, const QImage &base);
QImage gaussianBlur(const QImage &image, double radius);
QImage motionBlur(const QImage &image, double angle, double distance);
} // namespace compositor::DeepImage
