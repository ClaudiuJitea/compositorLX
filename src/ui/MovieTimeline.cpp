#include "ui/MovieTimeline.h"

#include "io/MovieDecoder.h"
#include "ui/EditorIcons.h"
#include "ui/GifTimeline.h"

#include <QAction>
#include <QCoreApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QSettings>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#ifdef COMPOSITOR_HAVE_MULTIMEDIA
#include <QAudioOutput>
#include <QMediaPlayer>
#endif

#include <algorithm>
#include <cmath>

namespace compositor {

namespace {

QString unavailable(bool playing)
{
    return playing ? QCoreApplication::translate("MovieTimeline", "Movie unavailable. Right-click the timeline to relink and play.")
                   : QCoreApplication::translate("MovieTimeline", "Movie unavailable. Right-click the timeline to relink and scrub.");
}

QFont tabularDigits(QFont font)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    font.setFeature(QFont::Tag("tnum"), 1);
#endif
    return font;
}

constexpr char mutedKey[] = "moviePlaybackMuted";

} // namespace

bool MovieTimeline::soundAvailable()
{
#ifdef COMPOSITOR_HAVE_MULTIMEDIA
    return true;
#else
    return false;
#endif
}

MovieTimeline::MovieTimeline(EditorSession &session, std::function<void()> changed, Preview preview, QWidget *parent)
    : QWidget(parent), session_(session), changed_(std::move(changed)), preview_(std::move(preview))
{
    setObjectName(QStringLiteral("movieTimeline")); setAttribute(Qt::WA_StyledBackground);
    setFixedHeight(layersFooterHeight);
    auto *row = new QHBoxLayout(this); row->setContentsMargins(10, 0, 10, 0); row->setSpacing(8);
    play_ = new QToolButton(this); play_->setObjectName(QStringLiteral("moviePlay")); play_->setFixedSize(28, 24); play_->setIconSize(QSize(14, 14));
    mute_ = new QToolButton(this); mute_->setObjectName(QStringLiteral("movieMute")); mute_->setFixedSize(28, 24); mute_->setIconSize(QSize(16, 16));
    playhead_ = new QSlider(Qt::Horizontal, this); playhead_->setObjectName(QStringLiteral("moviePlayhead"));
    playhead_->setAccessibleName(tr("Movie playhead"));
    time_ = new QLabel(this); time_->setObjectName(QStringLiteral("movieTime")); time_->setFont(tabularDigits(time_->font()));
    failure_ = new QLabel(this); failure_->setObjectName(QStringLiteral("movieFailure")); failure_->hide();
    row->addWidget(play_); row->addWidget(mute_); row->addWidget(playhead_, 1); row->addWidget(time_); row->addWidget(failure_);
    muted_ = QSettings().value(QLatin1String(mutedKey), false).toBool();
    if (!soundAvailable()) mute_->hide();

    // Right-click: grab the frame shown as a still layer, or link the layer to the movie where it is now.
    grab_ = new QAction(tr("Grab Frame"), this); grab_->setObjectName(QStringLiteral("movieGrabFrame"));
    relink_ = new QAction(tr("Relink…"), this); relink_->setObjectName(QStringLiteral("movieRelink"));
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        grab_->setEnabled(!isBusy() && !playing_ && layer() && session_.canEditLayers());
        QMenu menu(this); menu.addAction(grab_); menu.addAction(relink_);
        menu.exec(mapToGlobal(pos));
    });
    connect(grab_, &QAction::triggered, this, [this] { grabFrame(); });
    connect(relink_, &QAction::triggered, this, [this] {
        stopPlayback();
        const QString path = QFileDialog::getOpenFileName(this, tr("Relink Movie"), QFileInfo(EditorSession::moviePath(link_)).absolutePath(),
                                                          tr("Movies (*.mov *.mp4 *.m4v *.qt)"));
        if (!path.isEmpty()) relink(path);
    });

    ticker_ = new QTimer(this); ticker_->setInterval(16); ticker_->setTimerType(Qt::PreciseTimer);
    connect(ticker_, &QTimer::timeout, this, [this] { tick(); });
    connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] { finished(); });
    connect(play_, &QToolButton::clicked, this, [this] { togglePlayback(); });
    connect(mute_, &QToolButton::clicked, this, [this] { setMuted(!muted_); });
    connect(playhead_, &QSlider::sliderPressed, this, [this] {
        // Holding the playhead stops playback and any frame being settled on (mac onEditingChanged(true)).
        if (playing_) stopPlayback();
        pending_.reset();
        dragging_ = true;
    });
    connect(playhead_, &QSlider::valueChanged, this, [this](int milliseconds) {
        if (refreshing_) return;
        const double seconds = std::clamp(milliseconds / 1000.0, 0.0, link_.duration);
        if (dragging_) scrub(seconds); else seek(seconds);
    });
    connect(playhead_, &QSlider::sliderReleased, this, [this] {
        dragging_ = false;
        seek(requested_.value_or(link_.seconds));
    });
    hide();
}

