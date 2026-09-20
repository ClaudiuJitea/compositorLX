#pragma once

#include <QLayout>
#include <QWidget>
#include <algorithm>

namespace compositor {

// Wrap related controls as a unit, ignoring options hidden by the current tool.
class ToolOptionsLayout final : public QLayout {
public:
    explicit ToolOptionsLayout(QWidget *parent) : QLayout(parent) { setContentsMargins(12, 7, 12, 7); }
    ~ToolOptionsLayout() override { while (auto *item = takeAt(0)) delete item; }
    void addGroup(std::initializer_list<QWidget *> widgets)
    {
        ++group_;
        for (auto *widget : widgets) addWidget(widget);
    }
    void addItem(QLayoutItem *item) override { items_.append({item, group_}); invalidate(); }
    int count() const override { return items_.size(); }
    QLayoutItem *itemAt(int index) const override { return index >= 0 && index < count() ? items_[index].item : nullptr; }
    QLayoutItem *takeAt(int index) override
    {
        if (index < 0 || index >= count()) return nullptr;
        auto *item = items_.takeAt(index).item; invalidate(); return item;
    }
    Qt::Orientations expandingDirections() const override { return Qt::Horizontal; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), false); }
    QSize minimumSize() const override
    {
        QSize result;
        for (const auto &entry : items_) if (!entry.item->isEmpty()) result = result.expandedTo(entry.item->minimumSize());
        const auto m = contentsMargins();
        return result + QSize(m.left() + m.right(), m.top() + m.bottom());
    }
    QSize sizeHint() const override { return QSize(900, heightForWidth(900)); }
    void setGeometry(const QRect &rect) override { QLayout::setGeometry(rect); arrange(rect, true); }

private:
    int arrange(const QRect &rect, bool apply) const
    {
        const auto m = contentsMargins();
        const int left = rect.x() + m.left(), right = rect.x() + rect.width() - m.right();
        int x = left, y = rect.y() + m.top(), rowHeight = 0;
        constexpr int itemSpacing = 6;
        constexpr int groupSpacing = 20;
        constexpr int controlHeight = 28;
        for (int i = 0; i < items_.size();) {
            const int start = i, group = items_[i].group;
            int groupWidth = 0;
            int visibleCount = 0;
            for (; i < items_.size() && items_[i].group == group; ++i) {
                if (!items_[i].item->isEmpty()) {
                    groupWidth += items_[i].item->sizeHint().width();
                    ++visibleCount;
                }
            }
            if (!visibleCount) continue;
            groupWidth += (visibleCount - 1) * itemSpacing;
            if (x > left && x + groupWidth > right) { x = left; y += rowHeight + 7; rowHeight = 0; }
            for (int j = start; j < i; ++j) {
                auto *item = items_[j].item;
                if (item->isEmpty()) continue;
                const QSize size = item->sizeHint();
                if (x > left && x + size.width() > right) { x = left; y += rowHeight + 7; rowHeight = 0; }
                if (apply) item->setGeometry(QRect(x, y, size.width(), controlHeight));
                x += size.width() + itemSpacing;
                rowHeight = std::max(rowHeight, controlHeight);
            }
            x += (groupSpacing - itemSpacing);
        }
        return y - rect.y() + rowHeight + m.bottom();
    }
    struct Entry { QLayoutItem *item; int group; };
    QList<Entry> items_;
    int group_ = 0;
};

} // namespace compositor
