#include "ui/ShortcutManager.h"

#include <QCoreApplication>
#include <QSettings>
#include <QJsonObject>
#include <QJsonDocument>
#include <QVariantMap>

namespace compositor {

static const QString kStorageKey = QStringLiteral("keyboardShortcuts.v1");

ShortcutManager &ShortcutManager::instance()
{
    static ShortcutManager s_instance;
    return s_instance;
}

ShortcutManager::ShortcutManager(QObject *parent)
    : QObject(parent)
{
    initDefinitions();
    loadFromSettings();
}

const QVector<ShortcutDefinition> &ShortcutManager::definitions() const
{
    return definitions_;
}

const ShortcutDefinition *ShortcutManager::findDefinition(const QString &id) const
{
    auto it = definitionMap_.find(id);
    if (it != definitionMap_.end()) {
        return &it.value();
    }
    return nullptr;
}

QKeySequence ShortcutManager::defaultShortcut(const QString &id) const
{
    const auto *def = findDefinition(id);
    return def ? def->defaultShortcut : QKeySequence();
}

QKeySequence ShortcutManager::shortcut(const QString &id) const
{
    auto it = overrides_.find(id);
    if (it != overrides_.end()) {
        return it.value();
    }
    return defaultShortcut(id);
}

QMap<QString, QKeySequence> ShortcutManager::overrides() const
{
    return overrides_;
}

void ShortcutManager::setOverrides(const QMap<QString, QKeySequence> &overrides)
{
    if (validate(overrides).isEmpty()) {
        overrides_ = overrides;
        apply();
    }
}

void ShortcutManager::setOverride(const QString &id, const QKeySequence &sequence)
{
    QMap<QString, QKeySequence> candidate = overrides_;
    if (sequence.isEmpty() || sequence == defaultShortcut(id)) {
        candidate.remove(id);
    } else {
        candidate.insert(id, sequence);
    }
    setOverrides(candidate);
}

void ShortcutManager::clearOverride(const QString &id)
{
    if (overrides_.remove(id) > 0) {
        apply();
    }
}

QString ShortcutManager::validate(const QMap<QString, QKeySequence> &candidateOverrides) const
{
    QMap<QKeySequence, QString> assigned;

    for (const ShortcutDefinition &def : definitions_) {
        const QKeySequence chord = candidateOverrides.value(def.id(), def.defaultShortcut);
        if (chord.isEmpty() || chord.count() != 1) {
            return QStringLiteral("Choose a single key with optional modifiers.");
        }

        const QKeyCombination comb = chord[0];
        const Qt::KeyboardModifiers mods = comb.keyboardModifiers();

        if (def.group == QStringLiteral("Text Editing")) {
            if (!mods.testFlag(Qt::ControlModifier) && !mods.testFlag(Qt::AltModifier) && !mods.testFlag(Qt::MetaModifier)) {
                return QCoreApplication::translate("ShortcutManager", "Text-editing shortcuts need Ctrl, Alt or Super so they do not replace normal typing.");
            }
        }

        // Ctrl+Q quits on every Linux desktop; Super combinations and the Ctrl+Alt keys below belong to the desktop
        // (launcher, terminal, lock screen, switching to a text console), which takes them before the app ever sees them.
        if (chord == QKeySequence(Qt::CTRL | Qt::Key_Q)) {
            return QCoreApplication::translate("ShortcutManager", "%1 is reserved for Quit.").arg(chord.toString(QKeySequence::NativeText));
        }
        // Ctrl+comma opens Preferences (GNOME HIG), kept free for it.
        if (chord == QKeySequence(Qt::CTRL | Qt::Key_Comma)) {
            return QCoreApplication::translate("ShortcutManager", "%1 is reserved for Preferences.").arg(chord.toString(QKeySequence::NativeText));
        }
        const int key = comb.key();
        const bool ctrlAlt = mods.testFlag(Qt::ControlModifier) && mods.testFlag(Qt::AltModifier);
        if (mods.testFlag(Qt::MetaModifier)
            || (ctrlAlt && (key == Qt::Key_T || key == Qt::Key_L || key == Qt::Key_Delete || key == Qt::Key_Backspace
                            || (key >= Qt::Key_F1 && key <= Qt::Key_F12)))) {
            return QCoreApplication::translate("ShortcutManager", "%1 is used by the desktop, so the app would never receive it.")
                .arg(chord.toString(QKeySequence::NativeText));
        }

        if (assigned.contains(chord)) {
            return QStringLiteral("%1 is assigned to both %2 and %3.")
                .arg(chord.toString(QKeySequence::NativeText), assigned.value(chord), def.title);
        }
        assigned.insert(chord, def.title);
    }

    return QString();
}

void ShortcutManager::resetToDefaults()
{
    overrides_.clear();
    QSettings settings;
    settings.remove(kStorageKey);
    apply();
}

void ShortcutManager::loadFromSettings()
{
    QSettings settings;
    const QVariant val = settings.value(kStorageKey);
    if (!val.isValid()) return;

    QMap<QString, QKeySequence> loaded;
    if (val.userType() == QMetaType::QString) {
        const QJsonDocument doc = QJsonDocument::fromJson(val.toString().toUtf8());
        if (doc.isObject()) {
            const QJsonObject obj = doc.object();
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                loaded.insert(it.key(), QKeySequence(it.value().toString(), QKeySequence::PortableText));
            }
        }
    } else if (val.canConvert<QVariantMap>()) {
        const QVariantMap map = val.toMap();
        for (auto it = map.begin(); it != map.end(); ++it) {
            loaded.insert(it.key(), QKeySequence(it.value().toString(), QKeySequence::PortableText));
        }
    }

