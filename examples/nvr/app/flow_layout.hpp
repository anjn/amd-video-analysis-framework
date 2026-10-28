#pragma once
#include <QLayout>
#include <QWidget>
#include <algorithm>
namespace amdva::nvr {
// Left-to-right layout that wraps to the next row when the width runs out.
class FlowLayout : public QLayout {
  public:
    explicit FlowLayout(QWidget *parent = nullptr, int spacing = 4)
        : QLayout(parent), spacing_(spacing) {
        setContentsMargins(0, 0, 0, 0);
    }
    ~FlowLayout() override {
        while (auto *item = takeAt(0))
            delete item;
    }
    void addItem(QLayoutItem *item) override { items_.append(item); }
    int count() const override { return int(items_.size()); }
    QLayoutItem *itemAt(int index) const override { return items_.value(index); }
    QLayoutItem *takeAt(int index) override {
        return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;
    }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange({0, 0, width, 0}, false); }
    void setGeometry(const QRect &rect) override {
        QLayout::setGeometry(rect);
        arrange(rect, true);
    }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override {
        QSize size;
        for (auto *item : items_)
            size = size.expandedTo(item->minimumSize());
        const auto m = contentsMargins();
        return size + QSize(m.left() + m.right(), m.top() + m.bottom());
    }

  private:
    int arrange(const QRect &rect, bool apply) const {
        const auto m = contentsMargins();
        const auto area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
        int x = area.x(), y = area.y(), row = 0;
        for (auto *item : items_) {
            const auto hint = item->sizeHint();
            if (x + hint.width() > area.right() + 1 && row > 0) {
                x = area.x();
                y += row + spacing_;
                row = 0;
            }
            if (apply)
                item->setGeometry({{x, y}, hint});
            x += hint.width() + spacing_;
            row = std::max(row, hint.height());
        }
        return y + row - rect.y() + m.bottom();
    }
    QList<QLayoutItem *> items_;
    int spacing_;
};
} // namespace amdva::nvr
