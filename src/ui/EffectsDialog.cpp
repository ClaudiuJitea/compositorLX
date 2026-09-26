#include "ui/EffectsDialog.h"
#include "ui/NumericScrub.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace compositor {

namespace {

QWidget *createSliderField(QDoubleSpinBox *box, double minVal, double maxVal, double sliderMax,
                           const QString &suffix, const std::function<void()> &onChanged)
{
    auto *container = new QWidget;
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    box->setRange(minVal, maxVal);
    box->setSuffix(suffix);
    box->setMinimumWidth(80);

    auto *slider = new QSlider(Qt::Horizontal, container);
    slider->setRange(0, 1000);
    slider->setMinimumWidth(130);

    const auto posFromVal = [minVal, sliderMax](double v) {
        const double clamped = std::clamp(v, minVal, sliderMax);
        return qRound((clamped - minVal) / (sliderMax - minVal) * 1000.0);
    };

    const auto valFromPos = [minVal, sliderMax](int pos) {
        return minVal + (pos / 1000.0) * (sliderMax - minVal);
    };

    slider->setValue(posFromVal(box->value()));

    QObject::connect(slider, &QSlider::valueChanged, box, [box, valFromPos, onChanged](int pos) {
        const QSignalBlocker blocker(box);
        box->setValue(valFromPos(pos));
        onChanged();
    });

    QObject::connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), slider, [slider, posFromVal, onChanged](double v) {
        const QSignalBlocker blocker(slider);
        slider->setValue(posFromVal(v));
        onChanged();
    });

    layout->addWidget(slider, 1);
    layout->addWidget(box);
    return container;
}

} // namespace

EffectsDialog::EffectsDialog(QWidget *parent, EditorSession &session, const QUuid &layerId,
                             std::function<void()> onPreview,
                             std::optional<LayerEffectKind> initialKind)
    : QDialog(parent)
    , session_(session)
    , layerId_(layerId)
    , onPreview_(std::move(onPreview))
{
    setWindowTitle(tr("Layer Effects"));
    resize(520, 360);

    originalEffects_ = session_.layerEffects(layerId_).value_or(LayerEffects());
    currentEffects_ = originalEffects_;

    setupUi();
    loadFromEffects(currentEffects_);

    if (initialKind) {
        const int row = static_cast<int>(*initialKind);
        if (row >= 0 && row < effectList_->count()) {
            effectList_->setCurrentRow(row);
        }
    }
}

