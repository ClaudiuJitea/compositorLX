#include "ui/RawDevelopDialog.h"
#include "ui/NumericScrub.h"
#include "io/RawImporter.h"

#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFileInfo>
#include <QCloseEvent>
#include <QtConcurrent>

namespace compositor {

RawDevelopDialog::RawDevelopDialog(QWidget *parent, const QString &filePath)
    : QDialog(parent), filePath_(filePath)
{
    setWindowTitle(tr("Develop “%1”").arg(QFileInfo(filePath).fileName()));
    setMinimumSize(600, 520);

    cancelToken_ = std::make_shared<std::atomic<bool>>(false);

    const auto asShot = RawImporter::asShot(filePath);
    if (asShot) {
        settings_ = *asShot;
        asShotSettings_ = *asShot;
    } else {
        settings_ = RawDevelopSettings{};
        asShotSettings_ = settings_;
    }

    debounceTimer_ = new QTimer(this);
    debounceTimer_->setSingleShot(true);
    debounceTimer_->setInterval(50);
    connect(debounceTimer_, &QTimer::timeout, this, &RawDevelopDialog::startNextPreviewJob);

    previewWatcher_ = new QFutureWatcher<QImage>(this);
    connect(previewWatcher_, &QFutureWatcher<QImage>::finished, this, &RawDevelopDialog::onPreviewFinished);

    importWatcher_ = new QFutureWatcher<QImage>(this);
    connect(importWatcher_, &QFutureWatcher<QImage>::finished, this, &RawDevelopDialog::onImportFinished);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(12);

    previewLabel_ = new QLabel(this);
    previewLabel_->setMinimumSize(560, 320);
    previewLabel_->setAlignment(Qt::AlignCenter);
    previewLabel_->setObjectName(QStringLiteral("imagePreview"));   // its well comes from the theme
    mainLayout->addWidget(previewLabel_, 1);

    auto *formLayout = new QFormLayout();
    formLayout->setSpacing(8);

    auto addRow = [&](const QString &label, double minVal, double maxVal, double curVal, int decimals,
                      QDoubleSpinBox *&spin, QSlider *&slider, const QString &objName = QString()) {
        auto *rowLayout = new QHBoxLayout();
        slider = new QSlider(Qt::Horizontal, this);
        spin = new QDoubleSpinBox(this);
        spin->setDecimals(decimals);
        spin->setRange(minVal, maxVal);
        spin->setValue(curVal);
        const double sens = decimals > 0 ? std::pow(10.0, -decimals) : 1.0;
        spin->setSingleStep(sens);

        if (!objName.isEmpty()) {
            spin->setObjectName(objName);
            slider->setObjectName(objName + QStringLiteral("Slider"));
        }

        const double mult = std::pow(10, decimals);
        slider->setRange(static_cast<int>(minVal * mult), static_cast<int>(maxVal * mult));
        slider->setValue(static_cast<int>(curVal * mult));

        rowLayout->addWidget(slider, 1);
        rowLayout->addWidget(spin);

        connect(slider, &QSlider::valueChanged, this, [spin, mult, this](int val) {
            if (updating_) return;
            updating_ = true;
            spin->setValue(double(val) / mult);
            updating_ = false;
            onSliderChanged();
        });

        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [slider, mult, this](double val) {
            if (updating_) return;
            updating_ = true;
            slider->setValue(static_cast<int>(val * mult));
            updating_ = false;
            onSliderChanged();
        });

        auto *scrub = new ScrubLabel(label, spin, sens, sens, this);
        if (!objName.isEmpty()) {
            scrub->setObjectName(objName + QStringLiteral("Label"));
        }
        formLayout->addRow(scrub, rowLayout);
    };

    addRow(tr("Exposure"), -3.0, 3.0, settings_.exposure, 2, exposureSpin_, exposureSlider_, QStringLiteral("rawExposure"));
    addRow(tr("Temperature"), 2000.0, 12000.0, settings_.temperature, 0, tempSpin_, tempSlider_, QStringLiteral("rawTemperature"));
    addRow(tr("Tint"), -150.0, 150.0, settings_.tint, 0, tintSpin_, tintSlider_, QStringLiteral("rawTint"));
    addRow(tr("Boost"), 0.0, 1.0, settings_.boost, 2, boostSpin_, boostSlider_, QStringLiteral("rawBoost"));

    mainLayout->addLayout(formLayout);

    // The desktop's button order: Reset at the side, Import as the dialog's default.
    auto *buttons = new QDialogButtonBox(this);
    resetButton_ = buttons->addButton(QDialogButtonBox::Reset);
    resetButton_->setEnabled(!settings_.isAsShot());
    connect(resetButton_, &QPushButton::clicked, this, &RawDevelopDialog::onResetClicked);
    cancelButton_ = buttons->addButton(QDialogButtonBox::Cancel);
    connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);
    importButton_ = buttons->addButton(tr("Import"), QDialogButtonBox::AcceptRole);
    importButton_->setDefault(true);
    connect(importButton_, &QPushButton::clicked, this, &RawDevelopDialog::onImportClicked);
    mainLayout->addWidget(buttons);

    startNextPreviewJob();
}

