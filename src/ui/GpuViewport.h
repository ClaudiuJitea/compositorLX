#pragma once
#include <functional>
class QWidget;
class QPainter;
namespace compositor {
// How the canvas reaches the screen (rendering/canvas, overridden by COMPOSITOR_CANVAS_BACKEND=auto|raster|opengl).
// Automatic uses OpenGL with Mesa or Intel drivers and the raster path elsewhere until those drivers are tested (OD5).
enum class CanvasPresentation { Auto, Raster, OpenGL };
CanvasPresentation canvasPresentation();
CanvasPresentation savedCanvasPresentation();
void setCanvasPresentation(CanvasPresentation);
bool canvasPresentationOverridden();
// Returns null when a hardware OpenGL widget is unavailable (or, for Automatic, not on a tested driver). The owner
// keeps its raster paint path, input handling and overlays in either case.
QWidget *createGpuViewport(QWidget *parent, std::function<void(QPainter &)> paint,
                           CanvasPresentation mode = CanvasPresentation::Auto);
} // namespace compositor
