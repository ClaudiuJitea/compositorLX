#include "ui/CameraRawDialog.h"
#include "ui/NumericScrub.h"
#include "rendering/RasterOperations.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QMenu>
#include <QKeyEvent>
#include <QDialogButtonBox>
#include <QPainter>
#include <QPainterPath>
#include <QComboBox>
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
    resize(480, 680);

    if (auto doc = session_.document()) {
        for (const Layer &layer : doc->layers) {
            if (layer.id == layerId_) {
                originalLayer_ = layer;
                break;
            }
        }
    }


    setupUi();
    installEventFilter(this);
    updatePreview();
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
    tabWidget_->addTab(createLightTab(), tr("Light"));
    tabWidget_->addTab(createColorTab(), tr("Color"));
    tabWidget_->addTab(createEffectsTab(), tr("Effects"));
    tabWidget_->addTab(createCurveTab(), tr("Curve"));
    tabWidget_->addTab(createMixerTab(), tr("Mixer"));
    tabWidget_->addTab(createGradingTab(), tr("Grading"));
    tabWidget_->addTab(createDetailTab(), tr("Detail"));
    tabWidget_->addTab(createOpticsTab(), tr("Optics"));
    tabWidget_->addTab(createGeometryTab(), tr("Geometry"));
    tabWidget_->addTab(createCalibrationTab(), tr("Calibration"));

    mainLayout->addWidget(tabWidget_, 1);

    auto *bottomLayout = new QHBoxLayout();
    previewBox_ = new QCheckBox(tr("Preview"), this);
    previewBox_->setObjectName(QStringLiteral("previewBox"));
    previewBox_->setChecked(true);

    connect(previewBox_, &QCheckBox::toggled, this, &CameraRawDialog::updatePreview);
    bottomLayout->addWidget(previewBox_);

    bottomLayout->addStretch();

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &CameraRawDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &CameraRawDialog::reject);
    bottomLayout->addWidget(buttonBox);

    mainLayout->addLayout(bottomLayout);
}

static QHBoxLayout *makeSliderRow(QWidget *parent, double minVal, double maxVal, double curVal, int decimals,
                                  QDoubleSpinBox *&spin, std::function<void()> onChange)
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
                        const QString &objName = QString())
{
    auto *row = makeSliderRow(parent, minVal, maxVal, curVal, decimals, spin, onChange);
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

    addScrubRow(form, tr("Temperature"), this, -100.0, 100.0, 0.0, 0, tempSpin_, cb);
    addScrubRow(form, tr("Tint"), this, -100.0, 100.0, 0.0, 0, tintSpin_, cb);
    addScrubRow(form, tr("Vibrance"), this, -100.0, 100.0, 0.0, 0, vibranceSpin_, cb);
    addScrubRow(form, tr("Saturation"), this, -100.0, 100.0, 0.0, 0, saturationSpin_, cb);
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
    auto *form = new QFormLayout(w);
    const auto cb = [this] { syncToSettings(); if (curveGraph_) curveGraph_->update(); };

    curvePresetCombo_ = new QComboBox(this);
    curvePresetCombo_->addItems({tr("Custom"), tr("Linear"), tr("Medium Contrast"), tr("Strong Contrast")});
    connect(curvePresetCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (idx == 1) settings_.curve.rgb = CameraRawCurveSettings::linear;
        else if (idx == 2) settings_.curve.rgb = CameraRawCurveSettings::mediumContrast;
        else if (idx == 3) settings_.curve.rgb = CameraRawCurveSettings::strongContrast;
        syncToSettings();
        if (curveGraph_) curveGraph_->update();
    });
    form->addRow(tr("Point Curve Preset"), curvePresetCombo_);

    auto *graph = new CameraRawCurveGraph(w); graph->curve = &settings_.curve; graph->setToolTip(tr("Click to add a point, drag to move it, double-click an inner point to remove it."));
    auto *channelCombo = new QComboBox(w); channelCombo->addItems({tr("RGB"), tr("Red"), tr("Green"), tr("Blue")});
    connect(channelCombo, &QComboBox::currentIndexChanged, this, [graph](int index) { graph->channel = index; graph->update(); });
    graph->changed = [this] { if (curvePresetCombo_) { const QSignalBlocker blocker(curvePresetCombo_); curvePresetCombo_->setCurrentIndex(0); } syncToSettings(); };
    curveGraph_ = graph;
    form->addRow(tr("Channel"), channelCombo); form->addRow(graph);

    addScrubRow(form, tr("Highlights"), this, -100.0, 100.0, 0.0, 0, curveHighlightsSpin_, cb);
    addScrubRow(form, tr("Lights"), this, -100.0, 100.0, 0.0, 0, curveLightsSpin_, cb);
    addScrubRow(form, tr("Darks"), this, -100.0, 100.0, 0.0, 0, curveDarksSpin_, cb);
    addScrubRow(form, tr("Shadows"), this, -100.0, 100.0, 0.0, 0, curveShadowsSpin_, cb);
    addScrubRow(form, tr("Shadow Split"), this, 5.0, 90.0, 25.0, 0, curveShadowSplitSpin_, cb);
    addScrubRow(form, tr("Dark Split"), this, 10.0, 95.0, 50.0, 0, curveDarkSplitSpin_, cb);
    addScrubRow(form, tr("Light Split"), this, 15.0, 98.0, 75.0, 0, curveLightSplitSpin_, cb);
    addScrubRow(form, tr("Refine Saturation"), this, -100.0, 100.0, 0.0, 0, curveRefineSatSpin_, cb);
    return w;
}

