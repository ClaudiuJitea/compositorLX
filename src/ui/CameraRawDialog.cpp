#include "ui/CameraRawDialog.h"
#include "ui/NumericScrub.h"
#include "rendering/RasterOperations.h"
#include "rendering/LayerRenderer.h"
#include "ui/CanvasWidget.h"
#include "ui/SliderTracks.h"
#include <QLabel>
#include <QToolButton>
#include <QStackedWidget>
#include <QConicalGradient>
#include <QRadialGradient>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QMenu>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStyleOptionSlider>
#include <QDialogButtonBox>
#include <QPainter>
#include <QPainterPath>
#include <QComboBox>
#include <QListWidget>
#include <QMouseEvent>
#include <algorithm>
#include <cmath>

namespace compositor {

ScopeWidget::ScopeWidget(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(110);
    setStyleSheet(QStringLiteral("background-color: #1e1e1e; border-radius: 4px;"));

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    shadowClipBtn_ = new QPushButton(QStringLiteral("▲"), this);
    shadowClipBtn_->setFixedSize(18, 18);
    shadowClipBtn_->setCheckable(true);
    shadowClipBtn_->setToolTip(tr("Show clipped shadows in blue on the preview."));
    shadowClipBtn_->setStyleSheet(QStringLiteral("QPushButton { color: #888; background: transparent; border: none; font-size: 10px; } "
                                                 "QPushButton:checked { color: #3388ff; }"));
    connect(shadowClipBtn_, &QPushButton::toggled, this, &ScopeWidget::shadowClippingToggled);
    layout->addWidget(shadowClipBtn_, 0, Qt::AlignTop | Qt::AlignLeft);

    layout->addStretch();

    highlightClipBtn_ = new QPushButton(QStringLiteral("▲"), this);
    highlightClipBtn_->setFixedSize(18, 18);
    highlightClipBtn_->setCheckable(true);
    highlightClipBtn_->setToolTip(tr("Show clipped highlights in red on the preview."));
    highlightClipBtn_->setStyleSheet(QStringLiteral("QPushButton { color: #888; background: transparent; border: none; font-size: 10px; } "
                                                    "QPushButton:checked { color: #ff3333; }"));
    connect(highlightClipBtn_, &QPushButton::toggled, this, &ScopeWidget::highlightClippingToggled);
    layout->addWidget(highlightClipBtn_, 0, Qt::AlignTop | Qt::AlignRight);
}

void ScopeWidget::setScope(const CameraRawScope &scope)
{
    scope_ = scope;
    update();
}

void ScopeWidget::setScopeMode(CameraRawScopeMode mode)
{
    mode_ = mode;
    update();
}

void ScopeWidget::setShadowClippingActive(bool active)
{
    shadowClipBtn_->setChecked(active);
}

void ScopeWidget::setHighlightClippingActive(bool active)
{
    highlightClipBtn_->setChecked(active);
}

void ScopeWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(30, 30, 30));

    const double w = width();
    const double h = height();

    if (mode_ == CameraRawScopeMode::Histogram) {
        const double peak = scope_.peak();
        if (peak <= 0.0) return;

        auto drawRibbon = [&](const std::array<double, 256> &bins, const QColor &color) {
            QPainterPath path;
            path.moveTo(0, h);
            for (size_t i = 0; i < bins.size(); ++i) {
                const double x = double(i) * w / 255.0;
                const double barH = h * std::clamp(bins[i] / peak, 0.0, 1.0);
                path.lineTo(x, h - barH);
            }
            path.lineTo(w, h);
            path.closeSubpath();
            p.fillPath(path, color);
        };

        drawRibbon(scope_.red, QColor(255, 60, 60, 130));
        drawRibbon(scope_.green, QColor(60, 255, 60, 130));
        drawRibbon(scope_.blue, QColor(60, 120, 255, 130));
    } else {
        // Vectorscope 64x64
        double maxVal = 0.0;
        for (double v : scope_.vectorscope) if (v > maxVal) maxVal = v;
        if (maxVal <= 0.0) return;

        const double cellW = w / double(CameraRawScope::scopeSide);
        const double cellH = h / double(CameraRawScope::scopeSide);
        for (int row = 0; row < CameraRawScope::scopeSide; ++row) {
            for (int col = 0; col < CameraRawScope::scopeSide; ++col) {
                const double val = scope_.vectorscope[size_t(row * CameraRawScope::scopeSide + col)];
                if (val > 0.0) {
                    const double alpha = std::min(1.0, val / maxVal);
                    p.fillRect(QRectF(col * cellW, h - (row + 1) * cellH, cellW + 0.5, cellH + 0.5),
                               QColor(255, 255, 255, int(40 + 215 * alpha)));
                }
            }
        }
    }
}

void ScopeWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    QAction *histAction = menu.addAction(tr("Histogram"));
    QAction *scopeAction = menu.addAction(tr("Vectorscope"));
    histAction->setCheckable(true);
    scopeAction->setCheckable(true);
    histAction->setChecked(mode_ == CameraRawScopeMode::Histogram);
    scopeAction->setChecked(mode_ == CameraRawScopeMode::Vectorscope);

    connect(histAction, &QAction::triggered, this, [this] { setScopeMode(CameraRawScopeMode::Histogram); });
    connect(scopeAction, &QAction::triggered, this, [this] { setScopeMode(CameraRawScopeMode::Vectorscope); });

    menu.exec(event->globalPos());
}

CameraRawDialog::CameraRawDialog(QWidget *parent, EditorSession &session, const QUuid &layerId,
                                 std::function<void()> onPreview)
    : QDialog(parent), session_(session), layerId_(layerId), onPreview_(std::move(onPreview))
{
    setWindowTitle(tr("Camera Raw Filter"));
    resize(620, 700);

    if (auto doc = session_.document()) {
        for (const Layer &layer : doc->layers) {
            if (layer.id == layerId_) {
                originalLayer_ = layer;
                break;
            }
        }
    }


    settings_ = session_.lastCameraRaw.normalized();   // mac: the panel reopens with what the filter was last committed with
    setupUi();
    installEventFilter(this);
    // Focus on a slider or its field tells which Light slider an Option press should show clipping for.
    for (QSlider *slider : findChildren<QSlider *>()) slider->installEventFilter(this);
    for (QDoubleSpinBox *spin : findChildren<QDoubleSpinBox *>()) spin->installEventFilter(this);
    if (auto *main = qobject_cast<QWidget *>(parent)) canvas_ = main->findChild<CanvasWidget *>();
    if (canvas_) {
        canvas_->setProbeHover(true);
        connect(canvas_, &CanvasWidget::probePressed, this, &CameraRawDialog::probePress);
        connect(canvas_, &CanvasWidget::probeMoved, this, &CameraRawDialog::probeMove);
        connect(canvas_, &CanvasWidget::probeReleased, this, &CameraRawDialog::probeRelease);
        canvas_->probeOverlay = [this](QPainter &painter, const QRectF &rect, double zoom) {
            if (originalLayer_.image.isNull()) return;
            const QTransform toDocument = LayerRenderer::pixelToDocument(originalLayer_.transform, originalLayer_.image.size());
            const QSizeF size = originalLayer_.image.size();
            QVector<CameraRawGeometryGuide> guides = settings_.geometry.guides;
            if (guideDraft_) guides.push_back(*guideDraft_);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(QPen(QColor(255, 210, 60), 2));
            for (const auto &guide : guides) {
                const QPointF a = toDocument.map(QPointF(guide.startX * size.width(), guide.startY * size.height()));
                const QPointF b = toDocument.map(QPointF(guide.endX * size.width(), guide.endY * size.height()));
                painter.drawLine(rect.topLeft() + a * zoom, rect.topLeft() + b * zoom);
            }
        };
    }
    loadFromSettings();
    updatePreview();
}

CameraRawDialog::~CameraRawDialog() { releaseCanvas(); }

void CameraRawDialog::releaseCanvas()
{
    if (!canvas_) return;
    canvas_->setProbeMode(false);
    canvas_->setProbeHover(false);
    canvas_->probeOverlay = nullptr;
    disconnect(canvas_, nullptr, this, nullptr);
    canvas_->update();
    canvas_ = nullptr;
}


void CameraRawDialog::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);

    scopeWidget_ = new ScopeWidget(this);
    connect(scopeWidget_, &ScopeWidget::shadowClippingToggled, this, [this](bool active) {
        showShadowClipping_ = active;
        updatePreview();
    });
    connect(scopeWidget_, &ScopeWidget::highlightClippingToggled, this, [this](bool active) {
        showHighlightClipping_ = active;
        updatePreview();
    });
    mainLayout->addWidget(scopeWidget_);

    tabWidget_ = new QTabWidget(this);
    const std::pair<QString, QWidget *> tabs[] = {
        {tr("Light"), createLightTab()}, {tr("Color"), createColorTab()}, {tr("Effects"), createEffectsTab()}, {tr("Curve"), createCurveTab()},
        {tr("Mixer"), createMixerTab()}, {tr("Grading"), createGradingTab()}, {tr("Detail"), createDetailTab()}, {tr("Optics"), createOpticsTab()},
        {tr("Geometry"), createGeometryTab()}, {tr("Calibration"), createCalibrationTab()}};
    static const char *eyeNames[10] = {"Light", "Color", "Effects", "Curve", "Color Mixer", "Color Grading", "Detail", "Optics", "Geometry", "Calibration"};
    for (size_t i = 0; i < 10; ++i) {
        // mac's eye: shown while the group adjusts something; turning it off drops the group from preview and OK.
        auto *page = new QWidget(this);
        auto *pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(0, 0, 0, 0);
        auto *eye = new QCheckBox(tr("Apply %1").arg(tr(eyeNames[i])), page);
        eye->setObjectName(QStringLiteral("cameraRawEye%1").arg(i));
        eye->setChecked(true);
        eye->setToolTip(tr("Turn off to leave this group out of the picture and of the result without clearing its sliders."));
        connect(eye, &QCheckBox::toggled, this, [this, i](bool on) { shows_[i] = on; updatePreview(); });
        showBoxes_[i] = eye;
        pageLayout->addWidget(eye);
        pageLayout->addWidget(tabs[i].second, 1);
        tabWidget_->addTab(page, tabs[i].first);
    }

    // Ten sections do not fit a tab strip (Optics, Geometry and Calibration hid behind scroll arrows): a list on the left,
    // the tab widget's own strip hidden. The list and the pages stay in step both ways.
    tabWidget_->setObjectName(QStringLiteral("cameraRawTabs"));
    tabWidget_->tabBar()->hide();
    auto *sections = new QListWidget(this);
    sections->setObjectName(QStringLiteral("cameraRawSections"));
    sections->setFixedWidth(104);
    sections->setFrameShape(QFrame::NoFrame);
    sections->setFocusPolicy(Qt::NoFocus);
    for (int i = 0; i < tabWidget_->count(); ++i) sections->addItem(tabWidget_->tabText(i));
    connect(sections, &QListWidget::currentRowChanged, tabWidget_, [this](int row) { if (row >= 0) tabWidget_->setCurrentIndex(row); });
    connect(tabWidget_, &QTabWidget::currentChanged, sections, [sections](int index) { const QSignalBlocker block(sections); sections->setCurrentRow(index); });
    sections->setCurrentRow(tabWidget_->currentIndex());
    auto *body = new QHBoxLayout();
    body->setSpacing(8);
    body->addWidget(sections);
    body->addWidget(tabWidget_, 1);
    mainLayout->addLayout(body, 1);

    auto *bottomLayout = new QHBoxLayout();
    previewBox_ = new QCheckBox(tr("Preview"), this);
    previewBox_->setObjectName(QStringLiteral("previewBox"));
    previewBox_->setChecked(true);

    connect(previewBox_, &QCheckBox::toggled, this, &CameraRawDialog::updatePreview);
    bottomLayout->addWidget(previewBox_);

    resetButton_ = new QPushButton(tr("Reset"), this);
    resetButton_->setObjectName(QStringLiteral("cameraRawReset"));
    resetButton_->setToolTip(tr("Puts every Camera Raw control back to its default."));
    connect(resetButton_, &QPushButton::clicked, this, [this] { shows_.fill(true); setSettings(CameraRawSettings()); });
    bottomLayout->addWidget(resetButton_);
    readout_ = new QLabel(QStringLiteral("R —   G —   B —"), this);
    readout_->setObjectName(QStringLiteral("cameraRawReadout"));
    readout_->setToolTip(tr("Color under the pointer, in the adjusted preview."));
    bottomLayout->addWidget(readout_);

    bottomLayout->addStretch();

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &CameraRawDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &CameraRawDialog::reject);
    bottomLayout->addWidget(buttonBox);

    mainLayout->addLayout(bottomLayout);
}

