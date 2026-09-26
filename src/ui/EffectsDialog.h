#pragma once

#include "core/Document.h"
#include "core/EditorSession.h"

#include <QDialog>
#include <QUuid>

#include <functional>

class QListWidget;
class QStackedWidget;
class QCheckBox;
class QPushButton;
class QDoubleSpinBox;
class QComboBox;

namespace compositor {

class EffectsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit EffectsDialog(QWidget *parent, EditorSession &session, const QUuid &layerId,
                           std::function<void()> onPreview = nullptr,
                           std::optional<LayerEffectKind> initialKind = std::nullopt);

    void accept() override;
    void reject() override;

private:
    void setupUi();
    QWidget *createStrokePage();
    QWidget *createDropShadowPage();
    QWidget *createColorOverlayPage();
    QWidget *createInnerShadowPage();
    QWidget *createOuterGlowPage();
    QWidget *createInnerGlowPage();

    void updateSwatch(QPushButton *button, const QColor &color);
    void pickColor(LayerEffectKind kind, QPushButton *button);

    void loadFromEffects(const LayerEffects &effects);
    void syncToEffects();
    void updatePreview();

    EditorSession &session_;
    QUuid layerId_;
    LayerEffects originalEffects_;
    LayerEffects currentEffects_;
    std::function<void()> onPreview_;
    bool updating_ = false;

    QListWidget *effectList_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    QCheckBox *previewCheckbox_ = nullptr;

    // Stroke
    QCheckBox *strokeEnabled_ = nullptr;
    QComboBox *strokePosition_ = nullptr;
    QPushButton *strokeColorButton_ = nullptr;
    QDoubleSpinBox *strokeSize_ = nullptr;
    QDoubleSpinBox *strokeOpacity_ = nullptr;

    // Drop Shadow
    QCheckBox *shadowEnabled_ = nullptr;
    QPushButton *shadowColorButton_ = nullptr;
    QDoubleSpinBox *shadowOpacity_ = nullptr;
    QDoubleSpinBox *shadowAngle_ = nullptr;
    QDoubleSpinBox *shadowDistance_ = nullptr;
    QDoubleSpinBox *shadowBlur_ = nullptr;

    // Color Overlay
    QCheckBox *colorOverlayEnabled_ = nullptr;
    QPushButton *colorOverlayColorButton_ = nullptr;
    QDoubleSpinBox *colorOverlayOpacity_ = nullptr;

    // Inner Shadow
    QCheckBox *innerShadowEnabled_ = nullptr;
    QPushButton *innerShadowColorButton_ = nullptr;
    QDoubleSpinBox *innerShadowOpacity_ = nullptr;
    QDoubleSpinBox *innerShadowAngle_ = nullptr;
    QDoubleSpinBox *innerShadowDistance_ = nullptr;
    QDoubleSpinBox *innerShadowBlur_ = nullptr;

    // Outer Glow
    QCheckBox *outerGlowEnabled_ = nullptr;
    QPushButton *outerGlowColorButton_ = nullptr;
    QDoubleSpinBox *outerGlowSize_ = nullptr;
    QDoubleSpinBox *outerGlowOpacity_ = nullptr;

    // Inner Glow
    QCheckBox *innerGlowEnabled_ = nullptr;
    QPushButton *innerGlowColorButton_ = nullptr;
    QDoubleSpinBox *innerGlowSize_ = nullptr;
    QDoubleSpinBox *innerGlowOpacity_ = nullptr;
};

} // namespace compositor
