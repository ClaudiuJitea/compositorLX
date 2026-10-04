#pragma once

#include "core/ToolDefaults.h"
#include "ui/SliderJumpStyle.h"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QHash>
#include <QPalette>
#include <QRegularExpression>
#include <QSettings>
#include <QString>
#include <QStyleHints>

#include <optional>

namespace compositor {

// How the editor looks. Dark is the editor's own neutral dark (the default, as image editors are, so colors are judged
// against a quiet surround); Light is its light counterpart; System takes the desktop's palette (Breeze, Adwaita, a high-
// contrast scheme...) and follows it as it changes. All three build the same stylesheet from palette roles.
enum class ThemeMode { Dark, Light, System };

inline QString themeModeKey(ThemeMode mode)
{
    return mode == ThemeMode::Light ? QStringLiteral("light") : mode == ThemeMode::System ? QStringLiteral("system") : QStringLiteral("dark");
}

inline ThemeMode themeModeFromKey(const QString &key)
{
    return key == QLatin1String("light") ? ThemeMode::Light : key == QLatin1String("system") ? ThemeMode::System : ThemeMode::Dark;
}

inline ThemeMode savedThemeMode()
{
    if (!ToolDefaults::enabled()) return ThemeMode::Dark;
    return themeModeFromKey(QSettings().value(QStringLiteral("ui/theme"), QStringLiteral("dark")).toString());
}

namespace theme {

inline QColor mix(const QColor &a, const QColor &b, double t)
{
    const QColor x = a.toRgb(), y = b.toRgb();
    return QColor::fromRgbF(float(x.redF() + (y.redF() - x.redF()) * t), float(x.greenF() + (y.greenF() - x.greenF()) * t),
                            float(x.blueF() + (y.blueF() - x.blueF()) * t), float(x.alphaF() + (y.alphaF() - x.alphaF()) * t));
}

inline bool isDark(const QPalette &palette) { return palette.color(QPalette::Window).lightnessF() < 0.5; }

// The selection color the desktop asks for (KDE's and GNOME's accent), else the palette's highlight.
inline QColor accentOf(const QPalette &palette)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    return palette.color(QPalette::Accent);
#else
    return palette.color(QPalette::Highlight);
#endif
}

// The accent the desktop had when the app started, which the Dark and Light schemes use too.
inline QColor &systemAccent() { static QColor value; return value; }

inline QPalette darkPalette(const QColor &accent)
{
    QPalette p;
    const QColor window(31, 31, 31), text(232, 232, 232), disabled(105, 105, 105);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, QColor(34, 34, 34));
    p.setColor(QPalette::AlternateBase, QColor(42, 42, 42));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(51, 51, 53));
    p.setColor(QPalette::ButtonText, QColor(234, 234, 234));
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Light, QColor(72, 72, 75));
    p.setColor(QPalette::Midlight, QColor(60, 60, 63));
    p.setColor(QPalette::Mid, QColor(44, 44, 46));
    p.setColor(QPalette::Dark, QColor(22, 22, 23));
    p.setColor(QPalette::Shadow, Qt::black);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    p.setColor(QPalette::Accent, accent);
#endif
    p.setColor(QPalette::Link, mix(accent, Qt::white, .35));
    p.setColor(QPalette::ToolTipBase, QColor(48, 48, 51));
    p.setColor(QPalette::ToolTipText, QColor(237, 237, 237));
    p.setColor(QPalette::PlaceholderText, QColor(130, 132, 137));
    for (const auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText}) p.setColor(QPalette::Disabled, role, disabled);
    return p;
}

inline QPalette lightPalette(const QColor &accent)
{
    QPalette p;
    const QColor window(242, 242, 243), text(29, 29, 31), disabled(154, 154, 160);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, Qt::white);
    p.setColor(QPalette::AlternateBase, QColor(246, 246, 247));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(228, 228, 231));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::black);
    p.setColor(QPalette::Light, Qt::white);
    p.setColor(QPalette::Midlight, QColor(236, 236, 238));
    p.setColor(QPalette::Mid, QColor(196, 196, 200));
    p.setColor(QPalette::Dark, QColor(160, 160, 165));
    p.setColor(QPalette::Shadow, QColor(112, 112, 117));
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    p.setColor(QPalette::Accent, accent);
#endif
    p.setColor(QPalette::Link, accent.darker(115));
    p.setColor(QPalette::ToolTipBase, Qt::white);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, QColor(140, 140, 146));
    for (const auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText}) p.setColor(QPalette::Disabled, role, disabled);
    return p;
}