namespace {
// mac CameraRawSliderView: a double-click on the knob puts the slider back to its default.
class DoubleClickReset final : public QObject {
public:
    DoubleClickReset(QSlider *slider, QDoubleSpinBox *spin, double value) : QObject(slider), slider_(slider), spin_(spin), value_(value) {}
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() != QEvent::MouseButtonDblClick) return false;
        auto *mouse = static_cast<QMouseEvent *>(event);
        QStyleOptionSlider option; option.initFrom(slider_); option.orientation = slider_->orientation();
        option.minimum = slider_->minimum(); option.maximum = slider_->maximum(); option.sliderPosition = option.sliderValue = slider_->value();
        option.subControls = QStyle::SC_SliderHandle;
        const QRect handle = slider_->style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, slider_);
        if (!handle.contains(mouse->position().toPoint())) return false;
        spin_->setValue(value_);
        return true;
    }
private:
    QSlider *slider_; QDoubleSpinBox *spin_; double value_;
};
}

static QHBoxLayout *makeSliderRow(QWidget *parent, double minVal, double maxVal, double curVal, int decimals,
                                  QDoubleSpinBox *&spin, std::function<void()> onChange, const QList<QColor> &track = {})
{
    auto *row = new QHBoxLayout();
    auto *slider = new QSlider(Qt::Horizontal, parent);
    spin = new QDoubleSpinBox(parent);
    spin->setDecimals(decimals);
    spin->setRange(minVal, maxVal);
    spin->setValue(curVal);
    spin->setSingleStep(decimals == 2 ? 0.05 : 1.0);

    const double mult = std::pow(10, decimals);
    slider->setRange(static_cast<int>(minVal * mult), static_cast<int>(maxVal * mult));
    slider->setValue(static_cast<int>(curVal * mult));

    row->addWidget(slider, 1);
    row->addWidget(spin);
    slider->installEventFilter(new DoubleClickReset(slider, spin, curVal));
    slider->setProperty("cameraRawSpin", QVariant::fromValue(static_cast<QObject *>(spin)));
    spin->setProperty("resetValue", curVal);
    SliderTrack::apply(slider, track);

    QObject::connect(slider, &QSlider::valueChanged, parent, [spin, mult, onChange](int val) {
        if (spin->value() != double(val) / mult) {
            spin->setValue(double(val) / mult);
            onChange();
        }
    });

    QObject::connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), parent, [slider, mult, onChange](double val) {
        if (slider->value() != static_cast<int>(val * mult)) {
            slider->setValue(static_cast<int>(val * mult));
            onChange();
        }
    });

    return row;
}

static void addScrubRow(QFormLayout *form, const QString &labelText, QWidget *parent,
                        double minVal, double maxVal, double curVal, int decimals,
                        QDoubleSpinBox *&spin, std::function<void()> onChange,
                        std::optional<double> sensitivity = std::nullopt, std::optional<double> step = std::nullopt,
                        const QString &objName = QString(), const QList<QColor> &track = {})
{
    auto *row = makeSliderRow(parent, minVal, maxVal, curVal, decimals, spin, onChange, track);
    if (!objName.isEmpty()) {
        spin->setObjectName(objName);
    }
    const double sens = sensitivity.value_or(decimals > 0 ? std::pow(10.0, -decimals) : 1.0);
    const double stepVal = step.value_or(sens);
    auto *label = new ScrubLabel(labelText, spin, sens, stepVal, parent);
    if (!spin->objectName().isEmpty()) {
        label->setObjectName(spin->objectName() + QStringLiteral("Label"));
    }
    form->addRow(label, row);
}

QWidget *CameraRawDialog::createLightTab()
{
    auto *w = new QWidget(this);
    auto *form = new QFormLayout(w);
    const auto cb = [this] { syncToSettings(); };

    addScrubRow(form, tr("Exposure"), this, -5.0, 5.0, 0.0, 2, exposureSpin_, cb, std::nullopt, std::nullopt, QStringLiteral("exposureSpin"));
    addScrubRow(form, tr("Contrast"), this, -100.0, 100.0, 0.0, 0, contrastSpin_, cb);

    addScrubRow(form, tr("Highlights"), this, -100.0, 100.0, 0.0, 0, highlightsSpin_, cb);
    addScrubRow(form, tr("Shadows"), this, -100.0, 100.0, 0.0, 0, shadowsSpin_, cb);
    addScrubRow(form, tr("Whites"), this, -100.0, 100.0, 0.0, 0, whitesSpin_, cb);
    addScrubRow(form, tr("Blacks"), this, -100.0, 100.0, 0.0, 0, blacksSpin_, cb);
    return w;
}

QWidget *CameraRawDialog::createColorTab()
{
    auto *w = new QWidget(this);
    auto *form = new QFormLayout(w);
    const auto cb = [this] { syncToSettings(); };

    auto *wbRow = new QHBoxLayout();
    wbCombo_ = new QComboBox(this);
    wbCombo_->addItems({tr("Custom"), tr("Auto")});
    wbRow->addWidget(wbCombo_);

    wbEyedropper_ = new QPushButton(tr("Eyedropper"), this);
    wbEyedropper_->setCheckable(true);
    wbRow->addWidget(wbEyedropper_);
    form->addRow(tr("White Balance"), wbRow);

    connect(wbCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (idx == 1) { // Auto
            auto solved = CameraRawSettings::autoBalance(originalLayer_.image);
            if (solved) {
                tempSpin_->setValue(solved->first);
                tintSpin_->setValue(solved->second);
            }
        }
        syncToSettings();
    });

    addScrubRow(form, tr("Temperature"), this, -100.0, 100.0, 0.0, 0, tempSpin_, cb, std::nullopt, std::nullopt, QString(), SliderTrack::temperature());
    addScrubRow(form, tr("Tint"), this, -100.0, 100.0, 0.0, 0, tintSpin_, cb, std::nullopt, std::nullopt, QString(), SliderTrack::tint());
    addScrubRow(form, tr("Vibrance"), this, -100.0, 100.0, 0.0, 0, vibranceSpin_, cb, std::nullopt, std::nullopt, QString(), SliderTrack::chroma());
    addScrubRow(form, tr("Saturation"), this, -100.0, 100.0, 0.0, 0, saturationSpin_, cb, std::nullopt, std::nullopt, QString(), SliderTrack::chroma());
    return w;
}

QWidget *CameraRawDialog::createEffectsTab()
{
    auto *w = new QWidget(this);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto *content = new QWidget(scroll);
    auto *form = new QFormLayout(content);
    const auto cb = [this] { syncToSettings(); };

    addScrubRow(form, tr("Texture"), content, -100.0, 100.0, 0.0, 0, textureSpin_, cb);
    addScrubRow(form, tr("Clarity"), content, -100.0, 100.0, 0.0, 0, claritySpin_, cb);
    addScrubRow(form, tr("Dehaze"), content, -100.0, 100.0, 0.0, 0, dehazeSpin_, cb);

    glowStyleCombo_ = new QComboBox(this);
    glowStyleCombo_->addItems({tr("Diffusion"), tr("Bloom"), tr("Halation")});
    connect(glowStyleCombo_, &QComboBox::currentIndexChanged, this, cb);
    form->addRow(tr("Glow Style"), glowStyleCombo_);

    addScrubRow(form, tr("Glow Amount"), content, 0.0, 100.0, 0.0, 0, glowSpin_, cb);
    addScrubRow(form, tr("Glow Range"), content, -100.0, 100.0, 0.0, 0, glowRangeSpin_, cb);
    addScrubRow(form, tr("Glow Spread"), content, -100.0, 100.0, 0.0, 0, glowSpreadSpin_, cb);
    addScrubRow(form, tr("Glow Warmth"), content, -100.0, 100.0, 0.0, 0, glowWarmthSpin_, cb);

    vignetteStyleCombo_ = new QComboBox(this);
    vignetteStyleCombo_->addItems({tr("Highlight Priority"), tr("Color Priority"), tr("Paint Overlay")});
    connect(vignetteStyleCombo_, &QComboBox::currentIndexChanged, this, cb);
    form->addRow(tr("Vignette Style"), vignetteStyleCombo_);

    addScrubRow(form, tr("Vignette Amount"), content, -100.0, 100.0, 0.0, 0, vignetteAmountSpin_, cb);
    addScrubRow(form, tr("Vignette Midpoint"), content, 0.0, 100.0, 50.0, 0, vignetteMidpointSpin_, cb);
    addScrubRow(form, tr("Vignette Roundness"), content, -100.0, 100.0, 0.0, 0, vignetteRoundnessSpin_, cb);
    addScrubRow(form, tr("Vignette Feather"), content, 0.0, 100.0, 50.0, 0, vignetteFeatherSpin_, cb);
    addScrubRow(form, tr("Vignette Highlights"), content, 0.0, 100.0, 0.0, 0, vignetteHighlightsSpin_, cb);

    addScrubRow(form, tr("Grain Amount"), content, 0.0, 100.0, 0.0, 0, grainAmountSpin_, cb);
    addScrubRow(form, tr("Grain Size"), content, 0.0, 100.0, 25.0, 0, grainSizeSpin_, cb);
    addScrubRow(form, tr("Grain Roughness"), content, 0.0, 100.0, 50.0, 0, grainRoughnessSpin_, cb);

    scroll->setWidget(content);
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(scroll);
    return w;
}

