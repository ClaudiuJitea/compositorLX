#include "ui/KeyboardShortcutsDialog.h"

#include <QDialogButtonBox>
#include <QStyle>
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
    // Styled by the theme (QPushButton#shortcutRecorder, and [recording="true"] while it listens).
    setObjectName(QStringLiteral("shortcutRecorder"));
    if (recording_) {
        setText(tr("Press keys…"));
        setProperty("recording", true);
    } else {
        setText(sequence_.isEmpty() ? tr("None") : sequence_.toString(QKeySequence::NativeText));
        setProperty("recording", false);
    }
    style()->unpolish(this); style()->polish(this);
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
    instructionLabel->setObjectName(QStringLiteral("dialogSubtitle"));
    instructionLabel->setWordWrap(true);
    mainLayout->addWidget(instructionLabel);

    searchEdit_ = new QLineEdit(this);
    searchEdit_->setPlaceholderText(tr("Search shortcuts"));
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setAccessibleName(tr("Search shortcuts"));
    connect(searchEdit_, &QLineEdit::textChanged, this, &KeyboardShortcutsDialog::onSearchTextChanged);
    mainLayout->addWidget(searchEdit_);

    scrollArea_ = new QScrollArea(this);
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setFrameShape(QFrame::NoFrame);
    scrollArea_->setObjectName(QStringLiteral("shortcutList"));

    auto *container = new QWidget;
    container->setObjectName(QStringLiteral("shortcutListContents"));
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
        headerLabel->setObjectName(QStringLiteral("sectionTitle"));
        headerLayout->addWidget(headerLabel);

        containerLayout->addWidget(headerWidget);
        groupHeaders_.insert(group, headerWidget);

        for (const ShortcutDefinition &def : definitions) {
            if (def.group != group) continue;

            auto *rowWidget = new QWidget(container);
            auto *rowLayout = new QHBoxLayout(rowWidget);
            rowLayout->setContentsMargins(4, 2, 4, 2);

            auto *titleLabel = new QLabel(def.title, rowWidget);
            rowLayout->addWidget(titleLabel);
            rowLayout->addStretch();

            const QKeySequence currentSeq = draftOverrides_.value(def.id(), def.defaultShortcut);
            auto *recorder = new ShortcutRecorderButton(currentSeq, rowWidget);
            recorder->setAccessibleName(def.title);
            titleLabel->setBuddy(recorder);
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
    containerLayout->addSpacing(12);
    containerLayout->addWidget(footerDivider);
    containerLayout->addSpacing(8);

    auto *footerHeader = new QLabel(tr("Contextual keys & mouse gestures"), container);
    footerHeader->setObjectName(QStringLiteral("sectionTitle"));
    containerLayout->addWidget(footerHeader);

    auto *footerText1 = new QLabel(tr("Text fields keep standard editing keys. Dialogs share the Apply/Cancel assignments above. Numeric fields use Up/Down, with Shift for larger steps. Standard commands include Ctrl+Q to quit. The shortcut editor itself always uses Return to save and Esc to cancel when not recording."), container);
    footerText1->setWordWrap(true);
    footerText1->setObjectName(QStringLiteral("dialogSubtitle"));
    containerLayout->addWidget(footerText1);

    auto *footerText2 = new QLabel(tr("Alt temporarily selects the eyedropper in painting tools. Shift constrains shapes/movement or adds to a selection; Alt subtracts from selections or draws from center. Ctrl-drag moves selected pixels; Ctrl-Alt-drag copies them. Alt-drag duplicates layers/folders/effects; Alt-click at a layer boundary toggles clipping. Ctrl-click a thumbnail loads its selection. Control bypasses snapping. Right-drag adjusts brush size (Shift-right-drag, hardness). Modifier-and-mouse gestures are fixed."), container);
    footerText2->setWordWrap(true);
    footerText2->setObjectName(QStringLiteral("dialogSubtitle"));
    containerLayout->addWidget(footerText2);

    // Some desktops (Xfce, Cinnamon, MATE, older Plasma) move windows with Alt-drag, so Alt-clicks never reach the app.
    auto *footerText3 = new QLabel(tr("If your desktop moves windows with Alt-drag: hold a tool's key to use it for a moment and let go to return (hold I for the eyedropper); "
                                      "Clone Stamp has Set Source, Zoom has Zoom Out, Shape and Crop have From Center, and the selection tools have Subtract. "
                                      "Layer › Create Clipping Mask, Layer › Layer Mask › View Mask Alone and Duplicate Layer (Ctrl+J) cover the rest."), container);
    footerText3->setWordWrap(true);
    footerText3->setObjectName(QStringLiteral("dialogSubtitle"));
    containerLayout->addWidget(footerText3);

    containerLayout->addStretch();
    scrollArea_->setWidget(container);
    mainLayout->addWidget(scrollArea_);

    conflictLabel_ = new QLabel(this);
    conflictLabel_->setObjectName(QStringLiteral("warningLabel"));
    conflictLabel_->setWordWrap(true);
    conflictLabel_->hide();
    mainLayout->addWidget(conflictLabel_);

    // The desktop's button order (KDE and GNOME place them differently); Return saves and Esc cancels (see keyPressEvent).
    auto *buttons = new QDialogButtonBox(this);
    restoreDefaultsButton_ = buttons->addButton(QDialogButtonBox::RestoreDefaults);
    cancelButton_ = buttons->addButton(QDialogButtonBox::Cancel);
    saveButton_ = buttons->addButton(QDialogButtonBox::Save);
    saveButton_->setDefault(true);
    connect(restoreDefaultsButton_, &QPushButton::clicked, this, &KeyboardShortcutsDialog::onRestoreDefaultsClicked);
    connect(cancelButton_, &QPushButton::clicked, this, &KeyboardShortcutsDialog::onCancelClicked);
    connect(saveButton_, &QPushButton::clicked, this, &KeyboardShortcutsDialog::onSaveClicked);
    mainLayout->addWidget(buttons);
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