// The stylesheet's colors, worked out from the palette's roles, so one stylesheet serves every scheme.
inline QHash<QString, QString> tokens(const QPalette &p)
{
    const bool dark = isDark(p);
    const QColor W = p.color(QPalette::Window), T = p.color(QPalette::WindowText), B = p.color(QPalette::Button);
    const QColor base = p.color(QPalette::Base), A = accentOf(p), onA = p.color(QPalette::HighlightedText);
    const QColor black(0, 0, 0), white(255, 255, 255), red(208, 74, 85);
    QHash<QString, QColor> c;
    c[QStringLiteral("chrome")] = dark ? mix(W, black, .14) : mix(W, T, .05);
    c[QStringLiteral("surface")] = W;
    c[QStringLiteral("panel")] = dark ? mix(W, T, .025) : mix(W, white, .55);
    c[QStringLiteral("panelAlt")] = dark ? mix(W, T, .05) : mix(W, T, .03);
    c[QStringLiteral("rail")] = dark ? mix(W, T, .01) : mix(W, T, .02);
    c[QStringLiteral("menu")] = dark ? mix(W, T, .055) : base;
    c[QStringLiteral("control")] = B;
    c[QStringLiteral("controlHover")] = mix(B, T, .08);
    c[QStringLiteral("controlPressed")] = mix(B, dark ? black : T, .12);
    c[QStringLiteral("controlDisabled")] = mix(W, B, .5);
    c[QStringLiteral("field")] = dark ? mix(B, black, .05) : base;
    c[QStringLiteral("fieldDeep")] = dark ? mix(W, black, .05) : base;
    c[QStringLiteral("border")] = mix(B, T, dark ? .12 : .22);
    c[QStringLiteral("borderSoft")] = mix(W, T, dark ? .13 : .14);
    c[QStringLiteral("borderDisabled")] = mix(W, T, dark ? .08 : .1);
    c[QStringLiteral("separator")] = dark ? mix(W, black, .45) : mix(W, T, .16);
    c[QStringLiteral("text")] = T;
    c[QStringLiteral("textStrong")] = dark ? mix(T, white, .7) : mix(T, black, .5);
    c[QStringLiteral("textMuted")] = mix(T, W, .33);
    c[QStringLiteral("textFaint")] = mix(T, W, .48);
    c[QStringLiteral("textDisabled")] = p.color(QPalette::Disabled, QPalette::WindowText);
    c[QStringLiteral("accent")] = A;
    c[QStringLiteral("accentHover")] = dark ? A.lighter(112) : A.darker(108);
    c[QStringLiteral("accentPressed")] = A.darker(120);
    c[QStringLiteral("accentBorder")] = mix(W, A, dark ? .58 : .7);
    c[QStringLiteral("onAccent")] = onA;
    c[QStringLiteral("accentStrong")] = dark ? mix(W, A, .68) : A;
    c[QStringLiteral("accentStrongHover")] = dark ? mix(W, A, .78) : A.darker(108);
    c[QStringLiteral("accentSoft")] = mix(W, A, dark ? .3 : .18);
    c[QStringLiteral("accentSoftHover")] = mix(W, A, dark ? .38 : .26);
    c[QStringLiteral("accentSoftPressed")] = mix(W, A, dark ? .22 : .32);
    c[QStringLiteral("accentText")] = dark ? mix(A, white, .55) : A.darker(115);
    c[QStringLiteral("accentDisabled")] = mix(W, A, dark ? .22 : .2);
    c[QStringLiteral("focus")] = dark ? mix(A, white, .45) : A;
    c[QStringLiteral("danger")] = dark ? QColor(240, 128, 128) : QColor(198, 40, 40);
    c[QStringLiteral("warning")] = dark ? QColor(255, 176, 64) : QColor(166, 98, 0);
    c[QStringLiteral("dangerSoft")] = mix(W, red, dark ? .22 : .14);
    c[QStringLiteral("dangerBorder")] = mix(W, red, dark ? .45 : .5);
    c[QStringLiteral("tooltip")] = p.color(QPalette::ToolTipBase);
    c[QStringLiteral("tooltipText")] = p.color(QPalette::ToolTipText);
    QHash<QString, QString> result;
    for (auto it = c.cbegin(); it != c.cend(); ++it) result.insert(it.key(), it.value().name(QColor::HexRgb));
    // Arrows drawn in the text's tone: light ones on a dark scheme, dark ones on a light one.
    result.insert(QStringLiteral("chevronDown"), dark ? QStringLiteral(":/icons/chevron-down.svg") : QStringLiteral(":/icons/chevron-down-dark.svg"));
    result.insert(QStringLiteral("chevronUp"), dark ? QStringLiteral(":/icons/chevron-up.svg") : QStringLiteral(":/icons/chevron-up-dark.svg"));
    return result;
}

} // namespace theme

