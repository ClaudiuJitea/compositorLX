#pragma once

#include "core/ColorPalette.h"

#include <QColor>
#include <QDialog>

class QLabel;
class QLineEdit;
class QSpinBox;

namespace compositor {

class ColorPickerField;
class ColorHueStrip;

/// The app's colour picker (mac ColorPickerSheet.swift): a saturation/brightness field, a vertical hue strip, the
/// new colour above the current one, scrubbable R/G/B entry and a hex field. Nothing is committed until OK;
/// `currentColorChanged` follows the working colour as it moves so callers can preview, and Cancel leaves
/// `selectedColor()` as the original. It stays non-modal-friendly: the canvas can be clicked to sample into it.
class ColorPickerDialog final : public QDialog {
    Q_OBJECT
public:
    explicit ColorPickerDialog(const QColor &initial, QWidget *parent = nullptr);
    [[nodiscard]] QColor currentColor() const { return palette::quantized(hsb_.rgb()); }
    [[nodiscard]] QColor originalColor() const { return original_; }
    [[nodiscard]] QColor selectedColor() const { return selected_; }
    void setCurrentColor(const QColor &color);
    /// Hides the "click the canvas" hint, for pickers opened over a dialog that covers the canvas.
    void setCanvasSamplingHint(bool shown);
    static QColor getColor(const QColor &initial, QWidget *parent = nullptr, const QString &title = QString());

signals:
    void currentColorChanged(const QColor &color);

private:
    friend class ColorPickerField;
    friend class ColorHueStrip;
    void changed(bool fromHex = false);
    void commitHex();
    void syncControls();

    PickerHSB hsb_;
    QColor original_, selected_;
    ColorPickerField *field_ = nullptr;
    ColorHueStrip *strip_ = nullptr;
    QLabel *preview_ = nullptr, *hint_ = nullptr;
    QSpinBox *red_ = nullptr, *green_ = nullptr, *blue_ = nullptr;
    QLineEdit *hex_ = nullptr;
    bool syncing_ = false;
};

} // namespace compositor
