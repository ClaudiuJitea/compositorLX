#include "ui/ColorPickerDialog.h"

#include "ui/NumericScrub.h"

#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace compositor {

static constexpr int kFieldSize = 256;

class ColorPickerField final : public QWidget {
public:
    explicit ColorPickerField(ColorPickerDialog *owner) : QWidget(owner), owner_(owner)
    {
        setObjectName(QStringLiteral("colorPickerField")); setFixedSize(kFieldSize, kFieldSize);
        setCursor(Qt::CrossCursor); setAccessibleName(tr("Saturation and brightness"));
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        QLinearGradient across(0, 0, width(), 0); across.setColorAt(0, Qt::white); across.setColorAt(1, PickerHSB(owner_->hsb_.hue, 1, 1).rgb());
        p.fillRect(rect(), across);
        QLinearGradient down(0, 0, 0, height()); down.setColorAt(0, QColor(0, 0, 0, 0)); down.setColorAt(1, Qt::black);
        p.fillRect(rect(), down);
        p.setRenderHint(QPainter::Antialiasing);
        const QPointF at(owner_->hsb_.saturation * (width() - 1), (1 - owner_->hsb_.brightness) * (height() - 1));
        p.setBrush(Qt::NoBrush); p.setPen(QPen(Qt::black, 2.5)); p.drawEllipse(at, 6, 6);
        p.setPen(QPen(Qt::white, 1.5)); p.drawEllipse(at, 6, 6);
        p.setPen(QColor(0, 0, 0, 150)); p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
    void mousePressEvent(QMouseEvent *e) override { drag(e->position()); }
    void mouseMoveEvent(QMouseEvent *e) override { if (e->buttons() & Qt::LeftButton) drag(e->position()); }
private:
    void drag(const QPointF &at)
    {
        owner_->hsb_.saturation = std::clamp(at.x() / (width() - 1), 0.0, 1.0);
        owner_->hsb_.brightness = 1 - std::clamp(at.y() / (height() - 1), 0.0, 1.0);
        owner_->changed();
    }
    ColorPickerDialog *owner_;
};

class ColorHueStrip final : public QWidget {
public:
    explicit ColorHueStrip(ColorPickerDialog *owner) : QWidget(owner), owner_(owner)
    {
        setObjectName(QStringLiteral("colorPickerHue")); setFixedSize(34, kFieldSize); setAccessibleName(tr("Hue"));
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QRect bar(7, 0, 20, height());
        QLinearGradient g(0, 0, 0, height());
        for (int i = 0; i <= 6; ++i) g.setColorAt(i / 6.0, PickerHSB(360 - i * 60, 1, 1).rgb());
        p.fillRect(bar, g);
        p.setPen(QColor(0, 0, 0, 150)); p.drawRect(bar.adjusted(0, 0, -1, -1));
        const double y = (1 - owner_->hsb_.hue / 360.0) * (height() - 1);
        p.setRenderHint(QPainter::Antialiasing); p.setPen(Qt::NoPen); p.setBrush(palette().text());
        p.drawPolygon(QPolygonF{QPointF(0, y - 5), QPointF(7, y), QPointF(0, y + 5)});
        p.drawPolygon(QPolygonF{QPointF(width(), y - 5), QPointF(width() - 7, y), QPointF(width(), y + 5)});
    }
    void mousePressEvent(QMouseEvent *e) override { drag(e->position().y()); }
    void mouseMoveEvent(QMouseEvent *e) override { if (e->buttons() & Qt::LeftButton) drag(e->position().y()); }
private:
    void drag(double y)
    {
        owner_->hsb_.hue = (1 - std::clamp(y / (height() - 1), 0.0, 1.0)) * 360;
        owner_->changed();
    }
    ColorPickerDialog *owner_;
};

ColorPickerDialog::ColorPickerDialog(const QColor &initial, QWidget *parent)
    : QDialog(parent), hsb_(initial.isValid() ? initial.toRgb() : QColor(Qt::black)), original_(palette::quantized(hsb_.rgb())), selected_(original_)
{
    setWindowTitle(tr("Color Picker"));
    auto *root = new QHBoxLayout(this); root->setContentsMargins(20, 20, 20, 20); root->setSpacing(14);
    field_ = new ColorPickerField(this); strip_ = new ColorHueStrip(this);
    root->addWidget(field_); root->addWidget(strip_);
    auto *side = new QVBoxLayout; side->setSpacing(8); root->addLayout(side);

    auto *top = new QHBoxLayout; top->setSpacing(16); side->addLayout(top);
    preview_ = new QLabel(this); preview_->setObjectName(QStringLiteral("colorPickerPreview")); preview_->setFixedSize(64, 64);
    preview_->setToolTip(tr("New color above, current below"));
    top->addWidget(preview_, 0, Qt::AlignTop);
    auto *buttons = new QVBoxLayout; top->addLayout(buttons);
    auto *ok = new QPushButton(tr("OK"), this); ok->setDefault(true); ok->setObjectName(QStringLiteral("colorPickerOk"));
    auto *cancel = new QPushButton(tr("Cancel"), this); cancel->setObjectName(QStringLiteral("colorPickerCancel"));
    buttons->addWidget(ok); buttons->addWidget(cancel);
    connect(ok, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    side->addSpacing(12);

    auto *grid = new QGridLayout; grid->setHorizontalSpacing(8); grid->setVerticalSpacing(6); side->addLayout(grid);
    const auto channel = [&](int row, const QString &text, const QString &name, QSpinBox *&spin) {
        auto *label = new QLabel(text, this); label->setObjectName(QStringLiteral("color%1Label").arg(name));
        spin = new QSpinBox(this); spin->setRange(0, 255); spin->setObjectName(QStringLiteral("color%1Spin").arg(name));
        spin->setAccessibleName(name); label->setBuddy(spin);
        grid->addWidget(label, row, 0); grid->addWidget(spin, row, 1);
        attachScrubToLabel(label, spin, 1.0, 1.0);
        connect(spin, &QSpinBox::valueChanged, this, [this](int) {
            if (syncing_) return;
            hsb_.setRGB(QColor(red_->value(), green_->value(), blue_->value()));
            changed();
        });
    };
    channel(0, tr("R"), QStringLiteral("Red"), red_); channel(1, tr("G"), QStringLiteral("Green"), green_); channel(2, tr("B"), QStringLiteral("Blue"), blue_);
    grid->addWidget(new QLabel(QStringLiteral("#"), this), 3, 0);
    hex_ = new QLineEdit(this); hex_->setObjectName(QStringLiteral("colorPickerHex")); hex_->setMaxLength(7); hex_->setAccessibleName(tr("Hex color"));
    grid->addWidget(hex_, 3, 1);
    connect(hex_, &QLineEdit::editingFinished, this, &ColorPickerDialog::commitHex);
    side->addStretch();
    hint_ = new QLabel(tr("Click the canvas to sample"), this); hint_->setObjectName(QStringLiteral("colorPickerHint"));
    QFont small = hint_->font(); small.setPointSizeF(small.pointSizeF() * .85); hint_->setFont(small);
    side->addWidget(hint_);
    connect(this, &QDialog::accepted, this, [this] { selected_ = currentColor(); });
    syncControls();
}

void ColorPickerDialog::setCurrentColor(const QColor &color)
{
    if (!color.isValid()) return;
    hsb_.setRGB(color.toRgb());
    changed();
}

void ColorPickerDialog::setCanvasSamplingHint(bool shown) { hint_->setVisible(shown); }

void ColorPickerDialog::changed(bool)
{
    syncControls();
    emit currentColorChanged(currentColor());
}

void ColorPickerDialog::commitHex()
{
    if (const auto parsed = palette::fromHex(hex_->text())) { hsb_.setRGB(*parsed); changed(); }
    else syncControls();
}

void ColorPickerDialog::syncControls()
{
    syncing_ = true;
    const QColor c = currentColor();
    red_->setValue(c.red()); green_->setValue(c.green()); blue_->setValue(c.blue());
    if (!hex_->hasFocus()) hex_->setText(palette::hex(c));
    QPixmap swatch(64, 64); QPainter p(&swatch);
    p.fillRect(0, 0, 64, 32, c); p.fillRect(0, 32, 64, 32, original_);
    p.setPen(QColor(0, 0, 0, 150)); p.drawRect(0, 0, 63, 63);
    p.end(); preview_->setPixmap(swatch);
    field_->update(); strip_->update();
    syncing_ = false;
}

QColor ColorPickerDialog::getColor(const QColor &initial, QWidget *parent, const QString &title)
{
    ColorPickerDialog picker(initial, parent);
    picker.setCanvasSamplingHint(false);
    if (!title.isEmpty()) picker.setWindowTitle(title);
    return picker.exec() == QDialog::Accepted ? picker.selectedColor() : QColor();
}

} // namespace compositor