namespace {
// The point curve as a graph (mac 60d9117: dragging in it works, as in Image > Curves): click empty space to add a point,
// drag a point to move it (the end points only move up and down), double-click an inner point to remove it. The drawn line
// is the whole tone curve, parametric sliders included, as the image gets it.
class CameraRawCurveGraph final : public QWidget {
public:
    explicit CameraRawCurveGraph(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(240, 240); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); setFixedHeight(240); setObjectName(QStringLiteral("cameraRawCurveGraph"));
    }
    CameraRawCurveSettings *curve = nullptr;
    std::function<void()> changed;
    int channel = 0;   // RGB, Red, Green, Blue

    QVector<CurvePoint> &points() { return channel == 1 ? curve->red : channel == 2 ? curve->green : channel == 3 ? curve->blue : curve->rgb; }

protected:
    QPointF toWidget(const CurvePoint &p) const { return QPointF(p.x * width(), (1.0 - p.y) * height()); }
    CurvePoint fromWidget(const QPointF &p) const { return {std::clamp(p.x() / std::max(1, width()), 0.0, 1.0), std::clamp(1.0 - p.y() / std::max(1, height()), 0.0, 1.0)}; }
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this); painter.fillRect(rect(), QColor(18, 19, 22)); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(255, 255, 255, 30), 1));
        for (int i = 0; i <= 4; ++i) { const double f = i / 4.0; painter.drawLine(QPointF(f * width(), 0), QPointF(f * width(), height())); painter.drawLine(QPointF(0, f * height()), QPointF(width(), f * height())); }
        if (!curve) return;
        const auto table = channel == 0 ? curve->toneTable() : curve->channelTable(points());
        const QColor colors[] = {Qt::white, QColor(230, 70, 70), QColor(70, 210, 100), QColor(70, 130, 235)};
        QPainterPath path;
        for (int i = 0; i < 256; ++i) { const QPointF at(i / 255.0 * width(), (1.0 - double(table[size_t(i)])) * height()); if (!i) path.moveTo(at); else path.lineTo(at); }
        painter.setPen(QPen(colors[std::clamp(channel, 0, 3)], 2)); painter.setBrush(Qt::NoBrush); painter.drawPath(path);
        painter.setPen(QPen(QColor(25, 25, 25), 1));
        for (int i = 0; i < points().size(); ++i) { painter.setBrush(i == selected_ ? palette().color(QPalette::Highlight) : QColor(235, 235, 235)); painter.drawEllipse(toWidget(points()[i]), 5.0, 5.0); }
    }
    int nearestPoint(const QPointF &at, double *distance = nullptr)
    {
        int index = -1; double best = 1e9;
        for (int i = 0; i < points().size(); ++i) { const double d = QLineF(toWidget(points()[i]), at).length(); if (d < best) { best = d; index = i; } }
        if (distance) *distance = best;
        return index;
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (!curve || event->button() != Qt::LeftButton) return;
        double distance = 0; int index = nearestPoint(event->position(), &distance);
        if (distance >= 14) {
            const CurvePoint p = fromWidget(event->position());
            auto &pts = points();
            if (pts.size() >= 32 || p.x <= 0.004 || p.x >= 0.996 || std::any_of(pts.cbegin(), pts.cend(), [&](const CurvePoint &q) { return std::abs(q.x - p.x) <= 0.004; })) return;
            pts.push_back(p); std::sort(pts.begin(), pts.end(), [](const CurvePoint &a, const CurvePoint &b) { return a.x < b.x; });
            index = int(std::find_if(pts.cbegin(), pts.cend(), [&](const CurvePoint &q) { return q.x == p.x; }) - pts.cbegin());
        }
        dragging_ = selected_ = index; moveSelected(event->position()); event->accept();
    }
    void mouseMoveEvent(QMouseEvent *event) override { if (dragging_ >= 0 && (event->buttons() & Qt::LeftButton)) { moveSelected(event->position()); event->accept(); } }
    void mouseReleaseEvent(QMouseEvent *) override { dragging_ = -1; }
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (!curve) return;
        double distance = 0; const int index = nearestPoint(event->position(), &distance);
        auto &pts = points();
        if (distance < 14 && index > 0 && index + 1 < pts.size()) { pts.remove(index); selected_ = dragging_ = -1; update(); if (changed) changed(); }
    }
private:
    void moveSelected(const QPointF &at)
    {
        auto &pts = points();
        if (selected_ < 0 || selected_ >= pts.size()) return;
        const CurvePoint p = fromWidget(at);
        pts[selected_].y = p.y;
        if (selected_ > 0 && selected_ + 1 < pts.size()) pts[selected_].x = std::clamp(p.x, pts[selected_ - 1].x + 0.004, pts[selected_ + 1].x - 0.004);
        update(); if (changed) changed();
    }
    int selected_ = -1;
    int dragging_ = -1;
};
} // namespace

QWidget *CameraRawDialog::createCurveTab()
{
    auto *w = new QWidget(this);
    auto *outer = new QVBoxLayout(w);
    const auto cb = [this] { syncToSettings(); if (curveGraph_) curveGraph_->update(); };

    curvePageCombo_ = new QComboBox(w);
    curvePageCombo_->addItems({tr("Parametric"), tr("Point")});
    curvePageCombo_->setObjectName(QStringLiteral("curvePage"));
    curvePageCombo_->setToolTip(tr("Parametric lifts tonal regions. Point places anchors on the curve."));
    curveTarget_ = new QPushButton(tr("Targeted Adjustment"), w);
    curveTarget_->setCheckable(true);
    curveTarget_->setObjectName(QStringLiteral("curveTarget"));
    curveTarget_->setToolTip(tr("Drag on the picture to move the curve for the tone under the pointer."));
    connect(curveTarget_, &QPushButton::toggled, this, [this](bool on) { setProbe(on ? Probe::CurveTarget : (probe_ == Probe::CurveTarget ? Probe::None : probe_)); });
    auto *top = new QHBoxLayout; top->addWidget(curvePageCombo_); top->addWidget(curveTarget_); top->addStretch();
    outer->addLayout(top);

    curvePointPage_ = new QWidget(w);
    auto *pointForm = new QFormLayout(curvePointPage_);
    pointForm->setContentsMargins(0, 0, 0, 0);
    curvePresetCombo_ = new QComboBox(this);
    curvePresetCombo_->addItems({tr("Custom"), tr("Linear"), tr("Medium Contrast"), tr("Strong Contrast")});
    connect(curvePresetCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        QVector<CurvePoint> *points = curveChannelCombo_ && curveChannelCombo_->currentIndex() == 1 ? &settings_.curve.red
            : curveChannelCombo_ && curveChannelCombo_->currentIndex() == 2 ? &settings_.curve.green
            : curveChannelCombo_ && curveChannelCombo_->currentIndex() == 3 ? &settings_.curve.blue : &settings_.curve.rgb;
        if (idx == 1) *points = CameraRawCurveSettings::linear;
        else if (idx == 2) *points = CameraRawCurveSettings::mediumContrast;
        else if (idx == 3) *points = CameraRawCurveSettings::strongContrast;
        syncToSettings();
        if (curveGraph_) curveGraph_->update();
    });
    pointForm->addRow(tr("Point Curve Preset"), curvePresetCombo_);

    auto *graph = new CameraRawCurveGraph(curvePointPage_); graph->curve = &settings_.curve; graph->setToolTip(tr("Click to add a point, drag to move it, double-click an inner point to remove it."));
    curveChannelCombo_ = new QComboBox(curvePointPage_); curveChannelCombo_->addItems({tr("RGB"), tr("Red"), tr("Green"), tr("Blue")});
    connect(curveChannelCombo_, &QComboBox::currentIndexChanged, this, [graph](int index) { graph->channel = index; graph->update(); });
    graph->changed = [this] { if (curvePresetCombo_) { const QSignalBlocker blocker(curvePresetCombo_); curvePresetCombo_->setCurrentIndex(0); } syncToSettings(); };
    curveGraph_ = graph;
    pointForm->addRow(tr("Channel"), curveChannelCombo_); pointForm->addRow(graph);
    outer->addWidget(curvePointPage_);

    curveParametricPage_ = new QWidget(w);
    auto *form = new QFormLayout(curveParametricPage_);
    form->setContentsMargins(0, 0, 0, 0);
    addScrubRow(form, tr("Highlights"), this, -100.0, 100.0, 0.0, 0, curveHighlightsSpin_, cb);
    addScrubRow(form, tr("Lights"), this, -100.0, 100.0, 0.0, 0, curveLightsSpin_, cb);
    addScrubRow(form, tr("Darks"), this, -100.0, 100.0, 0.0, 0, curveDarksSpin_, cb);
    addScrubRow(form, tr("Shadows"), this, -100.0, 100.0, 0.0, 0, curveShadowsSpin_, cb);
    addScrubRow(form, tr("Shadow Split"), this, 5.0, 90.0, 25.0, 0, curveShadowSplitSpin_, cb);
    addScrubRow(form, tr("Dark Split"), this, 10.0, 95.0, 50.0, 0, curveDarkSplitSpin_, cb);
    addScrubRow(form, tr("Light Split"), this, 15.0, 98.0, 75.0, 0, curveLightSplitSpin_, cb);
    outer->addWidget(curveParametricPage_);
    auto *refine = new QFormLayout;
    addScrubRow(refine, tr("Refine Saturation"), this, -100.0, 100.0, 0.0, 0, curveRefineSatSpin_, cb);
    outer->addLayout(refine);
    outer->addStretch();
    curvePointPage_->setVisible(false);
    connect(curvePageCombo_, &QComboBox::currentIndexChanged, this, [this](int page) {
        curveParametricPage_->setVisible(page == 0); curvePointPage_->setVisible(page == 1);
    });
    return w;
}

namespace {
QColor swatchColor(double hueDegrees, double saturation = 0.8, double brightness = 0.9)
{
    return SliderTrack::hsv(hueDegrees, saturation, brightness);
}
QString swatchStyle(const QColor &color, bool selected)
{
    return QStringLiteral("QToolButton{background:%1;border:2px solid %2;border-radius:10px;min-width:16px;max-width:16px;min-height:16px;max-height:16px;}")
        .arg(color.name(), selected ? QStringLiteral("#ffffff") : QStringLiteral("transparent"));
}
} // namespace