RawDevelopDialog::~RawDevelopDialog()
{
    isClosed_ = true;
    if (cancelToken_) {
        cancelToken_->store(true);
    }
    if (debounceTimer_) {
        debounceTimer_->stop();
    }
    if (previewWatcher_) {
        previewWatcher_->disconnect();
        if (previewWatcher_->isRunning()) {
            previewWatcher_->waitForFinished();
        }
    }
    if (importWatcher_) {
        importWatcher_->disconnect();
        if (importWatcher_->isRunning()) {
            importWatcher_->waitForFinished();
        }
    }
}

bool RawDevelopDialog::isPreviewRunning() const
{
    return previewWatcher_ && previewWatcher_->isRunning();
}

bool RawDevelopDialog::isImportRunning() const
{
    return importWatcher_ && importWatcher_->isRunning();
}

void RawDevelopDialog::triggerPreviewImmediate()
{
    if (isClosed_) return;
    if (debounceTimer_) {
        debounceTimer_->stop();
    }
    ++previewRevision_;
    startNextPreviewJob();
}

void RawDevelopDialog::onSliderChanged()
{
    settings_.exposure = static_cast<float>(exposureSpin_->value());
    settings_.temperature = static_cast<float>(tempSpin_->value());
    settings_.tint = static_cast<float>(tintSpin_->value());
    settings_.boost = static_cast<float>(boostSpin_->value());

    resetButton_->setEnabled(!settings_.isAsShot());
    schedulePreviewUpdate();
}

void RawDevelopDialog::onResetClicked()
{
    settings_.reset();
    updating_ = true;
    exposureSpin_->setValue(settings_.exposure);
    exposureSlider_->setValue(static_cast<int>(settings_.exposure * 100));
    tempSpin_->setValue(settings_.temperature);
    tempSlider_->setValue(static_cast<int>(settings_.temperature));
    tintSpin_->setValue(settings_.tint);
    tintSlider_->setValue(static_cast<int>(settings_.tint));
    boostSpin_->setValue(settings_.boost);
    boostSlider_->setValue(static_cast<int>(settings_.boost * 100));
    updating_ = false;

    resetButton_->setEnabled(false);
    if (debounceTimer_) {
        debounceTimer_->stop();
    }
    ++previewRevision_;
    startNextPreviewJob();
}

void RawDevelopDialog::onImportClicked()
{
    if (isClosed_ || isImportRunning()) return;

    if (debounceTimer_) {
        debounceTimer_->stop();
    }
    previewPending_ = false;
    importButton_->setEnabled(false);
    cancelButton_->setEnabled(true);
    previewLabel_->setText(tr("Developing full resolution RAW image…"));

    const QString path = filePath_;
    const RawDevelopSettings devSettings = settings_;
    auto token = cancelToken_;

    importWatcher_->setFuture(QtConcurrent::run([path, devSettings, token]() -> QImage {
        return RawImporter::develop(path, devSettings, 0, token.get());
    }));
}

void RawDevelopDialog::schedulePreviewUpdate()
{
    if (isClosed_ || isImportRunning()) return;
    ++previewRevision_;
    if (debounceTimer_) {
        debounceTimer_->start(50);
    }
}

void RawDevelopDialog::startNextPreviewJob()
{
    if (isClosed_ || !cancelToken_ || cancelToken_->load() || isImportRunning()) return;

    if (previewWatcher_ && previewWatcher_->isRunning()) {
        previewPending_ = true;
        return;
    }

    previewPending_ = false;
    const quint64 rev = previewRevision_;
    inFlightRevision_ = rev;
    const QString path = filePath_;
    const RawDevelopSettings devSettings = settings_;
    auto token = cancelToken_;

    if (previewWatcher_) {
        previewWatcher_->setFuture(QtConcurrent::run([path, devSettings, token]() -> QImage {
            return RawImporter::develop(path, devSettings, 800, token.get());
        }));
    }
}

void RawDevelopDialog::onPreviewFinished()
{
    if (isClosed_ || !cancelToken_ || cancelToken_->load()) {
        return;
    }

    if (inFlightRevision_ == previewRevision_ && previewWatcher_) {
        const QImage img = previewWatcher_->result();
        if (!img.isNull() && previewLabel_) {
            const QPixmap pixmap = QPixmap::fromImage(img).scaled(
                previewLabel_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            previewLabel_->setPixmap(pixmap);
        }
    }

    if (previewPending_ || inFlightRevision_ != previewRevision_) {
        startNextPreviewJob();
    }
}

void RawDevelopDialog::onImportFinished()
{
    if (isClosed_ || !cancelToken_ || cancelToken_->load() || !importWatcher_) {
        return;
    }
    developedFullImage_ = importWatcher_->result();
    accept();
}

void RawDevelopDialog::done(int r)
{
    isClosed_ = true;
    if (r != QDialog::Accepted && cancelToken_) {
        cancelToken_->store(true);
    }
    if (debounceTimer_) {
        debounceTimer_->stop();
    }
    QDialog::done(r);
}

void RawDevelopDialog::reject()
{
    isClosed_ = true;
    if (cancelToken_) {
        cancelToken_->store(true);
    }
    if (debounceTimer_) {
        debounceTimer_->stop();
    }
    QDialog::reject();
}

void RawDevelopDialog::closeEvent(QCloseEvent *event)
{
    isClosed_ = true;
    if (cancelToken_) {
        cancelToken_->store(true);
    }
    if (debounceTimer_) {
        debounceTimer_->stop();
    }
    QDialog::closeEvent(event);
}

} // namespace compositor
