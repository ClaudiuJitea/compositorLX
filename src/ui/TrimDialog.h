#pragma once

#include "core/ImageTrim.h"
#include <QDialog>

class QRadioButton;
class QCheckBox;
class QDialogButtonBox;

namespace compositor {

class TrimDialog : public QDialog {
    Q_OBJECT
public:
    explicit TrimDialog(QWidget *parent = nullptr);

    [[nodiscard]] TrimOptions options() const;

private:
    void updateButtons();

    QRadioButton *radioTransparent_ = nullptr;
    QRadioButton *radioTopLeft_ = nullptr;
    QRadioButton *radioBottomRight_ = nullptr;

    QCheckBox *checkTop_ = nullptr;
    QCheckBox *checkBottom_ = nullptr;
    QCheckBox *checkLeft_ = nullptr;
    QCheckBox *checkRight_ = nullptr;

    QDialogButtonBox *buttonBox_ = nullptr;
};

} // namespace compositor