// The stylesheet as designed: colors are @tokens@ (see theme::tokens), sizes are pixels against a 12 px body text, turned
// into points relative to the system font by `editorStyleSheet`.
inline QString editorStyleTemplate()
{
    return QStringLiteral(R"css(
        QMainWindow#editorWindow { background: @surface@; }
        QMenuBar { background: @chrome@; color: @text@; font-size: 13px; font-weight: 500; border-bottom: 1px solid @borderSoft@; padding: 4px 9px; spacing: 3px; }
        QMenuBar::item { background: transparent; padding: 5px 10px; border-radius: 5px; }
        QMenuBar::item:selected { background: @controlHover@; color: @textStrong@; }
        QMenuBar::item:pressed { background: @accentSoft@; color: @accentText@; }
        QMenu { background: @menu@; color: @text@; font-size: 13px; border: 1px solid @border@; border-radius: 8px; padding: 6px; }
        QMenu::item { min-width: 190px; min-height: 28px; padding: 4px 30px 4px 12px; border-radius: 5px; }
        QMenu::item:selected { background: @accentStrong@; color: @onAccent@; }
        QMenu::item:disabled { color: @textDisabled@; background: transparent; }
        QMenu::separator { height: 1px; background: @border@; margin: 5px 7px; }
        QMenu::right-arrow { width: 8px; height: 8px; margin-right: 8px; }
        QDialog, QMessageBox { background: @surface@; color: @text@; }
        QMessageBox QLabel#qt_msgbox_label { color: @text@; font-size: 13px; min-width: 300px; }
        QMessageBox QLabel#qt_msgbox_informativelabel { color: @textMuted@; }
        QMessageBox QPushButton, QDialogButtonBox QPushButton { min-width: 86px; min-height: 31px; border-radius: 6px; }
        QWidget#tabBar { background: @chrome@; border-bottom: 1px solid @separator@; }
        QWidget#documentTab { background: @control@; border: 1px solid @border@; border-radius: 15px; }
        QTabBar#documentTabs { background: transparent; border: 0; }
        QTabBar#documentTabs::tab { background: transparent; color: @textMuted@; border: 1px solid transparent; border-radius: 5px; min-width: 122px; max-width: 190px; height: 26px; padding: 0 4px 0 10px; margin-right: 3px; }
        QTabBar#documentTabs::tab:selected { background: @panelAlt@; color: @textStrong@; border-color: @borderSoft@; }
        QTabBar#documentTabs::tab:hover:!selected { background: @panel@; color: @text@; }
        QToolButton#documentTabClose { background: transparent; border: 0; border-radius: 4px; min-width: 20px; max-width: 20px; min-height: 20px; max-height: 20px; padding: 0; }
        QToolButton#documentTabClose:hover { background: @controlHover@; }
        QToolButton#documentTabClose:pressed { background: @controlPressed@; }
        QToolButton#tabOverflow { border: 1px solid @border@; border-radius: 8px; padding: 1px 8px; color: @textMuted@; background: transparent; }
        QToolButton#tabOverflow::menu-indicator { image: none; }
        QLabel#tabDot { color: @textStrong@; font-size: 15px; }
        QToolButton#tabClose { border: 0; background: transparent; color: @textMuted@; font-size: 16px; }
        QToolButton#newTabButton, QToolButton#menuRestoreButton, QToolButton#roundTool { background: transparent; border: 1px solid transparent; border-radius: 13px; }
        QToolButton#menuRestoreButton::menu-indicator { image: none; }
        QToolButton#newTabButton { color: @text@; font-size: 18px; font-weight: 300; min-width: 25px; max-width: 25px; min-height: 25px; max-height: 25px; padding: 0 0 2px 0; }
        QToolButton#menuRestoreButton { color: @text@; font-size: 16px; min-width: 25px; max-width: 25px; min-height: 25px; max-height: 25px; padding: 0 0 2px 0; }
        QToolButton#roundTool { min-width: 25px; max-width: 25px; min-height: 25px; max-height: 25px; }
        QToolButton#newTabButton:hover, QToolButton#menuRestoreButton:hover, QToolButton#roundTool:hover { background: @controlHover@; border-color: @border@; }
        QToolButton#newTabButton:pressed, QToolButton#menuRestoreButton:pressed, QToolButton#roundTool:pressed { background: @controlPressed@; }
        QPushButton#toolbarPill { min-width: 42px; min-height: 25px; max-height: 25px; padding: 0 8px; border-radius: 12px; }
        QWidget#transformBar QWidget, QWidget#inspector QWidget { font-size: 11px; }
        QWidget#transformBar { background: @panel@; border-bottom: 1px solid @borderSoft@; }
        QWidget#transformBar QPushButton, QWidget#transformBar QToolButton, QWidget#transformBar QComboBox, QWidget#transformBar QDoubleSpinBox, QWidget#transformBar QSpinBox { min-height: 26px; max-height: 26px; height: 26px; }
        QWidget#transformBar QLabel#sectionTitle { color: @textStrong@; padding-right: 8px; }
        QWidget#transformBar QLabel[scrubbable="true"] { color: @textMuted@; font-size: 11px; padding: 0 2px; }
        QWidget#transformBar QLabel[scrubbable="true"]:hover { color: @textStrong@; }
        QWidget#transformBar QAbstractSpinBox:focus, QWidget#transformBar QComboBox:focus { border: 1px solid @focus@; }
        QWidget#transformBar QToolButton:checked { background: @accentSoft@; border-color: @focus@; }
        QWidget#segmentedControl,
        QWidget[segmented="true"] { background: @control@; border: 1px solid @border@; border-radius: 6px; }
        QWidget#segmentedControl QToolButton,
        QWidget[segmented="true"] QToolButton,
        QWidget#segmentedControl QPushButton,
        QWidget[segmented="true"] QPushButton {
            background: transparent;
            border: 0;
            border-right: 1px solid @borderSoft@;
            border-radius: 0;
            color: @textMuted@;
            padding: 0 10px;
            font-size: 11px;
            font-weight: 500;
            min-height: 28px;
            max-height: 28px;
            height: 28px;
        }
        QWidget#segmentedControl QToolButton[segmentIconOnly="true"],
        QWidget[segmented="true"] QToolButton[segmentIconOnly="true"] {
            padding: 0;
            min-width: 32px;
            max-width: 32px;
        }
        QWidget#segmentedControl QToolButton#textBold,
        QWidget[segmented="true"] QToolButton#textBold,
        QWidget#segmentedControl QToolButton#textItalic,
        QWidget[segmented="true"] QToolButton#textItalic,
        QWidget#segmentedControl QToolButton#textUnderline,
        QWidget[segmented="true"] QToolButton#textUnderline {
            padding: 0;
            min-width: 32px;
            max-width: 32px;
            font-size: 13px;
        }
        QWidget#segmentedControl QToolButton#textBold,
        QWidget[segmented="true"] QToolButton#textBold { font-weight: bold; }
        QWidget#segmentedControl QToolButton#textItalic,
        QWidget[segmented="true"] QToolButton#textItalic { font-style: italic; font-family: serif; font-size: 14px; }
        QWidget#segmentedControl QToolButton#textUnderline,
        QWidget[segmented="true"] QToolButton#textUnderline { text-decoration: underline; }
        QWidget#segmentedControl QToolButton:hover,
        QWidget[segmented="true"] QToolButton:hover,
        QWidget#segmentedControl QPushButton:hover,
        QWidget[segmented="true"] QPushButton:hover {
            background: @controlHover@;
            color: @textStrong@;
        }
        QWidget#segmentedControl QToolButton:pressed,
        QWidget[segmented="true"] QToolButton:pressed,
        QWidget#segmentedControl QPushButton:pressed,
        QWidget[segmented="true"] QPushButton:pressed {
            background: @controlPressed@;
        }
        QWidget#segmentedControl QToolButton:checked,
        QWidget[segmented="true"] QToolButton:checked,
        QWidget#transformBar QWidget#segmentedControl QToolButton:checked,
        QWidget#transformBar QWidget[segmented="true"] QToolButton:checked {
            background: @accentStrong@;
            border: 0;
            border-right: 1px solid @accentPressed@;
            color: @onAccent@;
            font-weight: 600;
        }
        QWidget#segmentedControl QToolButton:checked:hover,
        QWidget[segmented="true"] QToolButton:checked:hover,
        QWidget#transformBar QWidget#segmentedControl QToolButton:checked:hover,
        QWidget#transformBar QWidget[segmented="true"] QToolButton:checked:hover {
            background: @accentStrongHover@;
            color: @onAccent@;
        }
        QWidget#segmentedControl QToolButton[segmentPos="first"],
        QWidget[segmented="true"] QToolButton[segmentPos="first"],
        QWidget#segmentedControl QPushButton[segmentPos="first"],
        QWidget[segmented="true"] QPushButton[segmentPos="first"] {
            border-top-left-radius: 5px;
            border-bottom-left-radius: 5px;
        }
        QWidget#segmentedControl QToolButton[segmentPos="last"],
        QWidget[segmented="true"] QToolButton[segmentPos="last"],
        QWidget#segmentedControl QPushButton[segmentPos="last"],
        QWidget[segmented="true"] QPushButton[segmentPos="last"] {
            border-top-right-radius: 5px;
            border-bottom-right-radius: 5px;
            border-right: 0;
        }
        QWidget#segmentedControl QToolButton[segmentPos="only"],
        QWidget[segmented="true"] QToolButton[segmentPos="only"],
        QWidget#segmentedControl QPushButton[segmentPos="only"],
        QWidget[segmented="true"] QPushButton[segmentPos="only"] {
            border-radius: 5px;
            border-right: 0;
        }
        QWidget#segmentedControl QToolButton:checked[segmentPos="last"],
        QWidget[segmented="true"] QToolButton:checked[segmentPos="last"],
        QWidget#segmentedControl QToolButton:checked[segmentPos="only"],
        QWidget[segmented="true"] QToolButton:checked[segmentPos="only"],
        QWidget#segmentedControl QPushButton:checked[segmentPos="last"],
        QWidget[segmented="true"] QPushButton:checked[segmentPos="last"],
        QWidget#segmentedControl QPushButton:checked[segmentPos="only"],
        QWidget[segmented="true"] QPushButton:checked[segmentPos="only"] {
            border-right: 0;
        }
        QWidget#segmentedControl:disabled,
        QWidget[segmented="true"]:disabled {
            background: @controlDisabled@;
            border-color: @borderDisabled@;
        }
        QWidget#segmentedControl QToolButton:disabled,
        QWidget[segmented="true"] QToolButton:disabled,
        QWidget#segmentedControl QPushButton:disabled,
        QWidget[segmented="true"] QPushButton:disabled {
            color: @textDisabled@;
            background: transparent;
        }
        QWidget#segmentedControl QToolButton:checked:disabled,
        QWidget[segmented="true"] QToolButton:checked:disabled {
            background: @accentDisabled@;
            color: @textDisabled@;
        }
        QPushButton[primary="true"]:enabled { background: @accent@; border-color: @accentHover@; color: @onAccent@; }
        QWidget#transformBar QCheckBox { spacing: 5px; color: @text@; }
        QWidget#transformBar QCheckBox::indicator { width: 14px; height: 14px; }
        QToolButton#transformRatioLock { padding: 0; border-radius: 6px; }
        QToolButton#transformRatioLock:checked { background: @accentSoft@; border-color: @accentBorder@; }
        QToolButton#transformRatioLock:checked:hover { background: @accentSoftHover@; }
        QLabel#sectionTitle { font-weight: 600; color: @textStrong@; }
        QLabel#mutedLabel { color: @textFaint@; }
        QLabel#dialogSubtitle { color: @textMuted@; }
        QLabel#warningLabel { color: @warning@; font-weight: 500; }
        QScrollArea#shortcutList, QWidget#shortcutListContents { background: transparent; }
        QPushButton#shortcutRecorder { font-family: monospace; font-size: 11px; padding: 2px 8px; min-height: 24px; }
        QPushButton#shortcutRecorder[recording="true"] { background: @accent@; border-color: @accentHover@; color: @onAccent@; font-weight: 600; }
        QTabWidget#cameraRawTabs::pane { border: 0; background: transparent; }
        QListWidget#cameraRawSections { background: @fieldDeep@; border: 1px solid @borderSoft@; border-radius: 8px; padding: 4px; outline: 0; }
        QListWidget#cameraRawSections::item { color: @textMuted@; padding: 7px 10px; border-radius: 5px; }
        QListWidget#cameraRawSections::item:hover { background: @panelAlt@; color: @text@; }
        QListWidget#cameraRawSections::item:selected { background: @accentSoft@; color: @textStrong@; }
        QWidget#cameraRawHistogram, QLabel#imagePreview, QScrollArea#jpegPreview { background: @fieldDeep@; border-radius: 4px; }
        QPushButton#clippingShadows, QPushButton#clippingHighlights { color: @textFaint@; background: transparent; border: none; font-size: 10px; min-height: 0; padding: 0; }
        QPushButton#clippingShadows:checked { color: #3388ff; }
        QPushButton#clippingHighlights:checked { color: #ff3333; }
        QCheckBox { spacing: 6px; }
        QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid @border@; border-radius: 3px; background: @field@; }
        QCheckBox::indicator:checked { background: @accent@; border-color: @accentHover@; image: url(:/icons/check.svg); }
        QCheckBox::indicator:checked:disabled { background: @accentDisabled@; border-color: @accentDisabled@; }
        QCheckBox::indicator:hover:!checked { border-color: @textFaint@; }
        QCheckBox::indicator:focus { border-color: @focus@; }
        QPushButton, QToolButton, QComboBox, QDoubleSpinBox, QSpinBox { background: @control@; border: 1px solid @border@; border-radius: 6px; min-height: 27px; color: @text@; }
        QPushButton { padding: 0 11px; }
        QPushButton:hover, QToolButton:hover, QComboBox:hover { background: @controlHover@; }
        QPushButton:pressed, QToolButton:pressed { background: @controlPressed@; }
        QPushButton:focus, QComboBox:focus, QAbstractSpinBox:focus { border-color: @focus@; }
        QPushButton:disabled, QToolButton:disabled, QComboBox:disabled, QDoubleSpinBox:disabled, QSpinBox:disabled { color: @textDisabled@; background: @controlDisabled@; border-color: @borderDisabled@; }
        QPushButton#primaryButton:enabled, QPushButton#textDone:enabled { background: @accent@; border-color: @accentHover@; color: @onAccent@; }
        QPushButton#primaryButton:enabled:hover, QPushButton#textDone:enabled:hover { background: @accentHover@; }
        QFontComboBox#textFont { padding-right: 28px; }
        QLineEdit { background: @field@; color: @text@; border: 1px solid @border@; border-radius: 6px; padding: 4px 7px; selection-background-color: @accent@; selection-color: @onAccent@; }
        QLineEdit:focus { border-color: @focus@; }
        QDoubleSpinBox, QSpinBox { padding: 0 7px; background: @field@; selection-background-color: @accent@; selection-color: @onAccent@; }
        QAbstractSpinBox::up-button, QSpinBox::up-button, QDoubleSpinBox::up-button { subcontrol-origin: border; subcontrol-position: top right; width: 20px; border: 0; border-left: 1px solid @border@; border-top-right-radius: 5px; background: @control@; }
        QAbstractSpinBox::down-button, QSpinBox::down-button, QDoubleSpinBox::down-button { subcontrol-origin: border; subcontrol-position: bottom right; width: 20px; border: 0; border-left: 1px solid @border@; border-bottom-right-radius: 5px; background: @control@; }
        QAbstractSpinBox::up-button:hover, QAbstractSpinBox::down-button:hover, QSpinBox::up-button:hover, QSpinBox::down-button:hover, QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background: @accentSoftHover@; }
        QAbstractSpinBox::up-button:pressed, QAbstractSpinBox::down-button:pressed, QSpinBox::up-button:pressed, QSpinBox::down-button:pressed, QDoubleSpinBox::up-button:pressed, QDoubleSpinBox::down-button:pressed { background: @accentSoftPressed@; }
        QAbstractSpinBox::up-arrow, QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: url(@chevronUp@); width: 12px; height: 12px; }
        QAbstractSpinBox::down-arrow, QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: url(@chevronDown@); width: 12px; height: 12px; }
        QComboBox { padding: 0 28px 0 8px; font-weight: 400; }
        QComboBox::drop-down { subcontrol-origin: border; subcontrol-position: top right; border: 0; width: 24px; }
        QComboBox::down-arrow { image: url(@chevronDown@); width: 12px; height: 12px; }
        QComboBox::drop-down:hover { background: @accentSoftHover@; border-top-right-radius: 5px; border-bottom-right-radius: 5px; }
        QComboBox QAbstractItemView { background: @menu@; color: @text@; border: 1px solid @border@; selection-background-color: @accentStrong@; selection-color: @onAccent@; }
        QWidget#toolRail { background: @rail@; border-right: 1px solid @separator@; }
        QScrollArea#toolRailScroll, QScrollArea#toolRailScroll > QWidget, QWidget#toolRailTools { background: transparent; border: 0; }
        QScrollArea#toolRailScroll QScrollBar:vertical { width: 6px; }
        QWidget#toolRail QToolButton { border: 1px solid transparent; background: transparent; border-radius: 8px; padding: 0; }
        QWidget#toolRail QToolButton:hover { background: @controlHover@; border-color: @borderSoft@; }
        QWidget#toolRail QToolButton:checked { background: @accentSoft@; border-color: @accentBorder@; }
        QWidget#toolRail QToolButton:pressed { background: @accentSoftPressed@; }
        QWidget#toolRail QToolButton:focus { border-color: @focus@; }
        QWidget#layerFooter QToolButton { background: transparent; border: 1px solid transparent; border-radius: 6px; padding: 0; }
        QWidget#layerFooter QToolButton:hover { background: @accentSoft@; border-color: @accentBorder@; }
        QWidget#layerFooter QToolButton:pressed { background: @accentSoftPressed@; }
        QWidget#layerFooter QToolButton:focus { border-color: @focus@; }
        QWidget#layerFooter QToolButton::menu-indicator { image: none; }
        QWidget#layerFooter QToolButton#deleteLayerButton:hover { background: @dangerSoft@; border-color: @dangerBorder@; }
        QTextEdit#inlineTextEditor { background: transparent; border: 0; border-radius: 0; padding: 0; selection-background-color: @accent@; }
        QTextEdit#inlineTextEditor QWidget { background: transparent; }
        QLabel#inlineTextGrip { color: @accentText@; background: transparent; border: 0; font-size: 13px; }
        QWidget#inspector { background: @panel@; border-left: 1px solid @separator@; }
        QWidget#inspectorHeading { background: @panel@; border-bottom: 1px solid @separator@; }
        QWidget#appearancePanel { background: @panelAlt@; border-bottom: 1px solid @separator@; }
        QListView { background: @panel@; color: @text@; border: 0; outline: 0; }
        QListView::item { border: 0; }
        QScrollBar:vertical { width: 9px; background: transparent; }
        QScrollBar::handle:vertical { background: @border@; border-radius: 4px; min-height: 30px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QSlider::groove:horizontal { height: 4px; border-radius: 2px; background: @border@; }
        QSlider::sub-page:horizontal { background: @accent@; border-radius: 2px; }
        QSlider::handle:horizontal { width: 14px; margin: -5px 0; border-radius: 7px; background: @textStrong@; }
        QWidget#layerFooter { border-top: 1px solid @separator@; }
        QWidget#bottomStatus { background: @panel@; border-top: 1px solid @separator@; }
        QWidget#bottomStatus QLabel { color: @textMuted@; font-size: 11px; }
        QWidget#newCanvasPage { background: @chrome@; }
        QWidget#newCanvasPanel { background: @panel@; border: 1px solid @borderSoft@; border-radius: 14px; }
        QLabel#emptyStateTitle { color: @textStrong@; font-size: 20px; font-weight: 600; }
        QLabel#emptyStateSubtitle { color: @textMuted@; font-size: 12px; }
        QLabel#emptyStateHint { color: @textFaint@; font-size: 11px; }
        QWidget#newCanvasPanel QSpinBox { min-height: 35px; background: @fieldDeep@; border: 1px solid @border@; border-radius: 7px; padding: 0 10px; font-size: 13px; selection-background-color: @accent@; }
        QWidget#newCanvasPanel QSpinBox:focus { border-color: @focus@; }
        QWidget#newCanvasPanel QSpinBox::up-button { border-top-right-radius: 6px; }
        QWidget#newCanvasPanel QSpinBox::down-button { border-bottom-right-radius: 6px; }
        QWidget#newCanvasPanel QPushButton { min-height: 33px; border-radius: 7px; padding: 0 14px; }
        QPushButton#createCanvas { background: @accent@; border-color: @accentHover@; color: @onAccent@; font-weight: 600; }
        QPushButton#createCanvas:hover { background: @accentHover@; }
        QPushButton#recentProjectEntry { background: transparent; border: 1px solid transparent; text-align: left; padding: 0 8px; min-height: 28px; color: @text@; }
        QPushButton#recentProjectEntry:hover { background: @controlHover@; border-color: @borderSoft@; }
        QPushButton#recentProjectEntry:focus { border-color: @focus@; }
        QToolTip { color: @tooltipText@; background: @tooltip@; border: 1px solid @border@; padding: 5px; }
    )css");
}

