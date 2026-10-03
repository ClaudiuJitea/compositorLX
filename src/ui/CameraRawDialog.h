#pragma once

#include "core/CameraRaw.h"
#include "core/EditorSession.h"

#include <QDialog>
#include <QTabWidget>
#include <QScrollArea>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <functional>
#include <QStackedWidget>
#include <QToolButton>
#include <optional>

namespace compositor {

class CanvasWidget;

class ScopeWidget final : public QWidget {
    Q_OBJECT
public:
    explicit ScopeWidget(QWidget *parent = nullptr);
    void setScope(const CameraRawScope &scope);
    void setScopeMode(CameraRawScopeMode mode);
    [[nodiscard]] CameraRawScopeMode scopeMode() const { return mode_; }

    void setShadowClippingActive(bool active);
    void setHighlightClippingActive(bool active);

signals:
    void shadowClippingToggled(bool active);
    void highlightClippingToggled(bool active);

protected:
    void paintEvent(QPaintEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    CameraRawScope scope_;
    CameraRawScopeMode mode_ = CameraRawScopeMode::Histogram;
    QPushButton *shadowClipBtn_ = nullptr;
    QPushButton *highlightClipBtn_ = nullptr;
};

class CameraRawDialog final : public QDialog {
    Q_OBJECT
public:
    explicit CameraRawDialog(QWidget *parent, EditorSession &session, const QUuid &layerId,
                             std::function<void()> onPreview = nullptr);

    ~CameraRawDialog() override;
    void accept() override;
    void reject() override;

    [[nodiscard]] CameraRawSettings settings() const { return settings_; }
    [[nodiscard]] QPushButton *whiteBalanceEyedropper() const { return wbEyedropper_; }
    void sampleWhiteBalance(const QColor &color);
    // Group eyes (mac showsCameraRawLight ...): Light, Color, Effects, Curve, Mixer, Grading, Detail, Optics, Geometry,
    // Calibration. A hidden group is left out of the preview and of OK without clearing its sliders.
    [[nodiscard]] QCheckBox *eyeBox(int group) const { return showBoxes_.at(size_t(group)); }
    [[nodiscard]] CameraRawSettings renderSettings() const;
    void setSettings(const CameraRawSettings &settings);   // loads every control, as Reset does
    enum class Probe { None, PointColor, Defringe, CurveTarget, MixerTarget, Guide };
    void setProbe(Probe probe);
    [[nodiscard]] Probe probe() const { return probe_; }
    // What the canvas reports while a probe is armed or the pointer hovers (public so tests can drive it).
    void probePress(const QPointF &documentPoint);
    void probeMove(const QPointF &documentPoint, bool dragging);
    void probeRelease(const QPointF &documentPoint);
    [[nodiscard]] QString readoutText() const { return readout_ ? readout_->text() : QString(); }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private:
    void setupUi();
    QWidget *createLightTab();
    QWidget *createColorTab();
    QWidget *createEffectsTab();
    QWidget *createCurveTab();
    QWidget *createMixerTab();
    QWidget *createGradingTab();
    QWidget *createDetailTab();
    QWidget *createOpticsTab();
    QWidget *createGeometryTab();
    QWidget *createCalibrationTab();

    void syncToSettings();
    void loadFromSettings();
    void updatePreview();
    void refreshEyes();
    void refreshMixerColorPage();
    void refreshPointPage();
    void releaseCanvas();
    [[nodiscard]] std::optional<QPoint> layerPixel(const QPointF &documentPoint) const;
    struct ToneSample { double tone, hue, saturation, luminance; };
    [[nodiscard]] std::optional<ToneSample> sampleAt(const QPointF &documentPoint, bool fromPreview) const;

    EditorSession &session_;
    QUuid layerId_;
    Layer originalLayer_;
    std::function<void()> onPreview_;

    CameraRawSettings settings_;
    CameraRawScope currentScope_;
    bool altHeld_ = false;
    CameraRawClipping activeClipping_ = CameraRawClipping::None;
    bool showShadowClipping_ = false;
    bool showHighlightClipping_ = false;
    bool samplingWhiteBalance_ = false;

    ScopeWidget *scopeWidget_ = nullptr;
    QCheckBox *previewBox_ = nullptr;
    QTabWidget *tabWidget_ = nullptr;

    // Light
    QDoubleSpinBox *exposureSpin_ = nullptr;
    QDoubleSpinBox *contrastSpin_ = nullptr;
    QDoubleSpinBox *highlightsSpin_ = nullptr;
    QDoubleSpinBox *shadowsSpin_ = nullptr;
    QDoubleSpinBox *whitesSpin_ = nullptr;
    QDoubleSpinBox *blacksSpin_ = nullptr;

