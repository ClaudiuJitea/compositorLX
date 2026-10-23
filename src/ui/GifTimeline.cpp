#include "ui/GifTimeline.h"
#include "ui/EditorIcons.h"
#include <QAction>
#include <QActionGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
namespace compositor {
static constexpr int thumbnailSide = 72;

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
    setObjectName("gifTimeline"); setFixedHeight(110); setAttribute(Qt::WA_StyledBackground);
    auto *layout = new QHBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    auto *controls = new QVBoxLayout; controls->setContentsMargins(18, 0, 18, 0); controls->setSpacing(8); controls->setAlignment(Qt::AlignCenter);
    play_ = new QToolButton(this); play_->setObjectName("gifPlay"); play_->setFixedSize(70, 40); play_->setIconSize(QSize(18, 18));
    play_->setIcon(editorIcon(34)); play_->setToolTip(tr("Play")); play_->setAccessibleName(tr("Play"));
    // The delay as a menu of common ones: Original, each frame for its own, or every frame for the same time.
    delay_ = new QToolButton(this); delay_->setObjectName("gifDelay"); delay_->setAutoRaise(true);
    delay_->setPopupMode(QToolButton::InstantPopup); delay_->setToolTip(tr("How long each frame shows"));
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
    controls->addWidget(play_, 0, Qt::AlignHCenter); controls->addWidget(delay_, 0, Qt::AlignHCenter);
    layout->addLayout(controls);
    auto *divider = new QFrame(this); divider->setFrameShape(QFrame::VLine); divider->setFrameShadow(QFrame::Plain); divider->setObjectName("gifDivider");
    layout->addWidget(divider);
    frames_ = new QListWidget(this); frames_->setObjectName("gifFrames"); frames_->setViewMode(QListView::IconMode);
    frames_->setFlow(QListView::LeftToRight); frames_->setWrapping(false); frames_->setMovement(QListView::Static);
    frames_->setIconSize(QSize(thumbnailSide, thumbnailSide)); frames_->setSpacing(4); frames_->setFrameShape(QFrame::NoFrame);
    frames_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); frames_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(frames_, 1);
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
    timer_ = new QTimer(this); timer_->setSingleShot(true);
    connect(play_, &QToolButton::clicked, this, [this] {
        if (timer_->isActive()) { stop(); return; }
        const auto *l = session_.activeLayer(); if (!l || !l->gif || l->gif->frames.size() < 2) return;
        play_->setIcon(editorIcon(35)); play_->setToolTip(tr("Pause")); play_->setAccessibleName(tr("Pause"));
        timer_->start(l->gif->delay(l->gif->selected));
    });
    // While playing, each frame shows for its delay and then the next, round and round.
    connect(timer_, &QTimer::timeout, this, [this] {
        const auto *l = session_.activeLayer(); if (!l || !l->gif || l->id != layerId_ || !isVisible() || l->gif->frames.size() < 2) { stop(); return; }
        const auto &a = *l->gif;
        int pos = 0; for (int i = 0; i < a.frames.size(); ++i) if (a.frames[i].index == a.selected) pos = i;
        if (!session_.selectGifFrame(a.frames[(pos + 1) % a.frames.size()].index)) { stop(); return; }
        changed_(); refresh();
        l = session_.activeLayer(); if (l && l->gif) timer_->start(l->gif->delay(l->gif->selected));
    });
    connect(frames_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (refreshing_ || row < 0) return;
        stop(); if (session_.selectGifFrame(frames_->item(row)->data(Qt::UserRole).toInt())) { changed_(); refresh(); }
    });
    connect(group, &QActionGroup::triggered, this, [this, original](QAction *action) {
        if (refreshing_) return;
        stop();
        if (session_.setGifTiming(action == original ? std::nullopt : std::optional<int>(action->data().toInt()))) changed_();
        refresh();
    });
    hide();
}
void GifTimeline::editFrame(QAction *action, bool duplicate)
{
    stop();
    const std::optional<int> index = action->data().isValid() ? std::optional<int>(action->data().toInt()) : std::nullopt;
    if (duplicate ? session_.duplicateGifFrame(index) : session_.deleteGifFrame(index)) { changed_(); refresh(); }
}
void GifTimeline::stop()
{
    timer_->stop(); play_->setIcon(editorIcon(34)); play_->setToolTip(tr("Play")); play_->setAccessibleName(tr("Play"));
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
    delay_->setText(a.sameDelay ? tr("%1ms").arg(a.uniformDelay) : tr("Original"));
    for (auto *action : delayMenu_->actions())
        if (action->isCheckable()) action->setChecked(action->data().isValid() ? a.sameDelay && action->data().toInt() == a.uniformDelay : !a.sameDelay);
    play_->setEnabled(a.frames.size() > 1); if (a.frames.size() < 2) stop();
    refreshing_ = false;
}
}