// The stylesheet for `palette` and `systemFont`: colors from the palette's roles, every "font-size: Npx" as N/12 of the
// system font's size in points, the family the system's.
inline QString editorStyleSheet(const QPalette &palette, const QFont &systemFont)
{
    const double base = systemFont.pointSizeF() > 0 ? systemFont.pointSizeF() : (systemFont.pixelSize() > 0 ? systemFont.pixelSize() * 0.75 : 10.0);
    const QString sheet = editorStyleTemplate();
    const QHash<QString, QString> colors = theme::tokens(palette);
    static const QRegularExpression token(QStringLiteral("@([A-Za-z]+)@"));
    static const QRegularExpression size(QStringLiteral("font-size:\\s*(\\d+(?:\\.\\d+)?)px"));
    QString colored;
    qsizetype last = 0;
    for (auto it = token.globalMatch(sheet); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        colored += QStringView(sheet).mid(last, match.capturedStart() - last);
        colored += colors.value(match.captured(1), QStringLiteral("magenta"));
        last = match.capturedEnd();
    }
    colored += QStringView(sheet).mid(last);
    QString result;
    last = 0;
    for (auto it = size.globalMatch(colored); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        result += QStringView(colored).mid(last, match.capturedStart() - last);
        result += QStringLiteral("font-size: %1pt").arg(base * match.captured(1).toDouble() / 12.0, 0, 'f', 1);
        last = match.capturedEnd();
    }
    result += QStringView(colored).mid(last);
    return result;
}