MovieTimeline::~MovieTimeline()
{
    ++generation_;
    watcher_.waitForFinished();
}

const Layer *MovieTimeline::layer() const
{
    const Layer *active = session_.activeLayer();
    return active && active->id == layerId_ && active->movie ? active : nullptr;
}

QString MovieTimeline::failure() const { return failure_->isVisible() ? failure_->text() : QString(); }

double MovieTimeline::shownSeconds() const { return requested_.value_or(link_.seconds); }

void MovieTimeline::fail(const QString &message)
{
    failure_->setText(message); failure_->setToolTip(message);
    failure_->setVisible(!message.isEmpty());
}

void MovieTimeline::refresh(bool canvasOnly)
{
    const Layer *active = session_.activeLayer();
    const bool visible = !canvasOnly && active && active->movie;
    if (!visible || active->id != layerId_ || (active->movie && active->movie->bookmark != link_.bookmark)) {
        stop();
        fail({});
    }
    setVisible(visible);
    if (!visible) { layerId_ = {}; return; }
    layerId_ = active->id;
    link_ = *active->movie;
    updateBar();
}

void MovieTimeline::updateBar()
{
    refreshing_ = true;
    const double seconds = shownSeconds();
    const int total = std::max(1, int(std::lround(link_.duration * 1000)));
    playhead_->setRange(0, total);
    playhead_->setSingleStep(std::max(1, total / 100)); playhead_->setPageStep(std::max(1, total / 10));
    if (!playhead_->isSliderDown()) playhead_->setValue(int(std::lround(seconds * 1000)));
    refreshing_ = false;
    time_->setText(formatMovieTime(seconds));
    play_->setIcon(editorIcon(playing_ ? 35 : 34));
    play_->setToolTip(playing_ ? tr("Pause") : tr("Play")); play_->setAccessibleName(play_->toolTip());
    mute_->setIcon(editorIcon(muted_ ? 53 : 52));
    mute_->setToolTip(muted_ ? tr("Unmute movie") : tr("Mute movie")); mute_->setAccessibleName(mute_->toolTip());
}

void MovieTimeline::setMuted(bool muted)
{
    muted_ = muted;
    QSettings().setValue(QLatin1String(mutedKey), muted);
#ifdef COMPOSITOR_HAVE_MULTIMEDIA
    if (output_) output_->setMuted(muted);
#endif
    updateBar();
}

std::shared_ptr<MovieDecoder> MovieTimeline::source(QString *error)
{
    const QString path = EditorSession::moviePath(link_);
    if (decoder_ && decoderPath_ == path) return decoder_;
    releaseSource();
    if (path.isEmpty() || !QFileInfo(path).isFile()) return nullptr;
    std::unique_ptr<MovieDecoder> opened = MovieDecoder::open(path, error);
    if (!opened) return nullptr;
    decoder_ = std::move(opened);
    decoderPath_ = path;
    return decoder_;
}

void MovieTimeline::releaseSource()
{
    decoder_.reset();
    decoderPath_.clear();
#ifdef COMPOSITOR_HAVE_MULTIMEDIA
    if (sound_) sound_->stop();
#endif
}

void MovieTimeline::stop()
{
    ++generation_;
    pending_.reset();
    requested_.reset();
    dragging_ = false;
    stopPlayback();
    if (previewShown_ && preview_) preview_(layerId_, {});
    previewShown_ = false;
    releaseSource();
    if (isVisible()) updateBar();
}

void MovieTimeline::enqueue(const Request &request)
{
    // One frame at a time: a request made while one decodes replaces any still waiting, so a fast drag settles on the
    // newest position (mac scrub's "jump straight to its newest position once that seek finishes").
    if (inFlight_) { pending_ = request; return; }
    run(request);
}

void MovieTimeline::run(const Request &request)
{
    QString error;
    std::shared_ptr<MovieDecoder> decoder = source(&error);
    if (!decoder) {
        if (request.kind == Kind::Play) stopPlayback();
        fail(unavailable(request.kind == Kind::Play));
        requested_.reset();
        if (previewShown_ && preview_) preview_(layerId_, {});
        previewShown_ = false;
        updateBar();
        return;
    }
    inFlight_ = true;
    const QSize maximum = request.kind == Kind::Settle ? QSize() : QSize(previewSide, previewSide);
    watcher_.setFuture(QtConcurrent::run([decoder, request, maximum] {
        Result result{QImage(), QString(), request};
        result.image = decoder->frame(request.seconds, &result.error, maximum);
        return result;
    }));
}