void EffectsDialog::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(10);

    auto *contentLayout = new QHBoxLayout;
    contentLayout->setSpacing(12);

    // Left pane: Effect List
    effectList_ = new QListWidget(this);
    effectList_->setFixedWidth(160);
    const QStringList effectNames = {
        tr("Stroke"),
        tr("Drop Shadow"),
        tr("Color Overlay"),
        tr("Inner Shadow"),
        tr("Outer Glow"),
        tr("Inner Glow")
    };
    for (const QString &name : effectNames) {
        auto *item = new QListWidgetItem(name, effectList_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
    }
    contentLayout->addWidget(effectList_);

    // Right pane: Stacked Widget
    pages_ = new QStackedWidget(this);
    pages_->addWidget(createStrokePage());
    pages_->addWidget(createDropShadowPage());
    pages_->addWidget(createColorOverlayPage());
    pages_->addWidget(createInnerShadowPage());
    pages_->addWidget(createOuterGlowPage());
    pages_->addWidget(createInnerGlowPage());
    contentLayout->addWidget(pages_, 1);

    mainLayout->addLayout(contentLayout, 1);

    // Bottom row: Preview checkbox and Dialog buttons
    auto *bottomLayout = new QHBoxLayout;
    previewCheckbox_ = new QCheckBox(tr("Preview"), this);
    previewCheckbox_->setChecked(true);
    connect(previewCheckbox_, &QCheckBox::toggled, this, [this](bool) {
        updatePreview();
    });
    bottomLayout->addWidget(previewCheckbox_);

    auto *clearButton = new QPushButton(tr("Clear All"), this);
    connect(clearButton, &QPushButton::clicked, this, [this]() {
        currentEffects_ = LayerEffects();
        loadFromEffects(currentEffects_);
        updatePreview();
    });
    bottomLayout->addWidget(clearButton);
    bottomLayout->addStretch();

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &EffectsDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &EffectsDialog::reject);
    bottomLayout->addWidget(buttonBox);

    mainLayout->addLayout(bottomLayout);

    connect(effectList_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
    connect(effectList_, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (updating_) return;
        const int row = effectList_->row(item);
        const bool checked = (item->checkState() == Qt::Checked);
        const auto kind = static_cast<LayerEffectKind>(row);
        currentEffects_.setEnabled(kind, checked);
        if (checked && !currentEffects_.contains(kind)) {
            // Initialize default effect if previously unset
            switch (kind) {
            case LayerEffectKind::Stroke: currentEffects_.stroke = StrokeEffect(); break;
            case LayerEffectKind::DropShadow: currentEffects_.shadow = ShadowEffect(); break;
            case LayerEffectKind::ColorOverlay: currentEffects_.colorOverlay = ColorOverlayEffect(); break;
            case LayerEffectKind::InnerShadow: currentEffects_.innerShadow = InnerShadowEffect(); break;
            case LayerEffectKind::OuterGlow: currentEffects_.outerGlow = OuterGlowEffect(); break;
            case LayerEffectKind::InnerGlow: currentEffects_.innerGlow = InnerGlowEffect(); break;
            }
        }
        loadFromEffects(currentEffects_);
        updatePreview();
    });

    effectList_->setCurrentRow(0);
}

QWidget *EffectsDialog::createStrokePage()
{
    auto *page = new QWidget;
    auto *layout = new QFormLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    strokeEnabled_ = new QCheckBox(tr("Enable Stroke"), page);
    connect(strokeEnabled_, &QCheckBox::toggled, this, [this](bool checked) {
        if (updating_) return;
        currentEffects_.setEnabled(LayerEffectKind::Stroke, checked);
        if (checked && !currentEffects_.stroke) currentEffects_.stroke = StrokeEffect();
        loadFromEffects(currentEffects_);
        updatePreview();
    });
    layout->addRow(strokeEnabled_);

    strokePosition_ = new QComboBox(page);
    strokePosition_->addItem(tr("Outside"), false);
    strokePosition_->addItem(tr("Inside"), true);
    connect(strokePosition_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (updating_) return;
        if (!currentEffects_.stroke) currentEffects_.stroke = StrokeEffect();
        currentEffects_.stroke->inside = strokePosition_->currentData().toBool();
        updatePreview();
    });
    layout->addRow(tr("Position"), strokePosition_);

    strokeColorButton_ = new QPushButton(page);
    connect(strokeColorButton_, &QPushButton::clicked, this, [this]() {
        pickColor(LayerEffectKind::Stroke, strokeColorButton_);
    });
    layout->addRow(tr("Color"), strokeColorButton_);

    strokeSize_ = new QDoubleSpinBox(page);
    strokeSize_->setObjectName(QStringLiteral("strokeSizeSpinBox"));
    auto *sizeField = createSliderField(strokeSize_, 0.0, 500.0, 50.0, tr(" px"), [this]() {
        if (updating_) return;
        if (!currentEffects_.stroke) currentEffects_.stroke = StrokeEffect();
        currentEffects_.stroke->size = strokeSize_->value();
        updatePreview();
    });
    layout->addRow(new ScrubLabel(tr("Size"), strokeSize_, 1.0, 1.0, page), sizeField);

    strokeOpacity_ = new QDoubleSpinBox(page);
    strokeOpacity_->setDecimals(0);
    auto *opacityField = createSliderField(strokeOpacity_, 0.0, 100.0, 100.0, tr(" %"), [this]() {
        if (updating_) return;
        if (!currentEffects_.stroke) currentEffects_.stroke = StrokeEffect();
        currentEffects_.stroke->opacity = strokeOpacity_->value() / 100.0;
        updatePreview();
    });
    layout->addRow(new ScrubLabel(tr("Opacity"), strokeOpacity_, 1.0, 1.0, page), opacityField);

    return page;
}

