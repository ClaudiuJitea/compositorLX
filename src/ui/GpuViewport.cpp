#include "ui/GpuViewport.h"
#include "rendering/GpuRenderer.h"
#include <QWidget>
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
QWidget *createGpuViewport(QWidget *parent, std::function<void(QPainter &)> paint) {
    if (GpuRenderer::backend() == RenderBackend::Cpu || qEnvironmentVariable("COMPOSITOR_CANVAS_BACKEND") == "raster")
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
    const auto *name = probe.functions()->glGetString(GL_RENDERER);
    const QByteArray renderer = name ? QByteArray(reinterpret_cast<const char *>(name)).toLower() : QByteArray();
    probe.doneCurrent();
    if (renderer.isEmpty() || renderer.contains("llvmpipe") || renderer.contains("softpipe") ||
        renderer.contains("software"))
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
QWidget *createGpuViewport(QWidget *, std::function<void(QPainter &)>) { return nullptr; }
} // namespace compositor
#endif
