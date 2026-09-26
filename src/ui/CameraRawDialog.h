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

namespace compositor {

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

    void accept() override;
    void reject() override;

    [[nodiscard]] CameraRawSettings settings() const { return settings_; }
    [[nodiscard]] QPushButton *whiteBalanceEyedropper() const { return wbEyedropper_; }
    void sampleWhiteBalance(const QColor &color);

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
    void updatePreview();

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
};

} // namespace compositor