QWidget *CameraRawDialog::createMixerTab()
{
    auto *w = new QWidget(this);
    auto *outer = new QVBoxLayout(w);
    const auto cb = [this] { syncToSettings(); };
    static const char *names[8] = {"Reds", "Oranges", "Yellows", "Greens", "Aquas", "Blues", "Purples", "Magentas"};

    mixerPageCombo_ = new QComboBox(w);
    mixerPageCombo_->addItems({tr("HSL"), tr("Color"), tr("Point Color")});
    mixerPageCombo_->setObjectName(QStringLiteral("mixerPage"));
    mixerPageCombo_->setToolTip(tr("HSL lists every color. Color edits one family. Point Color adjusts a color you pick."));
    mixerComponentCombo_ = new QComboBox(w);
    mixerComponentCombo_->addItems({tr("Hue"), tr("Saturation"), tr("Luminance")});
    mixerComponentCombo_->setObjectName(QStringLiteral("mixerComponent"));
    mixerComponentCombo_->setToolTip(tr("What a targeted adjustment drag changes."));
    mixerTarget_ = new QPushButton(tr("Targeted Adjustment"), w);
    mixerTarget_->setCheckable(true);
    mixerTarget_->setObjectName(QStringLiteral("mixerTarget"));
    mixerTarget_->setToolTip(tr("Drag a color in the picture. Nearby color families move together."));
    connect(mixerTarget_, &QPushButton::toggled, this, [this](bool on) { setProbe(on ? Probe::MixerTarget : (probe_ == Probe::MixerTarget ? Probe::None : probe_)); });
    auto *top = new QHBoxLayout; top->addWidget(mixerPageCombo_); top->addWidget(mixerComponentCombo_); top->addWidget(mixerTarget_); top->addStretch();
    outer->addLayout(top);

    mixerStack_ = new QStackedWidget(w);
    outer->addWidget(mixerStack_, 1);

    // HSL
    auto *hslScroll = new QScrollArea(w);
    hslScroll->setWidgetResizable(true);
    auto *content = new QWidget(hslScroll);
    auto *form = new QFormLayout(content);
    for (int i = 0; i < 8; ++i) {
        auto *row = new QHBoxLayout();
        const double center = CameraRawMixerSettings::centers[size_t(i)];
        row->addWidget(new ScrubLabel(tr("H:"), mixerHueSpins_[size_t(i)], 1.0, 1.0, content));
        row->addLayout(makeSliderRow(this, -100.0, 100.0, 0.0, 0, mixerHueSpins_[size_t(i)], cb, SliderTrack::hue(center)));
        row->addWidget(new ScrubLabel(tr("S:"), mixerSatSpins_[size_t(i)], 1.0, 1.0, content));
        row->addLayout(makeSliderRow(this, -100.0, 100.0, 0.0, 0, mixerSatSpins_[size_t(i)], cb, SliderTrack::saturation(center)));
        row->addWidget(new ScrubLabel(tr("L:"), mixerLumSpins_[size_t(i)], 1.0, 1.0, content));
        row->addLayout(makeSliderRow(this, -100.0, 100.0, 0.0, 0, mixerLumSpins_[size_t(i)], cb, SliderTrack::luminance(center)));
        form->addRow(tr(names[i]), row);
        for (QDoubleSpinBox *spin : {mixerHueSpins_[size_t(i)], mixerSatSpins_[size_t(i)], mixerLumSpins_[size_t(i)]})
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, i](double) { if (i == mixerSwatch_) refreshMixerColorPage(); });
    }
    hslScroll->setWidget(content);
    mixerStack_->addWidget(hslScroll);

    // Color: one family at a time
    auto *colorPage = new QWidget(w);
    auto *colorLayout = new QVBoxLayout(colorPage);
    auto *swatches = new QHBoxLayout;
    for (int i = 0; i < 8; ++i) {
        auto *button = new QToolButton(colorPage);
        button->setObjectName(QStringLiteral("mixerSwatch%1").arg(i));
        button->setToolTip(tr("Edit %1.").arg(tr(names[i])));
        button->setStyleSheet(swatchStyle(swatchColor(CameraRawMixerSettings::centers[size_t(i)]), i == 0));
        connect(button, &QToolButton::clicked, this, [this, i] { mixerSwatch_ = i; refreshMixerColorPage(); });
        mixerSwatchButtons_[size_t(i)] = button;
        swatches->addWidget(button);
    }
    swatches->addStretch();
    colorLayout->addLayout(swatches);
    auto *colorForm = new QFormLayout;
    const auto write = [this](std::array<QDoubleSpinBox *, 8> CameraRawDialog::*group, QDoubleSpinBox *CameraRawDialog::*own) {
        return [this, group, own] { if (updating_) return; (this->*group)[size_t(mixerSwatch_)]->setValue((this->*own)->value()); };
    };
    addScrubRow(colorForm, tr("Hue"), this, -100.0, 100.0, 0.0, 0, mixerColorHueSpin_, write(&CameraRawDialog::mixerHueSpins_, &CameraRawDialog::mixerColorHueSpin_), std::nullopt, std::nullopt, QString(), SliderTrack::hue(0));
    addScrubRow(colorForm, tr("Saturation"), this, -100.0, 100.0, 0.0, 0, mixerColorSatSpin_, write(&CameraRawDialog::mixerSatSpins_, &CameraRawDialog::mixerColorSatSpin_), std::nullopt, std::nullopt, QString(), SliderTrack::saturation(0));
    addScrubRow(colorForm, tr("Luminance"), this, -100.0, 100.0, 0.0, 0, mixerColorLumSpin_, write(&CameraRawDialog::mixerLumSpins_, &CameraRawDialog::mixerColorLumSpin_), std::nullopt, std::nullopt, QString(), SliderTrack::luminance(0));
    colorLayout->addLayout(colorForm);
    colorLayout->addStretch();
    mixerStack_->addWidget(colorPage);

    // Point Color: colors picked from the picture, up to eight
    auto *pointPage = new QWidget(w);
    auto *pointLayout = new QVBoxLayout(pointPage);
    auto *pointTop = new QHBoxLayout;
    pointEyedropper_ = new QPushButton(tr("Eyedropper"), pointPage);
    pointEyedropper_->setCheckable(true);
    pointEyedropper_->setObjectName(QStringLiteral("pointColorEyedropper"));
    pointEyedropper_->setToolTip(tr("Click the picture to save a color. Up to eight colors."));
    connect(pointEyedropper_, &QPushButton::toggled, this, [this](bool on) { setProbe(on ? Probe::PointColor : (probe_ == Probe::PointColor ? Probe::None : probe_)); });
    pointTop->addWidget(pointEyedropper_);
    pointSwatchHost_ = new QWidget(pointPage);
    new QHBoxLayout(pointSwatchHost_);
    pointSwatchHost_->layout()->setContentsMargins(0, 0, 0, 0);
    pointTop->addWidget(pointSwatchHost_);
    auto *removePoint = new QPushButton(tr("Remove"), pointPage);
    removePoint->setObjectName(QStringLiteral("pointColorRemove"));
    connect(removePoint, &QPushButton::clicked, this, [this] {
        auto &points = settings_.mixer.points;
        if (pointIndex_ < 0 || pointIndex_ >= points.size()) return;
        points.remove(pointIndex_);
        pointIndex_ = std::max(0, std::min(pointIndex_, int(points.size()) - 1));
        refreshPointPage(); updatePreview(); refreshEyes();
    });
    pointTop->addWidget(removePoint);
    pointTop->addStretch();
    pointLayout->addLayout(pointTop);
    pointSliders_ = new QWidget(pointPage);
    auto *pointForm = new QFormLayout(pointSliders_);
    pointForm->setContentsMargins(0, 0, 0, 0);
    const auto writePoint = [this](double CameraRawPointColor::*field, QDoubleSpinBox *CameraRawDialog::*spin) {
        return [this, field, spin] {
            if (updating_) return;
            auto &points = settings_.mixer.points;
            if (pointIndex_ < 0 || pointIndex_ >= points.size()) return;
            points[pointIndex_].*field = (this->*spin)->value();
            updatePreview(); refreshEyes();
        };
    };
    addScrubRow(pointForm, tr("Hue Shift"), this, -100.0, 100.0, 0.0, 0, pointHueShiftSpin_, writePoint(&CameraRawPointColor::hueShift, &CameraRawDialog::pointHueShiftSpin_), std::nullopt, std::nullopt, QString(), SliderTrack::hue(0));
    addScrubRow(pointForm, tr("Saturation Shift"), this, -100.0, 100.0, 0.0, 0, pointSatShiftSpin_, writePoint(&CameraRawPointColor::saturationShift, &CameraRawDialog::pointSatShiftSpin_), std::nullopt, std::nullopt, QString(), SliderTrack::saturation(0));
    addScrubRow(pointForm, tr("Luminance Shift"), this, -100.0, 100.0, 0.0, 0, pointLumShiftSpin_, writePoint(&CameraRawPointColor::luminanceShift, &CameraRawDialog::pointLumShiftSpin_), std::nullopt, std::nullopt, QString(), SliderTrack::luminance(0));
    addScrubRow(pointForm, tr("Hue Range"), this, 5.0, 180.0, 30.0, 0, pointHueRangeSpin_, writePoint(&CameraRawPointColor::hueRange, &CameraRawDialog::pointHueRangeSpin_));
    addScrubRow(pointForm, tr("Saturation Range"), this, 0.05, 1.0, 0.4, 2, pointSatRangeSpin_, writePoint(&CameraRawPointColor::saturationRange, &CameraRawDialog::pointSatRangeSpin_));
    addScrubRow(pointForm, tr("Luminance Range"), this, 0.05, 1.0, 0.4, 2, pointLumRangeSpin_, writePoint(&CameraRawPointColor::luminanceRange, &CameraRawDialog::pointLumRangeSpin_));
    pointVisualizeBox_ = new QCheckBox(tr("Visualize Range"), pointSliders_);
    pointVisualizeBox_->setToolTip(tr("Dims the picture outside this color's range. It is not kept when you press OK."));
    connect(pointVisualizeBox_, &QCheckBox::toggled, this, [this](bool on) {
        if (updating_) return;
        auto &points = settings_.mixer.points;
        if (pointIndex_ < 0 || pointIndex_ >= points.size()) return;
        points[pointIndex_].visualize = on;
        updatePreview();
    });
    pointForm->addRow(pointVisualizeBox_);
    pointLayout->addWidget(pointSliders_);
    pointLayout->addStretch();
    mixerStack_->addWidget(pointPage);

    connect(mixerPageCombo_, &QComboBox::currentIndexChanged, this, [this](int page) { mixerStack_->setCurrentIndex(page); updatePreview(); });
    return w;
}

namespace {
// mac GradeWheel: hue runs counterclockwise from red at the right, distance from the centre is saturation; a double-click
// resets the wheel.
class GradeWheelWidget final : public QWidget {
public:
    GradeWheelWidget(QDoubleSpinBox *hue, QDoubleSpinBox *saturation, QWidget *parent)
        : QWidget(parent), hue_(hue), saturation_(saturation)
    {
        setFixedSize(86, 86);
        setToolTip(QObject::tr("Drag to set hue and saturation. Double-click to reset this wheel."));
        for (QDoubleSpinBox *spin : {hue, saturation}) connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { update(); });
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        const QRectF disk = QRectF(rect()).adjusted(6, 6, -6, -6);
        QConicalGradient gradient(disk.center(), 0);
        for (int i = 0; i <= 12; ++i) gradient.setColorAt(i / 12.0, QColor::fromHsvF(std::fmod(i * 30.0, 360.0) / 360.0, 1, 1));
        painter.setOpacity(.85); painter.setBrush(gradient); painter.setPen(Qt::NoPen); painter.drawEllipse(disk);
        painter.setOpacity(1); painter.setBrush(Qt::NoBrush); painter.setPen(QColor(255, 255, 255, 200)); painter.drawEllipse(disk);
        const double radius = disk.width() / 2, angle = hue_->value() * M_PI / 180, distance = saturation_->value() / 100 * radius;
        painter.setBrush(Qt::white); painter.setPen(QColor(30, 30, 30));
        painter.drawEllipse(disk.center() + QPointF(std::cos(angle) * distance, -std::sin(angle) * distance), 5, 5);
    }
    void mousePressEvent(QMouseEvent *event) override { set(event->position()); }
    void mouseMoveEvent(QMouseEvent *event) override { if (event->buttons() & Qt::LeftButton) set(event->position()); }
    void mouseDoubleClickEvent(QMouseEvent *) override { hue_->setValue(0); saturation_->setValue(0); }
private:
    void set(const QPointF &at)
    {
        const QPointF centre = QRectF(rect()).center(), delta(at.x() - centre.x(), centre.y() - at.y());
        double degrees = std::atan2(delta.y(), delta.x()) * 180 / M_PI;
        if (degrees < 0) degrees += 360;
        const double radius = width() / 2.0 - 6;
        hue_->setValue(std::round(degrees));
        saturation_->setValue(std::round(std::min(100.0, std::hypot(delta.x(), delta.y()) / radius * 100)));
    }
    QDoubleSpinBox *hue_, *saturation_;
};
} // namespace