QWidget *EffectsDialog::createDropShadowPage()
{
    auto *page = new QWidget;
    auto *layout = new QFormLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    shadowEnabled_ = new QCheckBox(tr("Enable Drop Shadow"), page);
    connect(shadowEnabled_, &QCheckBox::toggled, this, [this](bool checked) {
        if (updating_) return;
        currentEffects_.setEnabled(LayerEffectKind::DropShadow, checked);
        if (checked && !currentEffects_.shadow) currentEffects_.shadow = ShadowEffect();
        loadFromEffects(currentEffects_);
        updatePreview();
    });
    layout->addRow(shadowEnabled_);

    shadowColorButton_ = new QPushButton(page);
    connect(shadowColorButton_, &QPushButton::clicked, this, [this]() {
        pickColor(LayerEffectKind::DropShadow, shadowColorButton_);
    });
    layout->addRow(tr("Color"), shadowColorButton_);

    shadowOpacity_ = new QDoubleSpinBox(page);
    shadowOpacity_->setDecimals(0);
    auto *opacityField = createSliderField(shadowOpacity_, 0.0, 100.0, 100.0, tr(" %"), [this]() {
        if (updating_) return;
        if (!currentEffects_.shadow) currentEffects_.shadow = ShadowEffect();
        currentEffects_.shadow->opacity = shadowOpacity_->value() / 100.0;
        updatePreview();
    });
    layout->addRow(new ScrubLabel(tr("Opacity"), shadowOpacity_, 1.0, 1.0, page), opacityField);

    shadowAngle_ = new QDoubleSpinBox(page);
    shadowAngle_->setDecimals(0);
    auto *angleField = createSliderField(shadowAngle_, -180.0, 180.0, 180.0, tr(" °"), [this]() {
        if (updating_) return;
        if (!currentEffects_.shadow) currentEffects_.shadow = ShadowEffect();
        currentEffects_.shadow->angle = shadowAngle_->value();
        updatePreview();
    });
    layout->addRow(new ScrubLabel(tr("Angle"), shadowAngle_, 1.0, 1.0, page), angleField);

    shadowDistance_ = new QDoubleSpinBox(page);
    shadowDistance_->setDecimals(0);
    auto *distField = createSliderField(shadowDistance_, 0.0, 5000.0, 100.0, tr(" px"), [this]() {
        if (updating_) return;
        if (!currentEffects_.shadow) currentEffects_.shadow = ShadowEffect();
        currentEffects_.shadow->distance = shadowDistance_->value();
        updatePreview();
    });
    layout->addRow(new ScrubLabel(tr("Distance"), shadowDistance_, 1.0, 1.0, page), distField);

    shadowBlur_ = new QDoubleSpinBox(page);
    shadowBlur_->setDecimals(0);
    auto *blurField = createSliderField(shadowBlur_, 0.0, 500.0, 100.0, tr(" px"), [this]() {
        if (updating_) return;
        if (!currentEffects_.shadow) currentEffects_.shadow = ShadowEffect();
        currentEffects_.shadow->blur = shadowBlur_->value();
        updatePreview();
    });
    layout->addRow(new ScrubLabel(tr("Blur"), shadowBlur_, 1.0, 1.0, page), blurField);

    return page;
}

