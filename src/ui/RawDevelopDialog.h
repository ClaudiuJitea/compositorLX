#pragma once

#include "core/CameraRaw.h"

#include <QDialog>
#include <QLabel>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QTimer>
#include <QFutureWatcher>
#include <atomic>
#include <memory>

namespace compositor {

class RawDevelopDialog final : public QDialog {
    Q_OBJECT
public:
    explicit RawDevelopDialog(QWidget *parent, const QString &filePath);
    ~RawDevelopDialog() override;

    [[nodiscard]] RawDevelopSettings settings() const { return settings_; }
    [[nodiscard]] QImage developedImage() const { return developedFullImage_; }
    [[nodiscard]] bool isPreviewRunning() const;
    [[nodiscard]] bool isImportRunning() const;
    [[nodiscard]] bool isPreviewPending() const { return previewPending_; }
    [[nodiscard]] quint64 previewRevision() const { return previewRevision_; }

    void triggerPreviewImmediate();
    void done(int r) override;
    void reject() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onSliderChanged();
    void onResetClicked();
    void onImportClicked();
    void onPreviewFinished();
    void onImportFinished();

private:
    void schedulePreviewUpdate();
    void startNextPreviewJob();

    QString filePath_;
    RawDevelopSettings settings_;
    RawDevelopSettings asShotSettings_;
    QImage developedFullImage_;

    QLabel *previewLabel_ = nullptr;
    QDoubleSpinBox *exposureSpin_ = nullptr;
    QSlider *exposureSlider_ = nullptr;
    QDoubleSpinBox *tempSpin_ = nullptr;
    QSlider *tempSlider_ = nullptr;
    QDoubleSpinBox *tintSpin_ = nullptr;
    QSlider *tintSlider_ = nullptr;
    QDoubleSpinBox *boostSpin_ = nullptr;
    QSlider *boostSlider_ = nullptr;
    QPushButton *resetButton_ = nullptr;
    QPushButton *importButton_ = nullptr;
    QPushButton *cancelButton_ = nullptr;

    QTimer *debounceTimer_ = nullptr;
    QFutureWatcher<QImage> *previewWatcher_ = nullptr;
    QFutureWatcher<QImage> *importWatcher_ = nullptr;

    std::shared_ptr<std::atomic<bool>> cancelToken_;
    quint64 previewRevision_ = 0;
    quint64 inFlightRevision_ = 0;
    bool previewPending_ = false;
    bool updating_ = false;
    bool isClosed_ = false;
};

} // namespace compositor