QWidget *CameraRawDialog::createGradingTab()
{
    auto *w = new QWidget(this);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto *content = new QWidget(scroll);
    auto *form = new QFormLayout(content);
    const auto cb = [this] { syncToSettings(); };

    auto addWheel = [&](const QString &label, QDoubleSpinBox *&h, QDoubleSpinBox *&s, QDoubleSpinBox *&l) {
        auto *row = new QHBoxLayout();
        row->addWidget(new ScrubLabel(tr("Hue:"), h, 1.0, 1.0, content));
        row->addLayout(makeSliderRow(this, 0.0, 360.0, 0.0, 0, h, cb));
        row->addWidget(new ScrubLabel(tr("Sat:"), s, 1.0, 1.0, content));
        row->addLayout(makeSliderRow(this, 0.0, 100.0, 0.0, 0, s, cb));
        row->addWidget(new ScrubLabel(tr("Lum:"), l, 1.0, 1.0, content));
        row->addLayout(makeSliderRow(this, -100.0, 100.0, 0.0, 0, l, cb));
        form->addRow(label, row);
    };

    addWheel(tr("Shadows"), gradeShadowHueSpin_, gradeShadowSatSpin_, gradeShadowLumSpin_);
    addWheel(tr("Midtones"), gradeMidHueSpin_, gradeMidSatSpin_, gradeMidLumSpin_);
    addWheel(tr("Highlights"), gradeHighHueSpin_, gradeHighSatSpin_, gradeHighLumSpin_);
    addWheel(tr("Global"), gradeGlobalHueSpin_, gradeGlobalSatSpin_, gradeGlobalLumSpin_);
    auto *wheels = new QHBoxLayout;
    const auto addGrid = [&](const QString &title, QDoubleSpinBox *h, QDoubleSpinBox *sat) {
        auto *column = new QVBoxLayout;
        auto *caption = new QLabel(title, content); caption->setAlignment(Qt::AlignCenter);
        column->addWidget(caption); column->addWidget(new GradeWheelWidget(h, sat, content), 0, Qt::AlignCenter);
        wheels->addLayout(column);
    };
    addGrid(tr("Shadows"), gradeShadowHueSpin_, gradeShadowSatSpin_);
    addGrid(tr("Midtones"), gradeMidHueSpin_, gradeMidSatSpin_);
    addGrid(tr("Highlights"), gradeHighHueSpin_, gradeHighSatSpin_);
    addGrid(tr("Global"), gradeGlobalHueSpin_, gradeGlobalSatSpin_);
    form->insertRow(0, wheels);

    addScrubRow(form, tr("Blending"), content, 0.0, 100.0, 50.0, 0, gradeBlendingSpin_, cb);
    addScrubRow(form, tr("Balance"), content, -100.0, 100.0, 0.0, 0, gradeBalanceSpin_, cb);

    scroll->setWidget(content);
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(scroll);
    return w;
}

QWidget *CameraRawDialog::createDetailTab()
{
    auto *w = new QWidget(this);
    auto *form = new QFormLayout(w);
    const auto cb = [this] { syncToSettings(); };

    addScrubRow(form, tr("Sharpen Amount"), this, 0.0, 150.0, 0.0, 0, sharpenAmountSpin_, cb);
    addScrubRow(form, tr("Sharpen Radius"), this, 0.0, 100.0, 10.0, 0, sharpenRadiusSpin_, cb);
    addScrubRow(form, tr("Sharpen Detail"), this, 0.0, 100.0, 25.0, 0, sharpenDetailSpin_, cb);
    addScrubRow(form, tr("Sharpen Masking"), this, 0.0, 100.0, 0.0, 0, sharpenMaskingSpin_, cb);

    addScrubRow(form, tr("Noise Luminance"), this, 0.0, 100.0, 0.0, 0, noiseLumSpin_, cb);
    addScrubRow(form, tr("Noise Lum Detail"), this, 0.0, 100.0, 50.0, 0, noiseLumDetailSpin_, cb);
    addScrubRow(form, tr("Noise Lum Contrast"), this, 0.0, 100.0, 0.0, 0, noiseLumContrastSpin_, cb);
    addScrubRow(form, tr("Noise Color"), this, 0.0, 100.0, 0.0, 0, noiseColorSpin_, cb);
    addScrubRow(form, tr("Noise Color Detail"), this, 0.0, 100.0, 50.0, 0, noiseColorDetailSpin_, cb);
    addScrubRow(form, tr("Noise Color Smooth"), this, 0.0, 100.0, 50.0, 0, noiseColorSmoothSpin_, cb);
    return w;
}

QWidget *CameraRawDialog::createOpticsTab()
{
    auto *w = new QWidget(this);
    auto *form = new QFormLayout(w);
    const auto cb = [this] { syncToSettings(); };

    chromaticBox_ = new QCheckBox(tr("Remove Chromatic Aberration"), this);
    connect(chromaticBox_, &QCheckBox::toggled, this, cb);
    form->addRow(chromaticBox_);

    lensProfileBox_ = new QCheckBox(tr("Enable Lens Profile"), this);
    connect(lensProfileBox_, &QCheckBox::toggled, this, cb);
    form->addRow(lensProfileBox_);
    addScrubRow(form, tr("Profile Distortion"), this, 0.0, 100.0, 100.0, 0, profileDistortionSpin_, cb);
    addScrubRow(form, tr("Profile Vignetting"), this, 0.0, 100.0, 100.0, 0, profileVignettingSpin_, cb);

    addScrubRow(form, tr("Distortion"), this, -100.0, 100.0, 0.0, 0, opticsDistortionSpin_, cb);
    defringeEyedropper_ = new QPushButton(tr("Defringe Eyedropper"), this);
    defringeEyedropper_->setCheckable(true);
    defringeEyedropper_->setObjectName(QStringLiteral("defringeEyedropper"));
    defringeEyedropper_->setToolTip(tr("Click a purple or green fringe to set its hue range."));
    connect(defringeEyedropper_, &QPushButton::toggled, this, [this](bool on) { setProbe(on ? Probe::Defringe : (probe_ == Probe::Defringe ? Probe::None : probe_)); });
    form->addRow(defringeEyedropper_);
    addScrubRow(form, tr("Purple Defringe"), this, 0.0, 100.0, 0.0, 0, purpleAmountSpin_, cb);
    addScrubRow(form, tr("Purple Hue Low"), this, 0.0, 360.0, 270.0, 0, opticsPurpleLowSpin_, cb);
    addScrubRow(form, tr("Purple Hue High"), this, 0.0, 360.0, 310.0, 0, opticsPurpleHighSpin_, cb);
    addScrubRow(form, tr("Green Defringe"), this, 0.0, 100.0, 0.0, 0, greenAmountSpin_, cb);
    addScrubRow(form, tr("Green Hue Low"), this, 0.0, 360.0, 60.0, 0, opticsGreenLowSpin_, cb);
    addScrubRow(form, tr("Green Hue High"), this, 0.0, 360.0, 120.0, 0, opticsGreenHighSpin_, cb);
    addScrubRow(form, tr("Lens Vignette"), this, -100.0, 100.0, 0.0, 0, opticsVignetteSpin_, cb);
    addScrubRow(form, tr("Vignette Midpoint"), this, 0.0, 100.0, 50.0, 0, opticsVignetteMidpointSpin_, cb);
    return w;
}

QWidget *CameraRawDialog::createGeometryTab()
{
    auto *w = new QWidget(this);
    auto *form = new QFormLayout(w);
    const auto cb = [this] { syncToSettings(); };

    uprightCombo_ = new QComboBox(this);
    uprightCombo_->addItems({tr("Off"), tr("Guided")});
    connect(uprightCombo_, &QComboBox::currentIndexChanged, this, [this, cb](int mode) { if (mode != 1 && guideButton_) guideButton_->setChecked(false); cb(); });
    form->addRow(tr("Upright Mode"), uprightCombo_);
    guideButton_ = new QPushButton(tr("Draw Guides"), this);
    guideButton_->setCheckable(true);
    guideButton_->setObjectName(QStringLiteral("drawGuides"));
    guideButton_->setToolTip(tr("Draw two or more lines on the preview that should be level or vertical."));
    connect(guideButton_, &QPushButton::toggled, this, [this](bool on) { setProbe(on ? Probe::Guide : (probe_ == Probe::Guide ? Probe::None : probe_)); });
    clearGuidesButton_ = new QPushButton(tr("Clear Guides"), this);
    clearGuidesButton_->setObjectName(QStringLiteral("clearGuides"));
    connect(clearGuidesButton_, &QPushButton::clicked, this, [this] { settings_.geometry.guides.clear(); updatePreview(); if (canvas_) canvas_->update(); });
    auto *guideRow = new QHBoxLayout; guideRow->addWidget(guideButton_); guideRow->addWidget(clearGuidesButton_); guideRow->addStretch();
    form->addRow(guideRow);

    projectionCombo_ = new QComboBox(this);
    projectionCombo_->addItems({tr("Perspective"), tr("Rectilinear")});
    connect(projectionCombo_, &QComboBox::currentIndexChanged, this, cb);
    form->addRow(tr("Projection"), projectionCombo_);

    addScrubRow(form, tr("Vertical"), this, -100.0, 100.0, 0.0, 0, geoVerticalSpin_, cb);
    addScrubRow(form, tr("Horizontal"), this, -100.0, 100.0, 0.0, 0, geoHorizontalSpin_, cb);
    addScrubRow(form, tr("Rotate"), this, -45.0, 45.0, 0.0, 0, geoRotateSpin_, cb);
    addScrubRow(form, tr("Aspect"), this, -100.0, 100.0, 0.0, 0, geoAspectSpin_, cb);
    addScrubRow(form, tr("Scale"), this, -100.0, 100.0, 0.0, 0, geoScaleSpin_, cb);
    addScrubRow(form, tr("Offset X"), this, -100.0, 100.0, 0.0, 0, geoOffsetXSpin_, cb);
    addScrubRow(form, tr("Offset Y"), this, -100.0, 100.0, 0.0, 0, geoOffsetYSpin_, cb);

    constrainCropBox_ = new QCheckBox(tr("Constrain Crop"), this);
    connect(constrainCropBox_, &QCheckBox::toggled, this, cb);
    form->addRow(constrainCropBox_);
    return w;
}