QWidget *EffectsDialog::createColorOverlayPage()
{
    auto *page = new QWidget;
    auto *layout = new QFormLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    colorOverlayEnabled_ = new QCheckBox(tr("Enable Color Overlay"), page);
    connect(colorOverlayEnabled_, &QCheckBox::toggled, this, [this](bool checked) {
        if (updating_) return;
        currentEffects_.setEnabled(LayerEffectKind::ColorOverlay, checked);
        if (checked && !currentEffects_.colorOverlay) currentEffects_.colorOverlay = ColorOverlayEffect();
        loadFromEffects(currentEffects_);
        updatePreview();
    });
    layout->addRow(colorOverlayEnabled_);

    colorOverlayColorButton_ = new QPushButton(page);
    connect(colorOverlayColorButton_, &QPushButton::clicked, this, [this]() {
        pickColor(LayerEffectKind::ColorOverlay, colorOverlayColorButton_);
    });
    layout->addRow(tr("Color"), colorOverlayColorButton_);

    colorOverlayOpacity_ = new QDoubleSpinBox(page);
    colorOverlayOpacity_->setDecimals(0);
    auto *opacityField = createSliderField(colorOverlayOpacity_, 0.0, 100.0, 100.0, tr(" %"), [this]() {
        if (updating_) return;
        if (!currentEffects_.colorOverlay) currentEffects_.colorOverlay = ColorOverlayEffect();
        currentEffects_.colorOverlay->opacity = colorOverlayOpacity_->value() / 100.0;
        updatePreview();
    });
    layout->addRow(new ScrubLabel(tr("Opacity"), colorOverlayOpacity_, 1.0, 1.0, page), opacityField);

    return page;
}

QWidget *EffectsDialog::createInnerShadowPage()
{
    auto *page = new QWidget;
    auto *layout = new QFormLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    innerShadowEnabled_ = new QCheckBox(tr("Enable Inner Shadow"), page);
    connect(innerShadowEnabled_, &QCheckBox::toggled, this, [this](bool checked) {
        if (updating_) return;
        currentEffects_.setEnabled(LayerEffectKind::InnerShadow, checked);
        if (checked && !currentEffects_.innerShadow) currentEffects_.innerShadow = InnerShadowEffect();
        loadFromEffects(currentEffects_);
        updatePreview();
    });
    layout->addRow(innerShadowEnabled_);

    innerShadowColorButton_ = new QPushButton(page);
    connect(innerShadowColorButton_, &QPushButton::clicked, this, [this]() {
        pickColor(LayerEffectKind::InnerShadow, innerShadowColorButton_);
    });
    layout->addRow(tr("Color"), innerShadowColorButton_);

    innerShadowOpacity_ = new QDoubleSpinBox(page);
    innerShadowOpacity_->setDecimals(0);
    auto *opacityField = createSliderField(innerShadowOpacity_, 0.0, 100.0, 100.0, tr(" %"), [this]() {
        if (updating_) return;
        if (!currentEffects_.innerShadow) currentEffects_.innerShadow = InnerShadowEffect();
        currentEffects_.innerShadow->opacity = innerShadowOpacity_->value() / 100.0;
        updatePreview();
    });
    layout->addRow(new ScrubLabel(tr("Opacity"), innerShadowOpacity_, 1.0, 1.0, page), opacityField);

    innerShadowAngle_ = new QDoubleSpinBox(page);
    innerShadowAngle_->setDecimals(0);
    auto *angleField = createSliderField(innerShadowAngle_, -180.0, 180.0, 180.0, tr(" °"), [this]() {
        if (updating_) return;
        if (!currentEffects_.innerShadow) currentEffects_.innerShadow = InnerShadowEffect();
        currentEffects_.innerShadow->angle = innerShadowAngle_->value();
        updatePreview();
    });
    layout->addRow(new ScrubLabel(tr("Angle"), innerShadowAngle_, 1.0, 1.0, page), angleField);

    innerShadowDistance_ = new QDoubleSpinBox(page);
    innerShadowDistance_->setDecimals(0);
    auto *distField = createSliderField(innerShadowDistance_, 0.0, 5000.0, 50.0, tr(" px"), [this]() {
        if (updating_) return;
        if (!currentEffects_.innerShadow) currentEffects_.innerShadow = InnerShadowEffect();
        currentEffects_.innerShadow->distance = innerShadowDistance_->value();
        updatePreview();
    });
    layout->addRow(new ScrubLabel(tr("Distance"), innerShadowDistance_, 1.0, 1.0, page), distField);

    innerShadowBlur_ = new QDoubleSpinBox(page);
    innerShadowBlur_->setDecimals(0);
    auto *blurField = createSliderField(innerShadowBlur_, 0.0, 500.0, 100.0, tr(" px"), [this]() {
        if (updating_) return;
        if (!currentEffects_.innerShadow) currentEffects_.innerShadow = InnerShadowEffect();
        currentEffects_.innerShadow->blur = innerShadowBlur_->value();
        updatePreview();
    });
    layout->addRow(new ScrubLabel(tr("Blur"), innerShadowBlur_, 1.0, 1.0, page), blurField);

    return page;
}

