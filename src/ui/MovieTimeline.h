#pragma once

#include "core/EditorSession.h"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QImage>
#include <QUuid>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>

class QAction;
class QLabel;
class QSlider;
class QTimer;
class QToolButton;
#ifdef COMPOSITOR_HAVE_MULTIMEDIA
class QAudioOutput;
class QMediaPlayer;
#endif

namespace compositor {

class MovieDecoder;

// A movie layer's bar under the canvas (mac b469ec1 MovieTimeline): play and pause, sound on and off, the playhead and
// its time. Dragging the playhead shows frames on the canvas as it goes and lets go on the frame under it, which becomes
// the layer's image ("Select Movie Frame"); a pause settles on the frame playing. Right-click grabs the frame as a still
// layer, or relinks a movie that has moved.
class MovieTimeline : public QWidget {
public:
    using Preview = std::function<void(const QUuid &layerId, const QImage &frame)>;
    MovieTimeline(EditorSession &session, std::function<void()> changed, Preview preview, QWidget *parent = nullptr);
    ~MovieTimeline() override;

    void refresh(bool canvasOnly = false);
    // Stops playing and any frame being found, takes the frame preview off the canvas and lets go of the movie.
    void stop();

    [[nodiscard]] bool isPlaying() const { return playing_; }
    // A frame is being decoded (a scrub, playback or the frame a pause or release settles on).
    [[nodiscard]] bool isBusy() const { return inFlight_ || pending_.has_value(); }
    [[nodiscard]] QString failure() const;
    // Where the playhead shows: the time being scrubbed or played, else the layer's frame's.
    [[nodiscard]] double shownSeconds() const;
    // The largest a frame shown while playing or scrubbing is decoded; the frame settled on is full size.
    static constexpr int previewSide = 1920;
    // The menu's commands, for the window and tests: Grab Frame and Relink…
    void grabFrame();
    void relink(const QString &path);
    [[nodiscard]] static bool soundAvailable();

private:
    enum class Kind { Scrub, Play, Settle };
    struct Request { Kind kind; double seconds; quint64 generation; bool keepFrame = false; };
    struct Result { QImage image; QString error; Request request; };

    void togglePlayback();
    void startPlayback();
    void stopPlayback(bool keepFrame = false);
    void tick();
    void scrub(double seconds);
    void seek(double seconds, bool keepFrame = false);
    void enqueue(const Request &request);
    void run(const Request &request);
    void finished();
    void fail(const QString &message);
    void updateBar();
    void setMuted(bool muted);
    // The movie for this layer, opened when first needed; nullptr when it can't be found or read.
    std::shared_ptr<MovieDecoder> source(QString *error = nullptr);
    void releaseSource();
    [[nodiscard]] const Layer *layer() const;

    EditorSession &session_;
    std::function<void()> changed_;
    Preview preview_;
    QToolButton *play_, *mute_;
    QSlider *playhead_;
    QLabel *time_, *failure_;
    QAction *grab_, *relink_;
    QTimer *ticker_;
    QFutureWatcher<Result> watcher_;
    QUuid layerId_;
    MovieLink link_;
    std::shared_ptr<MovieDecoder> decoder_;
    QString decoderPath_;
    std::optional<Request> pending_;
    std::optional<double> requested_;
    quint64 generation_ = 0;
    bool inFlight_ = false, playing_ = false, dragging_ = false, refreshing_ = false, muted_ = false;
    bool previewShown_ = false;
    QElapsedTimer clock_;
    double playStart_ = 0;
#ifdef COMPOSITOR_HAVE_MULTIMEDIA
    QMediaPlayer *sound_ = nullptr;
    QAudioOutput *output_ = nullptr;
#endif
};

} // namespace compositor