QWidget *CameraRawDialog::createCalibrationTab()
{
    auto *w = new QWidget(this);
    auto *form = new QFormLayout(w);
    const auto cb = [this] { syncToSettings(); };

    processVersionCombo_ = new QComboBox(this);
    processVersionCombo_->addItems({tr("Version 1"), tr("Version 2"), tr("Version 3"),
                                    tr("Version 4"), tr("Version 5"), tr("Version 6")});
    processVersionCombo_->setCurrentIndex(5);
    connect(processVersionCombo_, &QComboBox::currentIndexChanged, this, cb);
    form->addRow(tr("Process Version"), processVersionCombo_);

    addScrubRow(form, tr("Shadow Tint"), this, -100.0, 100.0, 0.0, 0, calShadowTintSpin_, cb);
    addScrubRow(form, tr("Red Hue"), this, -100.0, 100.0, 0.0, 0, calRedHueSpin_, cb);
    addScrubRow(form, tr("Red Saturation"), this, -100.0, 100.0, 0.0, 0, calRedSatSpin_, cb);
    addScrubRow(form, tr("Green Hue"), this, -100.0, 100.0, 0.0, 0, calGreenHueSpin_, cb);
    addScrubRow(form, tr("Green Saturation"), this, -100.0, 100.0, 0.0, 0, calGreenSatSpin_, cb);
    addScrubRow(form, tr("Blue Hue"), this, -100.0, 100.0, 0.0, 0, calBlueHueSpin_, cb);
    addScrubRow(form, tr("Blue Saturation"), this, -100.0, 100.0, 0.0, 0, calBlueSatSpin_, cb);
    return w;
}

void CameraRawDialog::syncToSettings()
{
    if (updating_) return;
    updating_ = true;

    // Light
    settings_.exposure = exposureSpin_->value();
    settings_.contrast = contrastSpin_->value();
    settings_.highlights = highlightsSpin_->value();
    settings_.shadows = shadowsSpin_->value();
    settings_.whites = whitesSpin_->value();
    settings_.blacks = blacksSpin_->value();

    // Color
    settings_.whiteBalance = static_cast<CameraRawWhiteBalance>(wbCombo_->currentIndex());
    settings_.temperature = tempSpin_->value();
    settings_.tint = tintSpin_->value();
    settings_.vibrance = vibranceSpin_->value();
    settings_.saturation = saturationSpin_->value();

    // Effects
    settings_.texture = textureSpin_->value();
    settings_.clarity = claritySpin_->value();
    settings_.dehaze = dehazeSpin_->value();
    settings_.glowStyle = static_cast<CameraRawGlowStyle>(glowStyleCombo_->currentIndex());
    settings_.glow = glowSpin_->value();
    settings_.glowRange = glowRangeSpin_->value();
    settings_.glowSpread = glowSpreadSpin_->value();
    settings_.glowWarmth = glowWarmthSpin_->value();
    settings_.vignetteStyle = static_cast<CameraRawVignetteStyle>(vignetteStyleCombo_->currentIndex());
    settings_.vignetteAmount = vignetteAmountSpin_->value();
    settings_.vignetteMidpoint = vignetteMidpointSpin_->value();
    settings_.vignetteRoundness = vignetteRoundnessSpin_->value();
    settings_.vignetteFeather = vignetteFeatherSpin_->value();
    settings_.vignetteHighlights = vignetteHighlightsSpin_->value();
    settings_.grainAmount = grainAmountSpin_->value();
    settings_.grainSize = grainSizeSpin_->value();
    settings_.grainRoughness = grainRoughnessSpin_->value();

    // Curve
    settings_.curve.highlights = curveHighlightsSpin_->value();
    settings_.curve.lights = curveLightsSpin_->value();
    settings_.curve.darks = curveDarksSpin_->value();
    settings_.curve.shadows = curveShadowsSpin_->value();
    settings_.curve.shadowSplit = curveShadowSplitSpin_->value();
    settings_.curve.darkSplit = curveDarkSplitSpin_->value();
    settings_.curve.lightSplit = curveLightSplitSpin_->value();
    settings_.curve.refineSaturation = curveRefineSatSpin_->value();

    // Mixer
    for (size_t i = 0; i < 8; ++i) {
        settings_.mixer.hue[i] = mixerHueSpins_[i]->value();
        settings_.mixer.saturation[i] = mixerSatSpins_[i]->value();
        settings_.mixer.luminance[i] = mixerLumSpins_[i]->value();
    }

    // Grading
    settings_.grading.shadows.hue = gradeShadowHueSpin_->value();
    settings_.grading.shadows.saturation = gradeShadowSatSpin_->value();
    settings_.grading.shadows.luminance = gradeShadowLumSpin_->value();
    settings_.grading.midtones.hue = gradeMidHueSpin_->value();
    settings_.grading.midtones.saturation = gradeMidSatSpin_->value();
    settings_.grading.midtones.luminance = gradeMidLumSpin_->value();
    settings_.grading.highlights.hue = gradeHighHueSpin_->value();
    settings_.grading.highlights.saturation = gradeHighSatSpin_->value();
    settings_.grading.highlights.luminance = gradeHighLumSpin_->value();
    settings_.grading.global.hue = gradeGlobalHueSpin_->value();
    settings_.grading.global.saturation = gradeGlobalSatSpin_->value();
    settings_.grading.global.luminance = gradeGlobalLumSpin_->value();
    settings_.grading.blending = gradeBlendingSpin_->value();
    settings_.grading.balance = gradeBalanceSpin_->value();

    // Detail
    settings_.detail.sharpenAmount = sharpenAmountSpin_->value();
    settings_.detail.sharpenRadius = sharpenRadiusSpin_->value();
    settings_.detail.sharpenDetail = sharpenDetailSpin_->value();
    settings_.detail.sharpenMasking = sharpenMaskingSpin_->value();
    settings_.detail.noiseLuminance = noiseLumSpin_->value();
    settings_.detail.noiseLuminanceDetail = noiseLumDetailSpin_->value();
    settings_.detail.noiseLuminanceContrast = noiseLumContrastSpin_->value();
    settings_.detail.noiseColor = noiseColorSpin_->value();
    settings_.detail.noiseColorDetail = noiseColorDetailSpin_->value();
    settings_.detail.noiseColorSmoothness = noiseColorSmoothSpin_->value();

    // Optics
    settings_.optics.removeChromaticAberration = chromaticBox_->isChecked();
    settings_.optics.enableLensProfile = lensProfileBox_->isChecked();
    settings_.optics.profileDistortion = profileDistortionSpin_->value();
    settings_.optics.profileVignetting = profileVignettingSpin_->value();
    settings_.optics.purpleHueLow = opticsPurpleLowSpin_->value();
    settings_.optics.purpleHueHigh = opticsPurpleHighSpin_->value();
    settings_.optics.greenHueLow = opticsGreenLowSpin_->value();
    settings_.optics.greenHueHigh = opticsGreenHighSpin_->value();
    settings_.optics.distortion = opticsDistortionSpin_->value();
    settings_.optics.purpleAmount = purpleAmountSpin_->value();
    settings_.optics.greenAmount = greenAmountSpin_->value();
    settings_.optics.vignetteAmount = opticsVignetteSpin_->value();
    settings_.optics.vignetteMidpoint = opticsVignetteMidpointSpin_->value();

    // Geometry
    settings_.geometry.upright = static_cast<CameraRawUprightMode>(uprightCombo_->currentIndex());
    settings_.geometry.projection = static_cast<CameraRawProjection>(projectionCombo_->currentIndex());
    settings_.geometry.vertical = geoVerticalSpin_->value();
    settings_.geometry.horizontal = geoHorizontalSpin_->value();
    settings_.geometry.rotate = geoRotateSpin_->value();
    settings_.geometry.aspect = geoAspectSpin_->value();
    settings_.geometry.scale = geoScaleSpin_->value();
    settings_.geometry.offsetX = geoOffsetXSpin_->value();
    settings_.geometry.offsetY = geoOffsetYSpin_->value();
    settings_.geometry.constrainCrop = constrainCropBox_->isChecked();

    // Calibration
    settings_.calibration.process = static_cast<CameraRawProcessVersion>(processVersionCombo_->currentIndex() + 1);
    settings_.calibration.shadowTint = calShadowTintSpin_->value();
    settings_.calibration.redHue = calRedHueSpin_->value();
    settings_.calibration.redSaturation = calRedSatSpin_->value();
    settings_.calibration.greenHue = calGreenHueSpin_->value();
    settings_.calibration.greenSaturation = calGreenSatSpin_->value();
    settings_.calibration.blueHue = calBlueHueSpin_->value();
    settings_.calibration.blueSaturation = calBlueSatSpin_->value();

    updating_ = false;
    refreshEyes();
    updatePreview();
}

