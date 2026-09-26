#include "ui/KeyboardShortcutsDialog.h"
#include <QFrame>

namespace compositor {

ShortcutRecorderButton::ShortcutRecorderButton(const QKeySequence &sequence, QWidget *parent)
    : QPushButton(parent)
    , sequence_(sequence)
{
    setFixedSize(140, 26);
    setFocusPolicy(Qt::StrongFocus);
    updateAppearance();

    connect(this, &QPushButton::clicked, this, [this]() {
        if (!recording_) {
            setRecording(true);
        }
    });
}

void ShortcutRecorderButton::setSequence(const QKeySequence &seq)
{
    sequence_ = seq;
    updateAppearance();
}

void ShortcutRecorderButton::setRecording(bool recording)
{
    if (recording_ == recording) return;
    recording_ = recording;
    updateAppearance();
    if (recording_) {
        setFocus();
        emit recordingStarted();
    }
}

void ShortcutRecorderButton::updateAppearance()
{
    if (recording_) {
        setText(tr("Press keys…"));
        setStyleSheet(QStringLiteral("QPushButton { background-color: #0a84ff; color: #ffffff; border: 1px solid #0060df; border-radius: 4px; font-weight: bold; font-size: 11px; }"));
    } else {
        setText(sequence_.isEmpty() ? tr("None") : sequence_.toString(QKeySequence::NativeText));
        setStyleSheet(QStringLiteral("QPushButton { background-color: #2c2c2e; color: #e5e5e7; border: 1px solid #3a3a3c; border-radius: 4px; padding: 2px 8px; font-family: monospace; font-size: 11px; } "
                                     "QPushButton:hover { background-color: #3a3a3c; border-color: #48484a; }"));
    }
}

void ShortcutRecorderButton::keyPressEvent(QKeyEvent *event)
{
    if (!recording_) {
        QPushButton::keyPressEvent(event);
        return;
    }

    if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier) {
        setRecording(false);
        emit recordingFinished(false);
        event->accept();
        return;
    }

    const int key = event->key();
    if (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta) {
        event->accept();
        return;
    }

    const QKeySequence seq = ShortcutManager::sequenceFromKeyEvent(event);
    if (!seq.isEmpty()) {
        setSequence(seq);
        setRecording(false);
        emit sequenceChanged(seq);
        emit recordingFinished(true);
        event->accept();
        return;
    }

    QPushButton::keyPressEvent(event);
}

void ShortcutRecorderButton::focusOutEvent(QFocusEvent *event)
{
    if (recording_) {
        setRecording(false);
        emit recordingFinished(false);
    }
    QPushButton::focusOutEvent(event);
}

KeyboardShortcutsDialog::KeyboardShortcutsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Keyboard Shortcuts"));
    setModal(true);
    resize(680, 580);
    setMinimumSize(540, 420);

    draftOverrides_ = ShortcutManager::instance().overrides();
    setupUI();
    updateConflictStatus();
}

ShortcutRecorderButton *KeyboardShortcutsDialog::recorderForId(const QString &id) const
{
    for (const RowEntry &row : rows_) {
        if (row.id == id) {
            return row.button;
        }
    }
    return nullptr;
}