QWidget *EffectsDialog::createOuterGlowPage()
{
    auto *page = new QWidget;
    auto *layout = new QFormLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    outerGlowEnabled_ = new QCheckBox(tr("Enable Outer Glow"), page);
    connect(outerGlowEnabled_, &QCheckBox::toggled, this, [this](bool checked) {
        if (updating_) return;
        currentEffects_.setEnabled(LayerEffectKind::OuterGlow, checked);
        if (checked && !currentEffects_.outerGlow) currentEffects_.outerGlow = OuterGlowEffect();
        loadFromEffects(currentEffects_);
        updatePreview();
    });
    layout->addRow(outerGlowEnabled_);

    outerGlowColorButton_ = new QPushButton(page);
    connect(outerGlowColorButton_, &QPushButton::clicked, this, [this]() {
        pickColor(LayerEffectKind::OuterGlow, outerGlowColorButton_);
    });
    layout->addRow(tr("Color"), outerGlowColorButton_);

    outerGlowSize_ = new QDoubleSpinBox(page);
    outerGlowSize_->setDecimals(0);
    auto *sizeField = createSliderField(outerGlowSize_, 0.0, 500.0, 100.0, tr(" px"), [this]() {
        if (updating_) return;
        if (!currentEffects_.outerGlow) currentEffects_.outerGlow = OuterGlowEffect();
        currentEffects_.outerGlow->size = outerGlowSize_->value();
        updatePreview();
    });
    layout->addRow(new ScrubLabel(tr("Size"), outerGlowSize_, 1.0, 1.0, page), sizeField);

    outerGlowOpacity_ = new QDoubleSpinBox(page);
    outerGlowOpacity_->setDecimals(0);
    auto *opacityField = createSliderField(outerGlowOpacity_, 0.0, 100.0, 100.0, tr(" %"), [this]() {
        if (updating_) return;
        if (!currentEffects_.outerGlow) currentEffects_.outerGlow = OuterGlowEffect();
        currentEffects_.outerGlow->opacity = outerGlowOpacity_->value() / 100.0;
        updatePreview();
    });
    layout->addRow(new ScrubLabel(tr("Opacity"), outerGlowOpacity_, 1.0, 1.0, page), opacityField);

    return page;
}

