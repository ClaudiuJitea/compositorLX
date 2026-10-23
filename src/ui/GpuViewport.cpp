#include "ui/GpuViewport.h"
#include "rendering/GpuRenderer.h"
#include <QSettings>
#include <algorithm>
#include <QWidget>
namespace compositor {
namespace {
bool environmentPresentation(CanvasPresentation *mode) {
    const auto env = qgetenv("COMPOSITOR_CANVAS_BACKEND").toLower();
    if (env == "raster")
        *mode = CanvasPresentation::Raster;
    else if (env == "opengl" || env == "gl")
        *mode = CanvasPresentation::OpenGL;
    else if (env == "auto")
        *mode = CanvasPresentation::Auto;
    else
        return false;
    return true;
}
} // namespace
CanvasPresentation savedCanvasPresentation() {
    return CanvasPresentation(std::clamp(QSettings().value("rendering/canvas", 0).toInt(), 0, 2));
}
CanvasPresentation canvasPresentation() {
    CanvasPresentation mode;
    return environmentPresentation(&mode) ? mode : savedCanvasPresentation();
}
bool canvasPresentationOverridden() {
    CanvasPresentation ignored;
    return environmentPresentation(&ignored);
}
void setCanvasPresentation(CanvasPresentation mode) { QSettings().setValue("rendering/canvas", int(mode)); }
} // namespace compositor
#ifdef COMPOSITOR_GPU_CANVAS
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPainter>
#include <QTimer>
namespace compositor {
namespace {
class Viewport final : public QOpenGLWidget {
  public:
    Viewport(QWidget *parent, std::function<void(QPainter &)> paint) : QOpenGLWidget(parent), paint_(std::move(paint)) {
        setObjectName("gpuCanvasViewport");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
    }

  protected:
    void paintGL() override {
        QPainter painter(this);
        paint_(painter);
    }
    void showEvent(QShowEvent *event) override {
        QOpenGLWidget::showEvent(event);
        QTimer::singleShot(0, this, [this] {
            if (!isValid()) {
                setProperty("failed", true);
                hide();
                parentWidget()->update();
            }
        });
    }

  private:
    std::function<void(QPainter &)> paint_;
};
} // namespace
QWidget *createGpuViewport(QWidget *parent, std::function<void(QPainter &)> paint, CanvasPresentation mode) {
    if (mode == CanvasPresentation::Raster)
        return nullptr;
    const auto platform = QGuiApplication::platformName();
    if (platform == "offscreen" || platform == "minimal")
        return nullptr;
    QOpenGLContext probe;
    if (!probe.create())
        return nullptr;
    QOffscreenSurface surface;
    surface.setFormat(probe.format());
    surface.create();
    if (!surface.isValid() || !probe.makeCurrent(&surface))
        return nullptr;
    const auto text = [&](GLenum name) {
        const auto *value = probe.functions()->glGetString(name);
        return value ? QByteArray(reinterpret_cast<const char *>(value)).toLower() : QByteArray();
    };
    const QByteArray renderer = text(GL_RENDERER), vendor = text(GL_VENDOR), version = text(GL_VERSION);
    probe.doneCurrent();
    if (renderer.isEmpty() || renderer.contains("llvmpipe") || renderer.contains("softpipe") ||
        renderer.contains("software"))
        return nullptr;
    // OD5: Automatic only on the drivers the GL canvas has been tested with.
    if (mode == CanvasPresentation::Auto && !version.contains("mesa") && !vendor.contains("intel"))
        return nullptr;
    auto *viewport = new Viewport(parent, std::move(paint));
    viewport->setProperty("renderer", QString::fromUtf8(renderer));
    viewport->setGeometry(parent->rect());
    viewport->lower();
    return viewport;
}
} // namespace compositor
#else
namespace compositor {
QWidget *createGpuViewport(QWidget *, std::function<void(QPainter &)>, CanvasPresentation) { return nullptr; }
} // namespace compositor
#endif