inline QString editorStyleSheet(const QFont &systemFont) { return editorStyleSheet(QApplication::palette(), systemFont); }
inline QString editorStyleSheet() { return editorStyleSheet(QApplication::palette(), QApplication::font()); }

inline ThemeMode currentThemeMode()
{
    return themeModeFromKey(qApp ? qApp->property("editorThemeMode").toString() : QString());
}

// Applies `mode`: the palette, the stylesheet built from it, and, from Qt 6.8, the scheme window frames should follow.
inline void applyThemeMode(QApplication &application, ThemeMode mode)
{
    static bool applying = false;
    if (applying) return;
    applying = true;
    application.setProperty("editorThemeMode", themeModeKey(mode));
    const QColor accent = theme::systemAccent().isValid() ? theme::systemAccent() : QColor(45, 112, 202);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QGuiApplication::styleHints()->setColorScheme(mode == ThemeMode::Dark ? Qt::ColorScheme::Dark
        : mode == ThemeMode::Light ? Qt::ColorScheme::Light : Qt::ColorScheme::Unknown);
#endif
    // System: an empty palette resolves to the desktop's own, and keeps following it; otherwise the editor's.
    if (mode == ThemeMode::System) application.setPalette(QPalette());
    else application.setPalette(mode == ThemeMode::Light ? theme::lightPalette(accent) : theme::darkPalette(accent));
    application.setStyleSheet(editorStyleSheet(application.palette(), application.font()));
    applying = false;
}

