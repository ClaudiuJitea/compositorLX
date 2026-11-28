#include "ui/GifTimeline.h"
#include "ui/EditorIcons.h"
#include <QAction>
#include <QActionGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
namespace compositor {
static constexpr int thumbnailSide = GifTimeline::thumbnailSide;

// Digits that keep their width as they count, as SwiftUI's monospacedDigit.
static QFont tabularDigits(QFont font)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    font.setFeature(QFont::Tag("tnum"), 1);
#endif
    return font;
}

// A frame's thumbnail fitted into a rounded box, outlined in the accent while it is the frame shown.
static QPixmap framePixmap(const QImage &thumbnail, bool selected, const QPalette &palette, qreal ratio)
{
    QPixmap pixmap(QSize(thumbnailSide, thumbnailSide) * ratio); pixmap.setDevicePixelRatio(ratio); pixmap.fill(Qt::transparent);
    QPainter p(&pixmap); p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF box(1, 1, thumbnailSide - 2, thumbnailSide - 2);
    QPainterPath rounded; rounded.addRoundedRect(box, 5, 5);
    p.fillPath(rounded, QColor(0, 0, 0, 64));
    if (!thumbnail.isNull()) {
        const QSizeF fitted = QSizeF(thumbnail.size()).scaled(box.size() - QSizeF(4, 4), Qt::KeepAspectRatio);
        p.save(); p.setClipPath(rounded);
        p.drawImage(QRectF(box.center() - QPointF(fitted.width(), fitted.height()) / 2, fitted), thumbnail);
        p.restore();
    }
    QColor edge = selected ? palette.color(QPalette::Highlight) : palette.color(QPalette::WindowText);
    if (!selected) edge.setAlphaF(.12f);
    p.setPen(QPen(edge, selected ? 2 : 1)); p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(selected ? box.adjusted(.5, .5, -.5, -.5) : box, 5, 5);
    return pixmap;
}