QWidget *EffectsDialog::createInnerGlowPage()
{
    auto *page = new QWidget;
    auto *layout = new QFormLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    innerGlowEnabled_ = new QCheckBox(tr("Enable Inner Glow"), page);
    connect(innerGlowEnabled_, &QCheckBox::toggled, this, [this](bool checked) {
        if (updating_) return;
        currentEffects_.setEnabled(LayerEffectKind::InnerGlow, checked);
        if (checked && !currentEffects_.innerGlow) currentEffects_.innerGlow = InnerGlowEffect();
        loadFromEffects(currentEffects_);
        updatePreview();
    });
    layout->addRow(innerGlowEnabled_);

    innerGlowColorButton_ = new QPushButton(page);
    connect(innerGlowColorButton_, &QPushButton::clicked, this, [this]() {
        pickColor(LayerEffectKind::InnerGlow, innerGlowColorButton_);
    });
    layout->addRow(tr("Color"), innerGlowColorButton_);

    innerGlowSize_ = new QDoubleSpinBox(page);
    innerGlowSize_->setDecimals(0);
    auto *sizeField = createSliderField(innerGlowSize_, 0.0, 500.0, 100.0, tr(" px"), [this]() {
        if (updating_) return;
        if (!currentEffects_.innerGlow) currentEffects_.innerGlow = InnerGlowEffect();
        currentEffects_.innerGlow->size = innerGlowSize_->value();
        updatePreview();
    });
    layout->addRow(new ScrubLabel(tr("Size"), innerGlowSize_, 1.0, 1.0, page), sizeField);

    innerGlowOpacity_ = new QDoubleSpinBox(page);
    innerGlowOpacity_->setDecimals(0);
    auto *opacityField = createSliderField(innerGlowOpacity_, 0.0, 100.0, 100.0, tr(" %"), [this]() {
        if (updating_) return;
        if (!currentEffects_.innerGlow) currentEffects_.innerGlow = InnerGlowEffect();
        currentEffects_.innerGlow->opacity = innerGlowOpacity_->value() / 100.0;
        updatePreview();
    });
    layout->addRow(new ScrubLabel(tr("Opacity"), innerGlowOpacity_, 1.0, 1.0, page), opacityField);

    return page;
}

void EffectsDialog::updateSwatch(QPushButton *button, const QColor &color)
{
    QPixmap pixmap(36, 18);
    pixmap.fill(color);
    QPainter p(&pixmap);
    p.setPen(QColor(60, 60, 60));
    p.drawRect(0, 0, 35, 17);
    button->setIcon(QIcon(pixmap));
    button->setIconSize(QSize(36, 18));
    button->setText(color.name().toUpper());
}

void EffectsDialog::pickColor(LayerEffectKind kind, QPushButton *button)
{
    const QColor initial = currentEffects_.color(kind).value_or(Qt::black);
    const QColor chosen = QColorDialog::getColor(initial, this, tr("Select Effect Color"));
    if (chosen.isValid()) {
        currentEffects_.setColor(kind, chosen);
        updateSwatch(button, chosen);
        updatePreview();
    }
}