    // Color
    QComboBox *wbCombo_ = nullptr;
    QPushButton *wbEyedropper_ = nullptr;
    QDoubleSpinBox *tempSpin_ = nullptr;
    QDoubleSpinBox *tintSpin_ = nullptr;
    QDoubleSpinBox *vibranceSpin_ = nullptr;
    QDoubleSpinBox *saturationSpin_ = nullptr;

    // Effects
    QDoubleSpinBox *textureSpin_ = nullptr;
    QDoubleSpinBox *claritySpin_ = nullptr;
    QDoubleSpinBox *dehazeSpin_ = nullptr;
    QDoubleSpinBox *glowSpin_ = nullptr;
    QComboBox *glowStyleCombo_ = nullptr;
    QDoubleSpinBox *glowRangeSpin_ = nullptr;
    QDoubleSpinBox *glowSpreadSpin_ = nullptr;
    QDoubleSpinBox *glowWarmthSpin_ = nullptr;
    QDoubleSpinBox *vignetteAmountSpin_ = nullptr;
    QComboBox *vignetteStyleCombo_ = nullptr;
    QDoubleSpinBox *vignetteMidpointSpin_ = nullptr;
    QDoubleSpinBox *vignetteRoundnessSpin_ = nullptr;
    QDoubleSpinBox *vignetteFeatherSpin_ = nullptr;
    QDoubleSpinBox *vignetteHighlightsSpin_ = nullptr;
    QDoubleSpinBox *grainAmountSpin_ = nullptr;
    QDoubleSpinBox *grainSizeSpin_ = nullptr;
    QDoubleSpinBox *grainRoughnessSpin_ = nullptr;

    // Curve
    QDoubleSpinBox *curveHighlightsSpin_ = nullptr;
    QDoubleSpinBox *curveLightsSpin_ = nullptr;
    QDoubleSpinBox *curveDarksSpin_ = nullptr;
    QDoubleSpinBox *curveShadowsSpin_ = nullptr;
    QDoubleSpinBox *curveShadowSplitSpin_ = nullptr;
    QDoubleSpinBox *curveDarkSplitSpin_ = nullptr;
    QDoubleSpinBox *curveLightSplitSpin_ = nullptr;
    QComboBox *curvePresetCombo_ = nullptr;
    QWidget *curveGraph_ = nullptr;
    QDoubleSpinBox *curveRefineSatSpin_ = nullptr;

    // Mixer
    std::array<QDoubleSpinBox *, 8> mixerHueSpins_{};
    std::array<QDoubleSpinBox *, 8> mixerSatSpins_{};
    std::array<QDoubleSpinBox *, 8> mixerLumSpins_{};

    // Grading
    QDoubleSpinBox *gradeShadowHueSpin_ = nullptr;
    QDoubleSpinBox *gradeShadowSatSpin_ = nullptr;
    QDoubleSpinBox *gradeShadowLumSpin_ = nullptr;
    QDoubleSpinBox *gradeMidHueSpin_ = nullptr;
    QDoubleSpinBox *gradeMidSatSpin_ = nullptr;
    QDoubleSpinBox *gradeMidLumSpin_ = nullptr;
    QDoubleSpinBox *gradeHighHueSpin_ = nullptr;
    QDoubleSpinBox *gradeHighSatSpin_ = nullptr;
    QDoubleSpinBox *gradeHighLumSpin_ = nullptr;
    QDoubleSpinBox *gradeGlobalHueSpin_ = nullptr;
    QDoubleSpinBox *gradeGlobalSatSpin_ = nullptr;
    QDoubleSpinBox *gradeGlobalLumSpin_ = nullptr;
    QDoubleSpinBox *gradeBlendingSpin_ = nullptr;
    QDoubleSpinBox *gradeBalanceSpin_ = nullptr;

    // Detail
    QDoubleSpinBox *sharpenAmountSpin_ = nullptr;
    QDoubleSpinBox *sharpenRadiusSpin_ = nullptr;
    QDoubleSpinBox *sharpenDetailSpin_ = nullptr;
    QDoubleSpinBox *sharpenMaskingSpin_ = nullptr;
    QDoubleSpinBox *noiseLumSpin_ = nullptr;
    QDoubleSpinBox *noiseLumDetailSpin_ = nullptr;
    QDoubleSpinBox *noiseLumContrastSpin_ = nullptr;
    QDoubleSpinBox *noiseColorSpin_ = nullptr;
    QDoubleSpinBox *noiseColorDetailSpin_ = nullptr;
    QDoubleSpinBox *noiseColorSmoothSpin_ = nullptr;

