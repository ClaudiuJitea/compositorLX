#pragma once
#include <functional>
class QWidget;
class QPainter;
namespace compositor {
// Returns null when a hardware OpenGL widget is unavailable. The owner keeps
// its raster paint path, input handling and overlays in either case.
QWidget *createGpuViewport(QWidget *parent, std::function<void(QPainter &)> paint);
} // namespace compositor