void MovieTimeline::finished()
{
    inFlight_ = false;
    const Result result = watcher_.result();
    const bool current = result.request.generation == generation_;
    if (current) {
        if (result.image.isNull()) {
            if (result.request.kind == Kind::Play) stopPlayback();
            fail(unavailable(result.request.kind == Kind::Play));
            requested_.reset();
            if (previewShown_ && preview_) preview_(layerId_, {});
            previewShown_ = false;
            pending_.reset();
        } else if (result.request.kind == Kind::Settle) {
            // The frame a release or a pause settles on, unless something newer was asked for meanwhile.
            if (!pending_) {
                fail({});
                const double seconds = std::clamp(result.request.seconds, 0.0, link_.duration);
                if (session_.setMovieFrame(layerId_, result.image, seconds)) {
                    if (const Layer *l = layer()) link_ = *l->movie;
                    changed_();
                }
                requested_.reset();
                if (result.request.keepFrame) stopPlayback();
                if (previewShown_ && preview_) preview_(layerId_, {});
                previewShown_ = false;
            }
        } else if (result.request.kind == Kind::Scrub || playing_) {
            fail({});
            if (preview_) preview_(layerId_, result.image);
            previewShown_ = true;
        }
        updateBar();
    }
    if (pending_) {
        const Request next = *pending_;
        pending_.reset();
        if (next.generation == generation_) run(next);
    }
}

void MovieTimeline::scrub(double seconds)
{
    requested_ = seconds;
    enqueue({Kind::Scrub, seconds, generation_});
    updateBar();
}

void MovieTimeline::seek(double seconds, bool keepFrame)
{
    stopPlayback(keepFrame);
    requested_ = seconds;
    enqueue({Kind::Settle, seconds, generation_, keepFrame});
    updateBar();
}

void MovieTimeline::togglePlayback()
{
    if (playing_) {
        // A pause settles on the frame playing, which stays on the canvas until it has (mac keepFrame).
        seek(std::clamp(playStart_ + clock_.elapsed() / 1000.0, 0.0, link_.duration), true);
        return;
    }
    startPlayback();
}

void MovieTimeline::startPlayback()
{
    QString error;
    const std::shared_ptr<MovieDecoder> decoder = source(&error);
    if (!decoder) { fail(unavailable(true)); return; }
    fail({});
    pending_.reset();
    // From the frame shown, or from the start when it's at the end.
    const double seconds = requested_.value_or(link_.seconds);
    playStart_ = seconds >= link_.duration ? 0 : seconds;
    playing_ = true;
    clock_.start();
#ifdef COMPOSITOR_HAVE_MULTIMEDIA
    if (decoder->info().hasAudio) {
        if (!sound_) {
#ifdef COMPOSITOR_DEV_PLUGIN_DIR
            QCoreApplication::addLibraryPath(QStringLiteral(COMPOSITOR_DEV_PLUGIN_DIR));
#endif
            sound_ = new QMediaPlayer(this);
            output_ = new QAudioOutput(this);
            sound_->setAudioOutput(output_);
        }
        output_->setMuted(muted_);
        sound_->setSource(QUrl::fromLocalFile(decoderPath_));
        // The picture is decoded here, so the player only gives the sound.
        sound_->setActiveVideoTrack(-1);
        sound_->setPosition(qint64(std::llround(playStart_ * 1000)));
        sound_->play();
    }
#endif
    ticker_->start();
    tick();
}

void MovieTimeline::stopPlayback(bool keepFrame)
{
    if (!playing_) return;
    playing_ = false;
    ticker_->stop();
#ifdef COMPOSITOR_HAVE_MULTIMEDIA
    if (sound_) sound_->pause();
#endif
    if (!keepFrame) {
        if (previewShown_ && preview_ && !requested_) preview_(layerId_, {});
        if (!requested_) previewShown_ = false;
    }
    updateBar();
}

void MovieTimeline::tick()
{
    if (!playing_) return;
    if (!layer() || !isVisible()) { stop(); return; }
    double seconds = playStart_ + clock_.elapsed() / 1000.0;
#ifdef COMPOSITOR_HAVE_MULTIMEDIA
    // The sound sets the pace when it's playing: the picture follows its clock, as AVPlayer's does.
    if (sound_ && sound_->playbackState() == QMediaPlayer::PlayingState && sound_->position() > 0) {
        const double heard = sound_->position() / 1000.0;
        if (std::abs(heard - seconds) > .08) { playStart_ += heard - seconds; seconds = heard; }
    }
#endif
    if (seconds >= link_.duration) {
        // The end: it stops on the last frame (mac: time >= range.end → seek(duration, keepFrame)).
        seek(link_.duration, true);
        return;
    }
    requested_ = seconds;
    if (!inFlight_) enqueue({Kind::Play, seconds, generation_});
    updateBar();
}

void MovieTimeline::grabFrame()
{
    if (!layer() || isBusy() || playing_) return;
    if (previewShown_ && preview_) preview_(layerId_, {});
    previewShown_ = false;
    QString error;
    if (session_.grabMovieFrame(layerId_, &error)) changed_();
    else if (!error.isEmpty()) fail(error);
}

void MovieTimeline::relink(const QString &path)
{
    if (!layer()) return;
    stop();
    QString error;
    if (session_.relinkMovie(layerId_, path, &error)) {
        if (const Layer *l = layer()) link_ = *l->movie;
        fail({});
        changed_();
    } else {
        fail(error.isEmpty() ? tr("The movie could not be opened.") : error);
    }
    updateBar();
}

} // namespace compositor