    // Optics
    QCheckBox *chromaticBox_ = nullptr;
    QCheckBox *lensProfileBox_ = nullptr;
    QDoubleSpinBox *opticsDistortionSpin_ = nullptr;
    QDoubleSpinBox *purpleAmountSpin_ = nullptr;
    QDoubleSpinBox *greenAmountSpin_ = nullptr;
    QDoubleSpinBox *opticsVignetteSpin_ = nullptr;
    QDoubleSpinBox *opticsVignetteMidpointSpin_ = nullptr;

    // Geometry
    QComboBox *uprightCombo_ = nullptr;
    QComboBox *projectionCombo_ = nullptr;
    QDoubleSpinBox *geoVerticalSpin_ = nullptr;
    QDoubleSpinBox *geoHorizontalSpin_ = nullptr;
    QDoubleSpinBox *geoRotateSpin_ = nullptr;
    QDoubleSpinBox *geoAspectSpin_ = nullptr;
    QDoubleSpinBox *geoScaleSpin_ = nullptr;
    QDoubleSpinBox *geoOffsetXSpin_ = nullptr;
    QDoubleSpinBox *geoOffsetYSpin_ = nullptr;
    QCheckBox *constrainCropBox_ = nullptr;

    // Calibration
    QComboBox *processVersionCombo_ = nullptr;
    QDoubleSpinBox *calShadowTintSpin_ = nullptr;
    QDoubleSpinBox *calRedHueSpin_ = nullptr;
    QDoubleSpinBox *calRedSatSpin_ = nullptr;
    QDoubleSpinBox *calGreenHueSpin_ = nullptr;
    QDoubleSpinBox *calGreenSatSpin_ = nullptr;
    QDoubleSpinBox *calBlueHueSpin_ = nullptr;
    QDoubleSpinBox *calBlueSatSpin_ = nullptr;

    bool updating_ = false;

    CanvasWidget *canvas_ = nullptr;
    Probe probe_ = Probe::None;
    QImage lastGraded_;
    bool sharpenMaskPreview_ = false;
    std::array<bool, 10> shows_{true, true, true, true, true, true, true, true, true, true};
    std::array<QCheckBox *, 10> showBoxes_{};
    QLabel *readout_ = nullptr;
    QPushButton *resetButton_ = nullptr;
    QPushButton *pointEyedropper_ = nullptr;
    QPushButton *defringeEyedropper_ = nullptr;
    QPushButton *curveTarget_ = nullptr;
    QPushButton *mixerTarget_ = nullptr;
    QPushButton *guideButton_ = nullptr;
    QPushButton *clearGuidesButton_ = nullptr;
    QComboBox *curvePageCombo_ = nullptr;
    QWidget *curveParametricPage_ = nullptr;
    QWidget *curvePointPage_ = nullptr;
    QComboBox *curveChannelCombo_ = nullptr;
    QComboBox *mixerPageCombo_ = nullptr;
    QStackedWidget *mixerStack_ = nullptr;
    QComboBox *mixerComponentCombo_ = nullptr;
    int mixerSwatch_ = 0;
    std::array<QToolButton *, 8> mixerSwatchButtons_{};
    QDoubleSpinBox *mixerColorHueSpin_ = nullptr;
    QDoubleSpinBox *mixerColorSatSpin_ = nullptr;
    QDoubleSpinBox *mixerColorLumSpin_ = nullptr;
    int pointIndex_ = 0;
    QWidget *pointSwatchHost_ = nullptr;
    QWidget *pointSliders_ = nullptr;
    QDoubleSpinBox *pointHueShiftSpin_ = nullptr;
    QDoubleSpinBox *pointSatShiftSpin_ = nullptr;
    QDoubleSpinBox *pointLumShiftSpin_ = nullptr;
    QDoubleSpinBox *pointHueRangeSpin_ = nullptr;
    QDoubleSpinBox *pointSatRangeSpin_ = nullptr;
    QDoubleSpinBox *pointLumRangeSpin_ = nullptr;
    QCheckBox *pointVisualizeBox_ = nullptr;
    QDoubleSpinBox *opticsPurpleLowSpin_ = nullptr;
    QDoubleSpinBox *opticsPurpleHighSpin_ = nullptr;
    QDoubleSpinBox *opticsGreenLowSpin_ = nullptr;
    QDoubleSpinBox *opticsGreenHighSpin_ = nullptr;
    QDoubleSpinBox *profileDistortionSpin_ = nullptr;
    QDoubleSpinBox *profileVignettingSpin_ = nullptr;

    // A drag started by a targeted adjustment or a guide line.
    CameraRawSettings dragStart_;
    double dragStartY_ = 0;
    ToneSample dragSample_{};
    bool dragging_ = false;
    std::optional<CameraRawGeometryGuide> guideDraft_;
};

} // namespace compositor