void CameraRawDialog::loadFromSettings()
{
    const bool wasUpdating = updating_;
    updating_ = true;
    const QSignalBlocker presetBlock(curvePresetCombo_), wbBlock(wbCombo_);
    const CameraRawSettings &v = settings_;
    exposureSpin_->setValue(v.exposure); contrastSpin_->setValue(v.contrast); highlightsSpin_->setValue(v.highlights);
    shadowsSpin_->setValue(v.shadows); whitesSpin_->setValue(v.whites); blacksSpin_->setValue(v.blacks);
    wbCombo_->setCurrentIndex(int(v.whiteBalance)); tempSpin_->setValue(v.temperature); tintSpin_->setValue(v.tint);
    vibranceSpin_->setValue(v.vibrance); saturationSpin_->setValue(v.saturation);
    textureSpin_->setValue(v.texture); claritySpin_->setValue(v.clarity); dehazeSpin_->setValue(v.dehaze);
    glowStyleCombo_->setCurrentIndex(int(v.glowStyle)); glowSpin_->setValue(v.glow); glowRangeSpin_->setValue(v.glowRange);
    glowSpreadSpin_->setValue(v.glowSpread); glowWarmthSpin_->setValue(v.glowWarmth);
    vignetteStyleCombo_->setCurrentIndex(int(v.vignetteStyle)); vignetteAmountSpin_->setValue(v.vignetteAmount);
    vignetteMidpointSpin_->setValue(v.vignetteMidpoint); vignetteRoundnessSpin_->setValue(v.vignetteRoundness);
    vignetteFeatherSpin_->setValue(v.vignetteFeather); vignetteHighlightsSpin_->setValue(v.vignetteHighlights);
    grainAmountSpin_->setValue(v.grainAmount); grainSizeSpin_->setValue(v.grainSize); grainRoughnessSpin_->setValue(v.grainRoughness);
    curveHighlightsSpin_->setValue(v.curve.highlights); curveLightsSpin_->setValue(v.curve.lights); curveDarksSpin_->setValue(v.curve.darks);
    curveShadowsSpin_->setValue(v.curve.shadows); curveShadowSplitSpin_->setValue(v.curve.shadowSplit);
    curveDarkSplitSpin_->setValue(v.curve.darkSplit); curveLightSplitSpin_->setValue(v.curve.lightSplit);
    curveRefineSatSpin_->setValue(v.curve.refineSaturation);
    if (curveGraph_) curveGraph_->update();
    for (size_t i = 0; i < 8; ++i) { mixerHueSpins_[i]->setValue(v.mixer.hue[i]); mixerSatSpins_[i]->setValue(v.mixer.saturation[i]); mixerLumSpins_[i]->setValue(v.mixer.luminance[i]); }
    gradeShadowHueSpin_->setValue(v.grading.shadows.hue); gradeShadowSatSpin_->setValue(v.grading.shadows.saturation); gradeShadowLumSpin_->setValue(v.grading.shadows.luminance);
    gradeMidHueSpin_->setValue(v.grading.midtones.hue); gradeMidSatSpin_->setValue(v.grading.midtones.saturation); gradeMidLumSpin_->setValue(v.grading.midtones.luminance);
    gradeHighHueSpin_->setValue(v.grading.highlights.hue); gradeHighSatSpin_->setValue(v.grading.highlights.saturation); gradeHighLumSpin_->setValue(v.grading.highlights.luminance);
    gradeGlobalHueSpin_->setValue(v.grading.global.hue); gradeGlobalSatSpin_->setValue(v.grading.global.saturation); gradeGlobalLumSpin_->setValue(v.grading.global.luminance);
    gradeBlendingSpin_->setValue(v.grading.blending); gradeBalanceSpin_->setValue(v.grading.balance);
    sharpenAmountSpin_->setValue(v.detail.sharpenAmount); sharpenRadiusSpin_->setValue(v.detail.sharpenRadius);
    sharpenDetailSpin_->setValue(v.detail.sharpenDetail); sharpenMaskingSpin_->setValue(v.detail.sharpenMasking);
    noiseLumSpin_->setValue(v.detail.noiseLuminance); noiseLumDetailSpin_->setValue(v.detail.noiseLuminanceDetail);
    noiseLumContrastSpin_->setValue(v.detail.noiseLuminanceContrast); noiseColorSpin_->setValue(v.detail.noiseColor);
    noiseColorDetailSpin_->setValue(v.detail.noiseColorDetail); noiseColorSmoothSpin_->setValue(v.detail.noiseColorSmoothness);
    chromaticBox_->setChecked(v.optics.removeChromaticAberration); lensProfileBox_->setChecked(v.optics.enableLensProfile);
    profileDistortionSpin_->setValue(v.optics.profileDistortion); profileVignettingSpin_->setValue(v.optics.profileVignetting);
    opticsDistortionSpin_->setValue(v.optics.distortion); purpleAmountSpin_->setValue(v.optics.purpleAmount);
    opticsPurpleLowSpin_->setValue(v.optics.purpleHueLow); opticsPurpleHighSpin_->setValue(v.optics.purpleHueHigh);
    greenAmountSpin_->setValue(v.optics.greenAmount); opticsGreenLowSpin_->setValue(v.optics.greenHueLow); opticsGreenHighSpin_->setValue(v.optics.greenHueHigh);
    opticsVignetteSpin_->setValue(v.optics.vignetteAmount); opticsVignetteMidpointSpin_->setValue(v.optics.vignetteMidpoint);
    uprightCombo_->setCurrentIndex(int(v.geometry.upright)); projectionCombo_->setCurrentIndex(int(v.geometry.projection));
    geoVerticalSpin_->setValue(v.geometry.vertical); geoHorizontalSpin_->setValue(v.geometry.horizontal); geoRotateSpin_->setValue(v.geometry.rotate);
    geoAspectSpin_->setValue(v.geometry.aspect); geoScaleSpin_->setValue(v.geometry.scale);
    geoOffsetXSpin_->setValue(v.geometry.offsetX); geoOffsetYSpin_->setValue(v.geometry.offsetY); constrainCropBox_->setChecked(v.geometry.constrainCrop);
    processVersionCombo_->setCurrentIndex(int(v.calibration.process) - 1); calShadowTintSpin_->setValue(v.calibration.shadowTint);
    calRedHueSpin_->setValue(v.calibration.redHue); calRedSatSpin_->setValue(v.calibration.redSaturation);
    calGreenHueSpin_->setValue(v.calibration.greenHue); calGreenSatSpin_->setValue(v.calibration.greenSaturation);
    calBlueHueSpin_->setValue(v.calibration.blueHue); calBlueSatSpin_->setValue(v.calibration.blueSaturation);
    updating_ = wasUpdating;
    pointIndex_ = std::clamp(pointIndex_, 0, std::max(0, int(settings_.mixer.points.size()) - 1));
    refreshMixerColorPage(); refreshPointPage(); refreshEyes();
}

void CameraRawDialog::refreshEyes()
{
    const std::array<bool, 10> adjusts{settings_.adjustsLight(), settings_.adjustsColor(), settings_.adjustsEffects(), settings_.adjustsCurve(),
        settings_.adjustsMixer(), settings_.adjustsGrading(), settings_.adjustsDetail(), settings_.adjustsOptics(),
        settings_.adjustsGeometry(), settings_.adjustsCalibration()};
    for (size_t i = 0; i < 10; ++i) if (showBoxes_[i]) {
        const QSignalBlocker blocker(showBoxes_[i]);
        showBoxes_[i]->setChecked(shows_[i]);
        showBoxes_[i]->setVisible(adjusts[i] || !shows_[i]);
    }
}

void CameraRawDialog::refreshMixerColorPage()
{
    if (!mixerColorHueSpin_) return;
    const bool wasUpdating = updating_;
    updating_ = true;
    const size_t i = size_t(std::clamp(mixerSwatch_, 0, 7));
    mixerColorHueSpin_->setValue(mixerHueSpins_[i]->value());
    mixerColorSatSpin_->setValue(mixerSatSpins_[i]->value());
    mixerColorLumSpin_->setValue(mixerLumSpins_[i]->value());
    updating_ = wasUpdating;
    const double center = CameraRawMixerSettings::centers[i];
    for (QDoubleSpinBox *spin : {mixerColorHueSpin_, mixerColorSatSpin_, mixerColorLumSpin_}) {
        QSlider *track = spin->parentWidget() ? spin->parentWidget()->findChild<QSlider *>() : nullptr;
        Q_UNUSED(track)
    }
    for (size_t k = 0; k < 8; ++k) if (mixerSwatchButtons_[k]) mixerSwatchButtons_[k]->setStyleSheet(swatchStyle(swatchColor(CameraRawMixerSettings::centers[k]), k == i));
    Q_UNUSED(center)
}

void CameraRawDialog::refreshPointPage()
{
    if (!pointSwatchHost_) return;
    auto *layout = pointSwatchHost_->layout();
    while (QLayoutItem *item = layout->takeAt(0)) { delete item->widget(); delete item; }
    const auto &points = settings_.mixer.points;
    for (int i = 0; i < points.size(); ++i) {
        auto *button = new QToolButton(pointSwatchHost_);
        button->setObjectName(QStringLiteral("pointSwatch%1").arg(i));
        button->setToolTip(tr("Select this picked color."));
        button->setStyleSheet(swatchStyle(QColor::fromHsvF(std::fmod(points[i].hue, 360.0) / 360.0, std::clamp(points[i].saturation, 0.0, 1.0), std::clamp(points[i].luminance, 0.0, 1.0)), i == pointIndex_));
        connect(button, &QToolButton::clicked, this, [this, i] { pointIndex_ = i; refreshPointPage(); updatePreview(); });
        layout->addWidget(button);
    }
    const bool has = pointIndex_ >= 0 && pointIndex_ < points.size();
    pointSliders_->setVisible(has);
    if (!has) return;
    const CameraRawPointColor &p = points[pointIndex_];
    const bool wasUpdating = updating_;
    updating_ = true;
    pointHueShiftSpin_->setValue(p.hueShift); pointSatShiftSpin_->setValue(p.saturationShift); pointLumShiftSpin_->setValue(p.luminanceShift);
    pointHueRangeSpin_->setValue(p.hueRange); pointSatRangeSpin_->setValue(p.saturationRange); pointLumRangeSpin_->setValue(p.luminanceRange);
    { const QSignalBlocker blocker(pointVisualizeBox_); pointVisualizeBox_->setChecked(p.visualize); }
    updating_ = wasUpdating;
    for (auto [spin, track] : {std::pair{pointHueShiftSpin_, SliderTrack::hue(p.hue)}, std::pair{pointSatShiftSpin_, SliderTrack::saturation(p.hue)}, std::pair{pointLumShiftSpin_, SliderTrack::luminance(p.hue)}})
        if (QSlider *slider = spin->parentWidget() ? spin->parentWidget()->findChild<QSlider *>() : nullptr) SliderTrack::apply(slider, track);
}

CameraRawSettings CameraRawDialog::renderSettings() const
{
    return settings_.applying(shows_[0], shows_[1], shows_[2], shows_[3], shows_[4], shows_[5], shows_[6], shows_[7], shows_[8], shows_[9]);
}

void CameraRawDialog::setSettings(const CameraRawSettings &settings)
{
    settings_ = settings.normalized();
    loadFromSettings();
    updatePreview();
}

void CameraRawDialog::updatePreview()
{
    if (!previewBox_->isChecked()) {
        session_.rollbackLayerImage(layerId_, originalLayer_.image);
        lastGraded_ = QImage();
    } else {
        CameraRawClipping clip = activeClipping_;
        if (!altHeld_) clip = CameraRawClipping::None;
        const CameraRawSettings rendered = renderSettings();
        int visualize = -1;
        if (mixerPageCombo_ && mixerPageCombo_->currentIndex() == 2 && pointIndex_ >= 0 && pointIndex_ < rendered.mixer.points.size()
            && rendered.mixer.points[pointIndex_].visualize) visualize = pointIndex_;

        QImage graded = RasterOperations::cameraRaw(originalLayer_.image, rendered, clip, 1.0, 0, visualize, sharpenMaskPreview_);
        lastGraded_ = graded;
        if (showShadowClipping_ || showHighlightClipping_) {
            graded = CameraRawScope::overlay(graded, showShadowClipping_, showHighlightClipping_);
        }
        session_.previewLayerImage(layerId_, graded);
        auto scope = CameraRawScope::make(lastGraded_);
        if (scope) scopeWidget_->setScope(*scope);
    }

    if (onPreview_) onPreview_();
}

void CameraRawDialog::sampleWhiteBalance(const QColor &color)
{
    auto solved = CameraRawSettings::neutralizeStraight(
        color.redF(), color.greenF(), color.blueF());
    if (solved) {
        tempSpin_->setValue(solved->first);
        tintSpin_->setValue(solved->second);
        wbCombo_->setCurrentIndex(0); // Custom
        syncToSettings();
    }
}

void CameraRawDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Alt) {
        altHeld_ = true;
        updatePreview();
    }
    QDialog::keyPressEvent(event);
}

