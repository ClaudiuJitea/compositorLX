#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMap>
#include <QKeySequence>
#include "ui/ShortcutManager.h"

namespace compositor {

class ShortcutRecorderButton : public QPushButton {
    Q_OBJECT
public:
    explicit ShortcutRecorderButton(const QKeySequence &sequence, QWidget *parent = nullptr);

    QKeySequence sequence() const { return sequence_; }
    void setSequence(const QKeySequence &seq);

    bool isRecording() const { return recording_; }
    void setRecording(bool recording);

signals:
    void sequenceChanged(const QKeySequence &seq);
    void recordingStarted();
    void recordingFinished(bool committed);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    void updateAppearance();

    QKeySequence sequence_;
    bool recording_ = false;
};

class KeyboardShortcutsDialog : public QDialog {
    Q_OBJECT
public:
    explicit KeyboardShortcutsDialog(QWidget *parent = nullptr);
    ~KeyboardShortcutsDialog() override = default;

    QMap<QString, QKeySequence> draftOverrides() const { return draftOverrides_; }
    ShortcutRecorderButton *recorderForId(const QString &id) const;
    QLabel *conflictLabel() const { return conflictLabel_; }
    QPushButton *saveButton() const { return saveButton_; }
    QPushButton *restoreDefaultsButton() const { return restoreDefaultsButton_; }
    QLineEdit *searchEdit() const { return searchEdit_; }

private slots:
    void onSearchTextChanged(const QString &text);
    void onRestoreDefaultsClicked();
    void onSaveClicked();
    void onCancelClicked();

private:
    void setupUI();
    void updateConflictStatus();

    struct RowEntry {
        QString id;
        QString title;
        QString group;
        QWidget *widget = nullptr;
        ShortcutRecorderButton *button = nullptr;
    };

    QMap<QString, QKeySequence> draftOverrides_;
    QVector<RowEntry> rows_;
    QMap<QString, QWidget*> groupHeaders_;

    QLineEdit *searchEdit_ = nullptr;
    QScrollArea *scrollArea_ = nullptr;
    QLabel *conflictLabel_ = nullptr;
    QPushButton *restoreDefaultsButton_ = nullptr;
    QPushButton *cancelButton_ = nullptr;
    QPushButton *saveButton_ = nullptr;
    ShortcutRecorderButton *activeRecorder_ = nullptr;
};

} // namespace compositor