QWidget *CameraRawDialog::createMixerTab()
{
    auto *w = new QWidget(this);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto *content = new QWidget(scroll);
    auto *form = new QFormLayout(content);
    const auto cb = [this] { syncToSettings(); };

    const char *names[8] = {"Reds", "Oranges", "Yellows", "Greens", "Aquas", "Blues", "Purples", "Magentas"};
    for (int i = 0; i < 8; ++i) {
        auto *row = new QHBoxLayout();
        row->addWidget(new ScrubLabel(tr("H:"), mixerHueSpins_[size_t(i)], 1.0, 1.0, content));
        row->addLayout(makeSliderRow(this, -100.0, 100.0, 0.0, 0, mixerHueSpins_[size_t(i)], cb));
        row->addWidget(new ScrubLabel(tr("S:"), mixerSatSpins_[size_t(i)], 1.0, 1.0, content));
        row->addLayout(makeSliderRow(this, -100.0, 100.0, 0.0, 0, mixerSatSpins_[size_t(i)], cb));
        row->addWidget(new ScrubLabel(tr("L:"), mixerLumSpins_[size_t(i)], 1.0, 1.0, content));
        row->addLayout(makeSliderRow(this, -100.0, 100.0, 0.0, 0, mixerLumSpins_[size_t(i)], cb));
        form->addRow(tr(names[i]), row);
    }

    scroll->setWidget(content);
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(scroll);
    return w;
}

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

    addScrubRow(form, tr("Distortion"), this, -100.0, 100.0, 0.0, 0, opticsDistortionSpin_, cb);
    addScrubRow(form, tr("Purple Defringe"), this, 0.0, 100.0, 0.0, 0, purpleAmountSpin_, cb);
    addScrubRow(form, tr("Green Defringe"), this, 0.0, 100.0, 0.0, 0, greenAmountSpin_, cb);
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
    connect(uprightCombo_, &QComboBox::currentIndexChanged, this, cb);
    form->addRow(tr("Upright Mode"), uprightCombo_);

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
    updatePreview();
}

void CameraRawDialog::updatePreview()
{
    if (!previewBox_->isChecked()) {
        session_.rollbackLayerImage(layerId_, originalLayer_.image);
    } else {
        CameraRawClipping clip = activeClipping_;
        if (!altHeld_) clip = CameraRawClipping::None;

        QImage graded = RasterOperations::cameraRaw(originalLayer_.image, settings_, clip);
        if (showShadowClipping_ || showHighlightClipping_) {
            graded = CameraRawScope::overlay(graded, showShadowClipping_, showHighlightClipping_);
        }
        session_.previewLayerImage(layerId_, graded);
        auto scope = CameraRawScope::make(graded);
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
        if (watched == exposureSpin_ || watched == highlightsSpin_ || watched == whitesSpin_) {
            activeClipping_ = CameraRawClipping::Highlights;
        } else if (watched == shadowsSpin_ || watched == blacksSpin_) {
            activeClipping_ = CameraRawClipping::Shadows;
        } else {
            activeClipping_ = CameraRawClipping::None;
        }
        if (altHeld_) updatePreview();
    }
    return QDialog::eventFilter(watched, event);
}

void CameraRawDialog::accept()
{
    session_.rollbackLayer(layerId_, originalLayer_);
    if (!settings_.isIdentity()) {
        session_.applyCameraRaw(layerId_, settings_);
    }
    if (onPreview_) onPreview_();
    QDialog::accept();
}

void CameraRawDialog::reject()
{
    session_.rollbackLayer(layerId_, originalLayer_);
    if (onPreview_) onPreview_();
    QDialog::reject();
}


} // namespace compositor
