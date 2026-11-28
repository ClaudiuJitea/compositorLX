#pragma once
#include "core/EditorSession.h"
#include "core/GifPlayback.h"
#include <QElapsedTimer>
#include <QWidget>
#include <functional>
#include <optional>
class QAction;
class QLabel;
class QListWidget;
class QMenu;
class QSlider;
class QToolButton;
class QTimer;
namespace compositor {
// The height of the Layers panel's footer, which the playback bars under the canvas share, so the window's bottom edge
// lines up (mac LayersFooter.height).
inline constexpr int layersFooterHeight = 36;

// A GIF layer's frames under the canvas, as macOS GIFTimeline (1.7.6): a strip of thumbnails, then a compact playback
// bar with play and pause, one delay for every frame (or each its own, Original), a frame scrubber and the frame
// number. A click or a scrub shows a frame, which painting and filters then change; right-click duplicates or deletes it.
class GifTimeline : public QWidget {
public:
    GifTimeline(EditorSession &session, std::function<void()> changed, QWidget *parent = nullptr);
    void refresh(bool canvasOnly = false);
    void stop();
    // Delays the delay menu offers for every frame, in milliseconds.
    static constexpr int delayPresets[] = {20, 33, 40, 50, 67, 80, 100, 125, 150, 200, 250, 500, 1000};
    static constexpr int stripHeight = 88;
    static constexpr int thumbnailSide = 56;
    [[nodiscard]] bool isPlaying() const { return playing_; }
    // The GIF's own delays (each frame's override, or its delay in the file): one, or the shortest to the longest when
    // its frames differ, with the unit spaced as the menu's presets are.
    static QString originalDelays(const GifAnimation &animation);
    // "03 / 12": the frame shown and how many there are.
    static QString frameCounter(int number, int count);
private:
    void editFrame(QAction *action, bool duplicate);
    // Plays from the frame shown, on the schedule of the layer's current frames and delays.
    void startPlayback();
    void wake();
    // Shows `index` after any frame still being decoded: a fast scrub settles on the latest frame asked for, skipping
    // the ones in between (mac GIFTimeline.requestFrame).
    void requestFrame(int index);
    void showRequestedFrame();
    void updatePlayButton();
    EditorSession &session_;
    std::function<void()> changed_;
    QListWidget *frames_;
    QToolButton *play_, *delay_;
    QSlider *scrubber_;
    QLabel *counter_;
    QMenu *delayMenu_;
    QAction *duplicate_, *delete_;
    QTimer *timer_;
    QTimer *requestTimer_;
    std::optional<int> requestedFrame_;
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
