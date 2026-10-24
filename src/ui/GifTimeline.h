#pragma once
#include "core/EditorSession.h"
#include "core/GifPlayback.h"
#include <QElapsedTimer>
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
    [[nodiscard]] bool isPlaying() const { return playing_; }
    // The GIF's own delays (each frame's override, or its delay in the file): one, or the shortest to the longest when
    // its frames differ. The menu spaces the unit as its presets do; the label under the play button doesn't.
    static QString originalDelays(const GifAnimation &animation, bool spaced = true);
private:
    void editFrame(QAction *action, bool duplicate);
    // Plays from the frame shown, on the schedule of the layer's current frames and delays.
    void startPlayback();
    void wake();
    EditorSession &session_;
    std::function<void()> changed_;
    QListWidget *frames_;
    QToolButton *play_, *delay_;
    QMenu *delayMenu_;
    QAction *duplicate_, *delete_;
    QTimer *timer_;
    QElapsedTimer clock_;
    GifPlayback playback_;
    // What the schedule was made from: frame identities and their delays. A change restarts it, as macOS's playback
    // task restarts when its frames or timing change.
    QVector<int> playingFrames_;
    QVector<qint64> playingDurations_;
    QUuid layerId_;
    bool refreshing_ = false, playing_ = false;
};
}
