#pragma once

#include "ui/SliderJumpStyle.h"

#include <QApplication>
#include <QPalette>
#include <QString>

namespace compositor {
inline QString editorStyleSheet()
{
    return QStringLiteral(R"css(
        * { font-family: "Inter", "SF Pro Text", "Noto Sans", sans-serif; font-size: 12px; }
        QMainWindow#editorWindow { background: #202020; }
        QMenuBar { background: #1b1b1d; color: #e2e4e9; font-size: 13px; font-weight: 500; border-bottom: 1px solid #343438; padding: 4px 9px; spacing: 3px; }
        QMenuBar::item { background: transparent; padding: 5px 10px; border-radius: 5px; }
        QMenuBar::item:selected { background: #30343c; color: #ffffff; }
        QMenuBar::item:pressed { background: #2c435b; color: #b9dcff; }
        QMenu { background: #27272a; color: #ececef; font-size: 13px; border: 1px solid #414146; border-radius: 8px; padding: 6px; }
        QMenu::item { min-width: 190px; min-height: 28px; padding: 4px 30px 4px 12px; border-radius: 5px; }
        QMenu::item:selected { background: #365f89; color: #ffffff; }
        QMenu::item:disabled { color: #707278; background: transparent; }
        QMenu::separator { height: 1px; background: #414146; margin: 5px 7px; }
        QMenu::right-arrow { width: 8px; height: 8px; margin-right: 8px; }
        QDialog, QMessageBox { background: #222225; color: #ededf0; }
        QDialog#modernMessageDialog { background: transparent; }
        QWidget#modernMessagePanel { background: #242428; border: 1px solid #3b3b42; border-radius: 12px; }
        QLabel#modernMessageTitle { color: #f3f3f5; font-size: 15px; font-weight: 600; border: 0; }
        QLabel#modernMessageText { color: #e1e1e4; font-size: 13px; border: 0; }
        QLabel#modernMessageDetail { color: #999ba2; font-size: 12px; border: 0; }
        QWidget#modernMessagePanel QPushButton { min-height: 34px; border-radius: 7px; padding: 0 16px; }
        QWidget#modernMessagePanel QPushButton[dialogRole="primary"] { background: #2d78ce; border-color: #3887dd; color: white; font-weight: 600; }
        QWidget#modernMessagePanel QPushButton[dialogRole="primary"]:hover { background: #3886dc; }
        QWidget#modernMessagePanel QPushButton[dialogRole="destructive"] { color: #f08080; background: transparent; border-color: #554044; }
        QWidget#modernMessagePanel QPushButton[dialogRole="destructive"]:hover { background: #3b292d; border-color: #70454c; }
        QMessageBox { min-width: 410px; }
        QMessageBox QLabel#qt_msgbox_label { min-width: 310px; color: #d7d7db; font-size: 13px; }
        QMessageBox QLabel#qt_msgboxex_icon { min-width: 0; max-width: 0; min-height: 0; max-height: 0; margin: 0; padding: 0; image: none; }
        QMessageBox QPushButton, QDialogButtonBox QPushButton { min-width: 86px; min-height: 31px; border-radius: 6px; }
        QWidget#tabBar { background: #1b1b1d; border-bottom: 1px solid #101012; }
        QWidget#documentTab { background: #363637; border: 1px solid #484849; border-radius: 15px; }
        QTabBar#documentTabs { background: transparent; border: 0; }
        QTabBar#documentTabs::tab { background: transparent; color: #a9abb0; border: 1px solid transparent; border-radius: 5px; min-width: 122px; max-width: 190px; height: 26px; padding: 0 4px 0 10px; margin-right: 3px; }
        QTabBar#documentTabs::tab:selected { background: #2b2b2f; color: #f0f0f2; border-color: #3b3b40; }
        QTabBar#documentTabs::tab:hover:!selected { background: #242428; color: #d9d9dc; }
        QToolButton#documentTabClose { background: transparent; border: 0; border-radius: 4px; min-width: 20px; max-width: 20px; min-height: 20px; max-height: 20px; padding: 0; }
        QToolButton#documentTabClose:hover { background: #46464b; }
        QToolButton#documentTabClose:pressed { background: #19191c; }
        QLabel#tabDot { color: #f3f3f3; font-size: 15px; }
        QToolButton#tabClose { border: 0; background: transparent; color: #a9aaad; font-size: 16px; }
        QToolButton#newTabButton, QToolButton#menuRestoreButton, QToolButton#roundTool { background: transparent; border: 1px solid transparent; border-radius: 13px; }
        QToolButton#menuRestoreButton::menu-indicator { image: none; }
        QToolButton#newTabButton { color: #d4d5d8; font-size: 18px; font-weight: 300; min-width: 25px; max-width: 25px; min-height: 25px; max-height: 25px; padding: 0 0 2px 0; }
        QToolButton#menuRestoreButton { color: #cfd0d3; font-size: 16px; min-width: 25px; max-width: 25px; min-height: 25px; max-height: 25px; padding: 0 0 2px 0; }
        QToolButton#roundTool { min-width: 25px; max-width: 25px; min-height: 25px; max-height: 25px; }
        QToolButton#newTabButton:hover, QToolButton#menuRestoreButton:hover, QToolButton#roundTool:hover { background: #303034; border-color: #404045; }
        QToolButton#newTabButton:pressed, QToolButton#menuRestoreButton:pressed, QToolButton#roundTool:pressed { background: #202023; }
        QPushButton#toolbarPill { min-width: 42px; min-height: 25px; max-height: 25px; padding: 0 8px; border-radius: 12px; }
        QWidget#transformBar QWidget, QWidget#inspector QWidget { font-size: 11px; }
        QWidget#transformBar { background: #242425; border-bottom: 1px solid #414145; }
        QWidget#transformBar QPushButton, QWidget#transformBar QToolButton, QWidget#transformBar QComboBox, QWidget#transformBar QDoubleSpinBox, QWidget#transformBar QSpinBox { min-height: 26px; max-height: 26px; height: 26px; }
        QWidget#transformBar QLabel#sectionTitle { color: #ffffff; padding-right: 8px; }
        QWidget#transformBar QLabel[scrubbable="true"] { color: #a0a0a5; font-size: 11px; padding: 0 2px; }
        QWidget#transformBar QLabel[scrubbable="true"]:hover { color: #ffffff; }
        QWidget#transformBar QAbstractSpinBox:focus, QWidget#transformBar QComboBox:focus { border: 1px solid #67aaff; }
        QWidget#transformBar QToolButton:checked { background: #31577c; border-color: #67aaff; }
        QWidget#segmentedControl,
        QWidget[segmented="true"] { background: #28282a; border: 1px solid #444447; border-radius: 6px; }
        QWidget#segmentedControl QToolButton,
        QWidget[segmented="true"] QToolButton,
        QWidget#segmentedControl QPushButton,
        QWidget[segmented="true"] QPushButton {
            background: transparent;
            border: 0;
            border-right: 1px solid #3c3c40;
            border-radius: 0;
            color: #b8bac0;
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
        QWidget[segmented="true"] QToolButton#textItalic { font-style: italic; font-family: "Times New Roman", serif; font-size: 14px; }
        QWidget#segmentedControl QToolButton#textUnderline,
        QWidget[segmented="true"] QToolButton#textUnderline { text-decoration: underline; }
        QWidget#segmentedControl QToolButton:hover,
        QWidget[segmented="true"] QToolButton:hover,
        QWidget#segmentedControl QPushButton:hover,
        QWidget[segmented="true"] QPushButton:hover {
            background: #353539;
            color: #ffffff;
        }
        QWidget#segmentedControl QToolButton:pressed,
        QWidget[segmented="true"] QToolButton:pressed,
        QWidget#segmentedControl QPushButton:pressed,
        QWidget[segmented="true"] QPushButton:pressed {
            background: #222225;
        }
        QWidget#segmentedControl QToolButton:checked,
        QWidget[segmented="true"] QToolButton:checked,
        QWidget#transformBar QWidget#segmentedControl QToolButton:checked,
        QWidget#transformBar QWidget[segmented="true"] QToolButton:checked {
            background: #235587;
            border: 0;
            border-right: 1px solid #1a4269;
            color: #ffffff;
            font-weight: 600;
        }
        QWidget#segmentedControl QToolButton:checked:hover,
        QWidget[segmented="true"] QToolButton:checked:hover,
        QWidget#transformBar QWidget#segmentedControl QToolButton:checked:hover,
        QWidget#transformBar QWidget[segmented="true"] QToolButton:checked:hover {
            background: #2b639c;
            color: #ffffff;
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
            background: #222224;
            border-color: #333336;
        }
        QWidget#segmentedControl QToolButton:disabled,
        QWidget[segmented="true"] QToolButton:disabled,
        QWidget#segmentedControl QPushButton:disabled,
        QWidget[segmented="true"] QPushButton:disabled {
            color: #58595c;
            background: transparent;
        }
        QWidget#segmentedControl QToolButton:checked:disabled,
        QWidget[segmented="true"] QToolButton:checked:disabled {
            background: #28333e;
            color: #708090;
        }
        QPushButton[primary="true"]:enabled { background: #236fcb; border-color: #2d7ddd; }
        QWidget#transformBar QCheckBox { spacing: 5px; color: #dedee1; }
        QWidget#transformBar QCheckBox::indicator { width: 13px; height: 13px; }
        QToolButton#transformRatioLock { padding: 0; border-radius: 6px; }
        QToolButton#transformRatioLock:checked { background: #244b72; border-color: #3978b4; }
        QToolButton#transformRatioLock:checked:hover { background: #2b5883; }
        QLabel#sectionTitle { font-weight: 600; color: #eeeeef; }
        QLabel#mutedLabel { color: #787a7e; }
        QCheckBox { spacing: 6px; }
        QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid #62656a; border-radius: 3px; background: #303033; }
        QCheckBox::indicator:checked { background: #1678e8; border-color: #2288f3; image: none; }
        QPushButton, QToolButton, QComboBox, QDoubleSpinBox, QSpinBox { background: #333335; border: 1px solid #464649; border-radius: 6px; min-height: 27px; color: #ebebed; }
        QPushButton { padding: 0 11px; }
        QPushButton:hover, QToolButton:hover, QComboBox:hover { background: #3d3d40; }
        QPushButton:pressed, QToolButton:pressed { background: #29292b; }
        QPushButton:disabled, QToolButton:disabled, QComboBox:disabled, QDoubleSpinBox:disabled, QSpinBox:disabled { color: #696b6f; background: #29292a; border-color: #343436; }
        QPushButton#primaryButton:enabled { background: #236fcb; border-color: #2d7ddd; }
        QPushButton#textDone:enabled { background: #236fcb; border-color: #2d7ddd; }
        QFontComboBox#textFont { padding-right: 28px; }
        QDoubleSpinBox, QSpinBox { padding: 0 7px; background: #303032; selection-background-color: #286fc5; }
        QAbstractSpinBox::up-button, QSpinBox::up-button, QDoubleSpinBox::up-button { subcontrol-origin: border; subcontrol-position: top right; width: 20px; border: 0; border-left: 1px solid #44474d; border-top-right-radius: 5px; background: #35373c; }
        QAbstractSpinBox::down-button, QSpinBox::down-button, QDoubleSpinBox::down-button { subcontrol-origin: border; subcontrol-position: bottom right; width: 20px; border: 0; border-left: 1px solid #44474d; border-bottom-right-radius: 5px; background: #35373c; }
        QAbstractSpinBox::up-button:hover, QAbstractSpinBox::down-button:hover, QSpinBox::up-button:hover, QSpinBox::down-button:hover, QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background: #48566a; }
        QAbstractSpinBox::up-button:pressed, QAbstractSpinBox::down-button:pressed, QSpinBox::up-button:pressed, QSpinBox::down-button:pressed, QDoubleSpinBox::up-button:pressed, QDoubleSpinBox::down-button:pressed { background: #2a486a; }
        QAbstractSpinBox::up-arrow, QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: url(:/icons/chevron-up.svg); width: 12px; height: 12px; }
        QAbstractSpinBox::down-arrow, QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: url(:/icons/chevron-down.svg); width: 12px; height: 12px; }
        QComboBox { padding: 0 28px 0 8px; font-weight: 400; }
        QComboBox::drop-down { subcontrol-origin: border; subcontrol-position: top right; border: 0; width: 24px; }
        QComboBox::down-arrow { image: url(:/icons/chevron-down.svg); width: 12px; height: 12px; }
        QComboBox::drop-down:hover { background: #414954; border-top-right-radius: 5px; border-bottom-right-radius: 5px; }
        QComboBox QAbstractItemView { background: #303033; border: 1px solid #555; selection-background-color: #286fc5; }
        QWidget#toolRail { background: #202023; border-right: 1px solid #111113; }
        QWidget#toolRail QToolButton { border: 1px solid transparent; background: transparent; border-radius: 8px; padding: 0; }
        QWidget#toolRail QToolButton:hover { background: #2d2d31; border-color: #38383d; }
        QWidget#toolRail QToolButton:checked { background: #26384b; border-color: #39658f; }
        QWidget#toolRail QToolButton:pressed { background: #1b2d40; }
        QWidget#toolRail QToolButton:focus { border-color: #7ebcff; }
        QWidget#layerFooter QToolButton { background: transparent; border: 1px solid transparent; border-radius: 6px; padding: 0; }
        QWidget#layerFooter QToolButton:hover { background: #35404d; border-color: #4a5a6d; }
        QWidget#layerFooter QToolButton:pressed { background: #24374b; }
        QWidget#layerFooter QToolButton:focus { border-color: #7ebcff; }
        QWidget#layerFooter QToolButton::menu-indicator { image: none; }
        QWidget#layerFooter QToolButton#deleteLayerButton:hover { background: #563236; border-color: #99565e; }
        QTextEdit#inlineTextEditor { background: transparent; border: 0; border-radius: 0; padding: 0; selection-background-color: #286fc5; }
        QTextEdit#inlineTextEditor QWidget { background: transparent; }
        QLabel#inlineTextGrip { color: #8ebbe8; background: transparent; border: 0; font-size: 13px; }
        QWidget#inspector { background: #252526; border-left: 1px solid #111; }
        QWidget#inspectorHeading { background: #252526; border-bottom: 1px solid #161617; }
        QWidget#appearancePanel { background: #29292a; border-bottom: 1px solid #151516; }
        QListView { background: #252526; border: 0; outline: 0; }
        QListView::item { border: 0; }
        QScrollBar:vertical { width: 9px; background: transparent; }
        QScrollBar::handle:vertical { background: #505054; border-radius: 4px; min-height: 30px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QSlider::groove:horizontal { height: 4px; border-radius: 2px; background: #48494c; }
        QSlider::sub-page:horizontal { background: #167be2; border-radius: 2px; }
        QSlider::handle:horizontal { width: 14px; margin: -5px 0; border-radius: 7px; background: #d8dade; }
        QWidget#layerFooter { border-top: 1px solid #151516; }
        QWidget#bottomStatus { background: #242425; border-top: 1px solid #111; }
        QWidget#bottomStatus QLabel { color: #85888d; font-size: 10px; }
        QWidget#newCanvasPage { background: #1d1d20; }
        QWidget#newCanvasPanel { background: #252529; border: 1px solid #37373d; border-radius: 14px; }
        QLabel#emptyStateTitle { color: #f4f4f6; font-size: 20px; font-weight: 600; }
        QLabel#emptyStateSubtitle { color: #92949b; font-size: 12px; }
        QLabel#emptyStateHint { color: #777a82; font-size: 11px; }
        QWidget#newCanvasPanel QSpinBox { min-height: 35px; background: #1e1e21; border: 1px solid #414148; border-radius: 7px; padding: 0 10px; font-size: 13px; selection-background-color: #286fc5; }
        QWidget#newCanvasPanel QSpinBox:focus { border-color: #438bd3; }
        QWidget#newCanvasPanel QSpinBox::up-button { border-top-right-radius: 6px; }
        QWidget#newCanvasPanel QSpinBox::down-button { border-bottom-right-radius: 6px; }
        QWidget#newCanvasPanel QPushButton { min-height: 33px; border-radius: 7px; padding: 0 14px; }
        QPushButton#createCanvas { background: #2d78ce; border-color: #3887dd; color: white; font-weight: 600; }
        QPushButton#createCanvas:hover { background: #3886dc; }
        QToolTip { color: #ededed; background: #303033; border: 1px solid #555; padding: 5px; }
    )css");
}

// The application's look: Fusion with click-to-position sliders, the dark palette and the editor stylesheet. One place, so
// the app and the UI snapshot tool cannot drift apart.
inline void applyEditorTheme(QApplication &application)
{
    application.setStyle(new SliderJumpStyle(QStringLiteral("Fusion")));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(31, 31, 31));
    palette.setColor(QPalette::WindowText, QColor(232, 232, 232));
    palette.setColor(QPalette::Base, QColor(34, 34, 34));
    palette.setColor(QPalette::AlternateBase, QColor(42, 42, 42));
    palette.setColor(QPalette::Text, QColor(232, 232, 232));
    palette.setColor(QPalette::Button, QColor(50, 50, 50));
    palette.setColor(QPalette::ButtonText, QColor(234, 234, 234));
    palette.setColor(QPalette::Highlight, QColor(45, 112, 202));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(105, 105, 105));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(105, 105, 105));
    application.setPalette(palette);
    application.setStyleSheet(editorStyleSheet());
}
} // namespace compositor
