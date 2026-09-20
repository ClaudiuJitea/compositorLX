#pragma once

#include <QWidget>
#include <QButtonGroup>
#include <QToolButton>
#include <QHBoxLayout>
#include <QStyle>
#include <QStyleOption>
#include <QPainter>
#include <QVector>
#include <QStringList>
#include <QIcon>
#include <QKeyEvent>
#include <algorithm>

namespace compositor {

class SegmentedGroup : public QWidget {
    Q_OBJECT
public:
    explicit SegmentedGroup(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("segmentedControl"));
        setProperty("segmented", true);
        setAttribute(Qt::WA_StyledBackground, true);
        layout_ = new QHBoxLayout(this);
        layout_->setContentsMargins(0, 0, 0, 0);
        layout_->setSpacing(0);
        setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        setFixedHeight(28);
    }

    void addButton(QAbstractButton *button)
    {
        button->setParent(this);
        button->setFixedHeight(28);
        button->setFocusPolicy(Qt::TabFocus);
        layout_->addWidget(button);
        buttons_.push_back(button);
        updateSegmentPositions();
    }

    int count() const { return buttons_.size(); }

    QAbstractButton *buttonAt(int index) const
    {
        return (index >= 0 && index < buttons_.size()) ? buttons_[index] : nullptr;
    }

    QSize sizeHint() const override
    {
        int width = 0;
        for (const auto *b : buttons_) {
            if (b->property("segmentIconOnly").toBool()) {
                width += 32;
            } else {
                width += std::max(b->sizeHint().width(), b->minimumSizeHint().width());
            }
        }
        return QSize(width, 28);
    }

    QSize minimumSizeHint() const override
    {
        return sizeHint();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStyleOption opt;
        opt.initFrom(this);
        QPainter p(this);
        style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
    }

    void updateSegmentPositions()
    {
        for (int i = 0; i < buttons_.size(); ++i) {
            const char *pos = "middle";
            if (buttons_.size() == 1) pos = "only";
            else if (i == 0) pos = "first";
            else if (i == buttons_.size() - 1) pos = "last";
            buttons_[i]->setProperty("segmentPos", pos);
            buttons_[i]->style()->unpolish(buttons_[i]);
            buttons_[i]->style()->polish(buttons_[i]);
        }
    }

    QHBoxLayout *layout_ = nullptr;
    QVector<QAbstractButton *> buttons_;
};

class SegmentedControl : public SegmentedGroup {
    Q_OBJECT
public:
    explicit SegmentedControl(QWidget *parent = nullptr)
        : SegmentedGroup(parent), group_(new QButtonGroup(this))
    {
        group_->setExclusive(true);
        connect(group_, &QButtonGroup::idClicked, this, [this](int id) {
            if (currentIndex_ != id) {
                currentIndex_ = id;
                emit currentIndexChanged(id);
            }
        });
    }

    SegmentedControl(const QStringList &items, QWidget *parent = nullptr)
        : SegmentedControl(parent)
    {
        addItems(items);
    }

    void addItem(const QString &text, const QIcon &icon = QIcon(), const QString &toolTip = QString())
    {
        const int index = buttons_.size();
        auto *btn = new QToolButton(this);
        btn->setCheckable(true);
        btn->setAutoRaise(false);
        if (!icon.isNull()) {
            btn->setIcon(icon);
            btn->setIconSize(QSize(16, 16));
        }
        if (!text.isEmpty()) {
            btn->setText(text);
            if (!icon.isNull()) {
                btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            } else {
                btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
            }
            btn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
            btn->setFixedHeight(28);
        } else {
            btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
            btn->setProperty("segmentIconOnly", true);
            btn->setFixedSize(32, 28);
        }
        if (!toolTip.isEmpty()) {
            btn->setToolTip(toolTip);
        }
        addButton(btn);
        group_->addButton(btn, index);

        if (index == 0) {
            btn->setChecked(true);
            currentIndex_ = 0;
        }
    }

    void addItem(const QIcon &icon, const QString &toolTip = QString())
    {
        addItem(QString(), icon, toolTip);
    }

    void addItems(const QStringList &items)
    {
        for (const auto &item : items) addItem(item);
    }

    int currentIndex() const { return currentIndex_; }

    void setCurrentIndex(int index)
    {
        if (index < 0 || index >= buttons_.size()) return;
        if (currentIndex_ == index) {
            if (auto *b = group_->button(index)) b->setChecked(true);
            return;
        }
        currentIndex_ = index;
        if (auto *b = group_->button(index)) b->setChecked(true);
        if (!signalsBlocked()) emit currentIndexChanged(index);
    }

    void setItemToolTip(int index, const QString &toolTip)
    {
        if (index >= 0 && index < buttons_.size()) {
            buttons_[index]->setToolTip(toolTip);
        }
    }

    void setItemEnabled(int index, bool enabled)
    {
        if (index >= 0 && index < buttons_.size()) {
            buttons_[index]->setEnabled(enabled);
        }
    }

    QToolButton *button(int index) const
    {
        return (index >= 0 && index < buttons_.size()) ? qobject_cast<QToolButton *>(buttons_[index]) : nullptr;
    }

signals:
    void currentIndexChanged(int index);

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Left) {
            if (currentIndex_ > 0) setCurrentIndex(currentIndex_ - 1);
            event->accept();
        } else if (event->key() == Qt::Key_Right) {
            if (currentIndex_ + 1 < buttons_.size()) setCurrentIndex(currentIndex_ + 1);
            event->accept();
        } else {
            SegmentedGroup::keyPressEvent(event);
        }
    }

private:
    QButtonGroup *group_ = nullptr;
    int currentIndex_ = -1;
};

} // namespace compositor