// Rebuilds the stylesheet when the desktop's palette, color scheme or font change underneath a scheme that follows them.
class EditorThemeWatcher final : public QObject {
public:
    explicit EditorThemeWatcher(QApplication &application) : QObject(&application), application_(application)
    {
        application.installEventFilter(this);
        QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
            if (currentThemeMode() == ThemeMode::System) applyThemeMode(application_, ThemeMode::System);
        });
    }
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == &application_ && !pending_
            && ((event->type() == QEvent::ApplicationPaletteChange && currentThemeMode() == ThemeMode::System)
                || event->type() == QEvent::ApplicationFontChange)) {
            pending_ = true;
            QMetaObject::invokeMethod(this, [this] {
                pending_ = false;
                const QString sheet = editorStyleSheet(application_.palette(), application_.font());
                if (sheet != application_.styleSheet()) application_.setStyleSheet(sheet);
            }, Qt::QueuedConnection);
        }
        return QObject::eventFilter(watched, event);
    }
private:
    QApplication &application_;
    bool pending_ = false;
};

// The application's look: Fusion with click-to-position sliders, the editor's palette (or the desktop's) and the
// stylesheet built from it. One place, so the app and the UI snapshot tool cannot drift apart.
inline void applyEditorTheme(QApplication &application, std::optional<ThemeMode> mode = std::nullopt)
{
    if (!theme::systemAccent().isValid()) theme::systemAccent() = theme::accentOf(QGuiApplication::palette());
    application.setStyle(new SliderJumpStyle(QStringLiteral("Fusion")));
    applyThemeMode(application, mode.value_or(savedThemeMode()));
    static EditorThemeWatcher *watcher = nullptr;
    if (!watcher) watcher = new EditorThemeWatcher(application);
}

// Chooses and remembers the scheme (View > Theme).
inline void setEditorThemeMode(ThemeMode mode)
{
    if (!qApp) return;
    if (ToolDefaults::enabled()) QSettings().setValue(QStringLiteral("ui/theme"), themeModeKey(mode));
    applyThemeMode(*qApp, mode);
}

} // namespace compositor
