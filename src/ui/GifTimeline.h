#pragma once
#include "core/EditorSession.h"
#include <QWidget>
#include <functional>
class QAction;
class QListWidget;
class QMenu;
class QToolButton;
class QTimer;
namespace compositor {
// A GIF layer's frames under the canvas, as macOS GIFTimeline: play and pause, one delay for every frame (or each
// its own, Original), and a strip of thumbnails. A click shows a frame, which painting and filters then change;
// right-click duplicates or deletes it.
class GifTimeline : public QWidget {
public:
    GifTimeline(EditorSession &session, std::function<void()> changed, QWidget *parent = nullptr);
    void refresh(bool canvasOnly = false);
    void stop();
    // Delays the delay menu offers for every frame, in milliseconds.
    static constexpr int delayPresets[] = {20, 33, 40, 50, 67, 80, 100, 125, 150, 200, 250, 500, 1000};
private:
    void editFrame(QAction *action, bool duplicate);
    EditorSession &session_;
    std::function<void()> changed_;
    QListWidget *frames_;
    QToolButton *play_, *delay_;
    QMenu *delayMenu_;
    QAction *duplicate_, *delete_;
    QTimer *timer_;
    QUuid layerId_;
    bool refreshing_ = false;
};
}