GifTimeline::GifTimeline(EditorSession &session, std::function<void()> changed, QWidget *parent)
    : QWidget(parent), session_(session), changed_(std::move(changed))
{
    setObjectName("gifTimeline"); setAttribute(Qt::WA_StyledBackground);
    setFixedHeight(stripHeight + 1 + layersFooterHeight);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    // The strip of frames, 56-point thumbnails (mac 8c6ebeb: .frame(height: 88), padding 12 across, 8 above, 6 below).
    frames_ = new QListWidget(this); frames_->setObjectName("gifFrames"); frames_->setViewMode(QListView::IconMode);
    frames_->setFlow(QListView::LeftToRight); frames_->setWrapping(false); frames_->setMovement(QListView::Static);
    frames_->setIconSize(QSize(thumbnailSide, thumbnailSide)); frames_->setSpacing(4); frames_->setFrameShape(QFrame::NoFrame);
    frames_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); frames_->setContextMenuPolicy(Qt::CustomContextMenu);
    frames_->setFixedHeight(stripHeight);
    layout->addWidget(frames_);
    auto *divider = new QFrame(this); divider->setFrameShape(QFrame::HLine); divider->setFrameShadow(QFrame::Plain); divider->setObjectName("gifDivider");
    divider->setFixedHeight(1);
    layout->addWidget(divider);
    // The playback bar: play, the delay menu, the frame scrubber and "03 / 12", as tall as the Layers footer.
    auto *bar = new QWidget(this); bar->setObjectName("gifPlaybackBar"); bar->setFixedHeight(layersFooterHeight);
    auto *row = new QHBoxLayout(bar); row->setContentsMargins(10, 0, 10, 0); row->setSpacing(8);
    play_ = new QToolButton(bar); play_->setObjectName("gifPlay"); play_->setFixedSize(28, 24); play_->setIconSize(QSize(14, 14));
    play_->setIcon(editorIcon(34)); play_->setToolTip(tr("Play")); play_->setAccessibleName(tr("Play"));
    // The delay as a menu of common ones: Original, each frame for its own, or every frame for the same time.
    delay_ = new QToolButton(bar); delay_->setObjectName("gifDelay");
    delay_->setPopupMode(QToolButton::InstantPopup); delay_->setToolTip(tr("How long each frame shows"));
    delay_->setAccessibleName(tr("Frame delay")); delay_->setFont(tabularDigits(delay_->font()));
    delayMenu_ = new QMenu(delay_); delayMenu_->setObjectName("gifDelayMenu");
    auto *group = new QActionGroup(delayMenu_); group->setExclusive(true);
    auto *original = delayMenu_->addAction(tr("Original")); original->setObjectName("gifDelayOriginal"); original->setCheckable(true); group->addAction(original);
    delayMenu_->addSeparator();
    for (int milliseconds : delayPresets) {
        const double fps = 1000.0 / milliseconds;
        auto *action = delayMenu_->addAction(tr("%1 ms · %2 fps").arg(milliseconds).arg(QString::number(std::round(fps * 10) / 10, 'g', 4)));
        action->setCheckable(true); action->setData(milliseconds); action->setObjectName(QStringLiteral("gifDelay%1").arg(milliseconds)); group->addAction(action);
    }
    delay_->setMenu(delayMenu_);
    scrubber_ = new QSlider(Qt::Horizontal, bar); scrubber_->setObjectName("gifScrubber"); scrubber_->setAccessibleName(tr("GIF frame"));
    scrubber_->setRange(1, 1); scrubber_->setSingleStep(1); scrubber_->setPageStep(1);
    counter_ = new QLabel(bar); counter_->setObjectName("gifFrameCounter"); counter_->setFont(tabularDigits(counter_->font()));
    row->addWidget(play_); row->addWidget(delay_); row->addWidget(scrubber_, 1); row->addWidget(counter_);
    layout->addWidget(bar);
    // Right-click duplicates or deletes the frame under the pointer; triggered otherwise, the frame shown.
    duplicate_ = new QAction(tr("Duplicate Frame"), this); duplicate_->setObjectName("gifDuplicate"); addAction(duplicate_);
    delete_ = new QAction(tr("Delete Frame"), this); delete_->setObjectName("gifDelete"); addAction(delete_);
    connect(duplicate_, &QAction::triggered, this, [this] { editFrame(duplicate_, true); });
    connect(delete_, &QAction::triggered, this, [this] { editFrame(delete_, false); });
    connect(frames_, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        auto *item = frames_->itemAt(pos); if (!item) return;
        const QVariant index = item->data(Qt::UserRole);
        duplicate_->setData(index); delete_->setData(index);
        delete_->setEnabled(frames_->count() > 1);
        QMenu menu(this); menu.addAction(duplicate_); menu.addAction(delete_);
        menu.exec(frames_->viewport()->mapToGlobal(pos));
        duplicate_->setData({}); delete_->setData({});
    });
    timer_ = new QTimer(this); timer_->setSingleShot(true); timer_->setTimerType(Qt::PreciseTimer);
    requestTimer_ = new QTimer(this); requestTimer_->setSingleShot(true); requestTimer_->setInterval(0);
    connect(requestTimer_, &QTimer::timeout, this, [this] { showRequestedFrame(); });
    connect(play_, &QToolButton::clicked, this, [this] {
        if (playing_) { stop(); return; }
        const auto *l = session_.activeLayer(); if (!l || !l->gif || l->gif->frames.size() < 2 || requestedFrame_) return;
        playing_ = true;
        updatePlayButton();
        startPlayback();
    });
    connect(timer_, &QTimer::timeout, this, [this] { wake(); });
    connect(frames_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (refreshing_ || row < 0) return;
        requestFrame(frames_->item(row)->data(Qt::UserRole).toInt());
    });
    connect(scrubber_, &QSlider::valueChanged, this, [this](int number) {
        if (refreshing_) return;
        const auto *l = session_.activeLayer(); if (!l || !l->gif) return;
        const int position = number - 1;
        if (position >= 0 && position < l->gif->frames.size()) requestFrame(l->gif->frames[position].index);
    });
    connect(group, &QActionGroup::triggered, this, [this, original](QAction *action) {
        if (refreshing_) return;
        stop();
        if (session_.setGifTiming(action == original ? std::nullopt : std::optional<int>(action->data().toInt()))) changed_();
        refresh();
    });
    hide();
}
void GifTimeline::requestFrame(int index)
{
    if (playing_) stop();
    const auto *l = session_.activeLayer();
    const int shown = requestedFrame_.value_or(l && l->gif ? l->gif->selected : -1);
    if (index == shown) return;
    requestedFrame_ = index;
    updatePlayButton();
    // Shown once the events already queued, such as more of a scrub, have been seen: only the latest is decoded.
    if (!requestTimer_->isActive()) requestTimer_->start();
}
void GifTimeline::showRequestedFrame()
{
    if (!requestedFrame_) return;
    const int index = *requestedFrame_;
    const bool shown = session_.selectGifFrame(index);
    if (requestedFrame_ == index) requestedFrame_.reset();
    if (shown) changed_();
    refresh();
    if (requestedFrame_) requestTimer_->start();
}
void GifTimeline::updatePlayButton()
{
    play_->setIcon(editorIcon(playing_ ? 35 : 34));
    play_->setToolTip(playing_ ? tr("Pause") : tr("Play")); play_->setAccessibleName(play_->toolTip());
    const auto *l = session_.activeLayer();
    play_->setEnabled(l && l->gif && l->gif->frames.size() > 1 && !requestedFrame_);
}
void GifTimeline::editFrame(QAction *action, bool duplicate)
{
    stop();
    const std::optional<int> index = action->data().isValid() ? std::optional<int>(action->data().toInt()) : std::nullopt;
    if (duplicate ? session_.duplicateGifFrame(index) : session_.deleteGifFrame(index)) { changed_(); refresh(); }
}
void GifTimeline::stop()
{
    playing_ = false; playback_ = {};
    timer_->stop(); updatePlayButton();
}
void GifTimeline::startPlayback()
{
    const auto *l = session_.activeLayer(); if (!l || !l->gif) { stop(); return; }
    const auto &a = *l->gif;
    playingFrames_.clear(); playingDurations_.clear();
    int initial = 0;
    for (int i = 0; i < a.frames.size(); ++i) {
        playingFrames_.append(a.frames[i].index);
        playingDurations_.append(qint64(std::max(20, a.delay(a.frames[i].index))) * 1000);
        if (a.frames[i].index == a.selected) initial = i;
    }
    playback_ = GifPlayback(playingDurations_, initial);
    if (!playback_.valid()) { stop(); return; }
    clock_.start();
    timer_->start(int((playback_.next() + 999) / 1000));
}
// While playing, each frame shows from its own start in the loop: decoding never lengthens the saved delays, and a
// frame whose time passed while the last one decoded is skipped.
void GifTimeline::wake()
{
    const auto *l = session_.activeLayer();
    if (!playing_ || !l || !l->gif || l->id != layerId_ || !isVisible() || l->gif->frames.size() < 2) { stop(); return; }
    const auto step = playback_.advance(clock_.nsecsElapsed() / 1000);
    if (step.show >= 0) {
        if (!session_.selectGifFrame(playingFrames_[step.show])) { stop(); return; }
        changed_(); refresh();
        if (!playing_) return;
    }
    timer_->start(int(std::max<qint64>(0, playback_.next() - clock_.nsecsElapsed() / 1000 + 999) / 1000));
}
QString GifTimeline::frameCounter(int number, int count)
{
    return QStringLiteral("%1 / %2").arg(number, 2, 10, QLatin1Char('0')).arg(count, 2, 10, QLatin1Char('0'));
}
QString GifTimeline::originalDelays(const GifAnimation &a)
{
    int shortest = 0, longest = 0;
    for (int i = 0; i < a.frames.size(); ++i) {
        const auto &f = a.frames[i];
        const int delay = a.delays.contains(f.index) ? a.delays.value(f.index)
                          : a.source && f.sourceIndex < a.source->delays.size() ? a.source->delays[f.sourceIndex] : 100;
        shortest = i ? std::min(shortest, delay) : delay; longest = i ? std::max(longest, delay) : delay;
    }
    if (a.frames.isEmpty()) return {};
    return (shortest == longest ? QString::number(shortest) : QStringLiteral("%1–%2").arg(shortest).arg(longest)) + QStringLiteral(" ms");
}
void GifTimeline::refresh(bool canvasOnly)
{
    const auto *l = session_.activeLayer();
    const bool visible = !canvasOnly && l && l->gif;
    if (!visible || l->id != layerId_) stop();
    setVisible(visible); if (!visible) return;
    layerId_ = l->id;
    refreshing_ = true;
    const auto &a = *l->gif;
    bool rebuild = frames_->count() != a.frames.size();
    if (!rebuild) for (int i = 0; i < a.frames.size(); ++i) if (frames_->item(i)->data(Qt::UserRole).toInt() != a.frames[i].index) { rebuild = true; break; }
    if (rebuild) frames_->clear();
    QListWidgetItem *current = nullptr;
    for (int i = 0; i < a.frames.size(); ++i) {
        const auto &f = a.frames[i];
        auto *item = rebuild ? new QListWidgetItem(frames_) : frames_->item(i);
        // Drawn again only when the frame's pixels or whether it is shown change, so playback stays cheap.
        const bool edited = a.pixels.contains(f.index), selected = f.index == a.selected;
        const QImage &source = edited ? a.pixels[f.index] : a.source->thumbnails.value(f.sourceIndex);
        const QString key = QStringLiteral("%1:%2").arg(edited ? source.cacheKey() : -1 - f.sourceIndex).arg(selected);
        if (item->data(Qt::UserRole + 1).toString() != key) {
            const QImage thumb = edited ? source.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation) : source;
            const QPixmap pixmap = framePixmap(thumb, selected, palette(), devicePixelRatioF());
            QIcon icon; icon.addPixmap(pixmap, QIcon::Normal); icon.addPixmap(pixmap, QIcon::Selected);
            item->setIcon(icon); item->setData(Qt::UserRole + 1, key);
        }
        item->setText(QString::number(i + 1)); item->setData(Qt::UserRole, f.index);
        item->setToolTip(tr("Frame %1 · %2 ms").arg(i + 1).arg(a.delay(f.index)));
        item->setData(Qt::AccessibleTextRole, tr("Frame %1").arg(i + 1));
        if (f.index == a.selected) current = item;
    }
    if (current) { frames_->setCurrentItem(current); frames_->scrollToItem(current); }
    // The frame scrubber and "03 / 12" (mac 8c6ebeb); a lone frame has nothing to scrub.
    // The frame asked for, while it's still being shown (mac shownFrame).
    const int shown = requestedFrame_.value_or(a.selected);
    int number = 1;
    for (int i = 0; i < a.frames.size(); ++i) if (a.frames[i].index == shown) number = i + 1;
    scrubber_->setRange(1, std::max(1, int(a.frames.size())));
    if (!scrubber_->isSliderDown()) scrubber_->setValue(number);
    scrubber_->setEnabled(a.frames.size() > 1);
    counter_->setText(frameCounter(number, int(a.frames.size())));
    counter_->setAccessibleName(tr("Frame %1 of %2").arg(number).arg(a.frames.size()));
    delay_->setText(a.sameDelay ? tr("%1 ms").arg(a.uniformDelay) : originalDelays(a));
    if (auto *original = delayMenu_->findChild<QAction *>("gifDelayOriginal", Qt::FindDirectChildrenOnly))
        original->setText(tr("%1 · Original").arg(originalDelays(a)));
    for (auto *action : delayMenu_->actions())
        if (action->isCheckable()) action->setChecked(action->data().isValid() ? a.sameDelay && action->data().toInt() == a.uniformDelay : !a.sameDelay);
    updatePlayButton(); if (a.frames.size() < 2) stop();
    refreshing_ = false;
    if (playing_) {
        QVector<int> identities; QVector<qint64> durations;
        for (const auto &f : a.frames) { identities.append(f.index); durations.append(qint64(std::max(20, a.delay(f.index))) * 1000); }
        if (identities != playingFrames_ || durations != playingDurations_) startPlayback();
    }
}
}
