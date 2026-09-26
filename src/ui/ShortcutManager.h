#pragma once

#include <QObject>
#include <QString>
#include <QKeySequence>
#include <QKeyCombination>
#include <QMap>
#include <QVector>
#include <QPointer>
#include <QAction>
#include <QKeyEvent>

namespace compositor {

struct ShortcutDefinition {
    QString title;
    QString group;      // "Menus", "Canvas & Layers", "Text Editing"
    QKeySequence defaultShortcut;

    QString id() const { return group + QStringLiteral(":") + title; }
    bool isMenu() const { return group == QStringLiteral("Menus"); }
};

struct TranslatedKeyEvent {
    bool suppressed = false;
    int key = 0;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
};

class ShortcutManager : public QObject {
    Q_OBJECT
public:
    static ShortcutManager &instance();

    explicit ShortcutManager(QObject *parent = nullptr);
    ~ShortcutManager() override = default;

    const QVector<ShortcutDefinition> &definitions() const;
    const ShortcutDefinition *findDefinition(const QString &id) const;

    QKeySequence defaultShortcut(const QString &id) const;
    QKeySequence shortcut(const QString &id) const;

    QMap<QString, QKeySequence> overrides() const;
    void setOverrides(const QMap<QString, QKeySequence> &overrides);
    void setOverride(const QString &id, const QKeySequence &sequence);
    void clearOverride(const QString &id);

    // Validation returns empty QString if valid, or a descriptive conflict message
    QString validate(const QMap<QString, QKeySequence> &candidateOverrides) const;

    void resetToDefaults();
    void loadFromSettings();
    void saveToSettings() const;

    // Action registration and dynamic remapping
    void registerAction(const QString &id, QAction *action);
    void unregisterAction(QAction *action);
    void apply();
    bool dispatchKeyEvent(const QKeyEvent *event);

    // Key event helper & translation
    static QKeySequence sequenceFromKeyEvent(const QKeyEvent *event);
    bool matchesCommand(const QString &id, const QKeyEvent *event) const;
    TranslatedKeyEvent translateCanvasKeyEvent(const QKeyEvent *event) const;
    TranslatedKeyEvent translateTextKeyEvent(const QKeyEvent *event) const;

signals:
    void shortcutsChanged();

private:
    void initDefinitions();

    QVector<ShortcutDefinition> definitions_;
    QMap<QString, ShortcutDefinition> definitionMap_;
    QMap<QString, QKeySequence> overrides_;
    QMap<QString, QVector<QPointer<QAction>>> registeredActions_;
};

} // namespace compositor