void EffectsDialog::loadFromEffects(const LayerEffects &effects)
{
    updating_ = true;

    // Stroke
    const auto stroke = effects.stroke.value_or(StrokeEffect());
    const bool strokeActive = effects.isEnabled(LayerEffectKind::Stroke);
    strokeEnabled_->setChecked(strokeActive);
    effectList_->item(0)->setCheckState(strokeActive ? Qt::Checked : Qt::Unchecked);
    strokePosition_->setCurrentIndex(stroke.inside ? 1 : 0);
    strokeSize_->setValue(stroke.size);
    strokeOpacity_->setValue(stroke.opacity * 100.0);
    updateSwatch(strokeColorButton_, QColor::fromRgbF(stroke.red, stroke.green, stroke.blue));

    // Drop Shadow
    const auto shadow = effects.shadow.value_or(ShadowEffect());
    const bool shadowActive = effects.isEnabled(LayerEffectKind::DropShadow);
    shadowEnabled_->setChecked(shadowActive);
    effectList_->item(1)->setCheckState(shadowActive ? Qt::Checked : Qt::Unchecked);
    shadowOpacity_->setValue(shadow.opacity * 100.0);
    shadowAngle_->setValue(shadow.angle);
    shadowDistance_->setValue(shadow.distance);
    shadowBlur_->setValue(shadow.blur);
    updateSwatch(shadowColorButton_, QColor::fromRgbF(shadow.red, shadow.green, shadow.blue));

    // Color Overlay
    const auto overlay = effects.colorOverlay.value_or(ColorOverlayEffect());
    const bool overlayActive = effects.isEnabled(LayerEffectKind::ColorOverlay);
    colorOverlayEnabled_->setChecked(overlayActive);
    effectList_->item(2)->setCheckState(overlayActive ? Qt::Checked : Qt::Unchecked);
    colorOverlayOpacity_->setValue(overlay.opacity * 100.0);
    updateSwatch(colorOverlayColorButton_, QColor::fromRgbF(overlay.red, overlay.green, overlay.blue));

    // Inner Shadow
    const auto innerShadow = effects.innerShadow.value_or(InnerShadowEffect());
    const bool innerShadowActive = effects.isEnabled(LayerEffectKind::InnerShadow);
    innerShadowEnabled_->setChecked(innerShadowActive);
    effectList_->item(3)->setCheckState(innerShadowActive ? Qt::Checked : Qt::Unchecked);
    innerShadowOpacity_->setValue(innerShadow.opacity * 100.0);
    innerShadowAngle_->setValue(innerShadow.angle);
    innerShadowDistance_->setValue(innerShadow.distance);
    innerShadowBlur_->setValue(innerShadow.blur);
    updateSwatch(innerShadowColorButton_, QColor::fromRgbF(innerShadow.red, innerShadow.green, innerShadow.blue));

    // Outer Glow
    const auto outerGlow = effects.outerGlow.value_or(OuterGlowEffect());
    const bool outerGlowActive = effects.isEnabled(LayerEffectKind::OuterGlow);
    outerGlowEnabled_->setChecked(outerGlowActive);
    effectList_->item(4)->setCheckState(outerGlowActive ? Qt::Checked : Qt::Unchecked);
    outerGlowSize_->setValue(outerGlow.size);
    outerGlowOpacity_->setValue(outerGlow.opacity * 100.0);
    updateSwatch(outerGlowColorButton_, QColor::fromRgbF(outerGlow.red, outerGlow.green, outerGlow.blue));

    // Inner Glow
    const auto innerGlow = effects.innerGlow.value_or(InnerGlowEffect());
    const bool innerGlowActive = effects.isEnabled(LayerEffectKind::InnerGlow);
    innerGlowEnabled_->setChecked(innerGlowActive);
    effectList_->item(5)->setCheckState(innerGlowActive ? Qt::Checked : Qt::Unchecked);
    innerGlowSize_->setValue(innerGlow.size);
    innerGlowOpacity_->setValue(innerGlow.opacity * 100.0);
    updateSwatch(innerGlowColorButton_, QColor::fromRgbF(innerGlow.red, innerGlow.green, innerGlow.blue));

    updating_ = false;
}

void EffectsDialog::updatePreview()
{
    const bool preview = previewCheckbox_ && previewCheckbox_->isChecked();
    const LayerEffects eff = preview ? currentEffects_ : originalEffects_;

    // Temporarily apply to layer in document for interactive preview
    session_.previewLayerEffects(layerId_, eff.isEmpty() ? std::nullopt : std::optional<LayerEffects>(eff));
    if (onPreview_) {
        onPreview_();
    }
}

void EffectsDialog::accept()
{
    // Reset to original before calling setLayerEffects so that beginEdit records the original state
    if (auto doc = session_.document()) {
        for (Layer &layer : doc->layers) {
            if (layer.id == layerId_) {
                layer.effects = originalEffects_.isEmpty() ? std::nullopt : std::optional<LayerEffects>(originalEffects_);
                break;
            }
        }
    }
    session_.setLayerEffects(layerId_, currentEffects_, tr("Layer Effects"));
    if (onPreview_) {
        onPreview_();
    }
    QDialog::accept();
}

void EffectsDialog::reject()
{
    // Restore original effects without undo record
    if (auto doc = session_.document()) {
        for (Layer &layer : doc->layers) {
            if (layer.id == layerId_) {
                layer.effects = originalEffects_.isEmpty() ? std::nullopt : std::optional<LayerEffects>(originalEffects_);
                break;
            }
        }
    }
    if (onPreview_) {
        onPreview_();
    }
    QDialog::reject();
}

} // namespace compositor
