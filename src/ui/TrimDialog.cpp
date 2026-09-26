#include "TrimDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QRadioButton>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QLabel>

namespace compositor {

TrimDialog::TrimDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Trim"));
    setModal(true);
    setFixedWidth(320);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(14);
    mainLayout->setContentsMargins(18, 18, 18, 18);

    // 1. Based On Group
    auto *basedOnGroup = new QGroupBox(tr("Based On"), this);
    auto *basedOnLayout = new QVBoxLayout(basedOnGroup);
    basedOnLayout->setSpacing(8);

    radioTransparent_ = new QRadioButton(tr("Transparent Pixels"), basedOnGroup);
    radioTransparent_->setObjectName(QStringLiteral("trimRadioTransparent"));
    radioTransparent_->setChecked(true);

    radioTopLeft_ = new QRadioButton(tr("Top Left Pixel Color"), basedOnGroup);
    radioTopLeft_->setObjectName(QStringLiteral("trimRadioTopLeft"));

    radioBottomRight_ = new QRadioButton(tr("Bottom Right Pixel Color"), basedOnGroup);
    radioBottomRight_->setObjectName(QStringLiteral("trimRadioBottomRight"));

    basedOnLayout->addWidget(radioTransparent_);
    basedOnLayout->addWidget(radioTopLeft_);
    basedOnLayout->addWidget(radioBottomRight_);
    mainLayout->addWidget(basedOnGroup);

    // 2. Trim Away Group
    auto *trimAwayGroup = new QGroupBox(tr("Trim Away"), this);
    auto *trimAwayLayout = new QGridLayout(trimAwayGroup);
    trimAwayLayout->setHorizontalSpacing(24);
    trimAwayLayout->setVerticalSpacing(8);

    checkTop_ = new QCheckBox(tr("Top"), trimAwayGroup);
    checkTop_->setObjectName(QStringLiteral("trimCheckTop"));
    checkTop_->setChecked(true);

    checkBottom_ = new QCheckBox(tr("Bottom"), trimAwayGroup);
    checkBottom_->setObjectName(QStringLiteral("trimCheckBottom"));
    checkBottom_->setChecked(true);

    checkLeft_ = new QCheckBox(tr("Left"), trimAwayGroup);
    checkLeft_->setObjectName(QStringLiteral("trimCheckLeft"));
    checkLeft_->setChecked(true);

    checkRight_ = new QCheckBox(tr("Right"), trimAwayGroup);
    checkRight_->setObjectName(QStringLiteral("trimCheckRight"));
    checkRight_->setChecked(true);

    trimAwayLayout->addWidget(checkTop_, 0, 0);
    trimAwayLayout->addWidget(checkBottom_, 0, 1);
    trimAwayLayout->addWidget(checkLeft_, 1, 0);
    trimAwayLayout->addWidget(checkRight_, 1, 1);
    mainLayout->addWidget(trimAwayGroup);

    // 3. Dialog Button Box
    buttonBox_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttonBox_->button(QDialogButtonBox::Ok)->setText(tr("OK"));
    buttonBox_->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
    mainLayout->addWidget(buttonBox_);

    connect(buttonBox_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    const auto checkToggled = [this](bool) { updateButtons(); };
    connect(checkTop_, &QCheckBox::toggled, this, checkToggled);
    connect(checkBottom_, &QCheckBox::toggled, this, checkToggled);
    connect(checkLeft_, &QCheckBox::toggled, this, checkToggled);
    connect(checkRight_, &QCheckBox::toggled, this, checkToggled);

    updateButtons();
}

void TrimDialog::updateButtons()
{
    const bool canTrim = checkTop_->isChecked() || checkBottom_->isChecked() ||
                         checkLeft_->isChecked() || checkRight_->isChecked();
    if (auto *ok = buttonBox_->button(QDialogButtonBox::Ok)) {
        ok->setEnabled(canTrim);
    }
}

TrimOptions TrimDialog::options() const
{
    TrimOptions opts;
    if (radioTransparent_->isChecked()) {
        opts.basedOn = TrimBasedOn::TransparentPixels;
    } else if (radioTopLeft_->isChecked()) {
        opts.basedOn = TrimBasedOn::TopLeftPixelColor;
    } else {
        opts.basedOn = TrimBasedOn::BottomRightPixelColor;
    }

    opts.top = checkTop_->isChecked();
    opts.bottom = checkBottom_->isChecked();
    opts.left = checkLeft_->isChecked();
    opts.right = checkRight_->isChecked();
    opts.tolerance = 0;
    return opts;
}

} // namespace compositor