void KeyboardShortcutsDialog::setupUI()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 16, 20, 16);
    mainLayout->setSpacing(10);

    auto *instructionLabel = new QLabel(tr("Click a shortcut, then press its new key combination. Changes apply when you save."), this);
    instructionLabel->setStyleSheet(QStringLiteral("color: #8e8e93; font-size: 12px;"));
    mainLayout->addWidget(instructionLabel);

    searchEdit_ = new QLineEdit(this);
    searchEdit_->setPlaceholderText(tr("Search shortcuts"));
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setStyleSheet(QStringLiteral("QLineEdit { background-color: #1c1c1e; border: 1px solid #3a3a3c; border-radius: 6px; padding: 6px 10px; color: #ffffff; font-size: 13px; } "
                                              "QLineEdit:focus { border-color: #0a84ff; }"));
    connect(searchEdit_, &QLineEdit::textChanged, this, &KeyboardShortcutsDialog::onSearchTextChanged);
    mainLayout->addWidget(searchEdit_);

    scrollArea_ = new QScrollArea(this);
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setFrameShape(QFrame::NoFrame);
    scrollArea_->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; } QScrollBar:vertical { width: 8px; background: transparent; } QScrollBar::handle:vertical { background: #3a3a3c; border-radius: 4px; }"));

    auto *container = new QWidget;
    container->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *containerLayout = new QVBoxLayout(container);
    containerLayout->setContentsMargins(0, 4, 8, 8);
    containerLayout->setSpacing(4);

    const auto &definitions = ShortcutManager::instance().definitions();
    const QStringList groups = {QStringLiteral("Menus"), QStringLiteral("Canvas & Layers"), QStringLiteral("Text Editing")};

    for (const QString &group : groups) {
        auto *headerWidget = new QWidget(container);
        auto *headerLayout = new QVBoxLayout(headerWidget);
        headerLayout->setContentsMargins(0, 12, 0, 4);

        auto *headerLabel = new QLabel(group, headerWidget);
        headerLabel->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 13px; color: #ffffff;"));
        headerLayout->addWidget(headerLabel);

        containerLayout->addWidget(headerWidget);
        groupHeaders_.insert(group, headerWidget);

        for (const ShortcutDefinition &def : definitions) {
            if (def.group != group) continue;

            auto *rowWidget = new QWidget(container);
            auto *rowLayout = new QHBoxLayout(rowWidget);
            rowLayout->setContentsMargins(4, 2, 4, 2);

            auto *titleLabel = new QLabel(def.title, rowWidget);
            titleLabel->setStyleSheet(QStringLiteral("color: #e5e5e7; font-size: 12px;"));
            rowLayout->addWidget(titleLabel);
            rowLayout->addStretch();

            const QKeySequence currentSeq = draftOverrides_.value(def.id(), def.defaultShortcut);
            auto *recorder = new ShortcutRecorderButton(currentSeq, rowWidget);
            rowLayout->addWidget(recorder);

            const QString defId = def.id();
            connect(recorder, &ShortcutRecorderButton::recordingStarted, this, [this, recorder]() {
                if (activeRecorder_ && activeRecorder_ != recorder) {
                    activeRecorder_->setRecording(false);
                }
                activeRecorder_ = recorder;
                updateConflictStatus();
            });

            connect(recorder, &ShortcutRecorderButton::recordingFinished, this, [this]() {
                activeRecorder_ = nullptr;
                updateConflictStatus();
            });

            connect(recorder, &ShortcutRecorderButton::sequenceChanged, this, [this, defId](const QKeySequence &seq) {
                draftOverrides_[defId] = seq;
                updateConflictStatus();
            });

            containerLayout->addWidget(rowWidget);
            rows_.append({def.id(), def.title, group, rowWidget, recorder});
        }
    }

    // Contextual gestures footer
    auto *footerDivider = new QFrame(container);
    footerDivider->setFrameShape(QFrame::HLine);
    footerDivider->setFrameShadow(QFrame::Sunken);
    footerDivider->setStyleSheet(QStringLiteral("color: #3a3a3c; margin-top: 12px; margin-bottom: 8px;"));
    containerLayout->addWidget(footerDivider);

    auto *footerHeader = new QLabel(tr("Contextual keys & mouse gestures"), container);
    footerHeader->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 13px; color: #ffffff; margin-top: 4px;"));
    containerLayout->addWidget(footerHeader);

    auto *footerText1 = new QLabel(tr("Text fields keep standard editing keys. Dialogs share the Apply/Cancel assignments above. Numeric fields use Up/Down, with Shift for larger steps. Standard commands include Ctrl+Q to quit. The shortcut editor itself always uses Return to save and Esc to cancel when not recording."), container);
    footerText1->setWordWrap(true);
    footerText1->setStyleSheet(QStringLiteral("color: #8e8e93; font-size: 11px; margin-top: 4px;"));
    containerLayout->addWidget(footerText1);

    auto *footerText2 = new QLabel(tr("Alt temporarily selects the eyedropper in painting tools. Shift constrains shapes/movement or adds to a selection; Alt subtracts from selections or draws from center. Ctrl-drag moves selected pixels; Ctrl-Alt-drag copies them. Alt-drag duplicates layers/folders/effects; Alt-click at a layer boundary toggles clipping. Ctrl-click a thumbnail loads its selection. Control bypasses snapping. Right-drag adjusts brush size. Modifier-and-mouse gestures are fixed."), container);
    footerText2->setWordWrap(true);
    footerText2->setStyleSheet(QStringLiteral("color: #8e8e93; font-size: 11px; margin-top: 4px;"));
    containerLayout->addWidget(footerText2);

    containerLayout->addStretch();
    scrollArea_->setWidget(container);
    mainLayout->addWidget(scrollArea_);

    conflictLabel_ = new QLabel(this);
    conflictLabel_->setStyleSheet(QStringLiteral("color: #ff9f0a; font-size: 12px; font-weight: 500; min-height: 22px;"));
    conflictLabel_->setWordWrap(true);
    conflictLabel_->hide();
    mainLayout->addWidget(conflictLabel_);

    auto *bottomDivider = new QFrame(this);
    bottomDivider->setFrameShape(QFrame::HLine);
    bottomDivider->setStyleSheet(QStringLiteral("color: #3a3a3c;"));
    mainLayout->addWidget(bottomDivider);

    auto *bottomLayout = new QHBoxLayout;
    bottomLayout->setSpacing(10);

    restoreDefaultsButton_ = new QPushButton(tr("Restore Defaults"), this);
    restoreDefaultsButton_->setStyleSheet(QStringLiteral("QPushButton { background-color: #2c2c2e; color: #e5e5e7; border: 1px solid #3a3a3c; border-radius: 6px; padding: 6px 12px; font-size: 12px; } "
                                                         "QPushButton:hover { background-color: #3a3a3c; }"));
    connect(restoreDefaultsButton_, &QPushButton::clicked, this, &KeyboardShortcutsDialog::onRestoreDefaultsClicked);
    bottomLayout->addWidget(restoreDefaultsButton_);

    bottomLayout->addStretch();

    cancelButton_ = new QPushButton(tr("Cancel"), this);
    cancelButton_->setStyleSheet(QStringLiteral("QPushButton { background-color: #2c2c2e; color: #e5e5e7; border: 1px solid #3a3a3c; border-radius: 6px; padding: 6px 12px; font-size: 12px; } "
                                                "QPushButton:hover { background-color: #3a3a3c; }"));
    connect(cancelButton_, &QPushButton::clicked, this, &KeyboardShortcutsDialog::onCancelClicked);
    bottomLayout->addWidget(cancelButton_);

    saveButton_ = new QPushButton(tr("Save"), this);
    saveButton_->setDefault(true);
    saveButton_->setStyleSheet(QStringLiteral("QPushButton { background-color: #0a84ff; color: #ffffff; border: 1px solid #0060df; border-radius: 6px; padding: 6px 16px; font-weight: bold; font-size: 12px; } "
                                              "QPushButton:hover { background-color: #0071e3; } "
                                              "QPushButton:disabled { background-color: #3a3a3c; color: #636366; border-color: #3a3a3c; }"));
    connect(saveButton_, &QPushButton::clicked, this, &KeyboardShortcutsDialog::onSaveClicked);
    bottomLayout->addWidget(saveButton_);

    mainLayout->addLayout(bottomLayout);
}