void CameraRawDialog::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Alt) {
        altHeld_ = false;
        activeClipping_ = CameraRawClipping::None;
        updatePreview();
    }
    QDialog::keyReleaseEvent(event);
}

bool CameraRawDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::FocusIn) {
        QObject *spin = watched;
        if (auto *slider = qobject_cast<QSlider *>(watched)) spin = slider->property("cameraRawSpin").value<QObject *>();
        if (spin == exposureSpin_ || spin == highlightsSpin_ || spin == whitesSpin_) {
            activeClipping_ = CameraRawClipping::Highlights;
        } else if (spin == shadowsSpin_ || spin == blacksSpin_) {
            activeClipping_ = CameraRawClipping::Shadows;
        } else {
            activeClipping_ = CameraRawClipping::None;
        }
        // Option on Sharpening Masking shows the mask (mac cameraRawSharpenMask).
        const bool mask = spin == sharpenMaskingSpin_ && altHeld_;
        if (mask != sharpenMaskPreview_) { sharpenMaskPreview_ = mask; updatePreview(); }
        else if (altHeld_) updatePreview();
    }
    return QDialog::eventFilter(watched, event);
}

void CameraRawDialog::accept()
{
    // mac commitFilter: what was rendered (hidden groups dropped) is what the filter remembers and what OK applies.
    const CameraRawSettings rendered = renderSettings();
    session_.lastCameraRaw = rendered;
    session_.rollbackLayer(layerId_, originalLayer_);
    if (!rendered.isIdentity()) {
        session_.applyCameraRaw(layerId_, rendered);
    }
    releaseCanvas();
    if (onPreview_) onPreview_();
    QDialog::accept();
}

void CameraRawDialog::reject()
{
    session_.rollbackLayer(layerId_, originalLayer_);
    releaseCanvas();
    if (onPreview_) onPreview_();
    QDialog::reject();
}

// ---------------------------------------------------------------------------------------------- canvas probes

void CameraRawDialog::setProbe(Probe probe)
{
    probe_ = probe;
    const auto sync = [](QPushButton *button, bool on) { if (button && button->isChecked() != on) { const QSignalBlocker blocker(button); button->setChecked(on); } };
    sync(pointEyedropper_, probe == Probe::PointColor);
    sync(defringeEyedropper_, probe == Probe::Defringe);
    sync(curveTarget_, probe == Probe::CurveTarget);
    sync(mixerTarget_, probe == Probe::MixerTarget);
    sync(guideButton_, probe == Probe::Guide);
    if (wbEyedropper_ && probe != Probe::None && wbEyedropper_->isChecked()) wbEyedropper_->setChecked(false);
    if (canvas_) canvas_->setProbeMode(probe != Probe::None);
}

std::optional<QPoint> CameraRawDialog::layerPixel(const QPointF &documentPoint) const
{
    if (originalLayer_.image.isNull()) return std::nullopt;
    bool ok = false;
    const QTransform inverse = LayerRenderer::pixelToDocument(originalLayer_.transform, originalLayer_.image.size()).inverted(&ok);
    if (!ok) return std::nullopt;
    const QPointF pixel = inverse.map(documentPoint);
    const QPoint point(qFloor(pixel.x()), qFloor(pixel.y()));
    if (!QRect(QPoint(), originalLayer_.image.size()).contains(point)) return std::nullopt;
    return point;
}

std::optional<CameraRawDialog::ToneSample> CameraRawDialog::sampleAt(const QPointF &documentPoint, bool fromPreview) const
{
    const auto point = layerPixel(documentPoint);
    if (!point) return std::nullopt;
    const QImage &source = fromPreview && !lastGraded_.isNull() ? lastGraded_ : originalLayer_.image;
    if (!QRect(QPoint(), source.size()).contains(*point)) return std::nullopt;
    const QColor color = source.convertToFormat(QImage::Format_RGBA8888_Premultiplied).pixelColor(*point);
    if (color.alpha() == 0) return std::nullopt;
    const double r = color.redF(), g = color.greenF(), b = color.blueF();
    const double maxc = std::max({r, g, b}), minc = std::min({r, g, b}), chroma = maxc - minc;
    double hue = 0;
    if (chroma > 1e-6) {
        if (maxc == r) hue = (g - b) / chroma; else if (maxc == g) hue = 2 + (b - r) / chroma; else hue = 4 + (r - g) / chroma;
        hue /= 6; if (hue < 0) hue += 1;
    }
    return ToneSample{0.2126 * r + 0.7152 * g + 0.0722 * b, hue * 360, maxc == 0 ? 0 : chroma / maxc, (maxc + minc) / 2};
}

void CameraRawDialog::probePress(const QPointF &documentPoint)
{
    switch (probe_) {
    case Probe::PointColor: {
        const auto sample = sampleAt(documentPoint, true);
        if (!sample) return;
        CameraRawPointColor color; color.hue = sample->hue; color.saturation = sample->saturation; color.luminance = sample->luminance;
        auto &points = settings_.mixer.points;
        if (pointIndex_ >= 0 && pointIndex_ < points.size()) {
            color.hueShift = points[pointIndex_].hueShift; color.saturationShift = points[pointIndex_].saturationShift; color.luminanceShift = points[pointIndex_].luminanceShift;
            points[pointIndex_] = color;
        } else if (points.size() < 8) {
            points.push_back(color); pointIndex_ = int(points.size()) - 1;
        } else return;
        refreshPointPage(); refreshEyes(); updatePreview();
        break;
    }
    case Probe::Defringe: {
        const auto sample = sampleAt(documentPoint, false);
        if (!sample) return;
        auto &optics = settings_.optics;
        const double span = 25, hue = sample->hue;
        if (std::abs(hue - 290.0) < std::abs(hue - 90.0)) {
            optics.purpleHueLow = hue - span; optics.purpleHueHigh = hue + span;
            if (optics.purpleAmount == 0) optics.purpleAmount = 50;
        } else {
            optics.greenHueLow = hue - span; optics.greenHueHigh = hue + span;
            if (optics.greenAmount == 0) optics.greenAmount = 50;
        }
        optics = optics.normalized();
        loadFromSettings(); updatePreview();
        break;
    }
    case Probe::CurveTarget:
    case Probe::MixerTarget: {
        const auto sample = sampleAt(documentPoint, true);
        if (!sample) return;
        dragStart_ = settings_; dragStartY_ = documentPoint.y(); dragSample_ = *sample; dragging_ = true;
        break;
    }
    case Probe::Guide: {
        const auto point = layerPixel(documentPoint);
        if (!point) return;
        const QSizeF size = originalLayer_.image.size();
        const double x = (documentPoint.x() - 0) , y = documentPoint.y();
        Q_UNUSED(x) Q_UNUSED(y)
        bool ok = false;
        const QPointF pixel = LayerRenderer::pixelToDocument(originalLayer_.transform, originalLayer_.image.size()).inverted(&ok).map(documentPoint);
        guideDraft_ = CameraRawGeometryGuide{pixel.x() / size.width(), pixel.y() / size.height(), pixel.x() / size.width(), pixel.y() / size.height()};
        if (canvas_) canvas_->update();
        break;
    }
    case Probe::None: break;
    }
}

void CameraRawDialog::probeMove(const QPointF &documentPoint, bool dragging)
{
    if (!dragging) {
        if (!readout_) return;
        const auto point = layerPixel(documentPoint);
        const QImage &source = lastGraded_.isNull() ? originalLayer_.image : lastGraded_;
        if (!point || !QRect(QPoint(), source.size()).contains(*point)) { readout_->setText(QStringLiteral("R —   G —   B —")); return; }
        const QColor c = source.convertToFormat(QImage::Format_RGBA8888_Premultiplied).pixelColor(*point);
        readout_->setText(c.alpha() == 0 ? QStringLiteral("R —   G —   B —") : QStringLiteral("R %1   G %2   B %3").arg(c.red()).arg(c.green()).arg(c.blue()));
        return;
    }
    if (probe_ == Probe::Guide && guideDraft_) {
        bool ok = false;
        const QSizeF size = originalLayer_.image.size();
        const QPointF pixel = LayerRenderer::pixelToDocument(originalLayer_.transform, originalLayer_.image.size()).inverted(&ok).map(documentPoint);
        if (pixel.x() >= 0 && pixel.y() >= 0 && pixel.x() < size.width() && pixel.y() < size.height()) { guideDraft_->endX = pixel.x() / size.width(); guideDraft_->endY = pixel.y() / size.height(); }
        if (canvas_) canvas_->update();
        return;
    }
    if (!dragging_) return;
    // mac dragCameraRaw: dragging up raises, 0.35 per pixel, always measured from where the drag began.
    const double delta = (dragStartY_ - documentPoint.y()) * 0.35;
    CameraRawSettings next = dragStart_;
    if (probe_ == Probe::CurveTarget) {
        if (curvePageCombo_ && curvePageCombo_->currentIndex() == 0) {
            const auto &c = dragStart_.curve; const double tone = dragSample_.tone;
            double *field = tone < c.shadowSplit / 100 ? &next.curve.shadows : tone < c.darkSplit / 100 ? &next.curve.darks : tone < c.lightSplit / 100 ? &next.curve.lights : &next.curve.highlights;
            const double start = tone < c.shadowSplit / 100 ? c.shadows : tone < c.darkSplit / 100 ? c.darks : tone < c.lightSplit / 100 ? c.lights : c.highlights;
            *field = std::clamp(start + delta, -100.0, 100.0);
        } else {
            next.curve = dragStart_.curve.nudged(CameraRawPointChannel(curveChannelCombo_ ? curveChannelCombo_->currentIndex() : 0), dragSample_.tone, delta / 100);
        }
    } else if (probe_ == Probe::MixerTarget) {
        const auto weights = CameraRawMixerSettings::weights(dragSample_.hue);
        const int component = mixerComponentCombo_ ? mixerComponentCombo_->currentIndex() : 0;
        for (size_t i = 0; i < 8; ++i) if (weights[i] > 0) {
            auto &array = component == 0 ? next.mixer.hue : component == 1 ? next.mixer.saturation : next.mixer.luminance;
            const auto &origin = component == 0 ? dragStart_.mixer.hue : component == 1 ? dragStart_.mixer.saturation : dragStart_.mixer.luminance;
            array[i] = std::clamp(origin[i] + delta * weights[i], -100.0, 100.0);
        }
    } else return;
    settings_ = next;
    loadFromSettings();
    updatePreview();
}

void CameraRawDialog::probeRelease(const QPointF &documentPoint)
{
    Q_UNUSED(documentPoint)
    dragging_ = false;
    if (probe_ == Probe::Guide && guideDraft_) {
        const CameraRawGeometryGuide guide = *guideDraft_;
        guideDraft_.reset();
        if (std::hypot(guide.endX - guide.startX, guide.endY - guide.startY) > 0.005) {
            settings_.geometry.guides.push_back(guide);
            settings_.geometry.upright = CameraRawUprightMode::Guided;
            loadFromSettings();
            updatePreview();
        }
        if (canvas_) canvas_->update();
    }
}

} // namespace compositor
