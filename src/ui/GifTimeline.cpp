#include "ui/GifTimeline.h"
#include <QListWidget>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QTimer>
namespace compositor {
GifTimeline::GifTimeline(EditorSession &session, std::function<void()> changed, QWidget *parent)
    : QWidget(parent), session_(session), changed_(std::move(changed))
{
    setObjectName("gifTimeline");
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(8, 6, 8, 6); layout->setSpacing(4);
    auto *row = new QHBoxLayout;
    play_ = new QPushButton(tr("▶ Play"), this); play_->setObjectName("gifPlay"); play_->setMinimumWidth(90);
    auto *copy = new QPushButton(tr("Duplicate"), this); copy->setObjectName("gifDuplicate");
    remove_ = new QPushButton(tr("Delete"), this); remove_->setObjectName("gifDelete");
    delay_ = new QDoubleSpinBox(this); delay_->setObjectName("gifDelay"); delay_->setRange(.02, 655.35); delay_->setDecimals(2); delay_->setSingleStep(.01); delay_->setSuffix(tr(" s"));
    same_ = new QCheckBox(tr("Same for all"), this); same_->setObjectName("gifSameDelay");
    auto *original = new QPushButton(tr("Original delay"), this);
    row->addWidget(play_); row->addWidget(copy); row->addWidget(remove_); row->addStretch(); row->addWidget(new QLabel(tr("Delay"), this)); row->addWidget(delay_); row->addWidget(same_); row->addWidget(original);
    layout->addLayout(row);
    frames_ = new QListWidget(this); frames_->setObjectName("gifFrames"); frames_->setViewMode(QListView::IconMode);
    frames_->setFlow(QListView::LeftToRight); frames_->setWrapping(false); frames_->setMovement(QListView::Static);
    frames_->setIconSize(QSize(48, 48)); frames_->setFixedHeight(88); layout->addWidget(frames_);
    timer_ = new QTimer(this); timer_->setSingleShot(true);
    connect(play_, &QPushButton::clicked, this, [this] {
        if (timer_->isActive()) { stop(); return; }
        const auto *l = session_.activeLayer(); if (!l || !l->gif) return;
        play_->setText(tr("❚❚ Pause")); timer_->start(l->gif->delay(l->gif->selected));
    });
    connect(timer_, &QTimer::timeout, this, [this] {
        const auto *l = session_.activeLayer(); if (!l || !l->gif || l->id != layerId_ || !isVisible()) { stop(); return; }
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
    connect(copy, &QPushButton::clicked, this, [this] { stop(); if (session_.duplicateGifFrame()) { changed_(); refresh(); } });
    connect(remove_, &QPushButton::clicked, this, [this] { stop(); if (session_.deleteGifFrame()) { changed_(); refresh(); } });
    const auto timing = [this] { if (refreshing_) return; stop(); if (session_.setGifDelay(qRound(delay_->value() * 1000), same_->isChecked())) { changed_(); refresh(); } };
    connect(delay_, &QDoubleSpinBox::editingFinished, this, timing);
    connect(same_, &QCheckBox::toggled, this, timing);
    connect(original, &QPushButton::clicked, this, [this] { stop(); if (session_.setGifDelay(100, false, true)) { changed_(); refresh(); } });
    hide();
}
void GifTimeline::stop() { timer_->stop(); play_->setText(tr("▶ Play")); }
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
    for (int i = 0; i < a.frames.size(); ++i) {
        const auto &f = a.frames[i];
        auto *item = rebuild ? new QListWidgetItem(frames_) : frames_->item(i);
        const QImage thumb = a.pixels.contains(f.index) ? a.pixels.value(f.index).scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation) : a.source->thumbnails.value(f.sourceIndex);
        item->setIcon(QPixmap::fromImage(thumb)); item->setText(QStringLiteral("%1 · %2 s").arg(i + 1).arg(a.delay(f.index) / 1000.0, 0, 'f', 2)); item->setData(Qt::UserRole, f.index);
        if (f.index == a.selected) frames_->setCurrentRow(i);
    }
    delay_->setValue(a.delay(a.selected) / 1000.0); same_->setChecked(a.sameDelay); remove_->setEnabled(a.frames.size() > 1);
    refreshing_ = false;
}
}