void KeyboardShortcutsDialog::updateConflictStatus()
{
    const QString problem = ShortcutManager::instance().validate(draftOverrides_);
    if (!problem.isEmpty()) {
        conflictLabel_->setText(problem);
        conflictLabel_->show();
        saveButton_->setEnabled(false);
    } else if (activeRecorder_ != nullptr) {
        conflictLabel_->hide();
        saveButton_->setEnabled(false);
    } else {
        conflictLabel_->hide();
        saveButton_->setEnabled(true);
    }
}

void KeyboardShortcutsDialog::onSearchTextChanged(const QString &text)
{
    const QString query = text.trimmed();
    QMap<QString, bool> anyVisibleInGroup;

    for (const RowEntry &row : rows_) {
        const bool match = query.isEmpty() || row.title.contains(query, Qt::CaseInsensitive);
        row.widget->setVisible(match);
        if (match) {
            anyVisibleInGroup[row.group] = true;
        }
    }

    for (auto it = groupHeaders_.begin(); it != groupHeaders_.end(); ++it) {
        it.value()->setVisible(anyVisibleInGroup.value(it.key(), false));
    }
}

void KeyboardShortcutsDialog::onRestoreDefaultsClicked()
{
    if (activeRecorder_) {
        activeRecorder_->setRecording(false);
        activeRecorder_ = nullptr;
    }
    draftOverrides_.clear();
    for (const RowEntry &row : rows_) {
        row.button->setSequence(ShortcutManager::instance().defaultShortcut(row.id));
    }
    updateConflictStatus();
}

void KeyboardShortcutsDialog::onSaveClicked()
{
    ShortcutManager::instance().setOverrides(draftOverrides_);
    ShortcutManager::instance().saveToSettings();
    accept();
}

void KeyboardShortcutsDialog::onCancelClicked()
{
    reject();
}

} // namespace compositor
