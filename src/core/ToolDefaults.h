#pragma once

#include <QCoreApplication>
#include <QSettings>
#include <QString>

namespace compositor {

/// Toggles that belong to the person rather than to a document: Auto Select, the transform box, rulers, guides, the
/// grid and the snapping switches. They keep whatever they were last set to, across tabs and across launches, the way
/// Photoshop's tool options do (mac ToolDefaults).
///
/// Persistence only happens in the real application (the one that names its organization); tests and tools get the
/// compiled-in defaults, so one run flipping a switch can't reach another or the app the person actually uses.
struct ToolDefaults {
    [[nodiscard]] static bool enabled() { return !QCoreApplication::organizationName().isEmpty(); }
    [[nodiscard]] static bool boolean(const QString &key, bool fallback)
    {
        if (!enabled()) return fallback;
        const QVariant value = QSettings().value(QStringLiteral("tool/") + key);
        return value.isValid() ? value.toBool() : fallback;
    }
    static void set(const QString &key, bool value)
    {
        if (!enabled()) return;
        QSettings().setValue(QStringLiteral("tool/") + key, value);
    }
};

} // namespace compositor