    if (validate(loaded).isEmpty()) {
        overrides_ = loaded;
    }
}

void ShortcutManager::saveToSettings() const
{
    QJsonObject obj;
    for (auto it = overrides_.begin(); it != overrides_.end(); ++it) {
        obj.insert(it.key(), it.value().toString(QKeySequence::PortableText));
    }
    QSettings settings;
    settings.setValue(kStorageKey, QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
}

void ShortcutManager::registerAction(const QString &id, QAction *action)
{
    if (!action) return;
    registeredActions_[id].append(action);
    action->setShortcut(shortcut(id));
    connect(action, &QObject::destroyed, this, [this, action]() {
        unregisterAction(action);
    });
}

void ShortcutManager::unregisterAction(QAction *action)
{
    if (!action) return;
    for (auto &actionList : registeredActions_) {
        actionList.removeAll(action);
    }
}

void ShortcutManager::apply()
{
    // Step 1: Clear all shortcuts to prevent duplicate conflicts during remapping
    for (auto &actionList : registeredActions_) {
        for (QPointer<QAction> &action : actionList) {
            if (action) action->setShortcut(QKeySequence());
        }
    }

    // Step 2: Assign updated shortcuts
    for (auto it = registeredActions_.begin(); it != registeredActions_.end(); ++it) {
        const QString &id = it.key();
        const QKeySequence seq = shortcut(id);
        for (QPointer<QAction> &action : it.value()) {
            if (action) {
                action->setShortcut(seq);
            }
        }
    }

    emit shortcutsChanged();
}

bool ShortcutManager::dispatchKeyEvent(const QKeyEvent *event)
{
    if (!event) return false;
    const QKeySequence input = sequenceFromKeyEvent(event);
    if (input.isEmpty()) return false;

    for (const ShortcutDefinition &def : definitions_) {
        if (shortcut(def.id()) == input) {
            auto it = registeredActions_.find(def.id());
            if (it != registeredActions_.end()) {
                for (const auto &action : it.value()) {
                    if (action && action->isEnabled()) {
                        action->trigger();
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

QKeySequence ShortcutManager::sequenceFromKeyEvent(const QKeyEvent *event)
{
    if (!event) return QKeySequence();
    const int key = event->key();
    if (key == 0 || key == Qt::Key_unknown) {
        return QKeySequence();
    }
    // Ignore standalone modifier presses
    if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta) {
        return QKeySequence();
    }

    Qt::KeyboardModifiers modifiers = event->modifiers();
    modifiers &= (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);

    return QKeySequence(QKeyCombination(modifiers, Qt::Key(key)));
}

bool ShortcutManager::matchesCommand(const QString &id, const QKeyEvent *event) const
{
    if (!event) return false;
    const QKeySequence current = shortcut(id);
    if (current.isEmpty()) return false;
    const QKeySequence input = sequenceFromKeyEvent(event);
    return current == input;
}

TranslatedKeyEvent ShortcutManager::translateCanvasKeyEvent(const QKeyEvent *event) const
{
    TranslatedKeyEvent result;
    result.suppressed = false;
    result.key = event ? event->key() : 0;
    result.modifiers = event ? event->modifiers() : Qt::NoModifier;

    if (!event || overrides_.isEmpty()) {
        return result;
    }

    const QKeySequence input = sequenceFromKeyEvent(event);
    if (input.isEmpty()) {
        return result;
    }

    // 1. Check if input matches any currently configured Canvas & Layers shortcut
    for (const ShortcutDefinition &def : definitions_) {
        if (def.group == QStringLiteral("Canvas & Layers")) {
            const QKeySequence current = shortcut(def.id());
            if (current == input) {
                if (current == def.defaultShortcut) {
                    return result; // Unmodified default
                }
                if (def.defaultShortcut.isEmpty()) {
                    result.suppressed = true;
                    return result;
                }
                const QKeyCombination comb = def.defaultShortcut[0];
                result.key = int(comb.key());
                result.modifiers = comb.keyboardModifiers();
                return result;
            }
        }
    }

    // 2. Check if input matches the default shortcut of an action that was remapped
    for (const ShortcutDefinition &def : definitions_) {
        if (def.group != QStringLiteral("Text Editing")) {
            if (def.defaultShortcut == input && shortcut(def.id()) != input) {
                result.suppressed = true;
                return result;
            }
        }
    }

    return result;
}

TranslatedKeyEvent ShortcutManager::translateTextKeyEvent(const QKeyEvent *event) const
{
    TranslatedKeyEvent result;
    result.suppressed = false;
    result.key = event ? event->key() : 0;
    result.modifiers = event ? event->modifiers() : Qt::NoModifier;

    if (!event || overrides_.isEmpty()) {
        return result;
    }

    const QKeySequence input = sequenceFromKeyEvent(event);
    if (input.isEmpty()) {
        return result;
    }

    const auto isTextOrEscape = [](const ShortcutDefinition &def) {
        return def.group == QStringLiteral("Text Editing")
            || def.id() == QStringLiteral("Canvas & Layers:Cancel current canvas operation")
            || (def.defaultShortcut.count() > 0 && def.defaultShortcut[0].key() == Qt::Key_Escape);
    };

    // 1. Check if input matches any currently configured Text Editing shortcut,
    //    or the Escape shortcut (Cancel current canvas operation)
    for (const ShortcutDefinition &def : definitions_) {
        if (isTextOrEscape(def)) {
            const QKeySequence current = shortcut(def.id());
            if (current == input) {
                if (current == def.defaultShortcut) {
                    return result; // Unmodified default
                }
                if (def.defaultShortcut.isEmpty()) {
                    result.suppressed = true;
                    return result;
                }
                const QKeyCombination comb = def.defaultShortcut[0];
                result.key = int(comb.key());
                result.modifiers = comb.keyboardModifiers();
                return result;
            }
        }
    }

    // 2. Check if input matches the default shortcut of a Text Editing / Escape action that was remapped
    for (const ShortcutDefinition &def : definitions_) {
        if (isTextOrEscape(def)) {
            if (def.defaultShortcut == input && shortcut(def.id()) != input) {
                result.suppressed = true;
                return result;
            }
        }
    }

    return result;
}

void ShortcutManager::initDefinitions()
{
    definitions_.clear();
    definitionMap_.clear();

    const auto addEntry = [this](const QString &title, const QString &group, const QKeySequence &seq) {
        ShortcutDefinition def{title, group, seq};
        definitions_.append(def);
        definitionMap_.insert(def.id(), def);
    };

    // --- Menus ---
    addEntry(QStringLiteral("Undo"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_Z));
    addEntry(QStringLiteral("Redo"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z));
    addEntry(QStringLiteral("New Canvas"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_N));
    addEntry(QStringLiteral("Open Project"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_O));
    addEntry(QStringLiteral("Save"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_S));
    addEntry(QStringLiteral("Save As"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    addEntry(QStringLiteral("Export PNG"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    addEntry(QStringLiteral("Export JPEG"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_S));
    addEntry(QStringLiteral("Close Project"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_W));
    addEntry(QStringLiteral("Fit Canvas"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_0));
    addEntry(QStringLiteral("Actual Pixels"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_1));
    addEntry(QStringLiteral("Zoom In"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_Equal));
    addEntry(QStringLiteral("Zoom Out"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_Minus));
    addEntry(QStringLiteral("Show Transform Controls"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_H));
    addEntry(QStringLiteral("Cut"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_X));
    addEntry(QStringLiteral("Copy"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_C));
    addEntry(QStringLiteral("Copy Merged"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    addEntry(QStringLiteral("Paste"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_V));
    addEntry(QStringLiteral("Fill with Foreground"), QStringLiteral("Menus"), QKeySequence(Qt::ALT | Qt::Key_Backspace));
    addEntry(QStringLiteral("Fill with Background"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_Backspace));
    addEntry(QStringLiteral("Content-Aware Fill"), QStringLiteral("Menus"), QKeySequence(Qt::SHIFT | Qt::Key_Delete));
    addEntry(QStringLiteral("Select All"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_A));
    addEntry(QStringLiteral("Deselect"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_D));
    addEntry(QStringLiteral("Inverse Selection"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I));
    addEntry(QStringLiteral("Select Subject"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_A));
    addEntry(QStringLiteral("Curves"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_M));
    addEntry(QStringLiteral("Levels"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_L));
    addEntry(QStringLiteral("Hue/Saturation"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_U));
    addEntry(QStringLiteral("Invert Pixels / Mask"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_I));
    addEntry(QStringLiteral("Canvas Size"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_C));
    addEntry(QStringLiteral("Image Size"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_I));
    addEntry(QStringLiteral("Transform Layer / Selection"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_T));
    addEntry(QStringLiteral("Duplicate / Layer via Copy"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_J));
    addEntry(QStringLiteral("Toggle Clipping Mask"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_G));
    addEntry(QStringLiteral("Group Layers"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_G));
    addEntry(QStringLiteral("Ungroup Layers"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G));
    addEntry(QStringLiteral("New Blank Layer"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    addEntry(QStringLiteral("Move Layer Up"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_BracketRight));
    addEntry(QStringLiteral("Move Layer Down"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_BracketLeft));
    addEntry(QStringLiteral("Merge Layers"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_E));
    addEntry(QStringLiteral("Show Grid"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_Apostrophe));
    addEntry(QStringLiteral("Show Guides"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_Semicolon));
    addEntry(QStringLiteral("Show Rulers"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::Key_R));
    addEntry(QStringLiteral("Snap"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Semicolon));
    addEntry(QStringLiteral("Lock Guides"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Semicolon));
    addEntry(QStringLiteral("Camera Raw Filter"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    addEntry(QStringLiteral("Feather Selection"), QStringLiteral("Menus"), QKeySequence(Qt::SHIFT | Qt::Key_F6));
    addEntry(QStringLiteral("Show Menu Bar"), QStringLiteral("Menus"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));

    // --- Canvas & Layers ---
    addEntry(QStringLiteral("Select tool"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_A));
    addEntry(QStringLiteral("Move / Transform tool"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_V));
    addEntry(QStringLiteral("Hand tool"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_H));
    addEntry(QStringLiteral("Zoom tool"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_Z));
    addEntry(QStringLiteral("Brush tool"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_B));
    addEntry(QStringLiteral("Eraser"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_E));
    addEntry(QStringLiteral("Spot Healing"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_J));
    addEntry(QStringLiteral("Clone Stamp"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_S));
    addEntry(QStringLiteral("Type tool"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_T));
    addEntry(QStringLiteral("Gradient tool"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_G));
    addEntry(QStringLiteral("Shape tool"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_U));
    addEntry(QStringLiteral("Eyedropper tool"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_I));
    addEntry(QStringLiteral("Marquee / cycle shape"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_M));
    addEntry(QStringLiteral("Cycle marquee kind"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::SHIFT | Qt::Key_M));
    addEntry(QStringLiteral("Magic"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_W));
    addEntry(QStringLiteral("Cycle wand mode"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::SHIFT | Qt::Key_W));
    addEntry(QStringLiteral("Lasso / cycle mode"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_L));
    addEntry(QStringLiteral("Cycle lasso kind"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::SHIFT | Qt::Key_L));
    addEntry(QStringLiteral("Blur / Smudge / Liquify"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_R));
    addEntry(QStringLiteral("Cycle smear mode"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::SHIFT | Qt::Key_R));
    addEntry(QStringLiteral("Crop tool"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_C));
    addEntry(QStringLiteral("Swap foreground/background"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_X));
    addEntry(QStringLiteral("Reset colors"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_D));
    addEntry(QStringLiteral("Cycle tool mode"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_Tab));
    addEntry(QStringLiteral("Temporary Hand tool (hold)"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_Space));
    addEntry(QStringLiteral("Delete selection / layer / effect / lasso point"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_Delete));
    addEntry(QStringLiteral("Apply current canvas operation"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_Return));
    addEntry(QStringLiteral("Cancel current canvas operation"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_Escape));
    addEntry(QStringLiteral("Decrease brush size"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_BracketLeft));
    addEntry(QStringLiteral("Increase brush size"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_BracketRight));
    addEntry(QStringLiteral("Decrease brush hardness"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::SHIFT | Qt::Key_BracketLeft));
    addEntry(QStringLiteral("Increase brush hardness"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::SHIFT | Qt::Key_BracketRight));
    addEntry(QStringLiteral("Previous blend mode"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::SHIFT | Qt::Key_Minus));
    addEntry(QStringLiteral("Next blend mode"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::SHIFT | Qt::Key_Equal));
    addEntry(QStringLiteral("Cycle shape kind"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::SHIFT | Qt::Key_U));

    for (int digit = 0; digit <= 9; ++digit) {
        addEntry(QStringLiteral("Opacity digit %1 (type two for exact %)").arg(digit),
                 QStringLiteral("Canvas & Layers"),
                 QKeySequence(Qt::Key_0 + digit));
    }

    addEntry(QStringLiteral("Nudge Left 1 px"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_Left));
    addEntry(QStringLiteral("Nudge Right 1 px"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_Right));
    addEntry(QStringLiteral("Nudge Up 1 px"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_Up));
    addEntry(QStringLiteral("Nudge Down 1 px"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::Key_Down));

    addEntry(QStringLiteral("Nudge Left 10 px"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::SHIFT | Qt::Key_Left));
    addEntry(QStringLiteral("Nudge Right 10 px"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::SHIFT | Qt::Key_Right));
    addEntry(QStringLiteral("Nudge Up 10 px"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::SHIFT | Qt::Key_Up));
    addEntry(QStringLiteral("Nudge Down 10 px"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::SHIFT | Qt::Key_Down));

    addEntry(QStringLiteral("Move selected pixels Left 1 px"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::CTRL | Qt::Key_Left));
    addEntry(QStringLiteral("Move selected pixels Right 1 px"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::CTRL | Qt::Key_Right));
    addEntry(QStringLiteral("Move selected pixels Up 1 px"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::CTRL | Qt::Key_Up));
    addEntry(QStringLiteral("Move selected pixels Down 1 px"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::CTRL | Qt::Key_Down));

    addEntry(QStringLiteral("Move selected pixels Left 10 px"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Left));
    addEntry(QStringLiteral("Move selected pixels Right 10 px"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Right));
    addEntry(QStringLiteral("Move selected pixels Up 10 px"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Up));
    addEntry(QStringLiteral("Move selected pixels Down 10 px"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Down));

    addEntry(QStringLiteral("Toggle Levels preview"), QStringLiteral("Canvas & Layers"), QKeySequence(Qt::ALT | Qt::Key_P));

    // --- Text Editing ---
    addEntry(QStringLiteral("Finish editing text"), QStringLiteral("Text Editing"), QKeySequence(Qt::CTRL | Qt::Key_Return));
    addEntry(QStringLiteral("Decrease tracking"), QStringLiteral("Text Editing"), QKeySequence(Qt::ALT | Qt::Key_Left));
    addEntry(QStringLiteral("Increase tracking"), QStringLiteral("Text Editing"), QKeySequence(Qt::ALT | Qt::Key_Right));
    addEntry(QStringLiteral("Decrease leading"), QStringLiteral("Text Editing"), QKeySequence(Qt::ALT | Qt::Key_Up));
    addEntry(QStringLiteral("Increase leading"), QStringLiteral("Text Editing"), QKeySequence(Qt::ALT | Qt::Key_Down));

    addEntry(QStringLiteral("Decrease tracking by 10"), QStringLiteral("Text Editing"), QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Left));
    addEntry(QStringLiteral("Increase tracking by 10"), QStringLiteral("Text Editing"), QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Right));
    addEntry(QStringLiteral("Decrease leading by 10"), QStringLiteral("Text Editing"), QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Up));
    addEntry(QStringLiteral("Increase leading by 10"), QStringLiteral("Text Editing"), QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Down));
}

} // namespace compositor
