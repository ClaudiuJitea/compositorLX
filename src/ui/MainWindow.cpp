#include "ui/MainWindow.h"
#include "io/ImageExporter.h"

#include "io/ProjectReader.h"
#include "io/ProjectWriter.h"
#include "io/ImageImporter.h"
#include "io/PSDReader.h"
#include "rendering/LayerRenderer.h"
#include "rendering/TextLayout.h"
#include "rendering/RasterOperations.h"
#include "rendering/Dither.h"
#include "rendering/SubjectRemoval.h"
#include "ui/CanvasWidget.h"
#include "ui/LayerListModel.h"
#include "ui/ToolOptionsLayout.h"
#include "ui/EditorIcons.h"
#include "ui/SegmentedControl.h"
#include "ui/EffectsDialog.h"
#include "ui/CameraRawDialog.h"
#include "ui/RawDevelopDialog.h"
#include "io/RawImporter.h"
#include "io/SvgImporter.h"
#include "ui/CanvasRulerWidget.h"
#include "ui/TrimDialog.h"
#include "ui/NumericScrub.h"
#include "ui/ShortcutManager.h"
#include "ui/KeyboardShortcutsDialog.h"
#include "ui/InlineTextEditor.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QAbstractButton>
#include <QAbstractItemDelegate>
#include <QCheckBox>
#include <QColorDialog>
#include <QColorSpace>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QComboBox>
#include <QCompleter>
#include <QDoubleSpinBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QListWidget>
#include <QFontComboBox>
#include <QImageReader>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenuBar>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QMessageBox>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QImageWriter>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QProgressDialog>
#include <QPlainTextEdit>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QStandardPaths>
#include <QStandardItemModel>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QStyleOptionSlider>
#include <QStyleOptionViewItem>
#include <QToolButton>
#include <QUrl>
#include <QVersionNumber>
#include <QJsonDocument>
#include <QTabBar>
#include <QTextEdit>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

static inline void initCompositorResources()
{
    Q_INIT_RESOURCE(resources);
}

namespace compositor {

namespace {

enum class TextEditCommand { Undo, Redo, Cut, Copy, Paste, SelectAll, Delete, DeleteWordBackward, DeleteToBeginning };

bool textEditorHasFocus()
{
    QWidget *focus = QApplication::focusWidget();
    return qobject_cast<QLineEdit *>(focus) || qobject_cast<QTextEdit *>(focus) || qobject_cast<QPlainTextEdit *>(focus);
}

bool dispatchTextEditCommand(TextEditCommand command)
{
    QWidget *focus = QApplication::focusWidget();
    if (auto *line = qobject_cast<QLineEdit *>(focus)) {
        switch (command) {
        case TextEditCommand::Undo: line->undo(); break;
        case TextEditCommand::Redo: line->redo(); break;
        case TextEditCommand::Cut: line->cut(); break;
        case TextEditCommand::Copy: line->copy(); break;
        case TextEditCommand::Paste: line->paste(); break;
        case TextEditCommand::SelectAll: line->selectAll(); break;
        case TextEditCommand::Delete: line->del(); break;
        case TextEditCommand::DeleteWordBackward: {
            int start = line->cursorPosition();
            const QString value = line->text();
            while (start > 0 && value.at(start - 1).isSpace()) --start;
            while (start > 0 && !value.at(start - 1).isSpace()) --start;
            line->setSelection(start, line->cursorPosition() - start); line->del(); break;
        }
        case TextEditCommand::DeleteToBeginning: {
            const int end = line->cursorPosition(); line->setSelection(0, end); line->del(); break;
        }
        }
        return true;
    }
    const auto dispatchDocument = [command](auto *editor) {
        switch (command) {
        case TextEditCommand::Undo: editor->undo(); break;
        case TextEditCommand::Redo: editor->redo(); break;
        case TextEditCommand::Cut: editor->cut(); break;
        case TextEditCommand::Copy: editor->copy(); break;
        case TextEditCommand::Paste: editor->paste(); break;
        case TextEditCommand::SelectAll: editor->selectAll(); break;
        case TextEditCommand::Delete: editor->textCursor().deleteChar(); break;
        case TextEditCommand::DeleteWordBackward: { auto cursor = editor->textCursor(); cursor.movePosition(QTextCursor::PreviousWord, QTextCursor::KeepAnchor); cursor.removeSelectedText(); break; }
        case TextEditCommand::DeleteToBeginning: { auto cursor = editor->textCursor(); cursor.movePosition(QTextCursor::StartOfLine, QTextCursor::KeepAnchor); cursor.removeSelectedText(); break; }
        }
    };
    if (auto *editor = qobject_cast<QTextEdit *>(focus)) { dispatchDocument(editor); return true; }
    if (auto *editor = qobject_cast<QPlainTextEdit *>(focus)) { dispatchDocument(editor); return true; }
    return false;
}

class SnapSlider final : public QSlider {
public:
    using QSlider::QSlider;
protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && orientation() == Qt::Horizontal && isEnabled()) {
            QStyleOptionSlider option; initStyleOption(&option);
            const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
            if (!handle.contains(event->position().toPoint())) {
                const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
                const int travel = std::max(1, groove.width() - handle.width());
                const int position = std::clamp(qRound(event->position().x() - groove.left() - handle.width() / 2.0), 0, travel);
                setValue(QStyle::sliderValueFromPosition(minimum(), maximum(), position, travel, option.upsideDown));
            }
        }
        QSlider::mousePressEvent(event);
    }
};

int runFloatingDialog(QDialog &dialog)
{
    MainWindow *window = dynamic_cast<MainWindow *>(dialog.parentWidget());
    CanvasWidget *canvas = window ? window->findChild<CanvasWidget *>() : nullptr;
    const bool canvasWasBlocked = canvas && canvas->editorInteractionBlocked();
    if (canvas) canvas->setEditorInteractionBlocked(true);
    QVector<QPair<QAction *, bool>> actionStates;
    QVector<QPair<QWidget *, bool>> widgetStates;
    if (window) {
        for (QAction *action : window->findChildren<QAction *>()) { actionStates.push_back(qMakePair(action, action->isEnabled())); action->setEnabled(false); }
        for (const QString &name : {QStringLiteral("tabBar"), QStringLiteral("transformBar"), QStringLiteral("toolRail"), QStringLiteral("inspector")}) if (QWidget *widget = window->findChild<QWidget *>(name)) { widgetStates.push_back(qMakePair(widget, widget->isEnabled())); widget->setEnabled(false); }
    }
    dialog.setWindowModality(Qt::NonModal);
    dialog.show();
    QEventLoop loop;
    QObject::connect(&dialog, &QDialog::finished, &loop, &QEventLoop::quit);
    loop.exec();
    for (const auto &[widget, enabled] : widgetStates) widget->setEnabled(enabled);
    for (const auto &[action, enabled] : actionStates) action->setEnabled(enabled);
    if (canvas) canvas->setEditorInteractionBlocked(canvasWasBlocked);
    return dialog.result();
}

static std::function<std::optional<QMessageBox::StandardButton>(const QString &title, const QString &text)> sMessageDialogHook = nullptr;

QMessageBox::StandardButton showMessage(QWidget *parent, const QString &title, const QString &text,
                                        const QString &detail = QString(),
                                        QMessageBox::StandardButtons buttons = QMessageBox::Ok,
                                        QMessageBox::StandardButton defaultButton = QMessageBox::NoButton)
{
    if (sMessageDialogHook) {
        auto simulated = sMessageDialogHook(title, text);
        if (simulated.has_value()) {
            return *simulated;
        }
    }

    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("modernMessageDialog"));
    dialog.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    dialog.setAttribute(Qt::WA_TranslucentBackground);
    dialog.setModal(true);

    auto *outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(12, 12, 12, 12);
    auto *panel = new QWidget(&dialog);
    panel->setObjectName(QStringLiteral("modernMessagePanel"));
    panel->setMinimumWidth(460);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(24, 22, 24, 20);
    layout->setSpacing(0);

    auto *heading = new QLabel(title, panel);
    heading->setObjectName(QStringLiteral("modernMessageTitle"));
    layout->addWidget(heading);
    layout->addSpacing(18);
    auto *message = new QLabel(text, panel);
    message->setObjectName(QStringLiteral("modernMessageText"));
    message->setWordWrap(true);
    message->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(message);
    if (!detail.isEmpty()) {
        layout->addSpacing(9);
        auto *supporting = new QLabel(detail, panel);
        supporting->setObjectName(QStringLiteral("modernMessageDetail"));
        supporting->setWordWrap(true);
        supporting->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(supporting);
    }
    layout->addSpacing(22);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(9);
    const auto labelFor = [](QMessageBox::StandardButton button) {
        switch (button) {
        case QMessageBox::Save: return QObject::tr("Save");
        case QMessageBox::Cancel: return QObject::tr("Cancel");
        case QMessageBox::Discard: return QObject::tr("Don't Save");
        case QMessageBox::Yes: return QObject::tr("Yes");
        case QMessageBox::No: return QObject::tr("No");
        case QMessageBox::Open: return QObject::tr("Open");
        default: return QObject::tr("OK");
        }
    };
    if (defaultButton == QMessageBox::NoButton) {
        if (buttons.testFlag(QMessageBox::Save)) defaultButton = QMessageBox::Save;
        else if (buttons.testFlag(QMessageBox::Open)) defaultButton = QMessageBox::Open;
        else if (buttons.testFlag(QMessageBox::Yes)) defaultButton = QMessageBox::Yes;
        else if (buttons.testFlag(QMessageBox::Ok)) defaultButton = QMessageBox::Ok;
    }
    const auto addButton = [&](QMessageBox::StandardButton standard) {
        auto *button = new QPushButton(labelFor(standard), panel);
        button->setMinimumWidth(96);
        button->setFixedHeight(34);
        button->setProperty("dialogRole", standard == defaultButton ? QStringLiteral("primary")
            : standard == QMessageBox::Discard ? QStringLiteral("destructive") : QStringLiteral("secondary"));
        if (standard == defaultButton) { button->setDefault(true); button->setFocus(); }
        QObject::connect(button, &QPushButton::clicked, &dialog, [&dialog, standard] { dialog.done(int(standard)); });
        actions->addWidget(button);
    };
    if (buttons.testFlag(QMessageBox::Discard)) addButton(QMessageBox::Discard);
    actions->addStretch();
    if (buttons.testFlag(QMessageBox::No)) addButton(QMessageBox::No);
    if (buttons.testFlag(QMessageBox::Cancel)) addButton(QMessageBox::Cancel);
    if (buttons.testFlag(QMessageBox::Yes)) addButton(QMessageBox::Yes);
    if (buttons.testFlag(QMessageBox::Ok)) addButton(QMessageBox::Ok);
    if (buttons.testFlag(QMessageBox::Open)) addButton(QMessageBox::Open);
    if (buttons.testFlag(QMessageBox::Save)) addButton(QMessageBox::Save);
    layout->addLayout(actions);
    outer->addWidget(panel);

    const int result = dialog.exec();
    if (result == QDialog::Rejected || result == QMessageBox::NoButton)
        return buttons.testFlag(QMessageBox::Cancel) ? QMessageBox::Cancel : QMessageBox::NoButton;
    return QMessageBox::StandardButton(result);
}

QWidget *sliderField(QDoubleSpinBox *field, bool logarithmic = false)
{
    auto *container = new QWidget(field->parentWidget()); auto *layout = new QHBoxLayout(container); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(8);
    auto *slider = new SnapSlider(Qt::Horizontal, container); slider->setRange(0, 1000); slider->setMinimumWidth(180); field->setMinimumWidth(86); field->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    if (!field->objectName().isEmpty()) slider->setObjectName(field->objectName() + QStringLiteral("Slider"));
    const auto positionFor = [field, logarithmic](double value) {
        const double minimum = field->minimum(), maximum = field->maximum();
        if (logarithmic && minimum > 0) return qRound((std::log(value) - std::log(minimum)) / (std::log(maximum) - std::log(minimum)) * 1000);
        return qRound((value - minimum) / (maximum - minimum) * 1000);
    };
    const auto valueFor = [field, logarithmic](int position) {
        const double fraction = position / 1000.0, minimum = field->minimum(), maximum = field->maximum();
        return logarithmic && minimum > 0 ? std::exp(std::log(minimum) + fraction * (std::log(maximum) - std::log(minimum))) : minimum + fraction * (maximum - minimum);
    };
    slider->setValue(positionFor(field->value()));
    QObject::connect(slider, &QSlider::valueChanged, field, [field, valueFor](int position) { field->setValue(valueFor(position)); });
    QObject::connect(field, qOverload<double>(&QDoubleSpinBox::valueChanged), slider, [slider, positionFor](double value) { const QSignalBlocker blocker(slider); slider->setValue(positionFor(value)); });
    layout->addWidget(slider, 1); layout->addWidget(field); return container;
}

QWidget *sliderField(QSpinBox *field)
{
    auto *container = new QWidget(field->parentWidget()); auto *layout = new QHBoxLayout(container); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(8);
    auto *slider = new SnapSlider(Qt::Horizontal, container); slider->setRange(0, 1000); slider->setMinimumWidth(180); field->setMinimumWidth(76); field->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    if (!field->objectName().isEmpty()) slider->setObjectName(field->objectName() + QStringLiteral("Slider"));
    const auto positionFor = [field](int value) { return qRound((value - field->minimum()) / double(field->maximum() - field->minimum()) * 1000); };
    slider->setValue(positionFor(field->value()));
    QObject::connect(slider, &QSlider::valueChanged, field, [field](int position) { field->setValue(qRound(field->minimum() + position / 1000.0 * (field->maximum() - field->minimum()))); });
    QObject::connect(field, &QSpinBox::valueChanged, slider, [slider, positionFor](int value) { const QSignalBlocker blocker(slider); slider->setValue(positionFor(value)); });
    layout->addWidget(slider, 1); layout->addWidget(field); return container;
}

ScrubLabel *addScrubRow(QFormLayout *form, const QString &text, QDoubleSpinBox *field, bool logarithmic = false, bool includeSlider = true)
{
    const double sens = field->decimals() > 0 ? std::pow(10.0, -field->decimals()) : 1.0;
    const double stepVal = sens;
    field->setSingleStep(sens);
    auto *label = new ScrubLabel(text, field, sens, stepVal, form->parentWidget());
    if (!field->objectName().isEmpty()) {
        label->setObjectName(field->objectName() + QStringLiteral("Label"));
    }
    if (includeSlider) {
        form->addRow(label, sliderField(field, logarithmic));
    } else {
        form->addRow(label, field);
    }
    return label;
}

ScrubLabel *addScrubRow(QFormLayout *form, const QString &text, QSpinBox *field, bool includeSlider = true)
{
    auto *label = new ScrubLabel(text, field, 1.0, 1.0, form->parentWidget());
    if (!field->objectName().isEmpty()) {
        label->setObjectName(field->objectName() + QStringLiteral("Label"));
    }
    if (includeSlider) {
        form->addRow(label, sliderField(field));
    } else {
        form->addRow(label, field);
    }
    return label;
}

class HueSpectrumWidget final : public QWidget {
public:
    explicit HueSpectrumWidget(QWidget *parent = nullptr) : QWidget(parent) { setFixedHeight(58); setMinimumWidth(330); }
    HueSaturationSettings *settings = nullptr;
    std::function<void()> changed;

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (!settings) return;
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        const int slices = std::max(1, width());
        for (int x = 0; x < slices; ++x) {
            const double sourceHue = (x + .5) / slices * 360.0;
            double shift = 0;
            for (size_t i = 0; i < settings->adjustments.size(); ++i)
                shift += settings->adjustments[i].hue * settings->weight(ColorRange(i), sourceHue);
            const double shownHue = std::fmod(sourceHue + shift + 720.0, 360.0);
            painter.fillRect(QRect(x, 0, 2, 15), QColor::fromHslF(sourceHue / 360.0, 1, .5));
            painter.fillRect(QRect(x, 39, 2, 15), QColor::fromHslF(shownHue / 360.0, 1, .5));
        }
        const HueBand &band = settings->bands[size_t(settings->range)];
        const std::array<double,4> handles{band.falloffStart, band.rangeStart, band.rangeEnd, band.falloffEnd};
        painter.setPen(Qt::NoPen); painter.setBrush(palette().color(QPalette::Text));
        for (int i = 0; i < 4; ++i) {
            const double x = handles[size_t(i)] / 360.0 * width();
            if (i == 1 || i == 2) painter.drawRect(QRectF(x - 1, 18, 2, 17));
            else { QPolygonF mark; mark << QPointF(x, 21) << QPointF(x - 4, 27) << QPointF(x + 4, 27); painter.drawPolygon(mark); }
        }
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (!settings || settings->range == ColorRange::Master || event->button() != Qt::LeftButton) return;
        const double degrees = std::clamp(event->position().x() / std::max(1, width()) * 360.0, 0.0, 359.999);
        const HueBand &band = settings->bands[size_t(settings->range)];
        const std::array<double,4> handles{band.falloffStart, band.rangeStart, band.rangeEnd, band.falloffEnd};
        double nearest = 361; dragging_ = 0;
        for (int i = 0; i < 4; ++i) { const double gap = std::abs(handles[size_t(i)] - degrees); const double circular = std::min(gap, 360 - gap); if (circular < nearest) { nearest = circular; dragging_ = i; } }
        moveHandle(degrees); event->accept();
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (dragging_ < 0 || !(event->buttons() & Qt::LeftButton)) return;
        moveHandle(std::clamp(event->position().x() / std::max(1, width()) * 360.0, 0.0, 359.999)); event->accept();
    }
    void mouseReleaseEvent(QMouseEvent *) override { dragging_ = -1; }
private:
    void moveHandle(double degrees) { if (settings->bands[size_t(settings->range)].setHandle(dragging_, degrees)) { update(); if (changed) changed(); } }
    int dragging_ = -1;
};

class CurveEditorWidget final : public QWidget {
public:
    explicit CurveEditorWidget(QWidget *parent = nullptr) : QWidget(parent) { setMinimumSize(260, 260); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding); }
    CurvesSettings *settings = nullptr;
    std::function<void()> changed;
    std::function<void(int)> selectionChanged;
    [[nodiscard]] int selectedIndex() const { return selected_; }
    void clearSelection() { selected_ = dragging_ = -1; update(); if (selectionChanged) selectionChanged(-1); }
    void selectIndex(int index) { selected_ = index; update(); if (selectionChanged) selectionChanged(index); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this); painter.fillRect(rect(), QColor(18, 19, 22));
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(255, 255, 255, 30), 1));
        for (int i = 0; i <= 4; ++i) { const double fraction = i / 4.0; painter.drawLine(QPointF(fraction * width(), 0), QPointF(fraction * width(), height())); painter.drawLine(QPointF(0, fraction * height()), QPointF(width(), fraction * height())); }
        if (!settings) return;
        const int channel = std::clamp(settings->channel, 0, 3);
        QPainterPath path;
        for (int x = 0; x <= 255; ++x) { const QPointF point(x / 255.0 * width(), (1 - settings->value(x, channel) / 255.0) * height()); if (!x) path.moveTo(point); else path.lineTo(point); }
        painter.setPen(QPen(Qt::white, 2)); painter.setBrush(Qt::NoBrush); painter.drawPath(path);
        const auto &curve = settings->channels[size_t(channel)];
        painter.setPen(QPen(QColor(25, 25, 25), 1));
        for (int i = 0; i < curve.size(); ++i) { const QPointF point(curve[i].x / 255.0 * width(), (1 - curve[i].y / 255.0) * height()); painter.setBrush(i == selected_ ? palette().color(QPalette::Highlight) : Qt::white); painter.drawEllipse(point, 4, 4); }
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (!settings || event->button() != Qt::LeftButton) return;
        auto &curve = settings->channels[size_t(std::clamp(settings->channel, 0, 3))];
        const QPointF at = event->position(); double nearest = 1e9; int index = -1;
        for (int i = 0; i < curve.size(); ++i) { const QPointF point(curve[i].x / 255.0 * width(), (1 - curve[i].y / 255.0) * height()); const double distance = QLineF(point, at).length(); if (distance < nearest) { nearest = distance; index = i; } }
        const double x = std::clamp(at.x() / std::max(1, width()) * 255.0, 0.0, 255.0);
        const double y = std::clamp((1 - at.y() / std::max(1, height())) * 255.0, 0.0, 255.0);
        if (nearest >= 14) {
            if (curve.size() >= 32 || x <= 1 || x >= 254 || std::any_of(curve.cbegin(), curve.cend(), [x](const CurvePoint &point) { return std::abs(point.x - x) <= 1; })) return;
            curve.push_back({x, y}); std::sort(curve.begin(), curve.end(), [](const CurvePoint &a, const CurvePoint &b) { return a.x < b.x; });
            for (int i = 0; i < curve.size(); ++i) if (qFuzzyCompare(curve[i].x + 1, x + 1)) { index = i; break; }
        }
        dragging_ = selected_ = index; moveSelected(at); if (selectionChanged) selectionChanged(selected_); event->accept();
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (dragging_ < 0 || !(event->buttons() & Qt::LeftButton)) return;
        moveSelected(event->position()); event->accept();
    }
    void mouseReleaseEvent(QMouseEvent *) override { dragging_ = -1; }
private:
    void moveSelected(const QPointF &at)
    {
        if (!settings) return;
        auto &curve = settings->channels[size_t(std::clamp(settings->channel, 0, 3))];
        if (selected_ < 0 || selected_ >= curve.size()) return;
        const double x = std::clamp(at.x() / std::max(1, width()) * 255.0, 0.0, 255.0);
        curve[selected_].y = std::clamp((1 - at.y() / std::max(1, height())) * 255.0, 0.0, 255.0);
        if (selected_ > 0 && selected_ + 1 < curve.size()) curve[selected_].x = std::clamp(x, curve[selected_ - 1].x + 1, curve[selected_ + 1].x - 1);
        update(); if (selectionChanged) selectionChanged(selected_); if (changed) changed();
    }
    int selected_ = -1;
    int dragging_ = -1;
};

class LevelsHistogramWidget final : public QWidget {
public:
    explicit LevelsHistogramWidget(QWidget *parent = nullptr) : QWidget(parent) { setMinimumSize(256, 130); }
    const LevelsHistogram *histogram = nullptr;
    std::function<int()> channel;
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this); painter.fillRect(rect(), QColor(18, 19, 22)); if (!histogram || !channel) return;
        const int selected = std::clamp(channel(), 0, 3); const auto &bins = (*histogram)[size_t(selected)]; double peak = 0; for (double value : bins) peak = std::max(peak, value); if (peak <= 0) return;
        const QColor colors[] = {QColor(180, 184, 190), QColor(230, 70, 70), QColor(70, 210, 100), QColor(70, 130, 235)}; painter.setPen(Qt::NoPen); painter.setBrush(colors[selected]);
        for (int x = 0; x < 256; ++x) { const double height = std::clamp(bins[size_t(x)] / peak, 0.0, 1.0) * this->height(); painter.drawRect(QRectF(x * width() / 256.0, this->height() - height, width() / 256.0 + .5, height)); }
    }
};

class LevelsHandleWidget final : public QWidget {
public:
    explicit LevelsHandleWidget(bool output, QWidget *parent = nullptr) : QWidget(parent), output_(output) { setFixedHeight(output ? 34 : 20); setMinimumWidth(256); }
    LevelsSettings *settings = nullptr;
    std::function<int()> channel;
    std::function<void()> changed;
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing); if (!settings || !channel) return;
        if (output_) { QLinearGradient gradient(0, 0, width(), 0); gradient.setColorAt(0, Qt::black); gradient.setColorAt(1, Qt::white); painter.fillRect(QRect(0, 0, width(), 14), gradient); }
        const LevelRange &range = settings->ranges[size_t(std::clamp(channel(), 0, 3))]; const double gammaPosition = range.black + (range.white - range.black) * std::pow(.5, range.gamma);
        const std::array<double,3> input{range.black, gammaPosition, range.white}; const std::array<double,2> output{range.outputBlack, range.outputWhite}; const int count = output_ ? 2 : 3;
        for (int i = 0; i < count; ++i) { const double value = output_ ? output[size_t(i)] : input[size_t(i)]; const double x = value / 255.0 * width(); const double top = output_ ? 17 : 2; QPolygonF triangle; triangle << QPointF(x, top) << QPointF(x - 6, top + 11) << QPointF(x + 6, top + 11); painter.setPen(QPen(QColor(120, 120, 120), 1)); painter.setBrush(i == 0 ? Qt::black : i + 1 == count ? Qt::white : QColor(130, 130, 130)); painter.drawPolygon(triangle); }
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (!settings || !channel || event->button() != Qt::LeftButton) return;
        const LevelRange &range = settings->ranges[size_t(std::clamp(channel(), 0, 3))];
        const double gammaPosition = range.black + (range.white - range.black) * std::pow(.5, range.gamma); const std::array<double,3> input{range.black, gammaPosition, range.white}; const std::array<double,2> output{range.outputBlack, range.outputWhite}; const int count = output_ ? 2 : 3;
        double nearest = 1e9; dragging_ = 0; const double value = event->position().x() / std::max(1, width()) * 255.0;
        for (int i = 0; i < count; ++i) { const double distance = std::abs((output_ ? output[size_t(i)] : input[size_t(i)]) - value); if (distance < nearest) { nearest = distance; dragging_ = i; } }
        moveHandle(value); event->accept();
    }
    void mouseMoveEvent(QMouseEvent *event) override { if (dragging_ < 0 || !(event->buttons() & Qt::LeftButton)) return; moveHandle(event->position().x() / std::max(1, width()) * 255.0); event->accept(); }
    void mouseReleaseEvent(QMouseEvent *) override { dragging_ = -1; }
private:
    void moveHandle(double raw)
    {
        LevelRange &range = settings->ranges[size_t(std::clamp(channel(), 0, 3))]; const double value = std::clamp(raw, 0.0, 255.0);
        if (output_) { if (dragging_ == 0) range.outputBlack = std::round(value); else range.outputWhite = std::round(value); }
        else if (dragging_ == 0) range.black = std::min(range.white - 1, std::round(value));
        else if (dragging_ == 2) range.white = std::max(range.black + 1, std::round(value));
        else { const double fraction = std::clamp((value - range.black) / (range.white - range.black), .001, .999); range.gamma = std::log(fraction) / std::log(.5); }
        update(); if (changed) changed();
    }
    bool output_ = false;
    int dragging_ = -1;
};

QIcon editorIcon(int kind)
{
    return QIcon(new EditorIconEngine(kind));
}

QIcon tabCloseIcon()
{
    QIcon icon;
    const auto addPixmap = [&icon](const QColor &color, QIcon::Mode mode) {
        QPixmap pixmap(24, 24);
        pixmap.setDevicePixelRatio(2.0);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(color, 1.25, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(3.75, 3.75), QPointF(8.25, 8.25));
        painter.drawLine(QPointF(8.25, 3.75), QPointF(3.75, 8.25));
        icon.addPixmap(pixmap, mode);
    };
    addPixmap(QColor(166, 168, 173), QIcon::Normal);
    addPixmap(QColor(242, 243, 245), QIcon::Active);
    addPixmap(QColor(112, 113, 117), QIcon::Disabled);
    return icon;
}

QWidget *createTabCloseButton(QWidget *parent)
{
    auto *container = new QWidget(parent);
    container->setFixedSize(26, 20);
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 6, 0);
    layout->setSpacing(0);
    auto *closeButton = new QToolButton(container);
    closeButton->setObjectName(QStringLiteral("documentTabClose"));
    closeButton->setIcon(tabCloseIcon());
    closeButton->setIconSize(QSize(12, 12));
    closeButton->setFixedSize(20, 20);
    closeButton->setToolTip(QObject::tr("Close tab"));
    closeButton->setCursor(Qt::PointingHandCursor);
    closeButton->setAutoRaise(true);
    layout->addWidget(closeButton);
    return container;
}

class LayerDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override
    {
        if (index.data(LayerListModel::IsEffectRole).toBool()) {
            return {220, 24};
        }
        return {220, 54};
    }

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        auto *editor = new QLineEdit(parent); editor->setFrame(false); return editor;
    }
    void setEditorData(QWidget *widget, const QModelIndex &index) const override
    {
        if (auto *editor = qobject_cast<QLineEdit *>(widget)) { editor->setText(index.data(Qt::DisplayRole).toString()); editor->selectAll(); }
    }
    void setModelData(QWidget *widget, QAbstractItemModel *model, const QModelIndex &index) const override
    {
        if (auto *editor = qobject_cast<QLineEdit *>(widget)) model->setData(index, editor->text(), Qt::EditRole);
    }
    void updateEditorGeometry(QWidget *widget, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const int depth = std::min(index.data(Qt::UserRole + 1).toInt(), 8);
        widget->setGeometry(option.rect.adjusted(80 + depth * 18, 7, -7, -25));
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (index.data(LayerListModel::IsEffectRole).toBool()) {
            p->save();
            const QRect r = option.rect;
            if (option.state.testFlag(QStyle::State_Selected)) p->fillRect(r.adjusted(2, 1, -2, -1), QColor(64, 64, 64));
            const bool checked = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
            const int depth = index.data(Qt::UserRole + 1).toInt();
            const int indent = std::min(depth, 8) * 18;
            const int eyeX = r.left() + 38 + indent;

            p->setRenderHint(QPainter::Antialiasing);
            p->setPen(QPen(checked ? QColor(189, 195, 201) : QColor(102, 106, 111), 1.4));
            p->drawEllipse(QRectF(eyeX, r.center().y() - 4, 15, 8));
            if (checked) { p->setBrush(QColor(189, 195, 201)); p->drawEllipse(QPointF(eyeX + 7.5, r.center().y()), 2.1, 2.1); }

            const int textX = eyeX + 24;
            p->setPen(checked ? QColor(220, 222, 226) : QColor(140, 143, 148));
            QFont font = option.font; font.setPixelSize(11); p->setFont(font);
            p->drawText(QRect(textX, r.top(), r.right() - textX - 8, r.height()), Qt::AlignVCenter | Qt::AlignLeft, index.data().toString());
            p->restore();
            return;
        }

        p->save();
        const QRect r = option.rect;
        if (option.state.testFlag(QStyle::State_Selected)) p->fillRect(r.adjusted(2, 1, -2, -1), QColor(64, 64, 64));
        const bool checked = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(checked ? QColor(189, 195, 201) : QColor(102, 106, 111), 1.4));
        p->drawEllipse(QRectF(r.left() + 9, r.center().y() - 4, 15, 8));
        if (checked) { p->setBrush(QColor(189, 195, 201)); p->drawEllipse(QPointF(r.left() + 16.5, r.center().y()), 2.1, 2.1); }

        const int depth = index.data(Qt::UserRole + 1).toInt();
        const bool clipped = index.data(Qt::UserRole + 4).toBool();
        const bool group = index.data(Qt::UserRole + 2).toBool();
        const int indent = std::min(depth, 8) * 18;
        const int thumbX = r.left() + 34 + indent;
        if (group) {
            p->setPen(QColor(180, 184, 190));
            p->drawText(QRect(r.left() + 27 + indent, r.top(), 16, r.height()), Qt::AlignCenter,
                        index.data(Qt::UserRole + 5).toBool() ? QStringLiteral("⌄") : QStringLiteral("›"));
        } else if (clipped) { p->setPen(QColor(145, 190, 235)); p->drawText(QRect(r.left() + 27 + indent, r.top(), 18, r.height()), Qt::AlignCenter, QStringLiteral("↳")); }
        QRect thumb(thumbX, r.top() + 9, 36, 36);
        p->fillRect(thumb, QColor(54, 55, 57));
        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        if (!icon.isNull()) icon.paint(p, thumb.adjusted(1, 1, -1, -1), Qt::AlignCenter);
        p->setPen(QColor(92, 94, 98)); p->drawRect(thumb);

        const QImage mask = index.data(Qt::UserRole + 3).value<QImage>();
        int textStart = thumb.right() + 10;
        if (!mask.isNull()) {
            QRect maskRect(thumb.right() + 6, r.top() + 9, 36, 36);
            p->fillRect(maskRect, QColor(50, 50, 52));
            p->drawImage(maskRect, mask);
            p->setPen(QColor(92, 94, 98)); p->drawRect(maskRect);
            textStart = maskRect.right() + 8;
        }

        const int textX = textStart;
        const bool hasEffects = index.data(LayerListModel::HasEffectsRole).toBool();
        const int textRight = hasEffects ? r.right() - 36 : r.right() - 7;
        p->setPen(QColor(235, 237, 240));
        QFont mainFont = option.font; mainFont.setPixelSize(11); p->setFont(mainFont);
        p->drawText(QRect(textX, r.top() + 8, textRight - textX, 21), Qt::AlignVCenter | Qt::AlignLeft, index.data().toString());
        p->setPen(QColor(145, 148, 153));
        QFont smallFont = option.font; smallFont.setPixelSize(10); p->setFont(smallFont);
        p->drawText(QRect(textX, r.top() + 28, textRight - textX, 17), Qt::AlignVCenter | Qt::AlignLeft, index.data(Qt::UserRole).toString());

        if (hasEffects) {
            const bool expanded = index.data(LayerListModel::EffectsExpandedRole).toBool();
            p->setPen(QColor(160, 164, 170));
            QFont fxFont = option.font; fxFont.setPixelSize(10); p->setFont(fxFont);
            p->drawText(QRect(r.right() - 34, r.top() + 18, 30, 20), Qt::AlignCenter,
                        expanded ? QStringLiteral("fx ⌄") : QStringLiteral("fx ›"));
        }

        p->restore();
    }
};

QToolButton *toolButton(QWidget *parent, int icon, const QString &tip, bool selected = false)
{
    auto *button = new QToolButton(parent);
    button->setIcon(editorIcon(icon));
    button->setIconSize(QSize(22, 22));
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    button->setCheckable(true);
    button->setChecked(selected);
    button->setAutoExclusive(true);
    button->setFixedSize(34, 34);
    return button;
}

QDoubleSpinBox *numberField(QWidget *parent, const QString &label, double maximum = 1000000.0,
                            ScrubLabel **outLabel = nullptr, double sensitivity = 1.0,
                            std::optional<double> step = std::nullopt)
{
    auto *field = new QDoubleSpinBox(parent);
    if (!outLabel) {
        field->setPrefix(label + QStringLiteral("  "));
    }
    field->setRange(-maximum, maximum);
    field->setDecimals(0);
    field->setButtonSymbols(QAbstractSpinBox::NoButtons);
    field->setMinimumWidth(outLabel ? 54 : 84);
    field->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    field->setKeyboardTracking(false);
    if (outLabel) {
        field->setAccessibleName(label);
        *outLabel = new ScrubLabel(label, field, sensitivity, step, parent);
    }
    return field;
}

QJsonObject levelsAdjustment(const LevelsSettings &settings)
{
    return RasterOperations::levelsSettingsToJson(settings);
}

QJsonObject curvesAdjustment(const CurvesSettings &settings)
{
    return RasterOperations::curvesSettingsToJson(settings);
}

QJsonObject hueAdjustment(const HueSaturationSettings &settings)
{
    return RasterOperations::hueSaturationSettingsToJson(settings);
}

class PSDConversionDialog final : public QDialog {
public:
    PSDConversionDialog(const QString &fileName, const QVector<PSDConversion> &conversions, QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(QCoreApplication::translate("MainWindow", "Open “%1”?").arg(fileName));
        setMinimumSize(520, 360);
        auto *layout = new QVBoxLayout(this);
        layout->setSpacing(12);

        auto *title = new QLabel(QCoreApplication::translate("MainWindow", "Open “%1”?").arg(fileName), this);
        QFont f = title->font();
        f.setBold(true);
        f.setPointSize(f.pointSize() + 2);
        title->setFont(f);
        layout->addWidget(title);

        auto *subtitle = new QLabel(QCoreApplication::translate("MainWindow", "Compositor will convert these Photoshop features. Nothing is applied until you continue."), this);
        subtitle->setStyleSheet(QStringLiteral("color: #999;"));
        subtitle->setWordWrap(true);
        layout->addWidget(subtitle);

        auto *list = new QListWidget(this);
        list->setAlternatingRowColors(true);
        for (const auto &conv : conversions) {
            auto *item = new QListWidgetItem(list);
            auto *widget = new QWidget(list);
            auto *wLayout = new QVBoxLayout(widget);
            wLayout->setContentsMargins(6, 4, 6, 4);
            wLayout->setSpacing(2);

            auto *layerLabel = new QLabel(conv.layerName, widget);
            QFont lf = layerLabel->font();
            lf.setBold(true);
            layerLabel->setFont(lf);

            auto *msgLabel = new QLabel(conv.message, widget);
            msgLabel->setWordWrap(true);

            wLayout->addWidget(layerLabel);
            wLayout->addWidget(msgLabel);
            item->setSizeHint(widget->sizeHint());
            list->setItemWidget(item, widget);
        }
        layout->addWidget(list, 1);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
        auto *importBtn = buttons->addButton(QCoreApplication::translate("MainWindow", "Import"), QDialogButtonBox::AcceptRole);
        importBtn->setDefault(true);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
    }
};

} // namespace

void restoreLayer(Document *doc, EditorSession &session, const QUuid &target, const Layer &original)
{
    Q_UNUSED(doc);
    session.rollbackLayer(target, original);
}

void MainWindow::setMessageDialogHook(std::function<std::optional<QMessageBox::StandardButton>(const QString &title, const QString &text)> hook)
{
    sMessageDialogHook = std::move(hook);
}


MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    initCompositorResources();
    setAcceptDrops(true);
    setMinimumSize(900, 590);
    resize(1440, 860);
    setObjectName(QStringLiteral("editorWindow"));

    auto *shell = new QWidget(this);
    auto *shellLayout = new QVBoxLayout(shell);
    shellLayout->setContentsMargins(0, 0, 0, 0);
    shellLayout->setSpacing(0);

    auto *tabBar = new QWidget(shell);
    tabBar->setObjectName(QStringLiteral("tabBar"));
    tabBar->setFixedHeight(37);
    auto *tabLayout = new QHBoxLayout(tabBar);
    tabLayout->setContentsMargins(10, 4, 12, 4);
    tabLayout->setSpacing(5);
    menuRestoreButton_ = new QToolButton(tabBar);
    menuRestoreButton_->setText(QStringLiteral("≡"));
    menuRestoreButton_->setObjectName(QStringLiteral("menuRestoreButton"));
    menuRestoreButton_->setFixedSize(25, 25);
    updateMenuRestoreButton();
    tabLayout->addWidget(menuRestoreButton_);
    auto *newButton = new QToolButton(tabBar);
    newButton->setText(QStringLiteral("+"));
    newButton->setToolTip(tr("New canvas tab"));
    newButton->setObjectName(QStringLiteral("newTabButton"));
    newButton->setFixedSize(25, 25);
    tabLayout->addWidget(newButton);
    tabs_ = new QTabBar(tabBar); tabs_->setObjectName(QStringLiteral("documentTabs")); tabs_->setMovable(true); tabs_->setTabsClosable(false); tabs_->setExpanding(false);
    tabs_->setElideMode(Qt::ElideRight); tabs_->setUsesScrollButtons(true); tabs_->setDrawBase(false);
    tabs_->setAcceptDrops(true); tabs_->installEventFilter(this);
    newButton->setAcceptDrops(true); newButton->setProperty("newTabDropTarget", true); newButton->installEventFilter(this);
    tabs_->addTab(tr("Untitled")); installTabCloseButton(0); tabs_->setCurrentIndex(0); tabs_->setMinimumWidth(155); tabs_->setFixedHeight(29);
    workspaceTabs_.push_back(session_);
    tabRecoveryPaths_.push_back(newRecoveryPath());
    tabWatchers_.push_back(nullptr);
    tabDigests_.push_back(std::nullopt);
    tabPendingExternalChange_.push_back(false);
    tabPendingExternalDigest_.push_back(std::nullopt);
    tabPendingExternalDoc_.push_back(nullptr);
    tabLayout->addWidget(tabs_);
    tabLayout->addStretch();
    auto *fitTop = new QPushButton(tr("Fit"), tabBar); fitTop->setObjectName(QStringLiteral("toolbarPill"));
    auto *actualTop = new QPushButton(tr("100%"), tabBar); actualTop->setObjectName(QStringLiteral("toolbarPill"));
    auto *zoomOutTop = new QToolButton(tabBar); zoomOutTop->setIcon(editorIcon(13)); zoomOutTop->setObjectName(QStringLiteral("roundTool")); zoomOutTop->setToolTip(tr("Zoom out"));
    auto *zoomInTop = new QToolButton(tabBar); zoomInTop->setIcon(editorIcon(14)); zoomInTop->setObjectName(QStringLiteral("roundTool")); zoomInTop->setToolTip(tr("Zoom in"));
    fitTop->setFixedSize(52, 25); actualTop->setFixedSize(58, 25);
    zoomOutTop->setFixedSize(25, 25); zoomInTop->setFixedSize(25, 25);
    tabLayout->addWidget(fitTop); tabLayout->addWidget(actualTop); tabLayout->addSpacing(2); tabLayout->addWidget(zoomOutTop); tabLayout->addWidget(zoomInTop);
    shellLayout->addWidget(tabBar);

    auto *transformBar = new QWidget(shell);
    transformBar->setObjectName(QStringLiteral("transformBar"));
    transformBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *transformLayout = new ToolOptionsLayout(transformBar);
    auto *transformTitle = new QLabel(tr("Transform"), transformBar); transformTitle->setObjectName(QStringLiteral("sectionTitle"));
    smearMode_ = new SegmentedControl({tr("Liquify"), tr("Blur"), tr("Smudge")}, transformBar);
    smearMode_->setObjectName(QStringLiteral("smearMode"));
    smearMode_->setToolTip(tr("Liquify pushes pixels · Blur softens · Smudge drags color along"));
    smearMode_->setVisible(false);
    auto *smearMode = smearMode_;
    connect(smearMode_, &SegmentedControl::currentIndexChanged, this, [this](int) {
        updateBlurRadiusVisibility();
        updateSmearStatusHint();
    });
    auto *cloneAligned = new QCheckBox(tr("Aligned"), transformBar); cloneAligned->setChecked(true); cloneAligned->setVisible(false);
    auto *cloneSample = new SegmentedControl({tr("This Layer"), tr("All Layers")}, transformBar); cloneSample->setVisible(false);
    auto *healingMode = new SegmentedControl({tr("Content-Aware"), tr("Create Texture"), tr("Proximity Match")}, transformBar); healingMode->setVisible(false);
    auto *brushMode = new SegmentedControl({tr("Paint"), tr("Erase")}, transformBar); brushMode->setVisible(false);
    shapeRadiusField_ = numberField(transformBar, QStringLiteral("Radius"), 5000, &shapeRadiusLabel_, 1.0); shapeRadiusField_->setObjectName(QStringLiteral("shapeRadius")); shapeRadiusField_->setRange(0, 5000); shapeRadiusField_->setVisible(false);
    shapeRadiusLabel_->setObjectName(QStringLiteral("shapeRadiusLabel")); shapeRadiusLabel_->setVisible(false); shapeRadiusLabel_->setToolTip(tr("Corner Radius"));
    auto *shapeRadius = shapeRadiusField_;
    shapeLineWidthField_ = numberField(transformBar, QStringLiteral("Width"), 5000, &shapeLineWidthLabel_, 1.0); shapeLineWidthField_->setObjectName(QStringLiteral("shapeLineWidth")); shapeLineWidthField_->setRange(1, 5000); shapeLineWidthField_->setValue(2); shapeLineWidthField_->setVisible(false);
    shapeLineWidthLabel_->setObjectName(QStringLiteral("shapeLineWidthLabel")); shapeLineWidthLabel_->setVisible(false); shapeLineWidthLabel_->setToolTip(tr("Stroke Width"));
    auto *shapeLineWidth = shapeLineWidthField_;
    auto *shapeKind = new SegmentedControl({tr("Rectangle"), tr("Ellipse"), tr("Line")}, transformBar); shapeKind->setObjectName(QStringLiteral("shapeKind")); shapeKind->setVisible(false);
    auto *textFont = new QFontComboBox(transformBar); textFont->setObjectName(QStringLiteral("textFont")); textFont->setFixedWidth(170); textFont->setEditable(true); textFont->setInsertPolicy(QComboBox::NoInsert); textFont->setMaxVisibleItems(16); textFont->setToolTip(tr("Type to search or open the font list")); textFont->setVisible(false);
    if (textFont->completer()) { textFont->completer()->setCaseSensitivity(Qt::CaseInsensitive); textFont->completer()->setCompletionMode(QCompleter::PopupCompletion); }
    if (textFont->lineEdit()) { textFont->lineEdit()->setPlaceholderText(tr("Search fonts")); textFont->lineEdit()->setClearButtonEnabled(false); }
    textSizeField_ = new QSpinBox(transformBar); textSizeField_->setObjectName(QStringLiteral("textSize")); textSizeField_->setRange(4, 1000); textSizeField_->setValue(48); textSizeField_->setSuffix(tr(" px")); textSizeField_->setFixedWidth(88); textSizeField_->setVisible(false);
    textSizeLabel_ = new ScrubLabel(tr("Size"), textSizeField_, 1.0, 1.0, transformBar); textSizeLabel_->setObjectName(QStringLiteral("textSizeLabel")); textSizeLabel_->setVisible(false); textSizeLabel_->setToolTip(tr("Font Size"));
    auto *textSize = textSizeField_;
    textTrackingField_ = new QDoubleSpinBox(transformBar); textTrackingField_->setObjectName(QStringLiteral("textTracking")); textTrackingField_->setRange(-100.0, 1000.0); textTrackingField_->setValue(0.0); textTrackingField_->setDecimals(1); textTrackingField_->setSuffix(tr(" px")); textTrackingField_->setFixedWidth(88); textTrackingField_->setKeyboardTracking(false); textTrackingField_->setVisible(false);
    textTrackingLabel_ = new ScrubLabel(tr("Tracking"), textTrackingField_, 1.0, 1.0, transformBar); textTrackingLabel_->setObjectName(QStringLiteral("textTrackingLabel")); textTrackingLabel_->setVisible(false); textTrackingLabel_->setToolTip(tr("Tracking / Letter Spacing"));
    auto *textTracking = textTrackingField_;
    textLeadingField_ = new QDoubleSpinBox(transformBar); textLeadingField_->setObjectName(QStringLiteral("textLeading")); textLeadingField_->setRange(0.0, 1000.0); textLeadingField_->setValue(0.0); textLeadingField_->setDecimals(1); textLeadingField_->setSuffix(tr(" px")); textLeadingField_->setFixedWidth(88); textLeadingField_->setKeyboardTracking(false); textLeadingField_->setVisible(false);
    textLeadingLabel_ = new ScrubLabel(tr("Leading"), textLeadingField_, 1.0, 1.0, transformBar); textLeadingLabel_->setObjectName(QStringLiteral("textLeadingLabel")); textLeadingLabel_->setVisible(false); textLeadingLabel_->setToolTip(tr("Leading / Line Spacing"));
    auto *textLeading = textLeadingField_;
    auto *textBold = new QToolButton(); textBold->setObjectName(QStringLiteral("textBold")); textBold->setText(tr("B")); textBold->setCheckable(true); textBold->setToolTip(tr("Bold")); textBold->setFixedSize(32, 28);
    auto *textItalic = new QToolButton(); textItalic->setObjectName(QStringLiteral("textItalic")); textItalic->setText(tr("I")); textItalic->setCheckable(true); textItalic->setToolTip(tr("Italic")); textItalic->setFixedSize(32, 28);
    auto *textUnderline = new QToolButton(); textUnderline->setObjectName(QStringLiteral("textUnderline")); textUnderline->setText(tr("U")); textUnderline->setCheckable(true); textUnderline->setToolTip(tr("Underline")); textUnderline->setFixedSize(32, 28);
    auto *textStyleGroup = new SegmentedGroup(transformBar);
    textStyleGroup->addButton(textBold);
    textStyleGroup->addButton(textItalic);
    textStyleGroup->addButton(textUnderline);
    textStyleGroup->setVisible(false);
    auto *textAlignment = new SegmentedControl(transformBar); textAlignment->setObjectName(QStringLiteral("textAlignment"));
    textAlignment->addItem(editorIcon(24), tr("Align left"));
    textAlignment->addItem(editorIcon(25), tr("Align center"));
    textAlignment->addItem(editorIcon(26), tr("Align right"));
    textAlignment->setVisible(false);
    auto *textCancel = new QPushButton(tr("Cancel"), transformBar); textCancel->setObjectName(QStringLiteral("textCancel")); textCancel->setVisible(false);
    auto *textDone = new QPushButton(tr("Done"), transformBar); textDone->setObjectName(QStringLiteral("textDone")); textDone->setVisible(false);
    brushMode->setObjectName(QStringLiteral("brushMode")); shapeKind->setObjectName(QStringLiteral("shapeKind"));
    auto *gradientShape = new SegmentedControl({tr("Linear"), tr("Radial")}, transformBar); gradientShape->setVisible(false);
    auto *gradientStyle = new SegmentedControl(transformBar);
    gradientStyle->addItem(tr("To Transparent"), QIcon(), tr("Foreground to Transparent"));
    gradientStyle->addItem(tr("To Background"), QIcon(), tr("Foreground to Background"));
    gradientStyle->setVisible(false);
    auto *gradientReverse = new QCheckBox(tr("Reverse"), transformBar); gradientReverse->setVisible(false);
    gradientOpacityField_ = numberField(transformBar, QStringLiteral("Opacity"), 100, &gradientOpacityLabel_, 1.0); gradientOpacityField_->setObjectName(QStringLiteral("gradientOpacity")); gradientOpacityField_->setRange(1, 100); gradientOpacityField_->setValue(100); gradientOpacityField_->setSuffix(QStringLiteral(" %")); gradientOpacityField_->setVisible(false);
    gradientOpacityLabel_->setObjectName(QStringLiteral("gradientOpacityLabel")); gradientOpacityLabel_->setVisible(false); gradientOpacityLabel_->setToolTip(tr("Gradient Opacity"));
    auto *cropRatio = new SegmentedControl(transformBar);
    cropRatio->setObjectName(QStringLiteral("cropRatio"));
    cropRatio->addItem(tr("Free"), QIcon(), tr("Unconstrained"));
    cropRatio->addItem(tr("Original"), QIcon(), tr("Original Ratio"));
    cropRatio->addItem(tr("1:1"), QIcon(), tr("1:1 Square"));
    cropRatio->addItem(tr("4:3"), QIcon(), tr("4:3"));
    cropRatio->addItem(tr("3:4"), QIcon(), tr("3:4"));
    cropRatio->addItem(tr("16:9"), QIcon(), tr("16:9"));
    cropRatio->addItem(tr("9:16"), QIcon(), tr("9:16"));
    cropRatio->setVisible(false);
    auto *marqueeKind = new SegmentedControl({tr("Rectangle"), tr("Ellipse")}, transformBar); marqueeKind->setVisible(false);
    auto *lassoKind = new SegmentedControl({tr("Freehand"), tr("Polygonal")}, transformBar); lassoKind->setVisible(false);
    auto *selectionMode = new SegmentedControl(transformBar);
    selectionMode->addItem(tr("New"), editorIcon(27), tr("New selection"));
    selectionMode->addItem(tr("Add"), editorIcon(28), tr("Add to selection (Shift)"));
    selectionMode->addItem(tr("Subtract"), editorIcon(29), tr("Subtract from selection (Alt)"));
    selectionMode->setVisible(false);
    auto *selectionAntialias = new QCheckBox(tr("Anti-alias"), transformBar); selectionAntialias->setChecked(true); selectionAntialias->setVisible(false);
    wandToleranceField_ = new QSpinBox(transformBar); wandToleranceField_->setRange(0, 255); wandToleranceField_->setValue(32); wandToleranceField_->setObjectName(QStringLiteral("wandTolerance")); wandToleranceField_->setVisible(false);
    wandToleranceLabel_ = new ScrubLabel(tr("Tolerance"), wandToleranceField_, 1.0, 1.0, transformBar); wandToleranceLabel_->setObjectName(QStringLiteral("wandToleranceLabel")); wandToleranceLabel_->setVisible(false); wandToleranceLabel_->setToolTip(tr("Tolerance"));
    auto *wandTolerance = wandToleranceField_;
    auto *wandSampleSize = new SegmentedControl(transformBar);
    wandSampleSize->addItem(tr("Point"), QIcon(), tr("Point Sample"));
    wandSampleSize->addItem(tr("3×3"), QIcon(), tr("3 by 3 Average"));
    wandSampleSize->addItem(tr("5×5"), QIcon(), tr("5 by 5 Average"));
    wandSampleSize->setVisible(false);
    auto *wandModeControl = new SegmentedControl({tr("Wand"), tr("Object")}, transformBar); wandModeControl->setObjectName(QStringLiteral("wandModeControl")); wandModeControl->setVisible(false);
    objectEdgeOffsetField_ = new QSpinBox(transformBar); objectEdgeOffsetField_->setRange(-10, 10); objectEdgeOffsetField_->setValue(0); objectEdgeOffsetField_->setSuffix(tr(" px")); objectEdgeOffsetField_->setObjectName(QStringLiteral("objectEdgeOffset")); objectEdgeOffsetField_->setVisible(false);
    objectEdgeOffsetLabel_ = new ScrubLabel(tr("Edge Offset"), objectEdgeOffsetField_, 1.0, 1.0, transformBar); objectEdgeOffsetLabel_->setObjectName(QStringLiteral("objectEdgeOffsetLabel")); objectEdgeOffsetLabel_->setVisible(false); objectEdgeOffsetLabel_->setToolTip(tr("Edge Offset"));
    auto *objectEdgeOffset = objectEdgeOffsetField_;
    auto *objectSmoothEdges = new QCheckBox(tr("Smooth"), transformBar); objectSmoothEdges->setChecked(true); objectSmoothEdges->setObjectName(QStringLiteral("objectSmoothEdges")); objectSmoothEdges->setVisible(false);
    auto *wandSample = new SegmentedControl({tr("This Layer"), tr("All Layers")}, transformBar); wandSample->setVisible(false);
    auto *wandContiguous = new QCheckBox(tr("Contiguous"), transformBar); wandContiguous->setChecked(true); wandContiguous->setVisible(false);
    selectionAmountField_ = new QSpinBox(transformBar); selectionAmountField_->setRange(1, 500); selectionAmountField_->setValue(1); selectionAmountField_->setSuffix(tr(" px")); selectionAmountField_->setObjectName(QStringLiteral("selectionAmount")); selectionAmountField_->setVisible(false);
    selectionAmountLabel_ = new ScrubLabel(tr("Amount"), selectionAmountField_, 1.0, 1.0, transformBar); selectionAmountLabel_->setObjectName(QStringLiteral("selectionAmountLabel")); selectionAmountLabel_->setVisible(false); selectionAmountLabel_->setToolTip(tr("Amount"));
    auto *selectionAmount = selectionAmountField_;
    auto *expandSelection = new QPushButton(tr("Expand")); expandSelection->setObjectName(QStringLiteral("expandSelection"));
    auto *contractSelection = new QPushButton(tr("Contract")); contractSelection->setObjectName(QStringLiteral("contractSelection"));
    auto *featherSelection = new QPushButton(tr("Feather")); featherSelection->setObjectName(QStringLiteral("featherSelection"));
    auto *selectionModifyGroup = new SegmentedGroup(transformBar);
    selectionModifyGroup->addButton(expandSelection);
    selectionModifyGroup->addButton(contractSelection);
    selectionModifyGroup->addButton(featherSelection);
    selectionModifyGroup->setVisible(false);
    marqueeKind->setObjectName(QStringLiteral("marqueeKind")); lassoKind->setObjectName(QStringLiteral("lassoKind"));
    selectionMode->setObjectName(QStringLiteral("selectionMode")); selectionAntialias->setObjectName(QStringLiteral("selectionAntialias"));
    wandTolerance->setObjectName(QStringLiteral("wandTolerance")); wandSampleSize->setObjectName(QStringLiteral("wandSampleSize"));
    wandSample->setObjectName(QStringLiteral("wandSample")); wandContiguous->setObjectName(QStringLiteral("wandContiguous"));
    brushSizeField_ = numberField(transformBar, QStringLiteral("Size"), 2000, &brushSizeLabel_, 1.0);
    brushSizeField_->setRange(1, 2000); brushSizeField_->setValue(brushDiameter_); brushSizeField_->setSuffix(QStringLiteral(" px"));
    brushSizeField_->setVisible(false); brushSizeLabel_->setVisible(false);
    brushHardnessField_ = numberField(transformBar, QStringLiteral("Hardness"), 100, &brushHardnessLabel_, 1.0);
    brushHardnessField_->setRange(0, 100); brushHardnessField_->setValue(100); brushHardnessField_->setSuffix(QStringLiteral(" %"));
    brushHardnessField_->setVisible(false); brushHardnessLabel_->setVisible(false);
    brushOpacityField_ = numberField(transformBar, QStringLiteral("Opacity"), 100, &brushOpacityLabel_, 1.0);
    brushOpacityField_->setRange(1, 100); brushOpacityField_->setValue(100); brushOpacityField_->setSuffix(QStringLiteral(" %"));
    brushOpacityField_->setVisible(false); brushOpacityLabel_->setVisible(false);
    blurRadiusField_ = numberField(transformBar, QStringLiteral("Radius"), 50, &blurRadiusLabel_, 0.5);
    blurRadiusField_->setRange(0.5, 50); blurRadiusField_->setDecimals(1); blurRadiusField_->setValue(blurRadius_); blurRadiusField_->setSuffix(QStringLiteral(" px"));
    blurRadiusField_->setObjectName(QStringLiteral("blurRadius")); blurRadiusLabel_->setObjectName(QStringLiteral("blurRadiusLabel"));
    blurRadiusLabel_->setToolTip(tr("How far the blur softens, in pixels")); blurRadiusField_->setVisible(false); blurRadiusLabel_->setVisible(false);
    brushSmoothingField_ = numberField(transformBar, QStringLiteral("Smoothing"), 100, &brushSmoothingLabel_, 1.0);
    brushSmoothingField_->setRange(0, 100); brushSmoothingField_->setValue(0); brushSmoothingField_->setSuffix(QStringLiteral(" %"));
    brushSmoothingField_->setVisible(false); brushSmoothingLabel_->setVisible(false);
    brushSizeField_->setObjectName(QStringLiteral("brushSize")); brushHardnessField_->setObjectName(QStringLiteral("brushHardness"));
    brushOpacityField_->setObjectName(QStringLiteral("brushOpacity")); brushSmoothingField_->setObjectName(QStringLiteral("brushSmoothing"));
    brushSizeLabel_->setObjectName(QStringLiteral("brushSizeLabel")); brushHardnessLabel_->setObjectName(QStringLiteral("brushHardnessLabel"));
    brushOpacityLabel_->setObjectName(QStringLiteral("brushOpacityLabel")); brushSmoothingLabel_->setObjectName(QStringLiteral("brushSmoothingLabel"));
    brushSizeLabel_->setToolTip(tr("Brush Size")); brushHardnessLabel_->setToolTip(tr("Brush Hardness"));
    brushOpacityLabel_->setToolTip(tr("Brush Opacity")); brushSmoothingLabel_->setToolTip(tr("Brush Smoothing"));
    auto *autoSelect = new QCheckBox(tr("Auto Select"), transformBar); autoSelect->setObjectName(QStringLiteral("transformAutoSelect"));
    showTransformControls_ = new QCheckBox(tr("Show Controls"), transformBar); showTransformControls_->setObjectName(QStringLiteral("transformShowControls")); showTransformControls_->setChecked(true);
    xField_ = numberField(transformBar, QStringLiteral("X"), 1000000.0, &xLabel_, 1.0, 1.0);
    yField_ = numberField(transformBar, QStringLiteral("Y"), 1000000.0, &yLabel_, 1.0, 1.0);
    widthField_ = numberField(transformBar, QStringLiteral("W"), 1000000.0, &widthLabel_, 1.0, 1.0);
    heightField_ = numberField(transformBar, QStringLiteral("H"), 1000000.0, &heightLabel_, 1.0, 1.0);
    xField_->setObjectName(QStringLiteral("transformX")); yField_->setObjectName(QStringLiteral("transformY"));
    widthField_->setObjectName(QStringLiteral("transformWidth")); heightField_->setObjectName(QStringLiteral("transformHeight"));
    xLabel_->setObjectName(QStringLiteral("transformXLabel")); yLabel_->setObjectName(QStringLiteral("transformYLabel"));
    widthLabel_->setObjectName(QStringLiteral("transformWidthLabel")); heightLabel_->setObjectName(QStringLiteral("transformHeightLabel"));
    xLabel_->setToolTip(tr("Horizontal position in canvas pixels")); yLabel_->setToolTip(tr("Vertical position in canvas pixels"));
    widthLabel_->setToolTip(tr("Layer width in pixels")); heightLabel_->setToolTip(tr("Layer height in pixels"));
    xField_->setFixedWidth(64); yField_->setFixedWidth(64); widthField_->setFixedWidth(68); heightField_->setFixedWidth(68);
    auto *link = new QToolButton(transformBar); link->setObjectName(QStringLiteral("transformRatioLock")); link->setIcon(editorIcon(16)); link->setIconSize(QSize(18, 18)); link->setCheckable(true); link->setChecked(true); link->setToolTip(tr("Keep width and height proportional")); link->setFixedSize(29, 29);
    scaleField_ = numberField(transformBar, QStringLiteral("Scale"), 3200); scaleField_->setObjectName(QStringLiteral("transformScale")); scaleField_->setSuffix(QStringLiteral(" %")); scaleField_->setRange(0.1, 3200); scaleField_->setValue(100); scaleField_->setFixedWidth(105);
    rotationField_ = numberField(transformBar, QStringLiteral("°"), 360, &rotationLabel_, 1.0, 1.0); rotationField_->setObjectName(QStringLiteral("transformRotation")); rotationField_->setRange(-360, 360); rotationField_->setFixedWidth(64); rotationField_->setToolTip(tr("Rotation"));
    rotationLabel_->setObjectName(QStringLiteral("transformRotationLabel")); rotationLabel_->setToolTip(tr("Rotation"));
    sampling_ = new SegmentedControl({tr("High quality"), tr("Smooth"), tr("Nearest")}, transformBar); sampling_->setObjectName(QStringLiteral("transformSampling")); sampling_->setToolTip(tr("Resampling quality"));
    auto *flipH = new QPushButton(tr("Flip H")); flipH->setObjectName(QStringLiteral("transformFlipHorizontal")); flipH->setFixedWidth(57); flipH->setToolTip(tr("Flip horizontally"));
    auto *flipV = new QPushButton(tr("Flip V")); flipV->setObjectName(QStringLiteral("transformFlipVertical")); flipV->setFixedWidth(57); flipV->setToolTip(tr("Flip vertically"));
    auto *flipGroup = new SegmentedGroup(transformBar);
    flipGroup->addButton(flipH);
    flipGroup->addButton(flipV);
    transformCancel_ = new QPushButton(tr("Cancel"), transformBar); transformCancel_->setObjectName(QStringLiteral("transformCancel")); transformCancel_->setEnabled(false);
    transformApply_ = new QPushButton(tr("Apply"), transformBar); transformApply_->setObjectName(QStringLiteral("primaryButton")); transformApply_->setEnabled(false);
    transformLayout->addGroup({transformTitle});
    transformLayout->addGroup({autoSelect, showTransformControls_});
    transformLayout->addGroup({xLabel_, xField_, yLabel_, yField_});
    transformLayout->addGroup({widthLabel_, widthField_, link, heightLabel_, heightField_});
    transformLayout->addGroup({scaleField_, rotationLabel_, rotationField_});
    transformLayout->addGroup({flipGroup});
    transformLayout->addGroup({sampling_});
    transformLayout->addGroup({brushMode, smearMode, healingMode, cropRatio});
    transformLayout->addGroup({shapeKind});
    transformLayout->addGroup({shapeRadiusLabel_, shapeRadiusField_, shapeLineWidthLabel_, shapeLineWidthField_});
    transformLayout->addGroup({brushSizeLabel_, brushSizeField_, brushHardnessLabel_, brushHardnessField_,
                               brushOpacityLabel_, brushOpacityField_, blurRadiusLabel_, blurRadiusField_, brushSmoothingLabel_, brushSmoothingField_});
    transformLayout->addGroup({cloneAligned});
    transformLayout->addGroup({cloneSample});
    transformLayout->addGroup({textFont, textSizeLabel_, textSizeField_, textTrackingLabel_, textTrackingField_, textLeadingLabel_, textLeadingField_});
    textFont->setFixedWidth(210); textSize->setFixedWidth(70); textTracking->setFixedWidth(70); textLeading->setFixedWidth(70);
    transformLayout->addGroup({textStyleGroup});
    transformLayout->addGroup({textAlignment});
    transformLayout->addGroup({gradientShape});
    transformLayout->addGroup({gradientStyle});
    transformLayout->addGroup({gradientOpacityLabel_, gradientOpacityField_, gradientReverse});
    transformLayout->addGroup({marqueeKind});
    transformLayout->addGroup({lassoKind});
    transformLayout->addGroup({selectionMode});
    transformLayout->addGroup({wandModeControl});
    transformLayout->addGroup({wandToleranceLabel_, wandToleranceField_, objectEdgeOffsetLabel_, objectEdgeOffsetField_});
    transformLayout->addGroup({selectionAntialias, wandContiguous, objectSmoothEdges});
    transformLayout->addGroup({wandSampleSize});
    transformLayout->addGroup({wandSample});
    transformLayout->addGroup({selectionAmountLabel_, selectionAmountField_, selectionModifyGroup});
    transformLayout->addGroup({textCancel, textDone, transformCancel_, transformApply_});
    textDone->setProperty("primary", true);
    textDone->setToolTip(tr("Commit text (Ctrl+Enter)")); textCancel->setToolTip(tr("Discard text edits (Esc)"));
    textDone->setEnabled(false); textCancel->setEnabled(false);
    textSize->setKeyboardTracking(false);
    rotationField_->setPrefix(QString()); rotationField_->setSuffix(QStringLiteral("°")); rotationField_->setDecimals(1);
    scaleField_->setDecimals(1);
    widthField_->setMinimum(1); heightField_->setMinimum(1);
    xField_->setToolTip(tr("Horizontal position in canvas pixels")); yField_->setToolTip(tr("Vertical position in canvas pixels"));
    widthField_->setToolTip(tr("Layer width in pixels")); heightField_->setToolTip(tr("Layer height in pixels"));
    selectionAmount->setToolTip(tr("Amount to expand or contract the selection"));
    textAlignment->setToolTip(tr("Paragraph alignment (applies to the whole text layer)"));
    for (int i = 0; i < transformLayout->count(); ++i) {
        QWidget *control = transformLayout->itemAt(i)->widget();
        control->setFixedHeight(28);
        if (auto *field = qobject_cast<QDoubleSpinBox *>(control)) {
            field->setAccessibleName(field->prefix().trimmed());
        }
        // Fixed widths predate the themed padding and arrows. Let Qt measure
        // the complete control, including every option or the numeric range.
        if (auto *combo = qobject_cast<QComboBox *>(control)) combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        if (qobject_cast<QComboBox *>(control) || qobject_cast<QAbstractSpinBox *>(control) || qobject_cast<QPushButton *>(control) || qobject_cast<SegmentedGroup *>(control)) {
            control->setMinimumWidth(0);
            control->setMaximumWidth(QWIDGETSIZE_MAX);
            control->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        }
        if (auto *button = qobject_cast<QAbstractButton *>(control)) button->setFocusPolicy(Qt::TabFocus);
    }
    shellLayout->addWidget(transformBar);

    auto *workspace = new QWidget(shell);
    auto *workspaceLayout = new QHBoxLayout(workspace);
    workspaceLayout->setContentsMargins(0, 0, 0, 0);
    workspaceLayout->setSpacing(0);

    auto *rail = new QWidget(workspace); rail->setObjectName(QStringLiteral("toolRail")); rail->setFixedWidth(52);
    auto *railLayout = new QVBoxLayout(rail); railLayout->setContentsMargins(0, 10, 0, 9); railLayout->setSpacing(5);
    const std::array<const char *, 15> tips = {"Move", "Marquee", "Lasso", "Magic Wand", "Crop", "Brush", "Healing", "Clone Stamp", "Blur", "Gradient", "Shape", "Text", "Eyedropper", "Hand", "Zoom"};
    const std::array<int, 15> iconKinds = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 11, 12, 23};
    QVector<QToolButton *> toolButtons;
    for (int i = 0; i < int(tips.size()); ++i) {
        if (i == 5 || i == 12) railLayout->addSpacing(4);
        auto *button = toolButton(rail, iconKinds[size_t(i)], tr(tips[size_t(i)]), i == 0);
        toolButtons.push_back(button);
        connect(button, &QToolButton::clicked, this, [this, i] {
            const CanvasWidget::Tool tool = i == 0 ? CanvasWidget::Tool::Move : i == 1 ? CanvasWidget::Tool::Marquee
                : i == 2 ? CanvasWidget::Tool::Lasso : i == 3 ? CanvasWidget::Tool::Wand : i == 4 ? CanvasWidget::Tool::Crop : i == 5 ? CanvasWidget::Tool::Brush
                : i == 6 ? CanvasWidget::Tool::Healing : i == 7 ? CanvasWidget::Tool::Clone : i == 8 ? CanvasWidget::Tool::Blur
                : i == 9 ? CanvasWidget::Tool::Gradient : i == 10 ? CanvasWidget::Tool::Shape : i == 11 ? CanvasWidget::Tool::Text : i == 12 ? CanvasWidget::Tool::Eyedropper : i == 13 ? CanvasWidget::Tool::Hand
                : i == 14 ? CanvasWidget::Tool::Zoom : CanvasWidget::Tool::Other;
            if (tool == CanvasWidget::Tool::Blur && canvas_->tool() == CanvasWidget::Tool::Blur) {
                cycleSmearMode();
            } else {
                canvas_->setTool(tool);
            }
        });
        railLayout->addWidget(button, 0, Qt::AlignHCenter);
    }
    railLayout->addStretch();
    auto *colors = new QWidget(rail); colors->setFixedSize(42, 43);
    foregroundSwatch_ = new QLabel(colors); foregroundSwatch_->setGeometry(4, 3, 22, 22); foregroundSwatch_->setStyleSheet(QStringLiteral("background:#1686e8;border:1px solid white;border-radius:4px;"));
    backgroundSwatch_ = new QLabel(colors); backgroundSwatch_->setGeometry(16, 17, 22, 22); backgroundSwatch_->setStyleSheet(QStringLiteral("background:#1f0f0b;border:1px solid white;border-radius:4px;"));
    foregroundSwatch_->setObjectName(QStringLiteral("foregroundSwatch"));
    backgroundSwatch_->setObjectName(QStringLiteral("backgroundSwatch"));
    foregroundSwatch_->installEventFilter(this); backgroundSwatch_->installEventFilter(this);
    foregroundSwatch_->raise(); railLayout->addWidget(colors, 0, Qt::AlignHCenter);
    workspaceLayout->addWidget(rail);

    canvas_ = new CanvasWidget(workspace);
    canvas_->setEditorSession(&session_);
    connect(autoSelect, &QCheckBox::toggled, canvas_, &CanvasWidget::setTransformAutoSelect);
    connect(link, &QToolButton::toggled, canvas_, &CanvasWidget::setLockTransformRatio);
    connect(showTransformControls_, &QCheckBox::toggled, canvas_, &CanvasWidget::setShowTransformControls);
    const auto canvasPending = [this](bool pending) { canvasPendingTransform_ = pending; updateTransformButtons(); };
    connect(canvas_, &CanvasWidget::pendingDistortionChanged, this, canvasPending);
    connect(canvas_, &CanvasWidget::pendingCropChanged, this, canvasPending);
    connect(transformCancel_, &QPushButton::clicked, this, [this] { if (transformOriginalDocument_) finishPersistentTransform(false); else { canvas_->resolvePendingDistortion(false); canvas_->resolvePendingCrop(false); } });
    connect(transformApply_, &QPushButton::clicked, this, [this] { if (transformOriginalDocument_) finishPersistentTransform(true); else { canvas_->resolvePendingDistortion(true); canvas_->resolvePendingCrop(true); } });
    connect(canvas_, &CanvasWidget::transformCancelRequested, this, [this] { finishPersistentTransform(false); });
    connect(canvas_, &CanvasWidget::transformApplyRequested, this, [this] { finishPersistentTransform(true); });
    connect(shapeRadius, qOverload<double>(&QDoubleSpinBox::valueChanged), canvas_, &CanvasWidget::setShapeCornerRadius);
    connect(shapeLineWidth, qOverload<double>(&QDoubleSpinBox::valueChanged), canvas_, &CanvasWidget::setShapeLineWidth);
    connect(brushSizeField_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) { brushDiameter_ = value; canvas_->setBrushDiameter(value); });
    connect(brushHardnessField_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) { brushHardness_ = value / 100.0; });
    connect(brushOpacityField_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) { brushOpacity_ = value / 100.0; });
    connect(blurRadiusField_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) { blurRadius_ = value; });
    connect(brushSmoothingField_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        brushSmoothing_ = value;
        session_.setBrushSmoothing(value);
        canvas_->setBrushSmoothing(value);
    });
    connect(brushMode, &SegmentedControl::currentIndexChanged, canvas_, [this](int index) { canvas_->setTool(index == 1 ? CanvasWidget::Tool::Eraser : CanvasWidget::Tool::Brush); });
    connect(shapeKind, &SegmentedControl::currentIndexChanged, canvas_, [this, shapeRadius, shapeLineWidth](int index) {
        const ShapeKind kind = (index == 2 ? ShapeKind::Line : index == 1 ? ShapeKind::Ellipse : ShapeKind::Rectangle);
        canvas_->setShapeKind(kind);
        const bool isShape = canvas_->tool() == CanvasWidget::Tool::Shape;
        shapeRadius->setVisible(isShape && kind == ShapeKind::Rectangle);
        shapeRadiusLabel_->setVisible(isShape && kind == ShapeKind::Rectangle);
        shapeLineWidth->setVisible(isShape && kind == ShapeKind::Line);
        shapeLineWidthLabel_->setVisible(isShape && kind == ShapeKind::Line);
    });
    connect(canvas_, &CanvasWidget::shapeKindChanged, shapeKind, [shapeKind, shapeRadius, shapeLineWidth, this](ShapeKind kind) {
        shapeKind->setCurrentIndex(kind == ShapeKind::Line ? 2 : kind == ShapeKind::Ellipse ? 1 : 0);
        const bool isShape = canvas_->tool() == CanvasWidget::Tool::Shape;
        shapeRadius->setVisible(isShape && kind == ShapeKind::Rectangle);
        shapeRadiusLabel_->setVisible(isShape && kind == ShapeKind::Rectangle);
        shapeLineWidth->setVisible(isShape && kind == ShapeKind::Line);
        shapeLineWidthLabel_->setVisible(isShape && kind == ShapeKind::Line);
    });
    connect(marqueeKind, &SegmentedControl::currentIndexChanged, canvas_, [this](int index) { canvas_->setMarqueeElliptical(index == 1); });
    connect(lassoKind, &SegmentedControl::currentIndexChanged, canvas_, [this](int index) { canvas_->setPolygonalLasso(index == 1); });
    connect(canvas_, &CanvasWidget::marqueeKindChanged, marqueeKind, [marqueeKind](bool elliptical) { marqueeKind->setCurrentIndex(elliptical ? 1 : 0); });
    connect(canvas_, &CanvasWidget::lassoKindChanged, lassoKind, [lassoKind](bool polygonal) { lassoKind->setCurrentIndex(polygonal ? 1 : 0); });
    connect(selectionMode, &SegmentedControl::currentIndexChanged, canvas_, &CanvasWidget::setSelectionMode);
    connect(selectionAntialias, &QCheckBox::toggled, canvas_, &CanvasWidget::setSelectionAntialiased);
    connect(expandSelection, &QPushButton::clicked, this, [this, selectionAmount] { if (session_.expandSelection(selectionAmount->value())) syncDocumentViews(false); });
    connect(contractSelection, &QPushButton::clicked, this, [this, selectionAmount] { if (session_.contractSelection(selectionAmount->value())) syncDocumentViews(false); });
    connect(featherSelection, &QPushButton::clicked, this, [this, selectionAmount] {
        const int amount = std::clamp(selectionAmount->value(), 1, 250);
        if (session_.featherSelection(amount)) {
            session_.setSelectionFeatherAmount(amount);
            syncDocumentViews(false);
        }
    });
    connect(wandModeControl, &SegmentedControl::currentIndexChanged, canvas_, [this](int index) {
        canvas_->setWandMode(index == 1 ? CanvasWidget::WandMode::Object : CanvasWidget::WandMode::Wand);
    });
    connect(canvas_, &CanvasWidget::wandModeChanged, wandModeControl, [wandModeControl, wandTolerance, wandSampleSize, wandContiguous, objectEdgeOffset, objectSmoothEdges, this](CanvasWidget::WandMode mode) {
        const bool isObject = (mode == CanvasWidget::WandMode::Object);
        {
            const QSignalBlocker blocker(wandModeControl);
            wandModeControl->setCurrentIndex(isObject ? 1 : 0);
        }
        const bool isWandTool = (canvas_->tool() == CanvasWidget::Tool::Wand);
        wandTolerance->setVisible(isWandTool && !isObject);
        wandToleranceLabel_->setVisible(isWandTool && !isObject);
        wandSampleSize->setVisible(isWandTool && !isObject);
        wandContiguous->setVisible(isWandTool && !isObject);
        objectEdgeOffset->setVisible(isWandTool && isObject);
        objectEdgeOffsetLabel_->setVisible(isWandTool && isObject);
        objectSmoothEdges->setVisible(isWandTool && isObject);
    });
    connect(cropRatio, &SegmentedControl::currentIndexChanged, this, [this](int index) {
        const double ratios[] = {0, document_ ? double(document_->canvasSize.width()) / document_->canvasSize.height() : 0, 1, 4.0/3.0, 3.0/4.0, 16.0/9.0, 9.0/16.0};
        canvas_->setCropRatio(ratios[std::clamp(index, 0, 6)]);
    });
    const std::array<QWidget *, 16> moveControls{autoSelect, showTransformControls_,
                                                xLabel_, xField_, yLabel_, yField_,
                                                widthLabel_, widthField_, link,
                                                heightLabel_, heightField_,
                                                scaleField_, rotationLabel_, rotationField_,
                                                sampling_, flipGroup};
    connect(canvas_, &CanvasWidget::toolChanged, this, [this, transformTitle, cropRatio, smearMode, cloneAligned, cloneSample,
            healingMode, brushMode, shapeKind, shapeRadius, shapeLineWidth, textFont, textSize, textTracking, textLeading, textStyleGroup, textAlignment, textCancel, textDone, gradientShape, gradientStyle, gradientReverse, marqueeKind, lassoKind,
            selectionMode, selectionAntialias, wandModeControl, wandTolerance, wandSampleSize, wandSample, wandContiguous, objectEdgeOffset, objectSmoothEdges, selectionModifyGroup,
            selectionAmount, moveControls, toolButtons](CanvasWidget::Tool tool) {
        if (tool != CanvasWidget::Tool::Text)
            if (auto *editor = dynamic_cast<InlineTextEditor *>(canvas_->findChild<QTextEdit *>(QStringLiteral("inlineTextEditor"), Qt::FindDirectChildrenOnly))) editor->finish(true);
        if (tool != CanvasWidget::Tool::Move && transformOriginalDocument_) finishPersistentTransform(true);
        const bool brush = tool == CanvasWidget::Tool::Brush || tool == CanvasWidget::Tool::Eraser || tool == CanvasWidget::Tool::Healing
            || tool == CanvasWidget::Tool::Clone || tool == CanvasWidget::Tool::Blur;
        const bool move = tool == CanvasWidget::Tool::Move;
        const bool selection = tool == CanvasWidget::Tool::Marquee || tool == CanvasWidget::Tool::Lasso || tool == CanvasWidget::Tool::Wand;
        for (QWidget *control : moveControls) control->setVisible(move);
        brushSizeLabel_->setVisible(brush); brushSizeField_->setVisible(brush);
        brushHardnessLabel_->setVisible(brush); brushHardnessField_->setVisible(brush);
        brushOpacityLabel_->setVisible(brush); brushOpacityField_->setVisible(brush);
        brushOpacityLabel_->setText(tool == CanvasWidget::Tool::Blur ? tr("Strength") : tr("Opacity"));
        brushOpacityLabel_->setToolTip(tool == CanvasWidget::Tool::Blur ? tr("Strength") : tr("Brush Opacity"));
        if (tool == CanvasWidget::Tool::Blur) {
            updateSmearStatusHint();
        }
        updateBlurRadiusVisibility();
        const bool smoothingVisible = (tool == CanvasWidget::Tool::Brush || tool == CanvasWidget::Tool::Eraser);
        brushSmoothingLabel_->setVisible(smoothingVisible);
        brushSmoothingField_->setVisible(smoothingVisible);
        brushMode->setVisible(tool == CanvasWidget::Tool::Brush || tool == CanvasWidget::Tool::Eraser);
        if (tool == CanvasWidget::Tool::Brush || tool == CanvasWidget::Tool::Eraser) { const QSignalBlocker blocker(brushMode); brushMode->setCurrentIndex(tool == CanvasWidget::Tool::Eraser ? 1 : 0); }
        cropRatio->setVisible(tool == CanvasWidget::Tool::Crop);
        if (tool == CanvasWidget::Tool::Crop && !canvas_->cropRect().has_value()) {
            const QSignalBlocker blocker(cropRatio);
            cropRatio->setCurrentIndex(0);
            canvas_->setCropRatio(0);
        }
        smearMode->setVisible(tool == CanvasWidget::Tool::Blur);
        cloneAligned->setVisible(tool == CanvasWidget::Tool::Clone); cloneSample->setVisible(tool == CanvasWidget::Tool::Clone);
        healingMode->setVisible(tool == CanvasWidget::Tool::Healing);
        shapeKind->setVisible(tool == CanvasWidget::Tool::Shape);
        const bool shapeRect = (tool == CanvasWidget::Tool::Shape && canvas_->shapeKind() == ShapeKind::Rectangle);
        const bool shapeLine = (tool == CanvasWidget::Tool::Shape && canvas_->shapeKind() == ShapeKind::Line);
        shapeRadius->setVisible(shapeRect);
        shapeRadiusLabel_->setVisible(shapeRect);
        shapeLineWidth->setVisible(shapeLine);
        shapeLineWidthLabel_->setVisible(shapeLine);
        const bool text = tool == CanvasWidget::Tool::Text;
        textFont->setVisible(text);
        textSizeLabel_->setVisible(text); textSize->setVisible(text);
        textTrackingLabel_->setVisible(text); textTracking->setVisible(text);
        textLeadingLabel_->setVisible(text); textLeading->setVisible(text);
        textStyleGroup->setVisible(text); textAlignment->setVisible(text);
        textCancel->setVisible(text); textDone->setVisible(text);
        transformCancel_->setVisible(move || tool == CanvasWidget::Tool::Crop);
        transformApply_->setVisible(move || tool == CanvasWidget::Tool::Crop);
        gradientShape->setVisible(tool == CanvasWidget::Tool::Gradient); gradientStyle->setVisible(tool == CanvasWidget::Tool::Gradient);
        gradientReverse->setVisible(tool == CanvasWidget::Tool::Gradient);
        gradientOpacityLabel_->setVisible(tool == CanvasWidget::Tool::Gradient);
        gradientOpacityField_->setVisible(tool == CanvasWidget::Tool::Gradient);
        marqueeKind->setVisible(tool == CanvasWidget::Tool::Marquee); lassoKind->setVisible(tool == CanvasWidget::Tool::Lasso);
        selectionMode->setVisible(selection); selectionAntialias->setVisible(tool == CanvasWidget::Tool::Lasso || tool == CanvasWidget::Tool::Wand
                                                                             || (tool == CanvasWidget::Tool::Marquee && marqueeKind->currentIndex() == 1));
        wandModeControl->setVisible(tool == CanvasWidget::Tool::Wand);
        const bool isObject = (canvas_->wandMode() == CanvasWidget::WandMode::Object);
        const bool isWandTool = (tool == CanvasWidget::Tool::Wand);
        wandTolerance->setVisible(isWandTool && !isObject);
        wandToleranceLabel_->setVisible(isWandTool && !isObject);
        wandSampleSize->setVisible(isWandTool && !isObject);
        wandSample->setVisible(isWandTool);
        wandContiguous->setVisible(isWandTool && !isObject);
        objectEdgeOffset->setVisible(isWandTool && isObject);
        objectEdgeOffsetLabel_->setVisible(isWandTool && isObject);
        objectSmoothEdges->setVisible(isWandTool && isObject);
        selectionModifyGroup->setVisible(selection);
        selectionAmountField_->setVisible(selection);
        selectionAmountLabel_->setVisible(selection);
        if (brush) transformTitle->setText(tool == CanvasWidget::Tool::Healing ? tr("Spot Healing") : tool == CanvasWidget::Tool::Clone ? tr("Clone Stamp")
            : tool == CanvasWidget::Tool::Blur ? tr("Smear") : tool == CanvasWidget::Tool::Eraser ? tr("Eraser") : tr("Brush"));
        else if (selection) transformTitle->setText(tool == CanvasWidget::Tool::Marquee ? tr("Marquee") : tool == CanvasWidget::Tool::Lasso ? tr("Lasso") : (canvas_->wandMode() == CanvasWidget::WandMode::Object ? tr("Object Selection") : tr("Magic Wand")));
        else if (tool == CanvasWidget::Tool::Crop) transformTitle->setText(tr("Crop"));
        else if (tool == CanvasWidget::Tool::Gradient) transformTitle->setText(tr("Gradient"));
        else if (tool == CanvasWidget::Tool::Shape) transformTitle->setText(tr("Shape"));
        else if (tool == CanvasWidget::Tool::Text) transformTitle->setText(tr("Text"));
        else if (tool == CanvasWidget::Tool::Hand) transformTitle->setText(tr("Hand · Drag to pan"));
        else if (tool == CanvasWidget::Tool::Zoom) transformTitle->setText(tr("Zoom · Click to zoom in · Alt-click to zoom out"));
        else if (tool == CanvasWidget::Tool::Eyedropper) transformTitle->setText(tr("Eyedropper · Click to sample color"));
        else transformTitle->setText(tr("Transform"));
        int selected = tool == CanvasWidget::Tool::Move ? 0 : tool == CanvasWidget::Tool::Marquee ? 1 : tool == CanvasWidget::Tool::Lasso ? 2
            : tool == CanvasWidget::Tool::Wand ? 3 : tool == CanvasWidget::Tool::Crop ? 4 : (tool == CanvasWidget::Tool::Brush || tool == CanvasWidget::Tool::Eraser) ? 5
            : tool == CanvasWidget::Tool::Healing ? 6 : tool == CanvasWidget::Tool::Clone ? 7 : tool == CanvasWidget::Tool::Blur ? 8
            : tool == CanvasWidget::Tool::Gradient ? 9 : tool == CanvasWidget::Tool::Shape ? 10 : tool == CanvasWidget::Tool::Text ? 11 : tool == CanvasWidget::Tool::Eyedropper ? 12
            : tool == CanvasWidget::Tool::Hand ? 13 : tool == CanvasWidget::Tool::Zoom ? 14 : -1;
        if (selected >= 0) toolButtons.at(selected)->setChecked(true);
    });
    connect(marqueeKind, &SegmentedControl::currentIndexChanged, this, [this, selectionAntialias, marqueeKind](int) {
        selectionAntialias->setVisible(canvas_->tool() == CanvasWidget::Tool::Marquee && marqueeKind->currentIndex() == 1);
    });
    canvasStack_ = new QStackedWidget(workspace);
    auto *startPage = new QWidget(canvasStack_); startPage->setObjectName(QStringLiteral("newCanvasPage"));
    auto *startOuter = new QVBoxLayout(startPage); startOuter->addStretch();
    auto *startPanel = new QWidget(startPage); startPanel->setFixedWidth(500); startPanel->setObjectName(QStringLiteral("newCanvasPanel"));
    auto *startLayout = new QVBoxLayout(startPanel); startLayout->setContentsMargins(32, 30, 32, 30); startLayout->setSpacing(14);
    auto *startTitle = new QLabel(tr("Create a new canvas"), startPanel); startTitle->setObjectName(QStringLiteral("emptyStateTitle"));
    auto *startSubtitle = new QLabel(tr("Choose a size, or start from an existing image or project."), startPanel); startSubtitle->setObjectName(QStringLiteral("emptyStateSubtitle"));
    startLayout->addWidget(startTitle); startLayout->addWidget(startSubtitle);
    auto *startForm = new QFormLayout;
    startForm->setContentsMargins(0, 10, 0, 0); startForm->setHorizontalSpacing(18); startForm->setVerticalSpacing(12); startForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    newCanvasWidth_ = new QSpinBox(startPanel); newCanvasWidth_->setObjectName(QStringLiteral("newCanvasWidth")); newCanvasWidth_->setRange(1, 30000); newCanvasWidth_->setValue(1920); newCanvasWidth_->setSuffix(tr(" px"));
    newCanvasHeight_ = new QSpinBox(startPanel); newCanvasHeight_->setObjectName(QStringLiteral("newCanvasHeight")); newCanvasHeight_->setRange(1, 30000); newCanvasHeight_->setValue(1080); newCanvasHeight_->setSuffix(tr(" px"));
    startForm->addRow(tr("Width"), newCanvasWidth_); startForm->addRow(tr("Height"), newCanvasHeight_); startLayout->addLayout(startForm);
    auto *startHint = new QLabel(tr("Transparent background  ·  sRGB color space"), startPanel); startHint->setObjectName(QStringLiteral("emptyStateHint")); startLayout->addWidget(startHint);
    auto *startButtons = new QHBoxLayout;
    startButtons->setContentsMargins(0, 8, 0, 0); startButtons->setSpacing(10);
    auto *openStart = new QPushButton(tr("Open project"), startPanel); auto *importStart = new QPushButton(tr("Import image"), startPanel);
    auto *createStart = new QPushButton(tr("Create canvas"), startPanel); createStart->setObjectName(QStringLiteral("createCanvas"));
    startButtons->addWidget(openStart); startButtons->addWidget(importStart); startButtons->addStretch(); startButtons->addWidget(createStart); startLayout->addLayout(startButtons);
    startOuter->addWidget(startPanel, 0, Qt::AlignHCenter); startOuter->addStretch();
    canvasStack_->addWidget(startPage); canvasStack_->addWidget(canvas_); canvasStack_->setCurrentWidget(startPage);
    connect(createStart, &QPushButton::clicked, this, [this] {
        if (qint64(newCanvasWidth_->value()) * newCanvasHeight_->value() > 100000000LL) {
            showMessage(this, tr("Canvas Too Large"), tr("A canvas may contain at most 100 megapixels.")); return;
        }
        session_.createDocument(newCanvasWidth_->value(), newCanvasHeight_->value(), true); syncDocumentViews();
    });
    connect(openStart, &QPushButton::clicked, this, &MainWindow::chooseProject);
    connect(importStart, &QPushButton::clicked, this, &MainWindow::importImages);
    auto *canvasContainer = new QWidget(workspace);
    auto *canvasGrid = new QGridLayout(canvasContainer);
    canvasGrid->setContentsMargins(0, 0, 0, 0);
    canvasGrid->setSpacing(0);

    rulerCorner_ = new CanvasRulerCornerWidget(canvasContainer);
    rulerCorner_->setObjectName(QStringLiteral("rulerCorner"));
    horizontalRuler_ = new CanvasRulerWidget(CanvasGuide::Axis::Horizontal, session_, canvas_, canvasContainer);
    horizontalRuler_->setObjectName(QStringLiteral("horizontalRuler"));
    verticalRuler_ = new CanvasRulerWidget(CanvasGuide::Axis::Vertical, session_, canvas_, canvasContainer);
    verticalRuler_->setObjectName(QStringLiteral("verticalRuler"));

    canvasGrid->addWidget(rulerCorner_, 0, 0);
    canvasGrid->addWidget(horizontalRuler_, 0, 1);
    canvasGrid->addWidget(verticalRuler_, 1, 0);
    canvasGrid->addWidget(canvasStack_, 1, 1);
    canvasGrid->setRowStretch(1, 1);
    canvasGrid->setColumnStretch(1, 1);

    connect(horizontalRuler_, &CanvasRulerWidget::guideChanged, this, [this] { syncDocumentViews(false); });
    connect(verticalRuler_, &CanvasRulerWidget::guideChanged, this, [this] { syncDocumentViews(false); });
    connect(canvas_, &CanvasWidget::guideChanged, this, [this] { syncDocumentViews(false); });
    const auto updateRulers = [this] {
        if (horizontalRuler_ && horizontalRuler_->isVisible()) horizontalRuler_->update();
        if (verticalRuler_ && verticalRuler_->isVisible()) verticalRuler_->update();
    };
    connect(canvas_, &CanvasWidget::zoomChanged, this, updateRulers);
    connect(canvas_, &CanvasWidget::viewportChanged, this, updateRulers);

    updateRulerVisibility();
    workspaceLayout->addWidget(canvasContainer, 1);

    layerModel_ = new LayerListModel(this);
    auto *inspector = new QWidget(workspace); inspector->setObjectName(QStringLiteral("inspector")); inspector->setFixedWidth(252);
    auto *inspectorLayout = new QVBoxLayout(inspector); inspectorLayout->setContentsMargins(0, 0, 0, 0); inspectorLayout->setSpacing(0);
    auto *layerHeading = new QWidget(inspector); layerHeading->setObjectName(QStringLiteral("inspectorHeading")); layerHeading->setFixedHeight(47);
    auto *headingLayout = new QHBoxLayout(layerHeading); headingLayout->setContentsMargins(14, 0, 12, 0);
    auto *layersTitle = new QLabel(tr("Layers"), layerHeading); layersTitle->setObjectName(QStringLiteral("sectionTitle"));
    layerCount_ = new QLabel(QStringLiteral("0"), layerHeading); layerCount_->setObjectName(QStringLiteral("mutedLabel"));
    headingLayout->addWidget(layersTitle); headingLayout->addStretch(); headingLayout->addWidget(layerCount_);
    inspectorLayout->addWidget(layerHeading);
    auto *appearance = new QWidget(inspector); appearance->setObjectName(QStringLiteral("appearancePanel")); appearance->setFixedHeight(83);
    auto *appearanceLayout = new QVBoxLayout(appearance); appearanceLayout->setContentsMargins(12, 8, 12, 8); appearanceLayout->setSpacing(6);
    auto *blendRow = new QHBoxLayout; auto *blendLabel = new QLabel(tr("Blend"), appearance);
    blendMode_ = new QComboBox(appearance); blendMode_->addItems({
        tr("Normal"),
        tr("Darken"), tr("Multiply"), tr("Color Burn"), tr("Linear Burn"),
        tr("Lighten"), tr("Screen"), tr("Color Dodge"), tr("Linear Dodge (Add)"),
        tr("Overlay"), tr("Soft Light"), tr("Hard Light"), tr("Vivid Light"), tr("Linear Light"), tr("Pin Light"), tr("Hard Mix"),
        tr("Difference"), tr("Exclusion"), tr("Subtract"), tr("Divide"),
        tr("Hue"), tr("Saturation"), tr("Color"), tr("Luminosity")
    });
    blendMode_->setObjectName(QStringLiteral("blendMode"));
    blendMode_->view()->installEventFilter(this);
    blendRow->addWidget(blendLabel); blendRow->addWidget(blendMode_, 1); appearanceLayout->addLayout(blendRow);
    auto *opacityRow = new QHBoxLayout;
    opacitySlider_ = new SnapSlider(Qt::Horizontal, appearance); opacitySlider_->setObjectName(QStringLiteral("layerOpacity")); opacitySlider_->setRange(0, 100); opacitySlider_->setValue(100);
    layerOpacityLabel_ = new ScrubLabel(tr("Opacity"), opacitySlider_, 1.0, 1.0, appearance);
    layerOpacityLabel_->setObjectName(QStringLiteral("layerOpacityLabel"));
    layerOpacityLabel_->setToolTip(tr("Layer Opacity"));
    auto *opacityValue = new QLabel(QStringLiteral("100  %"), appearance); opacityValue->setMinimumWidth(43); opacityValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    opacityRow->addWidget(layerOpacityLabel_); opacityRow->addWidget(opacitySlider_, 1); opacityRow->addWidget(opacityValue); appearanceLayout->addLayout(opacityRow);
    inspectorLayout->addWidget(appearance);
    layerView_ = new QListView(inspector);
    layerView_->setObjectName(QStringLiteral("layerList"));
    layerView_->setModel(layerModel_);
    layerView_->setItemDelegate(new LayerDelegate(layerView_));
    layerView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    layerView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layerView_->setDragEnabled(true); layerView_->setAcceptDrops(true); layerView_->setDropIndicatorShown(true);
    layerView_->setDragDropMode(QAbstractItemView::DragDrop); layerView_->setDefaultDropAction(Qt::MoveAction);
    layerView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layerView_->viewport()->setMouseTracking(true);
    layerView_->installEventFilter(this);
    layerView_->viewport()->installEventFilter(this);
    inspectorLayout->addWidget(layerView_, 1);
    auto *layerFooter = new QWidget(inspector); layerFooter->setObjectName(QStringLiteral("layerFooter")); layerFooter->setFixedHeight(36);
    auto *footerLayout = new QHBoxLayout(layerFooter); footerLayout->setContentsMargins(8, 4, 6, 4); footerLayout->setSpacing(4);
    const auto layerButton = [layerFooter, footerLayout](int icon, const QString &name, const QString &tip) {
        auto *button = new QToolButton(layerFooter);
        button->setObjectName(name); button->setIcon(editorIcon(icon)); button->setIconSize(QSize(16, 16));
        button->setToolTip(tip); button->setAccessibleName(tip); button->setFixedSize(28, 28);
        footerLayout->addWidget(button); return button;
    };
    auto *addLayer = layerButton(17, QStringLiteral("addLayerButton"), tr("New blank layer"));
    auto *addGroup = layerButton(18, QStringLiteral("addGroupButton"), tr("New folder"));
    auto *duplicate = layerButton(19, QStringLiteral("duplicateLayerButton"), tr("Duplicate layer"));
    auto *addMask = layerButton(20, QStringLiteral("addMaskButton"), tr("Add layer mask"));
    auto *addEffect = layerButton(30, QStringLiteral("addEffectButton"), tr("Layer effects"));
    addEffect->setPopupMode(QToolButton::InstantPopup);
    auto *effectsMenu = new QMenu(addEffect);
    for (LayerEffectKind kind : {LayerEffectKind::Stroke, LayerEffectKind::DropShadow, LayerEffectKind::ColorOverlay,
                                 LayerEffectKind::InnerShadow, LayerEffectKind::OuterGlow, LayerEffectKind::InnerGlow}) {
        auto *action = effectsMenu->addAction(layerEffectKindToString(kind) + QStringLiteral("…"));
        connect(action, &QAction::triggered, this, [this, kind]() {
            if (!document_ || !document_->activeLayerId || !session_.canEditEffects()) return;
            const QUuid target = *document_->activeLayerId;
            session_.addLayerEffect(target, kind);
            layerEffectsDialog(target, kind);
        });
    }
    addEffect->setMenu(effectsMenu);
    layerMenuButton_ = layerButton(21, QStringLiteral("layerActionsButton"), tr("More layer actions"));
    layerMenuButton_->setPopupMode(QToolButton::InstantPopup);
    footerLayout->addStretch();
    auto *trash = layerButton(22, QStringLiteral("deleteLayerButton"), tr("Delete selected layers"));
    inspectorLayout->addWidget(layerFooter);
    workspaceLayout->addWidget(inspector);
    shellLayout->addWidget(workspace, 1);

    auto *status = new QWidget(shell); status->setObjectName(QStringLiteral("bottomStatus")); status->setFixedHeight(29);
    auto *statusLayout = new QHBoxLayout(status); statusLayout->setContentsMargins(16, 0, 16, 0); statusLayout->setSpacing(13);
    zoomField_ = new QDoubleSpinBox(status); zoomField_->setObjectName(QStringLiteral("zoomPercentage"));
    zoomField_->setRange(.1, 3200); zoomField_->setDecimals(1); zoomField_->setSuffix(QStringLiteral("%"));
    zoomField_->setValue(100); zoomField_->setButtonSymbols(QAbstractSpinBox::NoButtons); zoomField_->setFixedWidth(76); zoomField_->setKeyboardTracking(false);
    statusDimensions_ = new QLabel(QStringLiteral("No document"), status);
    auto *profile = new QLabel(QStringLiteral("sRGB · Transparent"), status); statusHint_ = new QLabel(tr("Open or drop a Compositor project to begin"), status);
    statusLayout->addWidget(zoomField_); statusLayout->addWidget(statusDimensions_); statusLayout->addWidget(profile); statusLayout->addStretch(); statusLayout->addWidget(statusHint_);
    shellLayout->addWidget(status);
    setCentralWidget(shell);

    connect(layerModel_, &QAbstractItemModel::dataChanged, canvas_, qOverload<>(&QWidget::update));
    connect(layerModel_, &LayerListModel::visibilityToggleRequested, this, [this](const QUuid &id) {
        session_.toggleLayerVisibility(id);
        syncDocumentViews();
    });
    connect(layerModel_, &LayerListModel::effectVisibilityToggleRequested, this, [this](const QUuid &id, LayerEffectKind kind) {
        session_.toggleLayerEffect(id, kind);
        syncDocumentViews();
    });
    connect(layerModel_, &LayerListModel::renameRequested, this, [this](const QUuid &id, const QString &name) {
        session_.renameLayer(id, name); syncDocumentViews(false);
    });
    connect(layerModel_, &LayerListModel::layersDropRequested, this,
            [this](const QVector<QUuid> &dragged, const QUuid &parentId, const QUuid &aboveId, bool atBottom, bool copy) {
        if (!document_ || dragged.isEmpty()) return;
        const std::optional<QUuid> parent = parentId.isNull() ? std::nullopt : std::optional<QUuid>(parentId);
        const std::optional<QUuid> above = aboveId.isNull() ? std::nullopt : std::optional<QUuid>(aboveId);
        QVector<QUuid> ids = dragged;
        if (parent && !above) std::reverse(ids.begin(), ids.end());
        session_.beginEdit(copy ? (ids.size() > 1 ? tr("Duplicate Layers") : tr("Duplicate Layer"))
                                : (ids.size() > 1 ? tr("Move Layers") : tr("Move Layer")));
        bool changed = false;
        for (const QUuid &id : ids) changed = (copy ? session_.duplicateLayer(id, parent, above, atBottom)
                                                    : session_.placeLayer(id, parent, above, atBottom)) || changed;
        if (changed && !copy) session_.selectLayers(QSet<QUuid>(dragged.cbegin(), dragged.cend()), dragged.constFirst());
        session_.endEdit();
        if (changed) syncDocumentViews();
    });
    connect(layerModel_, &LayerListModel::maskDropRequested, this, [this](const QUuid &sourceId, const QUuid &targetId) {
        if (!document_) return;
        if (session_.copyLayerMask(sourceId, targetId)) {
            syncDocumentViews();
        }
    });
    connect(layerModel_, &LayerListModel::effectDropRequested, this, [this](const QUuid &sourceId, LayerEffectKind kind, const QUuid &targetId) {
        if (!document_) return;
        if (session_.copyLayerEffect(kind, sourceId, targetId)) {
            selectedEffect_ = EffectSelection{targetId, kind};
            syncDocumentViews();
        }
    });
    connect(layerView_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        if (!document_) return;
        if (transformOriginalDocument_) finishPersistentTransform(true);
        QSet<QUuid> ids;
        for (const QModelIndex &index : layerView_->selectionModel()->selectedRows()) {
            if (const auto id = layerModel_->layerId(index)) ids.insert(*id);
        }
        std::optional<QUuid> primary;
        const QModelIndex current = layerView_->currentIndex();
        if (const auto id = layerModel_->layerId(current)) primary = id;
        if (layerModel_->isEffect(current)) {
            const auto kind = layerModel_->effectKind(current);
            if (primary && kind) {
                selectedEffect_ = EffectSelection{*primary, *kind};
            }
        } else {
            selectedEffect_ = std::nullopt;
        }
        if (session_.hasFloatingSelection()) {
            session_.commitSelectionTransform();
            bool hasValid = false;
            for (const Layer &layer : document_->layers) if (ids.contains(layer.id)) { hasValid = true; break; }
            if (!hasValid && document_->activeLayerId) { ids = {*document_->activeLayerId}; primary = document_->activeLayerId; }
            session_.selectLayers(ids, primary);
            syncDocumentViews();
            return;
        }
        session_.selectLayers(ids, primary);
        canvas_->setSelectedLayerIds(ids);
        updateInspector();
        canvas_->update();
    });
    connect(layerView_, &QListView::clicked, this, [this](const QModelIndex &index) {
        if (!document_ || !index.isValid()) return;
        const auto id = layerModel_->layerId(index); if (!id) return;

        if (layerModel_->isEffect(index)) {
            const auto kind = layerModel_->effectKind(index);
            if (!kind) return;
            const int depth = index.data(Qt::UserRole + 1).toInt();
            const int indent = std::min(depth, 8) * 18;
            const int eyeX = 38 + indent;
            const int x = layerView_->viewport()->mapFromGlobal(QCursor::pos()).x();
            if (x >= eyeX - 6 && x <= eyeX + 22) {
                session_.toggleLayerEffect(*id, *kind);
                syncDocumentViews();
                return;
            }
            selectedEffect_ = EffectSelection{*id, *kind};
            session_.selectLayer(*id);
            syncDocumentViews(false);
            return;
        }

        selectedEffect_ = std::nullopt;
        const auto found = std::find_if(document_->layers.cbegin(), document_->layers.cend(), [&](const Layer &candidate){ return candidate.id == *id; });
        if (found == document_->layers.cend()) return;
        const Layer &layer = *found;
        const int depth = index.data(Qt::UserRole + 1).toInt();
        const int thumbnail = 34 + std::min(depth, 8) * 18;
        const int x = layerView_->viewport()->mapFromGlobal(QCursor::pos()).x();
        if (layer.group && x >= thumbnail - 9 && x < thumbnail + 8) { layerModel_->toggleExpanded(layer.id); return; }

        if (layerModel_->data(index, LayerListModel::HasEffectsRole).toBool()) {
            const QRect rowRect = layerView_->visualRect(index);
            if (x >= rowRect.right() - 36 && x <= rowRect.right()) {
                layerModel_->toggleEffectsExpanded(layer.id);
                return;
            }
        }

        const Qt::KeyboardModifiers modifiers = QApplication::keyboardModifiers();
        if (modifiers.testFlag(Qt::ControlModifier) && x >= thumbnail && x <= thumbnail + 80) {
            const SelectionMode mode = modifiers.testFlag(Qt::AltModifier) ? SelectionMode::Subtract
                : modifiers.testFlag(Qt::ShiftModifier) ? SelectionMode::Add : SelectionMode::Replace;
            const bool maskThumbnail = !layer.mask.isNull() && x >= thumbnail + 42;
            if (maskThumbnail ? session_.loadMaskAsSelection(mode, layer.id) : session_.loadLayerAsSelection(mode, layer.id)) syncDocumentViews(false);
            return;
        }
        const QPoint localPointer = layerView_->viewport()->mapFromGlobal(QCursor::pos());
        if (modifiers.testFlag(Qt::AltModifier) && localPointer.y() >= layerView_->visualRect(index).bottom() - layerView_->visualRect(index).height() / 4
            && !(x >= thumbnail && x <= thumbnail + 80)) {
            if (session_.toggleClippingMask(layer.id)) syncDocumentViews();
            return;
        }
        canvas_->resolvePendingGradient(); canvas_->resolvePendingDistortion();
        session_.selectMaskTarget(!layer.mask.isNull() && x >= thumbnail + 42 && x <= thumbnail + 80);
        updateInspector(); canvas_->update();
    });
    connect(layerView_, &QListView::doubleClicked, this, [this](const QModelIndex &index) {
        if (!document_ || !index.isValid()) return;
        if (layerModel_->isEffect(index)) {
            const auto id = layerModel_->layerId(index);
            const auto kind = layerModel_->effectKind(index);
            if (id && kind) {
                layerEffectsDialog(*id, *kind);
            }
            return;
        }
        const Layer *layer = session_.activeLayer(); if (!layer) return;
        if (layer->adjustment.isEmpty()) { layerView_->edit(layerView_->currentIndex()); return; }
        const QString kind = layer->adjustment.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("Hue/Saturation")) hueSaturationDialog();
        else if (kind == QStringLiteral("Levels")) levelsDialog();
        else if (kind == QStringLiteral("Curves")) curvesDialog();
        else if (kind == QStringLiteral("Exposure")) exposureDialog();
        else if (kind == QStringLiteral("Gradient Map")) gradientMapDialog();
        else if (kind == QStringLiteral("Grain")) grainDialog();
        else if (kind == QStringLiteral("Black & White")) blackWhiteDialog();
        else if (kind == QStringLiteral("Color Balance")) colorBalanceDialog();
        else if (kind == QStringLiteral("Gaussian Blur")) gaussianBlurDialog();
        else if (kind == QStringLiteral("Motion Blur")) motionBlurDialog();
        else if (kind == QStringLiteral("Add Noise")) addNoiseDialog();
    });
    layerView_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(layerView_, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (!document_) return;
        const QModelIndex index = layerView_->indexAt(pos);
        QMenu menu(this);
        if (index.isValid() && layerModel_->isEffect(index)) {
            const auto id = layerModel_->layerId(index);
            const auto kind = layerModel_->effectKind(index);
            if (id && kind) {
                auto *editAct = menu.addAction(tr("Edit %1…").arg(layerEffectKindToString(*kind)));
                connect(editAct, &QAction::triggered, this, [this, id, kind]() {
                    layerEffectsDialog(*id, *kind);
                });
                auto *toggleAct = menu.addAction(tr("Toggle Visibility"));
                connect(toggleAct, &QAction::triggered, this, [this, id, kind]() {
                    session_.toggleLayerEffect(*id, *kind);
                    syncDocumentViews();
                });
                auto *deleteAct = menu.addAction(tr("Delete Effect"));
                connect(deleteAct, &QAction::triggered, this, [this, id, kind]() {
                    session_.removeLayerEffect(*id, *kind);
                    selectedEffect_ = std::nullopt;
                    syncDocumentViews();
                });
            }
        } else if (session_.canUngroupLayers()) {
            auto *groupAct = menu.addAction(tr("Group Selected Layers"));
            connect(groupAct, &QAction::triggered, this, [this] { session_.groupSelectedLayers(); syncDocumentViews(); });
            auto *ungroupAct = menu.addAction(tr("Ungroup Layers"));
            connect(ungroupAct, &QAction::triggered, this, [this] { session_.ungroupLayers(); syncDocumentViews(); });
        } else if (session_.canEditEffects()) {
            auto *effectsAct = menu.addAction(tr("Layer Effects…"));
            connect(effectsAct, &QAction::triggered, this, [this]() {
                layerEffectsDialog();
            });
            auto *addMenu = menu.addMenu(tr("Add Effect"));
            for (LayerEffectKind kind : {LayerEffectKind::Stroke, LayerEffectKind::DropShadow, LayerEffectKind::ColorOverlay,
                                         LayerEffectKind::InnerShadow, LayerEffectKind::OuterGlow, LayerEffectKind::InnerGlow}) {
                auto *act = addMenu->addAction(layerEffectKindToString(kind) + QStringLiteral("…"));
                connect(act, &QAction::triggered, this, [this, kind]() {
                    if (!document_ || !document_->activeLayerId) return;
                    const QUuid target = *document_->activeLayerId;
                    session_.addLayerEffect(target, kind);
                    layerEffectsDialog(target, kind);
                });
            }
        }
        if (!menu.isEmpty()) {
            menu.exec(layerView_->viewport()->mapToGlobal(pos));
        }
    });
    connect(opacitySlider_, &QSlider::valueChanged, this, [this, opacityValue](int value) {
        opacityValue->setText(QStringLiteral("%1  %").arg(value));
        if (!document_ || !document_->activeLayerId || opacitySlider_->signalsBlocked()) return;
        session_.setLayerOpacity(*document_->activeLayerId, value / 100.0);
        canvas_->invalidateDocument();
        refreshTitle();
    });
    connect(opacitySlider_, &QSlider::sliderPressed, this, [this] {
        if (document_ && document_->activeLayerId) session_.beginEdit(QStringLiteral("Layer Opacity"));
    });
    connect(opacitySlider_, &QSlider::sliderReleased, this, [this] {
        if (document_ && document_->activeLayerId) session_.endEdit();
        refreshTitle();
    });
    connect(layerOpacityLabel_, &ScrubLabel::dragStarted, this, [this] {
        if (document_ && document_->activeLayerId) session_.beginEdit(QStringLiteral("Layer Opacity"));
    });
    connect(layerOpacityLabel_, &ScrubLabel::dragEnded, this, [this] {
        if (document_ && document_->activeLayerId) session_.endEdit();
        refreshTitle();
    });
    connect(blendMode_, &QComboBox::currentIndexChanged, this, [this](int value) {
        if (!document_ || !document_->activeLayerId || blendMode_->signalsBlocked()) return;
        canvas_->setBlendModePreview(std::nullopt, std::nullopt);
        session_.setLayerBlendMode(*document_->activeLayerId, BlendMode(value));
        canvas_->invalidateDocument();
        refreshTitle();
    });
    connect(blendMode_, &QComboBox::highlighted, this, [this](int value) {
        if (!document_ || !document_->activeLayerId || value < 0 || value >= blendMode_->count()) return;
        canvas_->setBlendModePreview(document_->activeLayerId, BlendMode(value));
    });
    const auto changeTransform = [this, link] {
        if (!document_ || !document_->activeLayerId) return;
        const bool isWidth = (sender() == widthField_ || sender() == widthLabel_);
        const bool isHeight = (sender() == heightField_ || sender() == heightLabel_);
        if (link->isChecked() && (isWidth || isHeight)) {
            QRectF current = session_.selectedLayerIds().size() > 1 ? session_.selectedLayersBounds() : QRectF();
            if (current.isEmpty()) {
                if (const Layer *layer = session_.activeLayer()) current = QRectF(layer->transform.origin, layer->transform.size);
            }
            if (current.width() > 0 && current.height() > 0) {
                if (isWidth) {
                    const QSignalBlocker blocker(heightField_);
                    heightField_->setValue(std::max(1.0, widthField_->value() * current.height() / current.width()));
                } else {
                    const QSignalBlocker blocker(widthField_);
                    widthField_->setValue(std::max(1.0, heightField_->value() * current.width() / current.height()));
                }
            }
        }
        beginPersistentTransform(session_.isMaskSelected() ? QStringLiteral("Transform Layer Mask") : QStringLiteral("Transform Layer"));
        if (session_.selectedLayerIds().size() > 1 || (session_.activeLayer() && session_.activeLayer()->group)) {
            session_.transformSelectedLayers(QRectF(xField_->value(), yField_->value(),
                                                     std::max(1.0, widthField_->value()), std::max(1.0, heightField_->value())),
                                             rotationField_->value());
            updateInspector(); canvas_->invalidateDocument(); refreshTitle(); return;
        }
        session_.beginEdit(session_.isMaskSelected() ? QStringLiteral("Transform Layer Mask") : QStringLiteral("Transform Layer"));
        for (Layer &layer : document_->layers) {
            if (layer.id != *document_->activeLayerId) continue;
            const LayerTransform old=layer.transform;const std::optional<LayerTransform> oldMask=layer.maskPlacement;
            const bool maskTarget=session_.isMaskSelected()&&!layer.maskLinked;if(maskTarget&&!layer.maskPlacement)layer.maskPlacement=layer.transform;LayerTransform &edited=maskTarget?*layer.maskPlacement:layer.transform;
            edited.origin = QPointF(xField_->value(), yField_->value());
            edited.size = QSizeF(std::max(1.0, widthField_->value()), std::max(1.0, heightField_->value()));
            edited.rotation = rotationField_->value();
            if(!maskTarget&&!layer.mask.isNull()&&layer.mask.size()!=QSize(1,1)){if(layer.maskLinked){if(oldMask)layer.maskPlacement=oldMask->following(old,layer.transform);else layer.maskPlacement.reset();}else layer.maskPlacement=oldMask.value_or(old);}
            break;
        }
        session_.redrawSelectedShapes();
        session_.endEdit();
        canvas_->invalidateDocument();
        refreshTitle();
    };
    connect(xField_, &QDoubleSpinBox::editingFinished, this, changeTransform);
    connect(yField_, &QDoubleSpinBox::editingFinished, this, changeTransform);
    connect(widthField_, &QDoubleSpinBox::editingFinished, this, changeTransform);
    connect(heightField_, &QDoubleSpinBox::editingFinished, this, changeTransform);
    connect(rotationField_, &QDoubleSpinBox::editingFinished, this, changeTransform);
    connect(xLabel_, &ScrubLabel::scrubValueChanged, this, [changeTransform] { changeTransform(); });
    connect(yLabel_, &ScrubLabel::scrubValueChanged, this, [changeTransform] { changeTransform(); });
    connect(widthLabel_, &ScrubLabel::scrubValueChanged, this, [changeTransform] { changeTransform(); });
    connect(heightLabel_, &ScrubLabel::scrubValueChanged, this, [changeTransform] { changeTransform(); });
    connect(rotationLabel_, &ScrubLabel::scrubValueChanged, this, [changeTransform] { changeTransform(); });

    const auto startTransformScrub = [this] {
        beginPersistentTransform(session_.isMaskSelected() ? QStringLiteral("Transform Layer Mask") : QStringLiteral("Transform Layer"));
    };
    xLabel_->setOnStart(startTransformScrub);
    yLabel_->setOnStart(startTransformScrub);
    widthLabel_->setOnStart(startTransformScrub);
    heightLabel_->setOnStart(startTransformScrub);
    rotationLabel_->setOnStart(startTransformScrub);
    connect(scaleField_, &QDoubleSpinBox::editingFinished, this, [this] {
        Layer *layer = session_.activeLayer(); if (!layer || session_.selectedLayerIds().size() > 1) return;
        const bool maskTarget = session_.isMaskSelected() && !layer->maskLinked;
        beginPersistentTransform(maskTarget ? QStringLiteral("Transform Layer Mask") : QStringLiteral("Scale Layer"));
        const QSize pixels = maskTarget ? layer->mask.size().expandedTo(QSize(1, 1))
            : layer->image.isNull() ? layer->transform.size.toSize().expandedTo(QSize(1, 1)) : layer->image.size();
        const LayerTransform old = layer->transform; const std::optional<LayerTransform> oldMask = layer->maskPlacement;
        session_.beginEdit(maskTarget ? QStringLiteral("Transform Layer Mask") : QStringLiteral("Scale Layer"));
        if (maskTarget && !layer->maskPlacement) layer->maskPlacement = layer->transform;
        LayerTransform &edited = maskTarget ? *layer->maskPlacement : layer->transform;
        const QPointF center = edited.center(); edited.size = QSizeF(pixels) * (scaleField_->value() / 100.0);
        edited.origin = center - QPointF(edited.size.width() / 2, edited.size.height() / 2);
        if (!maskTarget && !layer->mask.isNull() && layer->mask.size() != QSize(1, 1) && layer->maskLinked) {
            if (oldMask) layer->maskPlacement = oldMask->following(old, layer->transform); else layer->maskPlacement.reset();
        }
        session_.redrawSelectedShapes(); session_.endEdit(); updateInspector(); canvas_->invalidateDocument(); refreshTitle();
    });
    connect(sampling_, &SegmentedControl::currentIndexChanged, this, [this](int index) {
        Layer *layer = session_.activeLayer(); if (!layer || sampling_->signalsBlocked()) return;
        beginPersistentTransform(QStringLiteral("Transform Layer"));
        const Sampling modes[] = {Sampling::HighQuality, Sampling::Smooth, Sampling::Nearest};
        session_.beginEdit(QStringLiteral("Change Sampling")); layer->transform.sampling = modes[std::clamp(index, 0, 2)]; session_.endEdit(); canvas_->invalidateDocument(); refreshTitle();
    });
    connect(newButton, &QToolButton::clicked, this, &MainWindow::newProject);
    connect(tabs_, &QTabBar::currentChanged, this, &MainWindow::activateTab);
    connect(tabs_, &QTabBar::tabCloseRequested, this, &MainWindow::closeTab);
    connect(tabs_, &QTabBar::tabMoved, this, [this](int from, int to) {
        if (from < 0 || to < 0 || from >= workspaceTabs_.size() || to >= workspaceTabs_.size()) return;
        stashCurrentTab();
        workspaceTabs_.move(from, to);
        tabRecoveryPaths_.move(from, to);
        if (tabWatchers_.size() > std::max(from, to)) tabWatchers_.move(from, to);
        if (tabDigests_.size() > std::max(from, to)) tabDigests_.move(from, to);
        if (tabPendingExternalChange_.size() > std::max(from, to)) tabPendingExternalChange_.move(from, to);
        if (tabPendingExternalDigest_.size() > std::max(from, to)) tabPendingExternalDigest_.move(from, to);
        if (tabPendingExternalDoc_.size() > std::max(from, to)) tabPendingExternalDoc_.move(from, to);
        currentTab_ = tabs_->currentIndex();
        session_ = workspaceTabs_.at(currentTab_);
        syncDocumentViews();
    });
    connect(addLayer, &QToolButton::clicked, this, [this] { session_.addBlankLayer(); syncDocumentViews(); });
    connect(addGroup, &QToolButton::clicked, this, [this] { session_.addGroup(); syncDocumentViews(); });
    connect(duplicate, &QToolButton::clicked, this, [this] { session_.duplicateActiveLayer(); syncDocumentViews(); });
    // As in Photoshop: the button reveals the selection; Option-click hides it (a black mask without one).
    connect(addMask, &QToolButton::clicked, this, [this] { if (session_.addLayerMask(!(QApplication::keyboardModifiers() & Qt::AltModifier))) syncDocumentViews(); });
    connect(trash, &QToolButton::clicked, this, &MainWindow::deleteLayersWithMaskChoice);
    connect(fitTop, &QPushButton::clicked, canvas_, &CanvasWidget::fitCanvas);
    connect(actualTop, &QPushButton::clicked, canvas_, &CanvasWidget::actualPixels);
    connect(zoomOutTop, &QToolButton::clicked, canvas_, &CanvasWidget::zoomOut);
    connect(zoomInTop, &QToolButton::clicked, canvas_, &CanvasWidget::zoomIn);
    connect(flipH, &QPushButton::clicked, this, [this] { if (session_.flipLayers(true)) syncDocumentViews(); });
    connect(flipV, &QPushButton::clicked, this, [this] { if (session_.flipLayers(false)) syncDocumentViews(); });
    connect(canvas_, &CanvasWidget::zoomChanged, this, [this](double zoom) { const QSignalBlocker blocker(zoomField_); zoomField_->setValue(zoom * 100); });
    connect(zoomField_, &QDoubleSpinBox::valueChanged, this, [this](double percent) { if (canvas_) canvas_->setZoom(percent / 100.0); });
    connect(canvas_, &CanvasWidget::layerTransformStarted, this, [this](bool duplicate) { session_.beginEdit(duplicate ? QStringLiteral("Duplicate Layer") : session_.isMaskSelected() ? QStringLiteral("Transform Layer Mask") : QStringLiteral("Move Layer")); });
    connect(canvas_, &CanvasWidget::duplicateLayerForTransformRequested, this, [this] { session_.duplicateActiveLayer(); syncDocumentViews(); });
    connect(canvas_, &CanvasWidget::layerTransformChanged, this, [this] { updateInspector(); refreshTitle(); });
    connect(canvas_, &CanvasWidget::layerTransformFinished, this, [this] { session_.redrawSelectedShapes(); session_.endEdit(); syncDocumentViews(); });
    connect(canvas_, &CanvasWidget::layerDistortionRequested, this, [this](const QPolygonF &points, bool maskOnly) {
        if (points.size() != 4) return;
        const std::array<QPointF, 4> corners{points[0], points[1], points[2], points[3]};
        if (session_.distortSelectedLayers(corners, maskOnly)) syncDocumentViews(); else canvas_->update();
    });
    connect(canvas_, &CanvasWidget::floatingTransformCommitRequested, this, [this] { if (session_.commitSelectionTransform()) syncDocumentViews(); });
    connect(canvas_, &CanvasWidget::floatingTransformCancelRequested, this, [this] { if (session_.cancelSelectionTransform()) syncDocumentViews(); });
    connect(canvas_, &CanvasWidget::layerSelectionRequested, this, [this](const QUuid &id) {
        if (transformOriginalDocument_) finishPersistentTransform(true);
        canvas_->resolvePendingGradient(); canvas_->resolvePendingDistortion(); session_.selectLayer(id); syncDocumentViews(false);
    });
    connect(canvas_, &CanvasWidget::rectangularSelectionRequested, this, [this](const QRect &rect, int mode) {
        session_.setRectangularSelection(rect, SelectionMode(mode)); syncDocumentViews(false);
    });
    connect(canvas_, &CanvasWidget::ellipticalSelectionRequested, this, [this](const QRect &rect, int mode, bool antialiased) {
        session_.setEllipticalSelection(rect, SelectionMode(mode), antialiased); syncDocumentViews(false);
    });
    connect(canvas_, &CanvasWidget::polygonSelectionRequested, this, [this](const QPolygonF &points, int mode, bool antialiased) {
        session_.setPolygonSelection(points, SelectionMode(mode), antialiased); syncDocumentViews(false);
    });
    connect(canvas_, &CanvasWidget::selectionMoveRequested, this, [this](const QPoint &offset) {
        if (session_.moveSelection(offset)) syncDocumentViews(false);
    });
    connect(canvas_, &CanvasWidget::selectedPixelsNudgeRequested, this, [this](const QPoint &offset) {
        if (session_.nudgeSelectedPixels(offset)) syncDocumentViews();
    });
    connect(canvas_, &CanvasWidget::selectedPixelsDragStarted, this, [this](bool duplicate) {
        if (session_.beginSelectionTransform(duplicate)) syncDocumentViews();
    });
    connect(canvas_, &CanvasWidget::magicWandRequested, this, [this, wandTolerance, wandSampleSize, wandSample, wandContiguous, selectionAntialias](const QPoint &point, int mode) {
        session_.magicWand(point, wandTolerance->value(), wandSampleSize->currentIndex(), wandContiguous->isChecked(),
                           wandSample->currentIndex() == 1, SelectionMode(mode), selectionAntialias->isChecked()); syncDocumentViews(false);
    });
    connect(canvas_, &CanvasWidget::objectSelectionRequested, this, [this, objectEdgeOffset, objectSmoothEdges, wandSample](const QPoint &point, int mode) {
        selectObjectRequested(point, mode, objectEdgeOffset->value(), objectSmoothEdges->isChecked(), wandSample->currentIndex() == 1);
    });
    connect(canvas_, &CanvasWidget::cropRequested, this, [this](const QRect &rect) {
        if (session_.crop(rect)) syncDocumentViews();
    });
    connect(canvas_, &CanvasWidget::zoomChanged, this, [this](double z) {
        session_.setViewportZoom(z);
    });
    // A stroke the document refuses says why, as Photoshop does, instead of silently doing nothing.
    const auto explainRefusal = [this] {
        const QString reason = session_.paintRefusal();
        if (reason.isEmpty()) return;
        QToolTip::showText(QCursor::pos(), reason, canvas_, QRect(), 5000);
    };
    connect(canvas_, &CanvasWidget::brushStrokeStarted, this, [this, explainRefusal](const QPointF &point, bool erasing) {
        explainRefusal();
        session_.setViewportZoom(canvas_->zoom());
        session_.setBrushSmoothing(brushSmoothing_);
        session_.beginBrushStroke(point, foregroundColor_, brushDiameter_, brushHardness_, brushOpacity_, erasing);
        canvas_->invalidateDocument();
    });
    connect(canvas_, &CanvasWidget::brushStrokeContinued, this, [this](const QPointF &point) {
        session_.continueBrushStroke(point); canvas_->invalidateDocument();
    });
    connect(canvas_, &CanvasWidget::brushStrokeFinished, this, [this] {
        if (session_.endBrushStroke()) syncDocumentViews();
    });
    connect(canvas_, &CanvasWidget::brushStrokeCancelRequested, this, [this] {
        if (session_.cancelBrushStroke()) syncDocumentViews();
    });
    connect(canvas_, &CanvasWidget::cloneSourceRequested, this, [this](const QPointF &point) {
        session_.setCloneSource(point); canvas_->setCloneSource(point);
    });
    connect(canvas_, &CanvasWidget::cloneStrokeStarted, this, [this,cloneAligned,cloneSample,explainRefusal](const QPointF &point) {
        explainRefusal();
        if (session_.beginCloneStroke(point, brushDiameter_, brushHardness_, brushOpacity_, cloneAligned->isChecked(), cloneSample->currentIndex() == 1)) {
            const auto offset = cloneAligned->isChecked() ? session_.cloneOffset()
                : std::optional<QPointF>(*session_.cloneSource() - point);
            canvas_->setCloneTracking(session_.cloneSource(), offset);
        }
        canvas_->invalidateDocument();
    });
    connect(cloneAligned, &QCheckBox::toggled, this, [this](bool aligned) {
        canvas_->setCloneAligned(aligned);
        canvas_->setCloneTracking(session_.cloneSource(), session_.cloneOffset());
    });
    connect(canvas_, &CanvasWidget::healingStrokeStarted, this, [this,healingMode,explainRefusal](const QPointF &point) {
        explainRefusal();
        session_.beginHealingStroke(point, brushDiameter_, brushHardness_, brushOpacity_, healingMode->currentIndex(), QRandomGenerator::global()->generate()); canvas_->invalidateDocument();
    });
    connect(canvas_, &CanvasWidget::blurStrokeStarted, this, [this, smearMode, explainRefusal](const QPointF &point) {
        explainRefusal();
        if (smearMode->currentIndex() == 1) session_.beginBlurStroke(point, brushDiameter_, brushHardness_, brushOpacity_, blurRadius_);
        else session_.beginWarpStroke(point, smearMode->currentIndex() == 0 ? 0 : 1, brushDiameter_, brushHardness_, brushOpacity_);
        canvas_->invalidateDocument();
    });
    connect(canvas_, &CanvasWidget::cycleSmearModeRequested, this, &MainWindow::cycleSmearMode);
    connect(canvas_, &CanvasWidget::cycleToolModeRequested, this, &MainWindow::cycleToolMode);
    connect(canvas_, &CanvasWidget::gradientRequested, this, [this,gradientShape,gradientStyle,gradientReverse,explainRefusal](const QPointF &start, const QPointF &end) {
        explainRefusal();
        previewGradient(start, end, gradientShape->currentIndex() == 1, gradientStyle->currentIndex() == 0,
                        gradientReverse->isChecked(), gradientOpacityField_->value() / 100.0);
    });
    connect(canvas_, &CanvasWidget::gradientCommitRequested, this, [this] { finishGradient(true); });
    connect(canvas_, &CanvasWidget::gradientCancelRequested, this, [this] { finishGradient(false); });
    connect(gradientShape, &SegmentedControl::currentIndexChanged, canvas_, &CanvasWidget::refreshPendingGradient);
    connect(gradientStyle, &SegmentedControl::currentIndexChanged, canvas_, &CanvasWidget::refreshPendingGradient);
    connect(gradientReverse, &QCheckBox::toggled, canvas_, &CanvasWidget::refreshPendingGradient);
    connect(gradientOpacityField_, &QDoubleSpinBox::valueChanged, canvas_, &CanvasWidget::refreshPendingGradient);
    connect(canvas_, &CanvasWidget::shapeCreated, this, [this](ShapeKind kind, const QRectF &rect, double strokeWidth, double cornerRadius, const std::optional<QPointF> &start, const std::optional<QPointF> &end) {
        if (session_.addShape(kind, rect, foregroundColor_, foregroundColor_, strokeWidth, cornerRadius, start, end)) syncDocumentViews();
    });
    const auto applyInlineTextFormat = [this, textFont, textSize, textTracking, textLeading, textBold, textItalic, textUnderline, textAlignment] {
        auto *editor = dynamic_cast<InlineTextEditor *>(canvas_->findChild<QTextEdit *>(QStringLiteral("inlineTextEditor"), Qt::FindDirectChildrenOnly));
        if (!editor) return;
        editor->tracking = textTracking->value();
        editor->leading = textLeading->value();
        QFont font = textFont->currentFont(); font.setPixelSize(std::max(1, qRound(textSize->value() * canvas_->zoom()))); font.setBold(textBold->isChecked()); font.setItalic(textItalic->isChecked()); font.setUnderline(textUnderline->isChecked());
        if (editor->tracking != 0.0) {
            font.setLetterSpacing(QFont::AbsoluteSpacing, editor->tracking * canvas_->zoom());
        }
        const QTextCursor originalCursor = editor->textCursor();
        formatTextDocument(*editor->document(), font, foregroundColor_, textAlignment->currentIndex(), editor->areaText, editor->tracking * canvas_->zoom(), editor->leading * canvas_->zoom());
        editor->setTextCursor(originalCursor);
        editor->setCurrentFont(font);
        editor->setTextColor(foregroundColor_);
        editor->growPointText();
    };
    connect(textFont, &QFontComboBox::currentFontChanged, this, [applyInlineTextFormat](const QFont &) { applyInlineTextFormat(); });
    connect(textSize, &QSpinBox::valueChanged, this, [applyInlineTextFormat](int) { applyInlineTextFormat(); });
    connect(textTracking, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [applyInlineTextFormat](double) { applyInlineTextFormat(); });
    connect(textLeading, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [applyInlineTextFormat](double) { applyInlineTextFormat(); });
    connect(textBold, &QToolButton::toggled, this, [applyInlineTextFormat](bool) { applyInlineTextFormat(); });
    connect(textItalic, &QToolButton::toggled, this, [applyInlineTextFormat](bool) { applyInlineTextFormat(); });
    connect(textUnderline, &QToolButton::toggled, this, [applyInlineTextFormat](bool) { applyInlineTextFormat(); });
    connect(textAlignment, &SegmentedControl::currentIndexChanged, this, [applyInlineTextFormat](int) { applyInlineTextFormat(); });
    const auto beginInlineText = [this, textFont, textSize, textTracking, textLeading, textBold, textItalic, textUnderline, textAlignment, textDone, textCancel, applyInlineTextFormat]
        (const QRectF &box, const QString &initialText, bool areaText, const std::optional<QUuid> &layerId, const QColor &color, double tracking = 0.0, double leading = 0.0) {
        if (auto *existing = dynamic_cast<InlineTextEditor *>(canvas_->findChild<QTextEdit *>(QStringLiteral("inlineTextEditor"), Qt::FindDirectChildrenOnly))) existing->finish(true);
        auto *editor = new InlineTextEditor(canvas_);
        editor->canvasBox = box; editor->areaText = areaText;
        editor->tracking = tracking;
        editor->leading = leading;
        {
            const QSignalBlocker tb(textTracking), lb(textLeading);
            textTracking->setValue(tracking);
            textLeading->setValue(leading);
        }
        editor->trackingAdjusted = [editor, textTracking](double) {
            const QSignalBlocker blocker(textTracking);
            textTracking->setValue(editor->tracking);
        };
        editor->leadingAdjusted = [editor, textLeading](double) {
            const QSignalBlocker blocker(textLeading);
            textLeading->setValue(editor->leading);
        };
        editor->syncCanvasGeometry();
        editor->setTextColor(color);
        editor->setPlainText(initialText);
        editor->setLineWrapMode(areaText ? QTextEdit::FixedPixelWidth : QTextEdit::NoWrap);
        textDone->setEnabled(true); textCancel->setEnabled(true);
        editor->finished = [this, editor, textFont, textSize, textBold, textItalic, textUnderline, textAlignment, textDone, textCancel, box, areaText, layerId](bool commit) {
            textDone->setEnabled(false); textCancel->setEnabled(false);
            canvas_->setTextEditingLayer(std::nullopt);
            if (commit && !editor->toPlainText().trimmed().isEmpty()) {
                // Minimum editor size and rounded screen coordinates must not change layer geometry.
                const QRectF documentBox = editor->canvasBox;
                const bool fixedBox = areaText || editor->wasResized();
                const bool changed = layerId
                    ? session_.updateText(*layerId, editor->toPlainText(), documentBox, textFont->currentFont().family(), textSize->value(), textBold->isChecked(),
                        textItalic->isChecked(), textUnderline->isChecked(), textAlignment->currentIndex(), foregroundColor_, fixedBox, editor->tracking, editor->leading)
                    : session_.addText(editor->toPlainText(), documentBox, textFont->currentFont().family(), textSize->value(), textBold->isChecked(),
                        textItalic->isChecked(), textUnderline->isChecked(), textAlignment->currentIndex(), foregroundColor_, fixedBox, editor->tracking, editor->leading);
                if (changed) { syncDocumentViews(); return; }
            }
            canvas_->invalidateDocument(); syncDocumentViews(false);
        };
        QTextCursor cursor = editor->textCursor(); cursor.movePosition(QTextCursor::End); editor->setTextCursor(cursor);
        editor->show(); editor->raise(); applyInlineTextFormat(); editor->setFocus(Qt::MouseFocusReason);
        connect(editor, &QTextEdit::textChanged, editor, [editor] { editor->growPointText(); });
        connect(canvas_, &CanvasWidget::zoomChanged, editor, [editor, applyInlineTextFormat](double) {
            editor->syncCanvasGeometry(); applyInlineTextFormat();
        });
        editor->document()->clearUndoRedoStacks();
    };
    connect(canvas_, &CanvasWidget::textBoxRequested, this, [this, beginInlineText](const QRectF &box, bool areaText) {
        beginInlineText(box, QString(), areaText, std::nullopt, foregroundColor_);
    });
    connect(canvas_, &CanvasWidget::textLayerEditRequested, this, [this, textFont, textSize, textTracking, textLeading, textBold, textItalic, textUnderline, textAlignment, beginInlineText](const QUuid &id) {
        // Commit using the previous layer's controls before loading the next layer's style.
        if (auto *existing = dynamic_cast<InlineTextEditor *>(canvas_->findChild<QTextEdit *>(QStringLiteral("inlineTextEditor"), Qt::FindDirectChildrenOnly))) existing->finish(true);
        if (!document_) return;
        auto it = std::find_if(document_->layers.begin(), document_->layers.end(), [&id](const Layer &layer) { return layer.id == id; });
        if (it == document_->layers.end()) return;
        const QJsonObject metadata = it->shape;
        const QSignalBlocker fontBlock(textFont), sizeBlock(textSize), trackingBlock(textTracking), leadingBlock(textLeading), boldBlock(textBold), italicBlock(textItalic), underlineBlock(textUnderline), alignmentBlock(textAlignment);
        QString textContent;
        QString fontName = QStringLiteral("Helvetica");
        int pixelSize = 48;
        bool bold = false;
        bool italic = false;
        bool underline = false;
        int align = 0;
        QColor color = foregroundColor_;
        bool areaText = false;
        double tracking = 0.0;
        double leading = 0.0;

        if (it->text.has_value()) {
            textContent = it->text->content;
            fontName = it->text->fontName;
            pixelSize = qRound(it->text->fontSize);
            color = QColor::fromRgbF(it->text->red, it->text->green, it->text->blue);
            if (it->text->alignment == TextAlignment::Center) align = 1;
            else if (it->text->alignment == TextAlignment::Right) align = 2;
            else align = 0;
            areaText = it->text->boxSize.has_value();
            tracking = it->text->tracking;
            leading = it->text->leading;
        } else {
            textContent = metadata.value(QStringLiteral("text")).toString();
            fontName = metadata.value(QStringLiteral("fontFamily")).toString(QStringLiteral("Helvetica"));
            pixelSize = metadata.value(QStringLiteral("pixelSize")).toInt(48);
            bold = metadata.value(QStringLiteral("bold")).toBool();
            italic = metadata.value(QStringLiteral("italic")).toBool();
            underline = metadata.value(QStringLiteral("underline")).toBool();
            align = std::clamp(metadata.value(QStringLiteral("alignment")).toInt(), 0, 2);
            const QColor c(metadata.value(QStringLiteral("fill")).toString());
            if (c.isValid()) color = c;
            areaText = metadata.value(QStringLiteral("areaText")).toBool();
        }

        textFont->setCurrentFont(QFont(fontName));
        textSize->setValue(pixelSize);
        textTracking->setValue(tracking);
        textLeading->setValue(leading);
        textBold->setChecked(bold); textItalic->setChecked(italic);
        textUnderline->setChecked(underline); textAlignment->setCurrentIndex(align);
        if (color.isValid()) {
            foregroundColor_ = color;
            canvas_->setPaletteForeground(color);
            foregroundSwatch_->setStyleSheet(QStringLiteral("background:%1;border:1px solid white;border-radius:4px;").arg(color.name(QColor::HexRgb)));
        }
        const QRectF box(it->transform.origin, it->transform.size);
        session_.selectLayer(id); canvas_->setTextEditingLayer(id);
        beginInlineText(box, textContent, areaText, id, color.isValid() ? color : foregroundColor_, tracking, leading);
    });
    connect(textDone, &QPushButton::clicked, this, [this] { if (auto *editor = dynamic_cast<InlineTextEditor *>(canvas_->findChild<QTextEdit *>(QStringLiteral("inlineTextEditor"), Qt::FindDirectChildrenOnly))) editor->finish(true); });
    connect(textCancel, &QPushButton::clicked, this, [this] { if (auto *editor = dynamic_cast<InlineTextEditor *>(canvas_->findChild<QTextEdit *>(QStringLiteral("inlineTextEditor"), Qt::FindDirectChildrenOnly))) editor->finish(false); });
    connect(canvas_, &CanvasWidget::colorSampleRequested, this, [this](const QPoint &point) {
        if (colorSampleOverride_) { colorSampleOverride_(point); return; }
        if (!document_ || !QRect(QPoint(), document_->canvasSize).contains(point)) return;
        const QColor sampled = LayerRenderer::flattened(*document_).pixelColor(point);
        if (!sampled.isValid() || sampled.alpha() == 0) return;
        foregroundColor_ = sampled;
        canvas_->setPaletteForeground(sampled);
        foregroundSwatch_->setStyleSheet(QStringLiteral("background:%1;border:1px solid white;border-radius:4px;").arg(sampled.name(QColor::HexRgb)));
    });
    canvas_->setPaletteForeground(foregroundColor_);
    createActions();
    quickFileMenu_ = new QMenu(menuRestoreButton_);
    quickFileMenu_->setObjectName(QStringLiteral("quickFileMenu"));
    quickFileMenu_->setTitle(tr("Application menu"));
    if (QAction *open = findChild<QAction *>(QStringLiteral("commandOpen"))) quickFileMenu_->addAction(open);
    quickFileMenu_->addSeparator();
    if (QAction *save = findChild<QAction *>(QStringLiteral("commandSave"))) quickFileMenu_->addAction(save);
    if (QAction *saveAs = findChild<QAction *>(QStringLiteral("commandSaveAs"))) quickFileMenu_->addAction(saveAs);
    auto *quickExportMenu = quickFileMenu_->addMenu(tr("Export"));
    quickExportMenu->setObjectName(QStringLiteral("quickExportMenu"));
    if (QAction *png = findChild<QAction *>(QStringLiteral("commandExportPng"))) quickExportMenu->addAction(png);
    if (QAction *jpeg = findChild<QAction *>(QStringLiteral("commandExportJpeg"))) quickExportMenu->addAction(jpeg);
    quickFileMenu_->addSeparator();
    // Reuse the same menus and actions so shortcuts, enabled states, and dynamic
    // command labels remain identical with the menu bar shown or hidden.
    for (QAction *category : menuBar()->actions())
        if (QMenu *menu = category->menu()) quickFileMenu_->addMenu(menu);
    quickFileMenu_->addSeparator();
    if (QAction *showMenuBar = findChild<QAction *>(QStringLiteral("showMenuBar"))) {
        quickFileMenu_->addAction(showMenuBar);
        connect(menuRestoreButton_, &QToolButton::clicked, showMenuBar, [showMenuBar] { showMenuBar->toggle(); });
    }
    menuRestoreButton_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(menuRestoreButton_, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        Q_UNUSED(pos);
        if (isMenuBarVisible()) return;
        if (quickFileMenu_) {
            quickFileMenu_->popup(menuRestoreButton_->mapToGlobal(QPoint(0, menuRestoreButton_->height() + 4)));
        }
    });
    qApp->installEventFilter(this);
    connect(qApp, &QApplication::focusChanged, this, [this] { updateCommandStates(); });
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this] { updateCommandStates(); });
    statusBar()->hide();
    refreshTitle();
    updateInspector();
    autosaveTimer_ = new QTimer(this);
    autosaveTimer_->setInterval(60000);
    connect(autosaveTimer_, &QTimer::timeout, this, &MainWindow::autosave);
    autosaveTimer_->start();
    QTimer::singleShot(0, this, &MainWindow::offerRecovery);
}

MainWindow::~MainWindow()
{
    if (qApp) {
        qApp->removeEventFilter(this);
        QObject::disconnect(qApp, nullptr, this, nullptr);
    }
    if (auto *cb = QGuiApplication::clipboard()) {
        QObject::disconnect(cb, nullptr, this, nullptr);
    }
}

void MainWindow::beginPersistentTransform(const QString &name)
{
    if (!document_ || transformOriginalDocument_) return;
    transformOriginalDocument_ = *document_;
    session_.beginEdit(name);
    canvas_->setPersistentTransform(true);
    updateTransformButtons();
}

void MainWindow::finishPersistentTransform(bool apply)
{
    if (!transformOriginalDocument_ || !document_) return;
    if (!apply) *document_ = *transformOriginalDocument_;
    session_.endEdit();
    transformOriginalDocument_.reset();
    canvas_->setPersistentTransform(false);
    updateTransformButtons();
    syncDocumentViews();
}

void MainWindow::updateTransformButtons()
{
    const bool enabled = canvasPendingTransform_ || transformOriginalDocument_.has_value();
    if (transformApply_) transformApply_->setEnabled(enabled);
    if (transformCancel_) transformCancel_->setEnabled(enabled);
}

void MainWindow::previewGradient(const QPointF &start, const QPointF &end, bool radial,
                                 bool foregroundTransparent, bool reversed, double opacity)
{
    Layer *active = session_.activeLayer();
    if (!document_ || !active || active->group) return;
    const bool mask = session_.isMaskSelected();
    if (gradientTarget_ && (*gradientTarget_ != active->id || gradientMaskTarget_ != mask)) finishGradient(true);
    if (!gradientTarget_) {
        gradientTarget_ = active->id;
        gradientMaskTarget_ = mask;
        gradientOriginal_ = *active;
        session_.beginEdit(mask ? tr("Gradient Mask") : tr("Gradient"));
    }
    auto it = std::find_if(document_->layers.begin(), document_->layers.end(), [this](const Layer &layer) {
        return gradientTarget_ && layer.id == *gradientTarget_;
    });
    if (it == document_->layers.end() || !gradientOriginal_) { finishGradient(false); return; }
    *it = *gradientOriginal_;
    document_->activeLayerId = it->id;
    session_.applyGradient(start, end, foregroundColor_, backgroundColor_, radial,
                           foregroundTransparent, reversed, opacity);
    syncDocumentViews();
}

void MainWindow::finishGradient(bool commit)
{
    if (!gradientTarget_) return;
    if (!commit && document_ && gradientOriginal_) {
        const auto it = std::find_if(document_->layers.begin(), document_->layers.end(), [this](const Layer &layer) {
            return layer.id == *gradientTarget_;
        });
        if (it != document_->layers.end()) *it = *gradientOriginal_;
    }
    session_.endEdit();
    gradientOriginal_.reset();
    gradientTarget_.reset();
    syncDocumentViews();
}

void MainWindow::cycleSmearMode()
{
    if (!smearMode_) return;
    const int count = smearMode_->count();
    if (count > 0) {
        smearMode_->setCurrentIndex((smearMode_->currentIndex() + 1) % count);
        updateSmearStatusHint();
    }
}

void MainWindow::cycleToolMode()
{
    if (!canvas_) return;
    switch (canvas_->tool()) {
    case CanvasWidget::Tool::Marquee:
        canvas_->toggleMarqueeKind();
        break;
    case CanvasWidget::Tool::Wand:
        canvas_->toggleWandMode();
        break;
    case CanvasWidget::Tool::Lasso:
        canvas_->toggleLassoKind();
        break;
    case CanvasWidget::Tool::Shape:
        canvas_->toggleShapeKind();
        break;
    case CanvasWidget::Tool::Blur:
        cycleSmearMode();
        break;
    default:
        break;
    }
}

void MainWindow::updateBlurRadiusVisibility()
{
    // Blur has a Radius of its own, apart from Strength; Liquify and Smudge have none.
    const bool visible = canvas_ && smearMode_ && canvas_->tool() == CanvasWidget::Tool::Blur && smearMode_->currentIndex() == 1;
    blurRadiusLabel_->setVisible(visible); blurRadiusField_->setVisible(visible);
}

void MainWindow::updateSmearStatusHint()
{
    if (!statusHint_ || !canvas_ || canvas_->tool() != CanvasWidget::Tool::Blur) return;
    const int mode = smearMode_ ? smearMode_->currentIndex() : 0;
    const QString actionHint = (mode == 1) ? tr("Drag to soften")
                             : (mode == 2) ? tr("Drag to smudge")
                             : tr("Drag to push pixels");
    statusHint_->setText(tr("%1 · [ ] size · Shift-[ ] hardness · 1–0 strength · Space to pan").arg(actionHint));
}

void MainWindow::openColorPicker(bool background)
{
    const QColor startingColor = background ? backgroundColor_ : foregroundColor_;
    if (colorPicker_) {
        colorPickerBackground_ = background;
        colorPicker_->setWindowTitle(background ? tr("Background Color") : tr("Foreground Color"));
        colorPicker_->setCurrentColor(startingColor);
        colorPicker_->raise();
        colorPicker_->activateWindow();
        return;
    }

    auto *picker = new QColorDialog(startingColor, this);
    picker->setObjectName(QStringLiteral("paletteColorPicker"));
    picker->setWindowTitle(background ? tr("Background Color") : tr("Foreground Color"));
    picker->setOption(QColorDialog::ShowAlphaChannel, false);
    picker->setOption(QColorDialog::DontUseNativeDialog, true);
    picker->setAttribute(Qt::WA_DeleteOnClose);
    picker->setWindowModality(Qt::NonModal);
    attachColorDialogScrubbing(picker);
    colorPicker_ = picker;
    colorPickerBackground_ = background;
    colorPickerPreviousTool_ = int(canvas_->tool());
    canvas_->setTool(CanvasWidget::Tool::Eyedropper);

    // Keep edits provisional until OK. While the non-modal picker is open the
    // canvas remains clickable and feeds its composited color into the picker.
    colorSampleOverride_ = [this](const QPoint &point) {
        if (!colorPicker_ || !document_ || !QRect(QPoint(), document_->canvasSize).contains(point)) return;
        const QColor sampled = LayerRenderer::flattened(*document_).pixelColor(point);
        if (sampled.isValid() && sampled.alpha() != 0) colorPicker_->setCurrentColor(sampled.toRgb());
    };
    connect(picker, &QColorDialog::finished, this, [this, picker](int result) {
        colorPickerPosition_ = picker->pos();
        if (result == QDialog::Accepted) {
            const QColor color = picker->currentColor().toRgb();
            QColor &target = colorPickerBackground_ ? backgroundColor_ : foregroundColor_;
            QLabel *swatch = colorPickerBackground_ ? backgroundSwatch_ : foregroundSwatch_;
            target = color;
            swatch->setStyleSheet(QStringLiteral("background:%1;border:1px solid white;border-radius:4px;").arg(color.name(QColor::HexRgb)));
            canvas_->refreshPendingGradient();
        }
        colorSampleOverride_ = {};
        if (colorPickerPreviousTool_ >= 0) canvas_->setTool(CanvasWidget::Tool(colorPickerPreviousTool_));
        colorPickerPreviousTool_ = -1;
        colorPicker_.clear();
    });
    picker->show();
    if (colorPickerPosition_) picker->move(*colorPickerPosition_);
}

void MainWindow::createActions()
{
    // Let the themed font and padding determine the bar's height.
    menuBar()->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *file = menuBar()->addMenu(tr("&File"));
    auto *newAction = file->addAction(tr("&New Canvas…"));
    newAction->setShortcut(QKeySequence::New);
    connect(newAction, &QAction::triggered, this, &MainWindow::newProject);
    auto *open = file->addAction(tr("&Open Project…"));
    open->setObjectName(QStringLiteral("commandOpen"));
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, &MainWindow::chooseProject);
    auto *importAction = file->addAction(tr("&Import Images…"));
    connect(importAction, &QAction::triggered, this, &MainWindow::importImages);
    auto *save = file->addAction(tr("&Save"));
    save->setObjectName(QStringLiteral("commandSave"));
    save->setShortcut(QKeySequence::Save);
    connect(save, &QAction::triggered, this, [this]() { saveProject(false); });
    auto *saveAs = file->addAction(tr("Save &As…"));
    saveAs->setObjectName(QStringLiteral("commandSaveAs"));
    saveAs->setShortcut(QKeySequence::SaveAs);
    connect(saveAs, &QAction::triggered, this, [this]() { saveProjectAs(false); });
    auto *exportAction = file->addAction(tr("Export &PNG…"));
    exportAction->setObjectName(QStringLiteral("commandExportPng"));
    exportAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    connect(exportAction, &QAction::triggered, this, [this] { exportPng(false); });
    auto *exportJpegAction = file->addAction(tr("Export &JPEG…"));
    exportJpegAction->setObjectName(QStringLiteral("commandExportJpeg"));
    exportJpegAction->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_S));
    connect(exportJpegAction, &QAction::triggered, this, [this] { exportPng(true); });
    file->addSeparator();
    auto *closeProject = file->addAction(tr("&Close Project"));
    closeProject->setShortcut(QKeySequence::Close);
    connect(closeProject, &QAction::triggered, this, [this] { closeTab(currentTab_); });
    file->addSeparator();
    auto *quit = file->addAction(tr("&Quit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);

    auto *edit = menuBar()->addMenu(tr("&Edit"));
    auto *undo = edit->addAction(tr("&Undo")); undo->setShortcut(QKeySequence::Undo);
    auto *redo = edit->addAction(tr("&Redo")); redo->setShortcut(QKeySequence::Redo);
    undo->setObjectName(QStringLiteral("commandUndo")); redo->setObjectName(QStringLiteral("commandRedo"));
    connect(undo, &QAction::triggered, this, [this] { if (dispatchTextEditCommand(TextEditCommand::Undo)) return; session_.undo(); syncDocumentViews(); });
    connect(redo, &QAction::triggered, this, [this] { if (dispatchTextEditCommand(TextEditCommand::Redo)) return; session_.redo(); syncDocumentViews(); });
    edit->addSeparator();
    auto *cut = edit->addAction(tr("Cu&t")); cut->setShortcut(QKeySequence::Cut);
    auto *copy = edit->addAction(tr("&Copy")); copy->setShortcut(QKeySequence::Copy);
    auto *copyMerged = edit->addAction(tr("Copy Merged")); copyMerged->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    auto *paste = edit->addAction(tr("&Paste")); paste->setShortcut(QKeySequence::Paste);
    cut->setObjectName(QStringLiteral("commandCut")); copy->setObjectName(QStringLiteral("commandCopy")); copyMerged->setObjectName(QStringLiteral("commandCopyMerged")); paste->setObjectName(QStringLiteral("commandPaste"));
    connect(cut, &QAction::triggered, this, [this] { if (!dispatchTextEditCommand(TextEditCommand::Cut)) cutPixels(); });
    connect(copy, &QAction::triggered, this, [this] { if (!dispatchTextEditCommand(TextEditCommand::Copy)) copyPixels(false); });
    connect(copyMerged, &QAction::triggered, this, [this] { copyPixels(true); });
    connect(paste, &QAction::triggered, this, [this] { if (!dispatchTextEditCommand(TextEditCommand::Paste)) pastePixels(); });
    auto *keyboardShortcuts = edit->addAction(tr("Keyboard Shortcuts…"));
    keyboardShortcuts->setObjectName(QStringLiteral("commandKeyboardShortcuts"));
    connect(keyboardShortcuts, &QAction::triggered, this, &MainWindow::keyboardShortcutsDialog);
    edit->addSeparator();
    auto *duplicate = edit->addAction(tr("Duplicate Layer / Layer via Copy")); duplicate->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_J));
    auto *remove = edit->addAction(tr("Delete Layer")); remove->setShortcut(QKeySequence::Delete);
    duplicate->setObjectName(QStringLiteral("commandDuplicate")); remove->setObjectName(QStringLiteral("commandDelete"));
    connect(duplicate, &QAction::triggered, this, &MainWindow::layerViaCopy);
    connect(remove, &QAction::triggered, this, [this] {
        if (dispatchTextEditCommand(TextEditCommand::Delete)) return;
        if (document_ && document_->selection) session_.clearSelectedPixels();
        else if (session_.isMaskSelected()) session_.deleteLayerMask();
        else { deleteLayersWithMaskChoice(); return; }
        syncDocumentViews();
    });
    auto *fill = edit->addAction(tr("Fill…")); fill->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F5));
    connect(fill, &QAction::triggered, this, [this] {
        const QColor color = QColorDialog::getColor(QColor(22, 134, 232), this, tr("Fill Color"), QColorDialog::ShowAlphaChannel);
        if (color.isValid() && session_.fillSelection(color)) syncDocumentViews();
    });
    auto *fillForeground = new QAction(tr("Fill with Foreground Color"), this); fillForeground->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Backspace));
    auto *fillBackground = new QAction(tr("Fill with Background Color"), this); fillBackground->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Backspace));
    connect(fillForeground, &QAction::triggered, this, [this] { if (dispatchTextEditCommand(TextEditCommand::DeleteWordBackward)) return; if (session_.fillSelection(foregroundColor_)) syncDocumentViews(); });
    connect(fillBackground, &QAction::triggered, this, [this] { if (dispatchTextEditCommand(TextEditCommand::DeleteToBeginning)) return; if (session_.fillSelection(backgroundColor_)) syncDocumentViews(); });
    edit->addAction(fillForeground);
    edit->addAction(fillBackground);
    auto *clearPixels = edit->addAction(tr("Clear Selection Pixels"));
    connect(clearPixels, &QAction::triggered, this, [this] { if (session_.clearSelectedPixels()) syncDocumentViews(); });
    auto *transformSelection = edit->addAction(tr("Free Transform"));
    transformSelection->setObjectName(QStringLiteral("commandTransform"));
    transformSelection->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
    connect(transformSelection, &QAction::triggered, this, [this] {
        if (session_.hasFloatingSelection()) session_.commitSelectionTransform();
        else if (session_.beginSelectionTransform()) canvas_->setTool(CanvasWidget::Tool::Move);
        syncDocumentViews();
    });

    auto *layerMenuActions = menuBar()->addMenu(tr("&Layer"));
    auto *newLayerAction = layerMenuActions->addAction(tr("New &Layer")); newLayerAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    auto *newGroupAction = layerMenuActions->addAction(tr("New &Folder"));
    newLayerAction->setObjectName(QStringLiteral("commandNewLayer")); newGroupAction->setObjectName(QStringLiteral("commandNewFolder"));
    connect(newLayerAction, &QAction::triggered, this, [this] { session_.addBlankLayer(); syncDocumentViews(); });
    connect(newGroupAction, &QAction::triggered, this, [this] { session_.addGroup(); syncDocumentViews(); });
    auto *groupLayers = layerMenuActions->addAction(tr("Group Selected Layers")); groupLayers->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    auto *ungroupLayers = layerMenuActions->addAction(tr("Ungroup Layers")); ungroupLayers->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G));
    ungroupLayers->setObjectName(QStringLiteral("commandUngroupLayers"));
    connect(ungroupLayers, &QAction::triggered, this, [this] { session_.ungroupLayers(); syncDocumentViews(); });
    auto *moveOut = layerMenuActions->addAction(tr("Move Out of Folder"));
    auto *renameLayer = layerMenuActions->addAction(tr("Rename Layer…"));
    auto *toggleVisibility = layerMenuActions->addAction(tr("Show/Hide Layer"));
    groupLayers->setObjectName(QStringLiteral("commandGroupLayers")); moveOut->setObjectName(QStringLiteral("commandMoveOut"));
    renameLayer->setObjectName(QStringLiteral("commandRenameLayer")); toggleVisibility->setObjectName(QStringLiteral("commandVisibility"));
    connect(groupLayers, &QAction::triggered, this, [this] { session_.groupSelectedLayers(); syncDocumentViews(); });
    connect(moveOut, &QAction::triggered, this, [this] { if (session_.moveActiveLayerOutOfGroup()) syncDocumentViews(); });
    connect(renameLayer, &QAction::triggered, this, [this] { if (layerView_->currentIndex().isValid()) layerView_->edit(layerView_->currentIndex()); });
    connect(toggleVisibility, &QAction::triggered, this, [this] { Layer *layer=session_.activeLayer();if(layer){session_.toggleLayerVisibility(layer->id);syncDocumentViews();} });
    layerMenuActions->addSeparator();
    auto *merge = layerMenuActions->addAction(tr("Merge Layers")); merge->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    merge->setObjectName(QStringLiteral("commandMerge"));
    connect(merge, &QAction::triggered, this, [this] { if (session_.mergeLayers()) syncDocumentViews(); });
    auto *moveUp = layerMenuActions->addAction(tr("Move Layer Up")); moveUp->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_BracketRight));
    auto *moveDown = layerMenuActions->addAction(tr("Move Layer Down")); moveDown->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_BracketLeft));
    moveUp->setObjectName(QStringLiteral("commandMoveUp")); moveDown->setObjectName(QStringLiteral("commandMoveDown"));
    connect(moveUp, &QAction::triggered, this, [this] { if(session_.canMoveActiveLayer(1)){session_.moveActiveLayer(1);syncDocumentViews();} });
    connect(moveDown, &QAction::triggered, this, [this] { if(session_.canMoveActiveLayer(-1)){session_.moveActiveLayer(-1);syncDocumentViews();} });
    auto *flipLayerH = layerMenuActions->addAction(tr("Flip Layer Horizontal")); auto *flipLayerV = layerMenuActions->addAction(tr("Flip Layer Vertical"));
    connect(flipLayerH, &QAction::triggered, this, [this] { if(session_.flipLayers(true))syncDocumentViews(); });
    connect(flipLayerV, &QAction::triggered, this, [this] { if(session_.flipLayers(false))syncDocumentViews(); });
    auto *clipping = layerMenuActions->addAction(tr("Create/Release Clipping Mask"));
    clipping->setObjectName(QStringLiteral("commandClipping"));
    clipping->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_G));
    connect(clipping, &QAction::triggered, this, [this] {
        if (document_ && document_->activeLayerId && session_.toggleClippingMask(*document_->activeLayerId)) syncDocumentViews();
    });
    auto *maskMenu = layerMenuActions->addMenu(tr("Layer Mask"));
    auto *revealMask = maskMenu->addAction(tr("Reveal All"));
    auto *hideMask = maskMenu->addAction(tr("Hide All"));
    auto *revealSelectionMask = maskMenu->addAction(tr("Reveal Selection"));
    auto *hideSelectionMask = maskMenu->addAction(tr("Hide Selection"));
    auto *toggleMask = maskMenu->addAction(tr("Enable/Disable"));
    auto *linkMask = maskMenu->addAction(tr("Link/Unlink from Layer"));
    auto *invertMask = maskMenu->addAction(tr("Invert Mask"));
    auto *loadMask = maskMenu->addAction(tr("Load as Selection"));
    auto *deleteMask = maskMenu->addAction(tr("Delete Mask"));
    connect(revealMask, &QAction::triggered, this, [this] { if (session_.addLayerMask(true, false)) syncDocumentViews(); });
    connect(hideMask, &QAction::triggered, this, [this] { if (session_.addLayerMask(false, false)) syncDocumentViews(); });
    connect(revealSelectionMask, &QAction::triggered, this, [this] { if (session_.addLayerMask(true, true)) syncDocumentViews(); });
    connect(hideSelectionMask, &QAction::triggered, this, [this] { if (session_.addLayerMask(false, true)) syncDocumentViews(); });
    connect(toggleMask, &QAction::triggered, this, [this] { if (session_.toggleLayerMask()) syncDocumentViews(); });
    connect(linkMask, &QAction::triggered, this, [this] { if (session_.toggleMaskLink()) syncDocumentViews(); });
    connect(invertMask, &QAction::triggered, this, [this] { if (session_.invertLayerMask()) syncDocumentViews(); });
    connect(loadMask, &QAction::triggered, this, [this] { if (session_.loadMaskAsSelection()) syncDocumentViews(false); });
    connect(deleteMask, &QAction::triggered, this, [this] { if (session_.deleteLayerMask()) syncDocumentViews(); });
    auto *adjustments = new QMenu(tr("New Adjustment Layer"), this);
    adjustments->menuAction()->setObjectName(QStringLiteral("commandNewAdjustment"));
    auto rebuildAdjustmentsMenu = [this, adjustments] {
        adjustments->clear();
        static const QStringList allKinds{
            QStringLiteral("Hue/Saturation"), QStringLiteral("Levels"), QStringLiteral("Curves"),
            QStringLiteral("Exposure"), QStringLiteral("Gradient Map"), QStringLiteral("Grain"),
            QStringLiteral("Invert"), QStringLiteral("Black & White"), QStringLiteral("Color Balance"),
            QStringLiteral("Gaussian Blur"), QStringLiteral("Motion Blur"), QStringLiteral("Add Noise")
        };
        for (const QString &kind : allKinds) {
            if (!session_.canSaveAdjustment(kind)) continue;
            auto *act = adjustments->addAction(kind);
            connect(act, &QAction::triggered, this, [this, kind] {
                QJsonObject settings;
                if (kind == QStringLiteral("Gradient Map")) {
                    const auto color = [](const QColor &value) {
                        return QJsonObject{{QStringLiteral("red"), value.redF()},
                            {QStringLiteral("green"), value.greenF()}, {QStringLiteral("blue"), value.blueF()}};
                    };
                    settings.insert(QStringLiteral("gradientMapSettings"), QJsonObject{
                        {QStringLiteral("shadows"), color(foregroundColor_)},
                        {QStringLiteral("highlights"), color(backgroundColor_)},
                        {QStringLiteral("reversed"), false}
                    });
                }
                if (!session_.addAdjustment(kind, settings)) return;
                syncDocumentViews();
                if (kind == QStringLiteral("Hue/Saturation")) hueSaturationDialog();
                else if (kind == QStringLiteral("Levels")) levelsDialog();
                else if (kind == QStringLiteral("Curves")) curvesDialog();
                else if (kind == QStringLiteral("Exposure")) exposureDialog();
                else if (kind == QStringLiteral("Gradient Map")) gradientMapDialog();
                else if (kind == QStringLiteral("Grain")) grainDialog();
                else if (kind == QStringLiteral("Black & White")) blackWhiteDialog();
                else if (kind == QStringLiteral("Color Balance")) colorBalanceDialog();
                else if (kind == QStringLiteral("Gaussian Blur")) gaussianBlurDialog();
                else if (kind == QStringLiteral("Motion Blur")) motionBlurDialog();
                else if (kind == QStringLiteral("Add Noise")) addNoiseDialog();
            });
        }
    };
    connect(adjustments, &QMenu::aboutToShow, this, rebuildAdjustmentsMenu);
    rebuildAdjustmentsMenu();
    layerMenuActions->addMenu(adjustments);
    auto *editAdjustment = layerMenuActions->addAction(tr("Edit Adjustment…"));
    editAdjustment->setObjectName(QStringLiteral("commandEditAdjustment"));
    connect(editAdjustment, &QAction::triggered, this, [this] {
        const Layer *layer = session_.activeLayer(); if (!layer || layer->adjustment.isEmpty()) return;
        const QString kind = layer->adjustment.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("Hue/Saturation")) hueSaturationDialog();
        else if (kind == QStringLiteral("Levels")) levelsDialog();
        else if (kind == QStringLiteral("Curves")) curvesDialog();
        else if (kind == QStringLiteral("Exposure")) exposureDialog();
        else if (kind == QStringLiteral("Gradient Map")) gradientMapDialog();
        else if (kind == QStringLiteral("Grain")) grainDialog();
        else if (kind == QStringLiteral("Black & White")) blackWhiteDialog();
        else if (kind == QStringLiteral("Color Balance")) colorBalanceDialog();
        else if (kind == QStringLiteral("Gaussian Blur")) gaussianBlurDialog();
        else if (kind == QStringLiteral("Motion Blur")) motionBlurDialog();
        else if (kind == QStringLiteral("Add Noise")) addNoiseDialog();
    });
    layerMenuActions->addSeparator();
    auto *layerEffects = layerMenuActions->addAction(tr("Layer Effects…"));
    layerEffects->setObjectName(QStringLiteral("commandLayerEffects"));
    connect(layerEffects, &QAction::triggered, this, [this]() { layerEffectsDialog(); });
    layerMenuActions->addAction(transformSelection);
    layerMenuButton_->setMenu(adjustments);

    auto *select = menuBar()->addMenu(tr("&Select"));
    auto *selectAll = select->addAction(tr("Select &All")); selectAll->setShortcut(QKeySequence::SelectAll);
    auto *deselect = select->addAction(tr("&Deselect")); deselect->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    auto *inverse = select->addAction(tr("&Inverse")); inverse->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I));
    auto *selectSubject = select->addAction(tr("Select &Subject")); selectSubject->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_A));
    selectAll->setObjectName(QStringLiteral("commandSelectAll")); deselect->setObjectName(QStringLiteral("commandDeselect"));
    inverse->setObjectName(QStringLiteral("commandInverseSelection")); selectSubject->setObjectName(QStringLiteral("commandSelectSubject"));
    connect(selectAll, &QAction::triggered, this, [this] { if (dispatchTextEditCommand(TextEditCommand::SelectAll)) return; session_.selectAll(); syncDocumentViews(false); });
    connect(deselect, &QAction::triggered, this, [this] { session_.deselect(); syncDocumentViews(false); });
    connect(inverse, &QAction::triggered, this, [this] { session_.invertSelection(); syncDocumentViews(false); });
    connect(selectSubject, &QAction::triggered, this, [this] { selectSubjectAction(); });
    auto *layerPixels = select->addAction(tr("Layer's Pixels"));
    auto *maskPixels = select->addAction(tr("Mask's Black Areas"));
    connect(layerPixels, &QAction::triggered, this, [this] { if (session_.loadLayerAsSelection()) syncDocumentViews(false); });
    connect(maskPixels, &QAction::triggered, this, [this] { if (session_.loadMaskAsSelection()) syncDocumentViews(false); });
    select->addSeparator();
    auto *colorRange = select->addAction(tr("Color Range…")); colorRange->setObjectName(QStringLiteral("commandColorRange"));
    connect(colorRange, &QAction::triggered, this, [this] { colorRangeDialog(); });
    auto *expand = select->addAction(tr("Expand…"));
    auto *contract = select->addAction(tr("Contract…"));
    auto *feather = select->addAction(tr("Feather…")); feather->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F6));
    feather->setObjectName(QStringLiteral("commandFeatherSelection"));
    connect(expand, &QAction::triggered, this, [this] {
        bool ok = false; const int amount = QInputDialog::getInt(this, tr("Expand Selection"), tr("Pixels"), 1, 1, 500, 1, &ok);
        if (ok && session_.expandSelection(amount)) syncDocumentViews(false);
    });
    connect(contract, &QAction::triggered, this, [this] {
        bool ok = false; const int amount = QInputDialog::getInt(this, tr("Contract Selection"), tr("Pixels"), 1, 1, 500, 1, &ok);
        if (ok && session_.contractSelection(amount)) syncDocumentViews(false);
    });
    connect(feather, &QAction::triggered, this, [this] {
        bool ok = false;
        const int amount = QInputDialog::getInt(this, tr("Feather Selection"), tr("Feather Radius (pixels):"), session_.selectionFeatherAmount(), 1, 250, 1, &ok);
        if (ok && session_.featherSelection(amount)) {
            session_.setSelectionFeatherAmount(amount);
            syncDocumentViews(false);
        }
    });

    auto *image = menuBar()->addMenu(tr("&Image"));
    auto *levels = image->addAction(tr("Levels…")); levels->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    auto *exposure = image->addAction(tr("Exposure…"));
    auto *hueSaturation = image->addAction(tr("Hue/Saturation…")); hueSaturation->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_U));
    auto *curves = image->addAction(tr("Curves…")); curves->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    auto *gradientMap = image->addAction(tr("Gradient Map…"));
    auto *grain = image->addAction(tr("Grain…"));
    connect(levels, &QAction::triggered, this, &MainWindow::levelsDialog);
    connect(exposure, &QAction::triggered, this, &MainWindow::exposureDialog);
    connect(hueSaturation, &QAction::triggered, this, &MainWindow::hueSaturationDialog);
    connect(curves, &QAction::triggered, this, &MainWindow::curvesDialog);
    connect(gradientMap, &QAction::triggered, this, &MainWindow::gradientMapDialog);
    connect(grain, &QAction::triggered, this, &MainWindow::grainDialog);
    image->addSeparator();
    auto *imageSize = image->addAction(tr("Image Size…"));
    imageSize->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_I));
    auto *canvasSize = image->addAction(tr("Canvas Size…"));
    canvasSize->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_C));
    auto *cropAction = image->addAction(tr("Crop…"));
    cropAction->setObjectName(QStringLiteral("imageCropAction"));
    auto *trimAction = image->addAction(tr("Trim…"));
    trimAction->setObjectName(QStringLiteral("imageTrimAction"));
    connect(imageSize, &QAction::triggered, this, &MainWindow::resizeImageDialog);
    connect(canvasSize, &QAction::triggered, this, &MainWindow::resizeCanvasDialog);
    connect(cropAction, &QAction::triggered, this, &MainWindow::cropDialog);
    connect(trimAction, &QAction::triggered, this, &MainWindow::trimDialog);
    image->addSeparator();
    auto *flipCanvasH = image->addAction(tr("Flip Canvas Horizontal"));
    auto *flipCanvasV = image->addAction(tr("Flip Canvas Vertical"));
    connect(flipCanvasH, &QAction::triggered, this, [this] { if (session_.flipCanvas(true)) syncDocumentViews(); });
    connect(flipCanvasV, &QAction::triggered, this, [this] { if (session_.flipCanvas(false)) syncDocumentViews(); });
    auto *flatten = image->addAction(tr("Flatten Image"));
    connect(flatten, &QAction::triggered, this, [this] { if (session_.flattenImage()) syncDocumentViews(); });
    image->addSeparator();
    auto *invert = image->addAction(tr("&Invert")); invert->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
    connect(invert, &QAction::triggered, this, [this] { if (session_.invertActiveLayerPixels()) syncDocumentViews(); });
    auto *blackWhite = image->addAction(tr("Black & White…"));
    auto *colorBalance = image->addAction(tr("Color Balance…"));
    connect(blackWhite, &QAction::triggered, this, &MainWindow::blackWhiteDialog);
    connect(colorBalance, &QAction::triggered, this, &MainWindow::colorBalanceDialog);
    auto *noise = image->addAction(tr("Add Noise…"));
    connect(noise, &QAction::triggered, this, &MainWindow::addNoiseDialog);
    auto *lens = image->addAction(tr("Lens Correction…"));
    connect(lens, &QAction::triggered, this, [this] {
        Layer *active=session_.activeLayer();if(!active||active->image.isNull())return;const QUuid target=active->id;const Layer original=*active;
        QDialog dialog(this);dialog.setWindowTitle(tr("Lens Correction"));auto *layout=new QVBoxLayout(&dialog);auto *amount=new QDoubleSpinBox(&dialog);amount->setRange(-100,100);amount->setValue(0);amount->setDecimals(0);layout->addWidget(new ScrubLabel(tr("Remove Distortion"),amount,1.0,std::nullopt,&dialog));layout->addWidget(sliderField(amount));auto *hint=new QLabel(tr("Positive straightens barrel distortion; negative straightens pincushion distortion."),&dialog);hint->setWordWrap(true);layout->addWidget(hint);auto *previewEnabled=new QCheckBox(tr("Preview"),&dialog);previewEnabled->setChecked(true);previewEnabled->setObjectName(QStringLiteral("filterPreview"));layout->addWidget(previewEnabled);
        session_.beginEdit(QStringLiteral("Lens Correction"));const auto preview=[this,target,original,amount,previewEnabled]{restoreLayer(document_.get(),session_,target,original);if(previewEnabled->isChecked())session_.distortActiveLayer(amount->value());canvas_->invalidateDocument();};connect(amount,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,preview);connect(previewEnabled,&QCheckBox::toggled,&dialog,preview);auto *buttons=new QDialogButtonBox(QDialogButtonBox::Cancel|QDialogButtonBox::Ok,&dialog);connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);layout->addWidget(buttons);if(runFloatingDialog(dialog)==QDialog::Accepted){previewEnabled->setChecked(true);preview();}else restoreLayer(document_.get(),session_,target,original);session_.endEdit();syncDocumentViews();
    });

    auto *filterMenu = menuBar()->addMenu(tr("&Filter"));
    auto *cameraRaw = filterMenu->addAction(tr("Camera Raw Filter…"));
    cameraRaw->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    connect(cameraRaw, &QAction::triggered, this, &MainWindow::cameraRawDialog);
    auto *gaussianBlur = filterMenu->addAction(tr("Gaussian Blur…"));
    auto *motionBlur = filterMenu->addAction(tr("Motion Blur…"));
    auto *vignetteAction = filterMenu->addAction(tr("Vignette…"));
    auto *bloomGlowAction = filterMenu->addAction(tr("Bloom / Glow…"));
    auto *ditherAction = filterMenu->addAction(tr("Dither…"));
    auto *tonalContrastAction = filterMenu->addAction(tr("Tonal Contrast…"));
    auto *removeBackground = filterMenu->addAction(tr("Remove Background…"));
    auto *contentFill = filterMenu->addAction(tr("Content-Aware Fill"));
    contentFill->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Delete));
    connect(gaussianBlur, &QAction::triggered, this, &MainWindow::gaussianBlurDialog);
    connect(motionBlur, &QAction::triggered, this, &MainWindow::motionBlurDialog);
    connect(vignetteAction, &QAction::triggered, this, &MainWindow::vignetteDialog);
    connect(bloomGlowAction, &QAction::triggered, this, &MainWindow::bloomGlowDialog);
    connect(ditherAction, &QAction::triggered, this, &MainWindow::ditherDialog);
    connect(tonalContrastAction, &QAction::triggered, this, &MainWindow::tonalContrastDialog);
    connect(removeBackground, &QAction::triggered, this, &MainWindow::removeBackgroundDialog);
    connect(contentFill, &QAction::triggered, this, [this] {
        if (!session_.contentAwareFill()) showMessage(this, tr("Content-Aware Fill"), tr("Not enough unselected opaque pixels surround this selection."));
        else syncDocumentViews();
    });
    filterMenu->addAction(noise); filterMenu->addAction(lens);

    auto *brushTool = new QAction(this); brushTool->setShortcut(QKeySequence(Qt::Key_B));
    auto *moveTool = new QAction(this); moveTool->setShortcut(QKeySequence(Qt::Key_V));
    auto *wandTool = new QAction(this); wandTool->setShortcut(QKeySequence(Qt::Key_W));
    auto *toggleWand = new QAction(this); toggleWand->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_W));
    auto *cropTool = new QAction(this); cropTool->setShortcut(QKeySequence(Qt::Key_C));
    auto *handTool = new QAction(this); handTool->setShortcut(QKeySequence(Qt::Key_H));
    auto *zoomTool = new QAction(this); zoomTool->setShortcut(QKeySequence(Qt::Key_Z));
    auto *idleTool = new QAction(this); idleTool->setShortcut(QKeySequence(Qt::Key_A));
    auto *eraserTool = new QAction(this); eraserTool->setShortcut(QKeySequence(Qt::Key_E));
    auto *marqueeTool = new QAction(this); marqueeTool->setShortcut(QKeySequence(Qt::Key_M));
    auto *toggleMarquee = new QAction(this); toggleMarquee->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_M));
    auto *lassoTool = new QAction(this); lassoTool->setShortcut(QKeySequence(Qt::Key_L));
    auto *toggleLasso = new QAction(this); toggleLasso->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_L));
    auto *gradientTool = new QAction(this); gradientTool->setShortcut(QKeySequence(Qt::Key_G));
    auto *shapeTool = new QAction(this); shapeTool->setShortcut(QKeySequence(Qt::Key_U));
    auto *textTool = new QAction(this); textTool->setShortcut(QKeySequence(Qt::Key_T));
    auto *toggleShape = new QAction(this); toggleShape->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_U));
    auto *cloneTool = new QAction(this); cloneTool->setShortcut(QKeySequence(Qt::Key_S));
    auto *healingTool = new QAction(this); healingTool->setShortcut(QKeySequence(Qt::Key_J));
    auto *blurToolAction = new QAction(this); blurToolAction->setShortcut(QKeySequence(Qt::Key_R));
    auto *toggleBlur = new QAction(this); toggleBlur->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_R));
    auto *eyedropperTool = new QAction(this); eyedropperTool->setShortcut(QKeySequence(Qt::Key_I));
    auto *smallerBrush = new QAction(this); smallerBrush->setShortcut(QKeySequence(Qt::Key_BracketLeft));
    auto *largerBrush = new QAction(this); largerBrush->setShortcut(QKeySequence(Qt::Key_BracketRight));
    auto *softerBrush = new QAction(this); softerBrush->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_BracketLeft));
    auto *harderBrush = new QAction(this); harderBrush->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_BracketRight));
    auto *swapColors = new QAction(this); swapColors->setShortcut(QKeySequence(Qt::Key_X));
    auto *resetColors = new QAction(this); resetColors->setShortcut(QKeySequence(Qt::Key_D));
    auto *nextBlend = new QAction(this); nextBlend->setObjectName(QStringLiteral("nextBlendMode"));
    nextBlend->setShortcuts({QKeySequence(Qt::SHIFT | Qt::Key_Plus), QKeySequence(Qt::SHIFT | Qt::Key_Equal)});
    auto *previousBlend = new QAction(this); previousBlend->setObjectName(QStringLiteral("previousBlendMode"));
    previousBlend->setShortcuts({QKeySequence(Qt::SHIFT | Qt::Key_Minus), QKeySequence(Qt::SHIFT | Qt::Key_Underscore)});
    connect(moveTool, &QAction::triggered, this, [this] { if (!textEditorHasFocus()) canvas_->setTool(CanvasWidget::Tool::Move); });
    connect(wandTool, &QAction::triggered, this, [this] { if (!textEditorHasFocus()) canvas_->setTool(CanvasWidget::Tool::Wand); });
    connect(toggleWand, &QAction::triggered, this, [this] { if (textEditorHasFocus()) return; canvas_->toggleWandMode(); canvas_->setTool(CanvasWidget::Tool::Wand); });
    connect(cropTool, &QAction::triggered, this, [this] { if (!textEditorHasFocus()) canvas_->setTool(CanvasWidget::Tool::Crop); });
    connect(handTool, &QAction::triggered, this, [this] { if (!textEditorHasFocus()) canvas_->setTool(CanvasWidget::Tool::Hand); });
    connect(zoomTool, &QAction::triggered, this, [this] { if (!textEditorHasFocus()) canvas_->setTool(CanvasWidget::Tool::Zoom); });
    connect(idleTool, &QAction::triggered, this, [this] { if (!textEditorHasFocus()) canvas_->setTool(CanvasWidget::Tool::Other); });
    connect(brushTool, &QAction::triggered, this, [this] { if (!textEditorHasFocus()) canvas_->setTool(CanvasWidget::Tool::Brush); });
    connect(eraserTool, &QAction::triggered, this, [this] { if (!textEditorHasFocus()) canvas_->setTool(CanvasWidget::Tool::Eraser); });
    connect(marqueeTool, &QAction::triggered, this, [this] { if (!textEditorHasFocus()) canvas_->setTool(CanvasWidget::Tool::Marquee); });
    connect(toggleMarquee, &QAction::triggered, this, [this] { if (textEditorHasFocus()) return; canvas_->toggleMarqueeKind(); canvas_->setTool(CanvasWidget::Tool::Marquee); });
    connect(lassoTool, &QAction::triggered, this, [this] { if (!textEditorHasFocus()) canvas_->setTool(CanvasWidget::Tool::Lasso); });
    connect(toggleLasso, &QAction::triggered, this, [this] { if (textEditorHasFocus()) return; canvas_->toggleLassoKind(); canvas_->setTool(CanvasWidget::Tool::Lasso); });
    connect(gradientTool, &QAction::triggered, this, [this] { if (!textEditorHasFocus()) canvas_->setTool(CanvasWidget::Tool::Gradient); });
    connect(shapeTool, &QAction::triggered, this, [this] { if (!textEditorHasFocus()) canvas_->setTool(CanvasWidget::Tool::Shape); });
    connect(textTool, &QAction::triggered, this, [this] { if (!textEditorHasFocus()) canvas_->setTool(CanvasWidget::Tool::Text); });
    connect(toggleShape, &QAction::triggered, this, [this] { if (textEditorHasFocus()) return; canvas_->toggleShapeKind(); canvas_->setTool(CanvasWidget::Tool::Shape); });
    connect(cloneTool, &QAction::triggered, this, [this] { if (!textEditorHasFocus()) canvas_->setTool(CanvasWidget::Tool::Clone); });
    connect(healingTool, &QAction::triggered, this, [this] { if (!textEditorHasFocus()) canvas_->setTool(CanvasWidget::Tool::Healing); });
    connect(blurToolAction, &QAction::triggered, this, [this] {
        if (textEditorHasFocus()) return;
        if (canvas_->tool() == CanvasWidget::Tool::Blur) {
            cycleSmearMode();
        } else {
            canvas_->setTool(CanvasWidget::Tool::Blur);
        }
    });
    connect(toggleBlur, &QAction::triggered, this, [this] {
        if (textEditorHasFocus()) return;
        if (canvas_->tool() != CanvasWidget::Tool::Blur) {
            canvas_->setTool(CanvasWidget::Tool::Blur);
        }
        cycleSmearMode();
    });
    connect(eyedropperTool, &QAction::triggered, this, [this] { if (!textEditorHasFocus()) canvas_->setTool(CanvasWidget::Tool::Eyedropper); });
    const auto usesBrushSettings = [this] {
        if (textEditorHasFocus()) return false;
        const CanvasWidget::Tool tool = canvas_->tool();
        return tool == CanvasWidget::Tool::Brush || tool == CanvasWidget::Tool::Eraser || tool == CanvasWidget::Tool::Healing || tool == CanvasWidget::Tool::Clone || tool == CanvasWidget::Tool::Blur;
    };
    connect(smallerBrush, &QAction::triggered, this, [this, usesBrushSettings] { if (usesBrushSettings()) brushSizeField_->setValue(std::max(1.0, std::min(brushDiameter_ - 1, std::round(brushDiameter_ / 1.2)))); });
    connect(largerBrush, &QAction::triggered, this, [this, usesBrushSettings] { if (usesBrushSettings()) brushSizeField_->setValue(std::min(2000.0, std::max(brushDiameter_ + 1, std::round(brushDiameter_ * 1.2)))); });
    connect(softerBrush, &QAction::triggered, this, [this, usesBrushSettings] { if (usesBrushSettings()) brushHardnessField_->setValue(std::max(0.0, std::ceil(brushHardness_ * 4 - .001) / 4 - .25) * 100); });
    connect(harderBrush, &QAction::triggered, this, [this, usesBrushSettings] { if (usesBrushSettings()) brushHardnessField_->setValue(std::min(1.0, std::floor(brushHardness_ * 4 + .001) / 4 + .25) * 100); });
    const auto cycleBlend = [this](bool forward) {
        if (textEditorHasFocus()) return;
        if (!document_ || !document_->activeLayerId || session_.selectedLayerIds().size() != 1 || !session_.activeLayer() || session_.activeLayer()->group) return;
        constexpr int count = int(BlendMode::Luminosity) + 1;
        const int current = int(session_.activeLayer()->blendMode);
        session_.setLayerBlendMode(*document_->activeLayerId, BlendMode((current + (forward ? 1 : count - 1)) % count));
        syncDocumentViews();
    };
    connect(nextBlend, &QAction::triggered, this, [cycleBlend] { cycleBlend(true); });
    connect(previousBlend, &QAction::triggered, this, [cycleBlend] { cycleBlend(false); });
    connect(swapColors, &QAction::triggered, this, [this] {
        if (textEditorHasFocus()) return;
        std::swap(foregroundColor_, backgroundColor_);
        foregroundSwatch_->setStyleSheet(QStringLiteral("background:%1;border:1px solid white;border-radius:4px;").arg(foregroundColor_.name(QColor::HexArgb)));
        backgroundSwatch_->setStyleSheet(QStringLiteral("background:%1;border:1px solid white;border-radius:4px;").arg(backgroundColor_.name(QColor::HexArgb)));
        canvas_->refreshPendingGradient();
    });
    connect(resetColors, &QAction::triggered, this, [this] {
        if (textEditorHasFocus()) return;
        foregroundColor_ = Qt::black; backgroundColor_ = Qt::white; canvas_->setPaletteForeground(foregroundColor_);
        foregroundSwatch_->setStyleSheet(QStringLiteral("background:#000000;border:1px solid white;border-radius:4px;"));
        backgroundSwatch_->setStyleSheet(QStringLiteral("background:#ffffff;border:1px solid white;border-radius:4px;"));
        canvas_->refreshPendingGradient();
    });

    auto *view = menuBar()->addMenu(tr("&View"));
    auto *showMenuBar = view->addAction(tr("Show Menu Bar"));
    showMenuBar->setObjectName(QStringLiteral("showMenuBar"));
    showMenuBar->setCheckable(true);
    showMenuBar->setChecked(QSettings().value(QStringLiteral("ui/showMenuBar"), true).toBool());
    showMenuBar->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
    showMenuBar->setShortcutContext(Qt::ApplicationShortcut);
    connect(showMenuBar, &QAction::toggled, this, [this](bool visible) {
        menuBar()->setVisible(visible);
        QSettings().setValue(QStringLiteral("ui/showMenuBar"), visible);
        updateMenuRestoreButton();
    });
    view->addSeparator();
    auto *fit = view->addAction(tr("Fit Canvas"));
    fit->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    connect(fit, &QAction::triggered, canvas_, &CanvasWidget::fitCanvas);
    auto *actual = view->addAction(tr("Actual Pixels"));
    actual->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_1));
    connect(actual, &QAction::triggered, canvas_, &CanvasWidget::actualPixels);
    auto *zoomIn = view->addAction(tr("Zoom In"));
    zoomIn->setShortcut(QKeySequence::ZoomIn);
    connect(zoomIn, &QAction::triggered, canvas_, &CanvasWidget::zoomIn);
    auto *zoomOut = view->addAction(tr("Zoom Out"));
    zoomOut->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOut, &QAction::triggered, canvas_, &CanvasWidget::zoomOut);
    view->addSeparator();
    auto *pixelGrid = view->addAction(tr("Pixel Grid (800% and above)"));
    pixelGrid->setCheckable(true);
    pixelGrid->setChecked(canvas_->showsPixelGrid());
    connect(pixelGrid, &QAction::toggled, canvas_, &CanvasWidget::setShowPixelGrid);
    auto *sampleRing = view->addAction(tr("Eyedropper Sample Ring")); sampleRing->setCheckable(true); sampleRing->setChecked(true);
    connect(sampleRing, &QAction::toggled, canvas_, &CanvasWidget::setShowSampleRing);
    auto *transformControls = view->addAction(tr("Show Transform Controls"));
    transformControls->setCheckable(true);
    transformControls->setChecked(showTransformControls_->isChecked());
    transformControls->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H));
    connect(transformControls, &QAction::toggled, showTransformControls_, &QCheckBox::setChecked);
    connect(showTransformControls_, &QCheckBox::toggled, transformControls, &QAction::setChecked);

    view->addSeparator();

    auto *showSubMenu = view->addMenu(tr("Show"));
    actionShowGrid_ = showSubMenu->addAction(tr("Grid"));
    actionShowGrid_->setObjectName(QStringLiteral("commandShowGrid"));
    actionShowGrid_->setCheckable(true);
    actionShowGrid_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Apostrophe));
    actionShowGrid_->setChecked(session_.showsGrid());
    connect(actionShowGrid_, &QAction::toggled, this, [this](bool show) {
        session_.setShowsGrid(show);
        canvas_->update();
    });

    actionShowGuides_ = showSubMenu->addAction(tr("Guides"));
    actionShowGuides_->setObjectName(QStringLiteral("commandShowGuides"));
    actionShowGuides_->setCheckable(true);
    actionShowGuides_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Semicolon));
    actionShowGuides_->setChecked(session_.showsGuides());
    connect(actionShowGuides_, &QAction::toggled, this, [this](bool show) {
        session_.setShowsGuides(show);
        canvas_->update();
    });

    actionShowRulers_ = view->addAction(tr("Rulers"));
    actionShowRulers_->setObjectName(QStringLiteral("commandShowRulers"));
    actionShowRulers_->setCheckable(true);
    actionShowRulers_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    actionShowRulers_->setChecked(session_.showsRulers());
    connect(actionShowRulers_, &QAction::toggled, this, [this](bool show) {
        session_.setShowsRulers(show);
        updateRulerVisibility();
        canvas_->update();
    });

    view->addSeparator();

    actionSnap_ = view->addAction(tr("Snap"));
    actionSnap_->setObjectName(QStringLiteral("commandSnap"));
    actionSnap_->setCheckable(true);
    actionSnap_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Semicolon));
    actionSnap_->setChecked(session_.snapEnabled());
    connect(actionSnap_, &QAction::toggled, this, [this](bool enabled) {
        session_.setSnapEnabled(enabled);
    });

    auto *snapToSubMenu = view->addMenu(tr("Snap To"));
    actionSnapToGuides_ = snapToSubMenu->addAction(tr("Guides"));
    actionSnapToGuides_->setObjectName(QStringLiteral("commandSnapToGuides"));
    actionSnapToGuides_->setCheckable(true);
    actionSnapToGuides_->setChecked(session_.snapToGuides());
    connect(actionSnapToGuides_, &QAction::toggled, this, [this](bool enabled) {
        session_.setSnapToGuides(enabled);
    });

    actionSnapToGrid_ = snapToSubMenu->addAction(tr("Grid"));
    actionSnapToGrid_->setObjectName(QStringLiteral("commandSnapToGrid"));
    actionSnapToGrid_->setCheckable(true);
    actionSnapToGrid_->setChecked(session_.snapToGrid());
    connect(actionSnapToGrid_, &QAction::toggled, this, [this](bool enabled) {
        session_.setSnapToGrid(enabled);
    });

    actionSnapToLayers_ = snapToSubMenu->addAction(tr("Layers"));
    actionSnapToLayers_->setObjectName(QStringLiteral("commandSnapToLayers"));
    actionSnapToLayers_->setCheckable(true);
    actionSnapToLayers_->setChecked(session_.snapToLayers());
    connect(actionSnapToLayers_, &QAction::toggled, this, [this](bool enabled) {
        session_.setSnapToLayers(enabled);
    });

    actionSnapToDocumentBounds_ = snapToSubMenu->addAction(tr("Document Bounds"));
    actionSnapToDocumentBounds_->setObjectName(QStringLiteral("commandSnapToDocumentBounds"));
    actionSnapToDocumentBounds_->setCheckable(true);
    actionSnapToDocumentBounds_->setChecked(session_.snapToDocumentBounds());
    connect(actionSnapToDocumentBounds_, &QAction::toggled, this, [this](bool enabled) {
        session_.setSnapToDocumentBounds(enabled);
    });

    view->addSeparator();

    actionLockGuides_ = view->addAction(tr("Lock Guides"));
    actionLockGuides_->setObjectName(QStringLiteral("commandLockGuides"));
    actionLockGuides_->setCheckable(true);
    actionLockGuides_->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Semicolon));
    actionLockGuides_->setChecked(session_.locksGuides());
    connect(actionLockGuides_, &QAction::toggled, this, [this](bool locked) {
        session_.setLocksGuides(locked);
    });

    {   // The grid's spacing and look are remembered between sessions.
        QSettings settings;
        session_.setLayoutGrid(LayoutGrid(settings.value(QStringLiteral("grid/spacing"), 64).toInt(), settings.value(QStringLiteral("grid/subdivisions"), 8).toInt()));
        GridAppearance look;
        look.preset = GridAppearance::Preset(std::clamp(settings.value(QStringLiteral("grid/preset"), 0).toInt(), 0, int(GridAppearance::Preset::Custom)));
        look.customColor = QColor(settings.value(QStringLiteral("grid/customColor"), QStringLiteral("#b3b3b3")).toString());
        look.style = GridAppearance::Style(std::clamp(settings.value(QStringLiteral("grid/style"), 0).toInt(), 0, int(GridAppearance::Style::Dots)));
        look.opacity = std::clamp(settings.value(QStringLiteral("grid/opacity"), 45).toInt(), GridAppearance::minOpacity, GridAppearance::maxOpacity);
        session_.setGridAppearance(look);
    }
    auto *gridSettings = view->addAction(tr("Grid Settings…")); gridSettings->setObjectName(QStringLiteral("commandGridSettings"));
    connect(gridSettings, &QAction::triggered, this, [this] { gridSettingsDialog(); });
    actionClearGuides_ = view->addAction(tr("Clear Guides"));
    actionClearGuides_->setObjectName(QStringLiteral("commandClearGuides"));
    actionClearGuides_->setEnabled(session_.canClearGuides());
    connect(actionClearGuides_, &QAction::triggered, this, [this] {
        session_.clearGuides();
        syncDocumentViews(false);
    });

    auto *help = menuBar()->addMenu(tr("&Help"));
    connect(help->addAction(tr("Check for Updates…")), &QAction::triggered, this, &MainWindow::checkForUpdates);
    connect(help->addAction(tr("About CompositorLX")), &QAction::triggered, this, &MainWindow::showAbout);

    addAction(newAction); addAction(open); addAction(importAction); addAction(save); addAction(saveAs); addAction(exportAction); addAction(exportJpegAction); addAction(closeProject); addAction(undo); addAction(redo);
    addAction(cut); addAction(copy); addAction(copyMerged); addAction(paste);
    addAction(duplicate); addAction(remove); addAction(newLayerAction); addAction(selectAll); addAction(deselect); addAction(inverse); addAction(selectSubject);
    addAction(levels); addAction(hueSaturation); addAction(curves); addAction(imageSize); addAction(canvasSize); addAction(invert);
    addAction(brushTool); addAction(eraserTool); addAction(marqueeTool); addAction(toggleMarquee); addAction(lassoTool); addAction(toggleLasso);
    addAction(moveTool); addAction(wandTool); addAction(toggleWand); addAction(cropTool); addAction(handTool); addAction(zoomTool); addAction(idleTool);
    addAction(gradientTool); addAction(shapeTool); addAction(toggleShape); addAction(textTool);
    addAction(cloneTool);
    addAction(healingTool);
    addAction(blurToolAction);
    addAction(eyedropperTool);
    addAction(smallerBrush); addAction(largerBrush); addAction(softerBrush); addAction(harderBrush);
    addAction(nextBlend); addAction(previousBlend);
    addAction(swapColors); addAction(resetColors);
    addAction(showMenuBar);
    addAction(actionShowGrid_);
    addAction(actionShowGuides_);
    addAction(actionShowRulers_);
    addAction(actionSnap_);
    addAction(actionLockGuides_);
    menuBar()->setVisible(showMenuBar->isChecked());
    updateMenuRestoreButton();
    auto &sm = ShortcutManager::instance();
    for (int digit = 0; digit <= 9; ++digit) {
        auto *opacityKey = new QAction(this); opacityKey->setShortcut(QKeySequence(Qt::Key_0 + digit));
        opacityKey->setShortcutContext(Qt::WindowShortcut); addAction(opacityKey);
        connect(opacityKey, &QAction::triggered, this, [this, digit] {
            if (textEditorHasFocus()) return;
            const CanvasWidget::Tool tool = canvas_->tool();
            const bool brush = tool == CanvasWidget::Tool::Brush || tool == CanvasWidget::Tool::Eraser || tool == CanvasWidget::Tool::Healing
                || tool == CanvasWidget::Tool::Clone || tool == CanvasWidget::Tool::Blur;
            if (!brush && tool != CanvasWidget::Tool::Gradient && tool != CanvasWidget::Tool::Move) return;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            int value = digit == 0 ? 100 : digit * 10;
            if (pendingOpacityDigit_ >= 0 && now - pendingOpacityAt_ < 600) { value = std::max(1, pendingOpacityDigit_ * 10 + digit); pendingOpacityDigit_ = -1; }
            else { pendingOpacityDigit_ = digit; pendingOpacityAt_ = now; }
            if (brush) brushOpacityField_->setValue(value);
            else if (tool == CanvasWidget::Tool::Gradient) gradientOpacityField_->setValue(value);
            else if (tool == CanvasWidget::Tool::Move) { session_.setSelectedLayersOpacity(value / 100.0); syncDocumentViews(); }
        });
        sm.registerAction(QStringLiteral("Canvas & Layers:Opacity digit %1 (type two for exact %)").arg(digit), opacityKey);
    }

    sm.registerAction(QStringLiteral("Menus:New Canvas"), newAction);
    sm.registerAction(QStringLiteral("Menus:Open Project"), open);
    sm.registerAction(QStringLiteral("Menus:Save"), save);
    sm.registerAction(QStringLiteral("Menus:Save As"), saveAs);
    sm.registerAction(QStringLiteral("Menus:Export PNG"), exportAction);
    sm.registerAction(QStringLiteral("Menus:Export JPEG"), exportJpegAction);
    sm.registerAction(QStringLiteral("Menus:Close Project"), closeProject);
    sm.registerAction(QStringLiteral("Menus:Undo"), undo);
    sm.registerAction(QStringLiteral("Menus:Redo"), redo);
    sm.registerAction(QStringLiteral("Menus:Cut"), cut);
    sm.registerAction(QStringLiteral("Menus:Copy"), copy);
    sm.registerAction(QStringLiteral("Menus:Copy Merged"), copyMerged);
    sm.registerAction(QStringLiteral("Menus:Paste"), paste);
    sm.registerAction(QStringLiteral("Menus:Duplicate / Layer via Copy"), duplicate);
    sm.registerAction(QStringLiteral("Menus:Delete Layer"), remove);
    sm.registerAction(QStringLiteral("Menus:Fill with Foreground"), fillForeground);
    sm.registerAction(QStringLiteral("Menus:Fill with Background"), fillBackground);
    sm.registerAction(QStringLiteral("Menus:Content-Aware Fill"), contentFill);
    sm.registerAction(QStringLiteral("Menus:Transform Layer / Selection"), transformSelection);

    sm.registerAction(QStringLiteral("Menus:New Blank Layer"), newLayerAction);
    sm.registerAction(QStringLiteral("Menus:Group Layers"), groupLayers);
    sm.registerAction(QStringLiteral("Menus:Ungroup Layers"), ungroupLayers);
    sm.registerAction(QStringLiteral("Menus:Merge Layers"), merge);
    sm.registerAction(QStringLiteral("Menus:Move Layer Up"), moveUp);
    sm.registerAction(QStringLiteral("Menus:Move Layer Down"), moveDown);
    sm.registerAction(QStringLiteral("Menus:Toggle Clipping Mask"), clipping);

    sm.registerAction(QStringLiteral("Menus:Select All"), selectAll);
    sm.registerAction(QStringLiteral("Menus:Deselect"), deselect);
    sm.registerAction(QStringLiteral("Menus:Inverse Selection"), inverse);
    sm.registerAction(QStringLiteral("Menus:Select Subject"), selectSubject);
    sm.registerAction(QStringLiteral("Menus:Feather Selection"), feather);

    sm.registerAction(QStringLiteral("Menus:Levels"), levels);
    sm.registerAction(QStringLiteral("Menus:Hue/Saturation"), hueSaturation);
    sm.registerAction(QStringLiteral("Menus:Curves"), curves);
    sm.registerAction(QStringLiteral("Menus:Image Size"), imageSize);
    sm.registerAction(QStringLiteral("Menus:Canvas Size"), canvasSize);
    sm.registerAction(QStringLiteral("Menus:Invert Pixels / Mask"), invert);
    sm.registerAction(QStringLiteral("Menus:Camera Raw Filter"), cameraRaw);

    sm.registerAction(QStringLiteral("Menus:Show Menu Bar"), showMenuBar);
    sm.registerAction(QStringLiteral("Menus:Fit Canvas"), fit);
    sm.registerAction(QStringLiteral("Menus:Actual Pixels"), actual);
    sm.registerAction(QStringLiteral("Menus:Zoom In"), zoomIn);
    sm.registerAction(QStringLiteral("Menus:Zoom Out"), zoomOut);
    sm.registerAction(QStringLiteral("Menus:Show Transform Controls"), transformControls);
    sm.registerAction(QStringLiteral("Menus:Show Grid"), actionShowGrid_);
    sm.registerAction(QStringLiteral("Menus:Show Guides"), actionShowGuides_);
    sm.registerAction(QStringLiteral("Menus:Show Rulers"), actionShowRulers_);
    sm.registerAction(QStringLiteral("Menus:Snap"), actionSnap_);
    sm.registerAction(QStringLiteral("Menus:Lock Guides"), actionLockGuides_);

    sm.registerAction(QStringLiteral("Canvas & Layers:Select tool"), idleTool);
    sm.registerAction(QStringLiteral("Canvas & Layers:Move / Transform tool"), moveTool);
    sm.registerAction(QStringLiteral("Canvas & Layers:Hand tool"), handTool);
    sm.registerAction(QStringLiteral("Canvas & Layers:Zoom tool"), zoomTool);
    sm.registerAction(QStringLiteral("Canvas & Layers:Brush tool"), brushTool);
    sm.registerAction(QStringLiteral("Canvas & Layers:Eraser"), eraserTool);
    sm.registerAction(QStringLiteral("Canvas & Layers:Spot Healing"), healingTool);
    sm.registerAction(QStringLiteral("Canvas & Layers:Clone Stamp"), cloneTool);
    sm.registerAction(QStringLiteral("Canvas & Layers:Type tool"), textTool);
    sm.registerAction(QStringLiteral("Canvas & Layers:Gradient tool"), gradientTool);
    sm.registerAction(QStringLiteral("Canvas & Layers:Shape tool"), shapeTool);
    sm.registerAction(QStringLiteral("Canvas & Layers:Eyedropper tool"), eyedropperTool);
    sm.registerAction(QStringLiteral("Canvas & Layers:Marquee / cycle shape"), marqueeTool);
    sm.registerAction(QStringLiteral("Canvas & Layers:Cycle marquee kind"), toggleMarquee);
    sm.registerAction(QStringLiteral("Canvas & Layers:Magic"), wandTool);
    sm.registerAction(QStringLiteral("Canvas & Layers:Cycle wand mode"), toggleWand);
    sm.registerAction(QStringLiteral("Canvas & Layers:Lasso / cycle mode"), lassoTool);
    sm.registerAction(QStringLiteral("Canvas & Layers:Cycle lasso kind"), toggleLasso);
    sm.registerAction(QStringLiteral("Canvas & Layers:Blur / Smudge / Liquify"), blurToolAction);
    sm.registerAction(QStringLiteral("Canvas & Layers:Cycle smear mode"), toggleBlur);
    auto *cycleToolModeAction = new QAction(this);
    cycleToolModeAction->setShortcut(QKeySequence(Qt::Key_Tab));
    connect(cycleToolModeAction, &QAction::triggered, this, [this] {
        if (textEditorHasFocus()) return;
        cycleToolMode();
    });
    sm.registerAction(QStringLiteral("Canvas & Layers:Cycle tool mode"), cycleToolModeAction);
    sm.registerAction(QStringLiteral("Canvas & Layers:Crop tool"), cropTool);
    sm.registerAction(QStringLiteral("Canvas & Layers:Swap foreground/background"), swapColors);
    sm.registerAction(QStringLiteral("Canvas & Layers:Reset colors"), resetColors);
    sm.registerAction(QStringLiteral("Canvas & Layers:Decrease brush size"), smallerBrush);
    sm.registerAction(QStringLiteral("Canvas & Layers:Increase brush size"), largerBrush);
    sm.registerAction(QStringLiteral("Canvas & Layers:Decrease brush hardness"), softerBrush);
    sm.registerAction(QStringLiteral("Canvas & Layers:Increase brush hardness"), harderBrush);
    sm.registerAction(QStringLiteral("Canvas & Layers:Previous blend mode"), previousBlend);
    sm.registerAction(QStringLiteral("Canvas & Layers:Next blend mode"), nextBlend);
    sm.registerAction(QStringLiteral("Canvas & Layers:Cycle shape kind"), toggleShape);

    sm.apply();

    addAction(contentFill); addAction(transformSelection); addAction(fillForeground); addAction(fillBackground);
    addAction(groupLayers); addAction(ungroupLayers); addAction(moveUp); addAction(moveDown);
    addAction(fit); addAction(actual); addAction(zoomIn); addAction(zoomOut);
}

void MainWindow::copyPixels(bool merged)
{
    const auto copied = session_.copiedPixels(merged);
    if (!copied) return;
    clipboardImage_ = copied->first; clipboardOrigin_ = copied->second;
    QGuiApplication::clipboard()->setImage(clipboardImage_);
    statusHint_->setText(merged ? tr("Copied merged pixels") : tr("Copied pixels"));
}

void MainWindow::cutPixels()
{
    if (!document_ || !document_->selection) return;
    copyPixels(false);
    if (!clipboardImage_.isNull() && session_.clearSelectedPixels()) syncDocumentViews();
}

void MainWindow::pastePixels()
{
    if (!document_) return;
    const QImage image = QGuiApplication::clipboard()->image();
    if (image.isNull()) return;
    QPointF origin;
    if (!clipboardImage_.isNull() && image == clipboardImage_) origin = clipboardOrigin_;
    else origin = QPointF(std::floor((document_->canvasSize.width() - image.width()) / 2.0),
                          std::floor((document_->canvasSize.height() - image.height()) / 2.0));
    if (session_.insertPixelLayer(image, origin, QStringLiteral("Pasted Layer"), QStringLiteral("Paste"))) syncDocumentViews();
}

void MainWindow::layerViaCopy()
{
    if (!document_) return;
    if (!document_->selection) { session_.duplicateActiveLayer(); syncDocumentViews(); return; }
    const auto copied = session_.copiedPixels(false);
    if (copied && session_.insertPixelLayer(copied->first, copied->second, QStringLiteral("Layer via Copy"),
                                            QStringLiteral("Layer via Copy"))) syncDocumentViews();
}

void MainWindow::levelsDialog()
{
    Layer *active = session_.activeLayer(); const bool live = active && active->adjustment.value(QStringLiteral("kind")).toString() == QStringLiteral("Levels");
    if (!active || (!live && active->image.isNull())) return;
    const QUuid target = active->id;
    QDialog dialog(this); dialog.setWindowTitle(tr("Levels")); auto *layout = new QVBoxLayout(&dialog); auto *form = new QFormLayout;
    auto *channel = new QComboBox(&dialog); channel->addItems({tr("RGB"), tr("Red"), tr("Green"), tr("Blue")});
    auto make = [&dialog](double minimum, double maximum, double value, int decimals = 0) { auto *field = new QDoubleSpinBox(&dialog); field->setRange(minimum, maximum); field->setValue(value); field->setDecimals(decimals); return field; };
    auto *black = make(0, 254, 0); auto *gamma = make(.1, 9.99, 1, 2); auto *white = make(1, 255, 255);
    auto *outputBlack = make(0, 255, 0); auto *outputWhite = make(0, 255, 255);
    black->setObjectName(QStringLiteral("levelsInputBlack")); gamma->setObjectName(QStringLiteral("levelsGamma")); white->setObjectName(QStringLiteral("levelsInputWhite")); outputBlack->setObjectName(QStringLiteral("levelsOutputBlack")); outputWhite->setObjectName(QStringLiteral("levelsOutputWhite"));
    LevelsSettings settings; const QJsonObject originalAdjustment = active->adjustment; const Layer originalLayer = *active;
    if (live) { const QJsonArray saved = originalAdjustment.value(QStringLiteral("levels")).toObject().value(QStringLiteral("ranges")).toArray(); for (int i=0;i<std::min(4,int(saved.size()));++i) { const QJsonObject r=saved[i].toObject(); settings.ranges[size_t(i)]={r.value(QStringLiteral("black")).toDouble(),r.value(QStringLiteral("gamma")).toDouble(1),r.value(QStringLiteral("white")).toDouble(255),r.value(QStringLiteral("outputBlack")).toDouble(),r.value(QStringLiteral("outputWhite")).toDouble(255)}; } }
    const auto storeAt = [&](int index) { settings.ranges[size_t(index)] = {black->value(), gamma->value(), white->value(), outputBlack->value(), outputWhite->value()}; };
    const auto storeCurrent = [&] { storeAt(channel->currentIndex()); };
    const auto loadCurrent = [&] { const LevelRange &r=settings.ranges[size_t(channel->currentIndex())]; const QSignalBlocker a(black),b(gamma),c(white),d(outputBlack),e(outputWhite); black->setValue(r.black);gamma->setValue(r.gamma);white->setValue(r.white);outputBlack->setValue(r.outputBlack);outputWhite->setValue(r.outputWhite); };
    const LevelsHistogram histogram=session_.levelsHistogram(); auto *histogramView = new LevelsHistogramWidget(&dialog); histogramView->setObjectName(QStringLiteral("levelsHistogram")); histogramView->histogram = &histogram; histogramView->channel = [channel] { return channel->currentIndex(); }; layout->addWidget(histogramView);
    auto *inputHandles = new LevelsHandleWidget(false, &dialog); inputHandles->setObjectName(QStringLiteral("levelsInputHandles")); inputHandles->settings = &settings; inputHandles->channel = [channel] { return channel->currentIndex(); }; layout->addWidget(inputHandles);
    form->addRow(tr("Channel"), channel);
    addScrubRow(form, tr("Input black"), black, false, false);
    addScrubRow(form, tr("Gamma"), gamma, false, false);
    addScrubRow(form, tr("Input white"), white, false, false);
    addScrubRow(form, tr("Output black"), outputBlack, false, false);
    addScrubRow(form, tr("Output white"), outputWhite, false, false);
    layout->addLayout(form);
    auto *outputHandles = new LevelsHandleWidget(true, &dialog); outputHandles->setObjectName(QStringLiteral("levelsOutputHandles")); outputHandles->settings = &settings; outputHandles->channel = [channel] { return channel->currentIndex(); }; layout->addWidget(outputHandles);
    auto *samples = new QHBoxLayout; samples->addWidget(new QLabel(tr("Sample"), &dialog));
    auto *sampleBlack = new QPushButton(tr("Black"), &dialog); auto *sampleGray = new QPushButton(tr("Gray"), &dialog); auto *sampleWhite = new QPushButton(tr("White"), &dialog);
    for (QPushButton *button : {sampleBlack, sampleGray, sampleWhite}) { button->setCheckable(true); samples->addWidget(button); }
    layout->addLayout(samples); auto *sampleHint = new QLabel(&dialog); sampleHint->setWordWrap(true); layout->addWidget(sampleHint);
    auto *automatic=new QHBoxLayout;auto *autoContrast=new QPushButton(tr("Auto Contrast"),&dialog);auto *autoColor=new QPushButton(tr("Auto Color"),&dialog);auto *autoNeutral=new QPushButton(tr("Auto Neutral"),&dialog);automatic->addWidget(autoContrast);automatic->addWidget(autoColor);automatic->addWidget(autoNeutral);layout->addLayout(automatic);
    auto *previewEnabled = new QCheckBox(tr("Preview"), &dialog); previewEnabled->setChecked(true); previewEnabled->setObjectName(QStringLiteral("levelsPreview")); auto *reset = new QPushButton(tr("Reset"), &dialog); reset->setObjectName(QStringLiteral("levelsReset")); auto *previewRow = new QHBoxLayout; previewRow->addWidget(previewEnabled); previewRow->addStretch(); previewRow->addWidget(reset); layout->addLayout(previewRow);
    connect(black, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, [white](double value) { white->setMinimum(value + 1); });
    connect(channel, &QComboBox::currentIndexChanged, &dialog, [&, storeAt, loadCurrent, previousChannel = 0](int current) mutable { storeAt(previousChannel); previousChannel=current; loadCurrent(); histogramView->update(); inputHandles->update(); outputHandles->update(); });
    const auto preview = [this, live, target, storeCurrent, &settings, originalLayer, previewEnabled] {
        storeCurrent();
        if (!previewEnabled->isChecked()) {
            if (live) session_.previewAdjustment(target, originalLayer.adjustment);
            else restoreLayer(document_.get(), session_, target, originalLayer);
        } else if (live) session_.previewAdjustment(target, levelsAdjustment(settings));
        else { restoreLayer(document_.get(), session_, target, originalLayer); session_.applyLevels(settings); }
        canvas_->invalidateDocument();
    };
    const CanvasWidget::Tool previousTool = canvas_->tool(); int sampleMode = -1;
    const auto armSample = [&, previousTool](QPushButton *chosen, int mode, bool checked) {
        sampleMode = checked ? mode : -1;
        for (QPushButton *button : {sampleBlack, sampleGray, sampleWhite}) if (button != chosen) { const QSignalBlocker blocker(button); button->setChecked(false); }
        sampleHint->setText(checked ? tr("Click the original layer to set %1. Click the eyedropper again to stop.").arg(chosen->text().toLower()) : QString());
        canvas_->setTool(checked ? CanvasWidget::Tool::Eyedropper : previousTool);
    };
    connect(sampleBlack, &QPushButton::toggled, &dialog, [&, armSample](bool checked) { armSample(sampleBlack, 0, checked); });
    connect(sampleGray, &QPushButton::toggled, &dialog, [&, armSample](bool checked) { armSample(sampleGray, 1, checked); });
    connect(sampleWhite, &QPushButton::toggled, &dialog, [&, armSample](bool checked) { armSample(sampleWhite, 2, checked); });
    colorSampleOverride_ = [&, preview](const QPoint &point) {
        if (sampleMode < 0) return;
        const auto sampled = session_.levelsSampleAt(point); if (!sampled) return;
        storeCurrent(); settings = RasterOperations::sampledLevels(settings, *sampled, sampleMode); loadCurrent(); inputHandles->update(); outputHandles->update(); preview();
    };
    const auto applyAutomatic=[&](int mode){settings=RasterOperations::automaticLevels(histogram,mode);loadCurrent();inputHandles->update();outputHandles->update();preview();};connect(autoContrast,&QPushButton::clicked,&dialog,[&]{applyAutomatic(0);});connect(autoColor,&QPushButton::clicked,&dialog,[&]{applyAutomatic(1);});connect(autoNeutral,&QPushButton::clicked,&dialog,[&]{applyAutomatic(2);});
    session_.beginEdit(live ? QStringLiteral("Edit Levels") : QStringLiteral("Levels"));
    const auto fieldChanged = [&, preview] { preview(); inputHandles->update(); outputHandles->update(); };
    connect(black,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,fieldChanged);connect(gamma,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,fieldChanged);connect(white,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,fieldChanged);connect(outputBlack,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,fieldChanged);connect(outputWhite,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,fieldChanged);
    inputHandles->changed = [&, preview] { loadCurrent(); preview(); }; outputHandles->changed = [&, preview] { loadCurrent(); preview(); };
    connect(previewEnabled, &QCheckBox::toggled, &dialog, preview);
    connect(reset, &QPushButton::clicked, &dialog, [&] { settings = LevelsSettings(); loadCurrent(); inputHandles->update(); outputHandles->update(); preview(); });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
    loadCurrent(); const int result=runFloatingDialog(dialog); colorSampleOverride_ = {}; canvas_->setTool(previousTool); storeCurrent();
    if (result == QDialog::Accepted) { previewEnabled->setChecked(true); preview(); }
    else if (live) session_.previewAdjustment(target, originalAdjustment);
    else restoreLayer(document_.get(), session_, target, originalLayer);
    session_.endEdit(); syncDocumentViews();
}

void MainWindow::exposureDialog()
{
    Layer *active = session_.activeLayer(); const bool live = active && active->adjustment.value(QStringLiteral("kind")).toString() == QStringLiteral("Exposure");
    if (!active || (!live && active->image.isNull())) return;
    const QUuid target = active->id;
    QDialog dialog(this); dialog.setWindowTitle(tr("Exposure")); auto *layout = new QVBoxLayout(&dialog); auto *form = new QFormLayout;
    auto make = [&dialog](double minimum, double maximum, double value) { auto *field = new QDoubleSpinBox(&dialog); field->setRange(minimum, maximum); field->setValue(value); field->setDecimals(2); field->setSingleStep(.1); return field; };
    auto *stops = make(-20, 20, 0); auto *offset = make(-.5, .5, 0); auto *gamma = make(.01, 9.99, 1);
    stops->setObjectName(QStringLiteral("exposureStops")); offset->setObjectName(QStringLiteral("exposureOffset")); gamma->setObjectName(QStringLiteral("exposureGamma"));
    auto *previewEnabled = new QCheckBox(tr("Preview"), &dialog); previewEnabled->setChecked(true); previewEnabled->setObjectName(QStringLiteral("filterPreview"));
    const QJsonObject originalAdjustment = active->adjustment; const Layer originalLayer = *active;
    const auto value = [&] { return QJsonObject{{QStringLiteral("kind"), QStringLiteral("Exposure")}, {QStringLiteral("exposureSettings"), QJsonObject{{QStringLiteral("exposure"), stops->value()}, {QStringLiteral("offset"), offset->value()}, {QStringLiteral("gamma"), gamma->value()}}}}; };
    if (live) {
        const QJsonObject saved = originalAdjustment.value(QStringLiteral("exposureSettings")).toObject(); stops->setValue(saved.value(QStringLiteral("exposure")).toDouble()); offset->setValue(saved.value(QStringLiteral("offset")).toDouble()); gamma->setValue(saved.value(QStringLiteral("gamma")).toDouble(1));
    }
    session_.beginEdit(live ? QStringLiteral("Edit Exposure") : QStringLiteral("Exposure"));
    const auto preview = [this, live, target, value, originalLayer, stops, offset, gamma, previewEnabled] { if (!previewEnabled->isChecked()) { if (live) session_.previewAdjustment(target, originalLayer.adjustment); else restoreLayer(document_.get(), session_, target, originalLayer); } else if (live) session_.previewAdjustment(target, value()); else { restoreLayer(document_.get(), session_, target, originalLayer); session_.applyExposure(stops->value(),offset->value(),gamma->value()); } canvas_->invalidateDocument(); };
    connect(stops, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview); connect(offset, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview); connect(gamma, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    addScrubRow(form, tr("Exposure (stops)"), stops); addScrubRow(form, tr("Offset"), offset); addScrubRow(form, tr("Gamma"), gamma, true); layout->addLayout(form); layout->addWidget(previewEnabled); connect(previewEnabled, &QCheckBox::toggled, &dialog, preview);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
    const int result = runFloatingDialog(dialog);
    if(result==QDialog::Accepted){previewEnabled->setChecked(true);preview();}else if(live)session_.previewAdjustment(target,originalAdjustment);else restoreLayer(document_.get(), session_, target, originalLayer);
    session_.endEdit();syncDocumentViews();
}

// Select > Color Range (mac c3e360a): every pixel near the colors clicked on the canvas, anywhere in the image. The
// dialog shows the selection live; OK keeps it as one undo step, Cancel puts back the one there was.
// View > Grid Settings... (mac 1c819d0): the grid's spacing and subdivisions, and how it is drawn. Changes show as
// they are made; Cancel puts back what was there.
void MainWindow::gridSettingsDialog()
{
    const LayoutGrid originalGrid = session_.layoutGrid();
    const GridAppearance originalLook = session_.gridAppearance();
    QDialog dialog(this); dialog.setWindowTitle(tr("Grid Settings")); dialog.setObjectName(QStringLiteral("gridSettingsDialog"));
    auto *form = new QFormLayout(&dialog);
    auto *preset = new QComboBox(&dialog); preset->setObjectName(QStringLiteral("gridColorPreset"));
    for (int i = 0; i <= int(GridAppearance::Preset::Custom); ++i) preset->addItem(GridAppearance::presetName(GridAppearance::Preset(i)));
    auto *color = new QPushButton(&dialog); color->setObjectName(QStringLiteral("gridCustomColor")); color->setFixedWidth(60);
    auto *colorRow = new QHBoxLayout; colorRow->addWidget(preset, 1); colorRow->addWidget(color);
    auto *style = new QComboBox(&dialog); style->setObjectName(QStringLiteral("gridStyle"));
    for (int i = 0; i <= int(GridAppearance::Style::Dots); ++i) style->addItem(GridAppearance::styleName(GridAppearance::Style(i)));
    auto *opacity = new QSpinBox(&dialog); opacity->setRange(GridAppearance::minOpacity, GridAppearance::maxOpacity); opacity->setSuffix(QStringLiteral("%")); opacity->setObjectName(QStringLiteral("gridOpacity"));
    auto *spacing = new QSpinBox(&dialog); spacing->setRange(LayoutGrid::minSpacing, LayoutGrid::maxSpacing); spacing->setSuffix(tr(" px")); spacing->setObjectName(QStringLiteral("gridSpacing"));
    auto *subdivisions = new QSpinBox(&dialog); subdivisions->setRange(LayoutGrid::minSubdivisions, LayoutGrid::maxSubdivisions); subdivisions->setObjectName(QStringLiteral("gridSubdivisions"));
    form->addRow(tr("Color"), colorRow); form->addRow(tr("Style"), style); form->addRow(tr("Opacity"), opacity);
    form->addRow(tr("Gridline every"), spacing); form->addRow(tr("Subdivisions"), subdivisions);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::RestoreDefaults | QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    GridAppearance look = originalLook;
    const auto showFields = [&](const LayoutGrid &grid, const GridAppearance &appearance) {
        const QSignalBlocker b1(preset), b2(style), b3(opacity), b4(spacing), b5(subdivisions);
        preset->setCurrentIndex(int(appearance.preset)); style->setCurrentIndex(int(appearance.style)); opacity->setValue(appearance.opacity);
        spacing->setValue(grid.spacing); subdivisions->setValue(grid.subdivisions);
        color->setStyleSheet(QStringLiteral("background:%1;").arg(appearance.customColor.name())); color->setEnabled(appearance.preset == GridAppearance::Preset::Custom);
    };
    const auto apply = [&] {
        look.preset = GridAppearance::Preset(preset->currentIndex()); look.style = GridAppearance::Style(style->currentIndex()); look.opacity = opacity->value();
        // Subdivisions are never finer than a pixel: the field follows the spacing.
        subdivisions->setMaximum(std::min(LayoutGrid::maxSubdivisions, spacing->value()));
        session_.setLayoutGrid(LayoutGrid(spacing->value(), subdivisions->value())); session_.setGridAppearance(look);
        color->setEnabled(look.preset == GridAppearance::Preset::Custom); canvas_->update();
    };
    for (QComboBox *box : {preset, style}) connect(box, &QComboBox::currentIndexChanged, &dialog, [&](int) { apply(); });
    for (QSpinBox *box : {opacity, spacing, subdivisions}) connect(box, &QSpinBox::valueChanged, &dialog, [&](int) { apply(); });
    connect(color, &QPushButton::clicked, &dialog, [&] {
        const QColor chosen = QColorDialog::getColor(look.customColor, &dialog, tr("Grid Color"));
        if (!chosen.isValid()) return;
        look.customColor = chosen; color->setStyleSheet(QStringLiteral("background:%1;").arg(chosen.name())); apply();
    });
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dialog, [&] {
        look = GridAppearance(); showFields(LayoutGrid(), look); apply();
    });
    showFields(originalGrid, originalLook);
    if (dialog.exec() == QDialog::Accepted) {
        QSettings settings;
        settings.setValue(QStringLiteral("grid/spacing"), session_.layoutGrid().spacing); settings.setValue(QStringLiteral("grid/subdivisions"), session_.layoutGrid().subdivisions);
        settings.setValue(QStringLiteral("grid/preset"), int(look.preset)); settings.setValue(QStringLiteral("grid/customColor"), look.customColor.name());
        settings.setValue(QStringLiteral("grid/style"), int(look.style)); settings.setValue(QStringLiteral("grid/opacity"), look.opacity);
    } else { session_.setLayoutGrid(originalGrid); session_.setGridAppearance(originalLook); }
    canvas_->update();
}

void MainWindow::colorRangeDialog()
{
    if (!document_) return;
    const QImage sample = LayerRenderer::flattened(*document_);
    const std::optional<QImage> original = document_->selection;
    QDialog dialog(this); dialog.setWindowTitle(tr("Color Range")); dialog.setObjectName(QStringLiteral("colorRangeDialog"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *tools = new QHBoxLayout;
    auto *replaceTool = new QToolButton(&dialog); replaceTool->setText(tr("Sample")); replaceTool->setToolTip(tr("Click the image to select that color"));
    auto *addTool = new QToolButton(&dialog); addTool->setText(tr("+")); addTool->setToolTip(tr("Click the image to add that color to the selection"));
    auto *removeTool = new QToolButton(&dialog); removeTool->setText(tr("−")); removeTool->setToolTip(tr("Click the image to take that color out of the selection"));
    auto *toolGroup = new QButtonGroup(&dialog);
    int toolIndex = 0;
    for (QToolButton *button : {replaceTool, addTool, removeTool}) { button->setCheckable(true); toolGroup->addButton(button, toolIndex++); tools->addWidget(button); }
    replaceTool->setChecked(true);
    replaceTool->setObjectName(QStringLiteral("colorRangeSample")); addTool->setObjectName(QStringLiteral("colorRangeAdd")); removeTool->setObjectName(QStringLiteral("colorRangeRemove"));
    tools->addStretch(); layout->addLayout(tools);
    auto *preview = new QLabel(&dialog); preview->setAlignment(Qt::AlignCenter); preview->setStyleSheet(QStringLiteral("background:black;border:1px solid #444;"));
    const QSizeF fit = QSizeF(sample.size()).scaled(QSizeF(292, 200), Qt::KeepAspectRatio);
    preview->setFixedSize(fit.toSize().expandedTo(QSize(1, 1)));
    layout->addWidget(preview, 0, Qt::AlignHCenter);
    auto *hint = new QLabel(tr("Click the image to pick the color to select."), &dialog); hint->setWordWrap(true); layout->addWidget(hint);
    auto *form = new QHBoxLayout;
    auto *fuzz = new QSlider(Qt::Horizontal, &dialog); fuzz->setRange(0, 200); fuzz->setValue(40); fuzz->setObjectName(QStringLiteral("colorRangeFuzziness"));
    auto *fuzzValue = new QSpinBox(&dialog); fuzzValue->setRange(0, 200); fuzzValue->setValue(40);
    form->addWidget(new QLabel(tr("Fuzziness"), &dialog)); form->addWidget(fuzz, 1); form->addWidget(fuzzValue); layout->addLayout(form);
    auto *invert = new QCheckBox(tr("Invert"), &dialog); invert->setToolTip(tr("Select everything except those colors, such as all but a green screen")); layout->addWidget(invert);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    QVector<quint8> include, exclude;
    const auto currentMask = [&] { return RasterOperations::colorRangeMask(sample, include, exclude, fuzz->value(), invert->isChecked()); };
    const auto refresh = [&] {
        if (include.isEmpty()) { session_.previewSelection(original); preview->clear(); hint->setText(tr("Click the image to pick the color to select.")); canvas_->update(); return; }
        const QImage mask = currentMask();
        bool any = false;
        for (int y = 0; y < mask.height() && !any; ++y) { const uchar *row = mask.constScanLine(y); for (int x = 0; x < mask.width(); ++x) if (row[x]) { any = true; break; } }
        session_.previewSelection(any ? std::optional<QImage>(mask) : std::nullopt);
        preview->setPixmap(QPixmap::fromImage(mask.scaled(preview->size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)));
        hint->setText(tr("Shift-click adds a color, Option/Alt-click takes one away."));
        canvas_->update();
    };
    connect(fuzz, &QSlider::valueChanged, &dialog, [&](int value) { const QSignalBlocker b(fuzzValue); fuzzValue->setValue(value); refresh(); });
    connect(fuzzValue, &QSpinBox::valueChanged, &dialog, [&](int value) { const QSignalBlocker b(fuzz); fuzz->setValue(value); refresh(); });
    connect(invert, &QCheckBox::toggled, &dialog, [&](bool) { refresh(); });
    const CanvasWidget::Tool previousTool = canvas_->tool();
    canvas_->setTool(CanvasWidget::Tool::Eyedropper);
    colorSampleOverride_ = [&](const QPoint &point) {
        const auto color = RasterOperations::colorRangeSample(sample, point);
        if (!color) { QApplication::beep(); return; }
        const Qt::KeyboardModifiers mods = QApplication::keyboardModifiers();
        // Shift adds and Option/Alt takes away, whichever eyedropper is chosen.
        const int mode = mods.testFlag(Qt::AltModifier) ? 2 : mods.testFlag(Qt::ShiftModifier) ? 1 : toolGroup->checkedId();
        const QVector<quint8> bytes{(*color)[0], (*color)[1], (*color)[2]};
        if (mode == 0) { include = bytes; exclude.clear(); } else if (mode == 1) include += bytes; else exclude += bytes;
        refresh();
    };
    const int result = runFloatingDialog(dialog);
    colorSampleOverride_ = {}; canvas_->setTool(previousTool);
    QImage finalMask;
    if (result == QDialog::Accepted && !include.isEmpty()) finalMask = currentMask();
    session_.previewSelection(original);
    if (!finalMask.isNull()) session_.replaceSelection(finalMask, QStringLiteral("Color Range"));
    syncDocumentViews(false);
}

void MainWindow::hueSaturationDialog()
{
    Layer *active = session_.activeLayer(); const bool live = active && active->adjustment.value(QStringLiteral("kind")).toString() == QStringLiteral("Hue/Saturation");
    if (!active || (!live && active->image.isNull())) return;
    const QUuid target = active->id;
    QDialog dialog(this); dialog.setWindowTitle(tr("Hue/Saturation")); dialog.setObjectName(QStringLiteral("hueSaturationDialog"));
    auto *layout = new QVBoxLayout(&dialog); auto *top = new QHBoxLayout;
    auto *range = new QComboBox(&dialog); range->addItems({tr("Master"), tr("Reds"), tr("Yellows"), tr("Greens"), tr("Cyans"), tr("Blues"), tr("Magentas")});
    range->setObjectName(QStringLiteral("hueRange")); top->addWidget(range); top->addStretch();
    auto *sample = new QToolButton(&dialog); sample->setText(tr("Sample")); sample->setCheckable(true); sample->setToolTip(tr("Center this range on a sampled color")); sample->setObjectName(QStringLiteral("hueSample"));
    auto *addSample = new QToolButton(&dialog); addSample->setText(tr("+")); addSample->setCheckable(true); addSample->setToolTip(tr("Widen this range to include a sampled color")); addSample->setObjectName(QStringLiteral("hueSampleAdd"));
    auto *removeSample = new QToolButton(&dialog); removeSample->setText(tr("−")); removeSample->setCheckable(true); removeSample->setToolTip(tr("Narrow this range to exclude a sampled color")); removeSample->setObjectName(QStringLiteral("hueSampleRemove"));
    auto *targeted = new QToolButton(&dialog); targeted->setText(tr("Target")); targeted->setCheckable(true); targeted->setToolTip(tr("Drag on an image color to change saturation; hold Ctrl to change hue")); targeted->setObjectName(QStringLiteral("hueTargetedAdjustment"));
    for (QToolButton *button : {sample, addSample, removeSample, targeted}) top->addWidget(button);
    layout->addLayout(top); auto *form = new QFormLayout;
    auto make = [&dialog](int minimum, int maximum) { auto *field = new QSpinBox(&dialog); field->setRange(minimum, maximum); return field; };
    auto *hue = make(-180, 180); auto *saturation = make(-100, 100); auto *lightness = make(-100, 100);
    hue->setObjectName(QStringLiteral("hueValue")); saturation->setObjectName(QStringLiteral("saturationValue")); lightness->setObjectName(QStringLiteral("lightnessValue"));
    auto *colorize = new QCheckBox(tr("Colorize"), &dialog); auto *invert = new QCheckBox(tr("Apply outside this range instead"), &dialog);
    auto *previewEnabled = new QCheckBox(tr("Preview"), &dialog); previewEnabled->setChecked(true);
    auto *reset = new QPushButton(tr("Reset"), &dialog); reset->setObjectName(QStringLiteral("hueReset"));
    HueSaturationSettings settings;
    const QJsonObject originalAdjustment = active->adjustment; const Layer originalLayer = *active;
    if (live) {
        const QJsonObject hsv=originalAdjustment.value(QStringLiteral("hsvSettings")).toObject();static const QStringList names{QStringLiteral("Master"),QStringLiteral("Reds"),QStringLiteral("Yellows"),QStringLiteral("Greens"),QStringLiteral("Cyans"),QStringLiteral("Blues"),QStringLiteral("Magentas")};
        const int selected=names.indexOf(hsv.value(QStringLiteral("range")).toString());if(selected>=0)settings.range=ColorRange(selected);settings.colorize=hsv.value(QStringLiteral("colorize")).toBool(originalAdjustment.value(QStringLiteral("colorize")).toBool());settings.invertRange=hsv.value(QStringLiteral("invertRange")).toBool();
        settings.adjustments[0]={originalAdjustment.value(QStringLiteral("hue")).toDouble(),originalAdjustment.value(QStringLiteral("saturation")).toDouble(),originalAdjustment.value(QStringLiteral("lightness")).toDouble()};
        const QJsonArray values=hsv.value(QStringLiteral("adjustments")).toArray();for(int i=0;i+1<values.size();i+=2){const int r=names.indexOf(values[i].toString());const QJsonObject a=values[i+1].toObject();if(r>=0)settings.adjustments[size_t(r)]={a.value(QStringLiteral("hue")).toDouble(),a.value(QStringLiteral("saturation")).toDouble(),a.value(QStringLiteral("lightness")).toDouble()};}
        const QJsonArray savedBands=hsv.value(QStringLiteral("bands")).toArray();for(int i=0;i+1<savedBands.size();i+=2){const int r=names.indexOf(savedBands[i].toString());const QJsonObject b=savedBands[i+1].toObject();if(r>=0)settings.bands[size_t(r)]={b.value(QStringLiteral("falloffStart")).toDouble(),b.value(QStringLiteral("rangeStart")).toDouble(),b.value(QStringLiteral("rangeEnd")).toDouble(),b.value(QStringLiteral("falloffEnd")).toDouble()};}
        range->setCurrentIndex(int(settings.range));const auto &a=settings.adjustments[size_t(settings.range)];hue->setValue(qRound(a.hue));saturation->setValue(qRound(a.saturation));lightness->setValue(qRound(a.lightness));colorize->setChecked(settings.colorize);invert->setChecked(settings.invertRange);
    }
    const auto store = [&] {
        settings.range = ColorRange(range->currentIndex());
        settings.adjustments[size_t(settings.range)] = {double(hue->value()), double(saturation->value()), double(lightness->value())};
        settings.colorize = colorize->isChecked(); settings.invertRange = invert->isChecked();
    };
    auto *spectrum = new HueSpectrumWidget(&dialog); spectrum->settings = &settings; spectrum->setObjectName(QStringLiteral("hueSpectrum"));
    auto *handleText = new QLabel(&dialog); handleText->setObjectName(QStringLiteral("hueBandHandles")); handleText->setAlignment(Qt::AlignCenter);
    layout->addLayout(form); layout->addWidget(spectrum); layout->addWidget(handleText);
    const auto syncSliders = [&] {
        for (QSpinBox *field : {hue, saturation, lightness}) if (QSlider *slider = dialog.findChild<QSlider *>(field->objectName() + QStringLiteral("Slider"))) { const QSignalBlocker blocker(slider); slider->setValue(qRound((field->value() - field->minimum()) / double(field->maximum() - field->minimum()) * 1000)); }
    };
    auto refreshBand = [&] {
        const HueBand &band = settings.bands[size_t(settings.range)];
        handleText->setText(tr("%1°   %2°   %3°   %4°").arg(qRound(band.falloffStart)).arg(qRound(band.rangeStart)).arg(qRound(band.rangeEnd)).arg(qRound(band.falloffEnd)));
        const bool spectrumVisible = settings.range != ColorRange::Master && !settings.colorize;
        spectrum->setVisible(spectrumVisible); handleText->setVisible(spectrumVisible); invert->setVisible(spectrumVisible);
        for (QToolButton *button : {sample, addSample, removeSample}) button->setVisible(spectrumVisible);
        targeted->setEnabled(!settings.colorize); spectrum->update();
    };
    connect(range, &QComboBox::currentIndexChanged, &dialog, [&, previous = range->currentIndex()](int current) mutable {
        settings.adjustments[size_t(previous)] = {double(hue->value()), double(saturation->value()), double(lightness->value())};
        previous = current; const auto &value = settings.adjustments[size_t(current)];
        const QSignalBlocker bh(hue), bs(saturation), bl(lightness); hue->setValue(qRound(value.hue)); saturation->setValue(qRound(value.saturation)); lightness->setValue(qRound(value.lightness));
        settings.range = ColorRange(current); refreshBand();
    });
    form->addRow(tr("Range"), range); addScrubRow(form, tr("Hue"), hue); addScrubRow(form, tr("Saturation"), saturation); addScrubRow(form, tr("Lightness"), lightness);
    const auto liveValue = [&] { store(); return hueAdjustment(settings); };
    session_.beginEdit(live ? QStringLiteral("Edit Hue/Saturation") : QStringLiteral("Hue/Saturation"));
    const auto preview = [this, live, target, liveValue, originalLayer, &settings, previewEnabled] {
        if (!previewEnabled->isChecked()) {
            if (live) session_.previewAdjustment(target, originalLayer.adjustment);
            else restoreLayer(document_.get(), session_, target, originalLayer);
        } else if(live) session_.previewAdjustment(target,liveValue());
        else { restoreLayer(document_.get(), session_, target, originalLayer); liveValue(); session_.applyHueSaturation(settings); }
        canvas_->invalidateDocument();
    };
    connect(hue, qOverload<int>(&QSpinBox::valueChanged), &dialog, preview); connect(saturation, qOverload<int>(&QSpinBox::valueChanged), &dialog, preview); connect(lightness, qOverload<int>(&QSpinBox::valueChanged), &dialog, preview); connect(colorize, &QCheckBox::toggled, &dialog, preview); connect(invert, &QCheckBox::toggled, &dialog, preview);
    connect(previewEnabled, &QCheckBox::toggled, &dialog, preview);
    spectrum->changed = [&, preview] { refreshBand(); preview(); };
    const CanvasWidget::Tool previousTool = canvas_->tool(); int sampleMode = -1;
    auto stopCanvasModes = [&] {
        sampleMode = -1; canvas_->setHueTargeting(false);
        for (QToolButton *button : {sample, addSample, removeSample, targeted}) { const QSignalBlocker blocker(button); button->setChecked(false); }
        canvas_->setTool(previousTool);
    };
    const auto armSample = [&](QToolButton *chosen, int mode, bool checked) {
        stopCanvasModes(); if (!checked) return; sampleMode = mode;
        { const QSignalBlocker blocker(chosen); chosen->setChecked(true); }
        canvas_->setTool(CanvasWidget::Tool::Eyedropper);
    };
    connect(sample, &QToolButton::toggled, &dialog, [&, armSample](bool checked) { armSample(sample, 0, checked); });
    connect(addSample, &QToolButton::toggled, &dialog, [&, armSample](bool checked) { armSample(addSample, 1, checked); });
    connect(removeSample, &QToolButton::toggled, &dialog, [&, armSample](bool checked) { armSample(removeSample, 2, checked); });
    connect(targeted, &QToolButton::toggled, &dialog, [&](bool checked) {
        stopCanvasModes(); if (checked) { const QSignalBlocker blocker(targeted); targeted->setChecked(true); canvas_->setHueTargeting(true); }
    });
    const auto sampledHue = [this](const QPoint &point) -> std::optional<double> {
        if (!document_ || !QRect(QPoint(), document_->canvasSize).contains(point)) return std::nullopt;
        const QColor color = LayerRenderer::flattened(*document_).pixelColor(point); float h = 0, s = 0, l = 0; color.getHslF(&h, &s, &l);
        if (!color.isValid() || color.alpha() == 0 || s <= .02f || h < 0) return std::nullopt;
        return h * 360.0;
    };
    colorSampleOverride_ = [&, preview](const QPoint &point) {
        if (sampleMode < 0 || settings.range == ColorRange::Master || settings.colorize) return;
        const auto sampledHueValue = sampledHue(point); if (!sampledHueValue) { QApplication::beep(); return; }
        HueBand &band = settings.bands[size_t(settings.range)];
        if (sampleMode == 0) band = band.centered(*sampledHueValue); else if (sampleMode == 1) band.include(*sampledHueValue); else band.exclude(*sampledHueValue);
        refreshBand(); preview();
    };
    ColorRange targetRange = ColorRange::Master; RangeAdjustment targetStart; bool targetValid = false;
    connect(canvas_, &CanvasWidget::hueTargetStarted, &dialog, [&](const QPoint &point) {
        store(); const auto value = sampledHue(point); if (!value) { targetValid = false; QApplication::beep(); return; }
        double best = -1; for (int i = 1; i < int(ColorRange::Count); ++i) { const double weight = settings.weight(ColorRange(i), *value); if (weight > best) { best = weight; targetRange = ColorRange(i); } }
        targetValid = true; targetStart = settings.adjustments[size_t(targetRange)]; range->setCurrentIndex(int(targetRange)); preview();
    });
    connect(canvas_, &CanvasWidget::hueTargetDragged, &dialog, [&](double delta, bool adjustsHue) {
        if (!targetValid) return;
        RangeAdjustment &value = settings.adjustments[size_t(targetRange)];
        if (adjustsHue) value.hue = std::clamp(targetStart.hue + delta / 2, -180.0, 180.0);
        else value.saturation = std::clamp(targetStart.saturation + delta / 2, -100.0, 100.0);
        const QSignalBlocker bh(hue), bs(saturation); hue->setValue(qRound(value.hue)); saturation->setValue(qRound(value.saturation)); spectrum->update(); preview();
    });
    connect(canvas_, &CanvasWidget::hueTargetFinished, &dialog, [&] { targetValid = false; });
    connect(colorize, &QCheckBox::toggled, &dialog, [&](bool enabled) {
        stopCanvasModes(); settings = enabled ? HueSaturationSettings(0, 25, 0, true) : HueSaturationSettings();
        { const QSignalBlocker br(range), bh(hue), bs(saturation), bl(lightness), bc(colorize), bi(invert); range->setCurrentIndex(0); hue->setRange(enabled ? 0 : -180, enabled ? 360 : 180); saturation->setRange(enabled ? 0 : -100, 100); hue->setValue(0); saturation->setValue(enabled ? 25 : 0); lightness->setValue(0); colorize->setChecked(enabled); invert->setChecked(false); }
        range->setEnabled(!enabled); syncSliders(); refreshBand(); preview();
    });
    connect(reset, &QPushButton::clicked, &dialog, [&] {
        const bool enabled = colorize->isChecked(); settings = enabled ? HueSaturationSettings(0, 25, 0, true) : HueSaturationSettings();
        { const QSignalBlocker br(range), bh(hue), bs(saturation), bl(lightness), bi(invert); range->setCurrentIndex(0); hue->setValue(0); saturation->setValue(enabled ? 25 : 0); lightness->setValue(0); invert->setChecked(false); }
        refreshBand(); preview();
    });
    auto *options = new QHBoxLayout; options->addWidget(colorize); options->addWidget(previewEnabled); options->addWidget(reset); options->addStretch(); layout->addLayout(options); layout->addWidget(invert);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
    range->setEnabled(!settings.colorize); hue->setRange(settings.colorize ? 0 : -180, settings.colorize ? 360 : 180); saturation->setRange(settings.colorize ? 0 : -100, 100);
    { const RangeAdjustment &selected = settings.adjustments[size_t(settings.range)]; const QSignalBlocker bh(hue), bs(saturation), bl(lightness); hue->setValue(qRound(selected.hue)); saturation->setValue(qRound(selected.saturation)); lightness->setValue(qRound(selected.lightness)); }
    syncSliders(); refreshBand();
    const int result = runFloatingDialog(dialog); stopCanvasModes(); colorSampleOverride_ = {};
    if(result==QDialog::Accepted) { previewEnabled->setChecked(true); preview(); } else if(live)session_.previewAdjustment(target,originalAdjustment);else restoreLayer(document_.get(), session_, target, originalLayer);
    session_.endEdit();syncDocumentViews();
}

void MainWindow::curvesDialog()
{
    Layer *active = session_.activeLayer(); const bool live = active && active->adjustment.value(QStringLiteral("kind")).toString() == QStringLiteral("Curves");
    if (!active || (!live && active->image.isNull())) return;
    const QUuid target = active->id;
    QDialog dialog(this); dialog.setWindowTitle(tr("Curves")); dialog.resize(390, 430);
    auto *layout = new QVBoxLayout(&dialog); auto *channel = new QComboBox(&dialog); channel->addItems({tr("RGB"), tr("Red"), tr("Green"), tr("Blue")});
    channel->setObjectName(QStringLiteral("curvesChannel")); auto *graph = new CurveEditorWidget(&dialog); graph->setObjectName(QStringLiteral("curvesGraph")); auto *form = new QFormLayout;
    auto *input = new QSpinBox(&dialog); input->setRange(0, 255); auto *output = new QSpinBox(&dialog); output->setRange(0, 255);
    input->setObjectName(QStringLiteral("curveInput")); output->setObjectName(QStringLiteral("curveOutput"));
    auto *inLabel = new ScrubLabel(tr("Input"), input, 1.0, 1.0, &dialog); inLabel->setObjectName(QStringLiteral("curveInputLabel"));
    auto *outLabel = new ScrubLabel(tr("Output"), output, 1.0, 1.0, &dialog); outLabel->setObjectName(QStringLiteral("curveOutputLabel"));
    form->addRow(inLabel, input); form->addRow(outLabel, output);
    auto *hint = new QLabel(tr("Click to add a point. Drag to adjust."), &dialog); auto *pointButtons = new QHBoxLayout;
    auto *status = new QLabel(&dialog); status->setObjectName(QStringLiteral("curvePointStatus")); auto *remove = new QPushButton(tr("Remove point"), &dialog); auto *reset = new QPushButton(tr("Reset curve"), &dialog);
    remove->setObjectName(QStringLiteral("removeCurvePoint")); reset->setObjectName(QStringLiteral("resetCurve")); pointButtons->addWidget(status); pointButtons->addStretch(); pointButtons->addWidget(remove);
    auto *previewEnabled = new QCheckBox(tr("Preview"), &dialog); previewEnabled->setChecked(true); previewEnabled->setObjectName(QStringLiteral("filterPreview"));
    layout->addWidget(channel); layout->addWidget(graph, 1); layout->addWidget(hint); layout->addLayout(form); layout->addLayout(pointButtons); layout->addWidget(reset); layout->addWidget(previewEnabled);
    CurvesSettings settings; const QJsonObject originalAdjustment=active->adjustment; const Layer originalLayer=*active;
    if(live){const QJsonArray saved=originalAdjustment.value(QStringLiteral("curves")).toObject().value(QStringLiteral("channels")).toArray();for(int c=0;c<std::min(4,int(saved.size()));++c){QVector<CurvePoint> curve;for(const QJsonValue &entry:saved[c].toArray()){const QJsonObject point=entry.toObject();curve.push_back({point.value(QStringLiteral("x")).toDouble(),point.value(QStringLiteral("y")).toDouble()});}if(!curve.isEmpty())settings.channels[size_t(c)]=curve;}}
    graph->settings = &settings;
    const auto preview=[this,live,target,&settings,originalLayer,previewEnabled]{if(!previewEnabled->isChecked()){if(live)session_.previewAdjustment(target,originalLayer.adjustment);else restoreLayer(document_.get(), session_, target, originalLayer);}else if(live)session_.previewAdjustment(target,curvesAdjustment(settings));else{restoreLayer(document_.get(), session_, target, originalLayer);session_.applyCurves(settings);}canvas_->invalidateDocument();};
    session_.beginEdit(live?QStringLiteral("Edit Curves"):QStringLiteral("Curves"));
    const auto refreshSelection = [&](int row) {
        const auto &curve = settings.channels[size_t(channel->currentIndex())]; const bool valid = row >= 0 && row < curve.size();
        input->setEnabled(valid && row > 0 && row + 1 < curve.size()); output->setEnabled(valid); remove->setEnabled(valid && row > 0 && row + 1 < curve.size());
        if (valid) { const QSignalBlocker bi(input), bo(output); input->setValue(qRound(curve[row].x)); output->setValue(qRound(curve[row].y)); status->setText(tr("Input %1 · Output %2").arg(qRound(curve[row].x)).arg(qRound(curve[row].y))); }
        else status->clear();
    };
    graph->selectionChanged = refreshSelection; graph->changed = [&, preview] { refreshSelection(graph->selectedIndex()); preview(); };
    connect(channel, &QComboBox::currentIndexChanged, &dialog, [&](int value) { settings.channel = value; graph->clearSelection(); graph->update(); });
    connect(input, &QSpinBox::valueChanged, &dialog, [&](int value) {
        auto &curve = settings.channels[size_t(channel->currentIndex())]; const int row = graph->selectedIndex(); if (row <= 0 || row + 1 >= curve.size()) return;
        curve[row].x = std::clamp(double(value), curve[row - 1].x + 1, curve[row + 1].x - 1); graph->update(); refreshSelection(row); preview();
    });
    connect(output, &QSpinBox::valueChanged, &dialog, [&](int value) {
        auto &curve = settings.channels[size_t(channel->currentIndex())]; const int row = graph->selectedIndex(); if (row < 0 || row >= curve.size()) return;
        curve[row].y = value; graph->update(); refreshSelection(row); preview();
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] { auto &curve = settings.channels[size_t(channel->currentIndex())]; const int row = graph->selectedIndex(); if (row > 0 && row + 1 < curve.size()) { curve.removeAt(row); graph->clearSelection(); graph->update(); preview(); } });
    connect(reset, &QPushButton::clicked, &dialog, [&] { settings.channels[size_t(channel->currentIndex())] = {{0, 0}, {255, 255}}; graph->clearSelection(); graph->update(); preview(); });
    connect(previewEnabled, &QCheckBox::toggled, &dialog, preview);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
    refreshSelection(-1);
    const int result=runFloatingDialog(dialog);
    if(result==QDialog::Accepted){previewEnabled->setChecked(true);preview();}else if(live)session_.previewAdjustment(target,originalAdjustment);else restoreLayer(document_.get(), session_, target, originalLayer);
    session_.endEdit();syncDocumentViews();
}

void MainWindow::gradientMapDialog()
{
    Layer *active = session_.activeLayer(); const bool live = active && active->adjustment.value(QStringLiteral("kind")).toString() == QStringLiteral("Gradient Map");
    if (!active || (!live && active->image.isNull())) return;
    const QUuid target = active->id;
    QDialog dialog(this); dialog.setWindowTitle(tr("Gradient Map")); auto *layout = new QVBoxLayout(&dialog);
    QColor shadows = Qt::black, highlights = Qt::white;
    const QJsonObject originalAdjustment = active->adjustment; const Layer originalLayer=*active;
    if (live) { const QJsonObject saved = originalAdjustment.value(QStringLiteral("gradientMapSettings")).toObject(); const auto read = [](const QJsonObject &c, const QColor &fallback) { return c.isEmpty() ? fallback : QColor::fromRgbF(c.value(QStringLiteral("red")).toDouble(), c.value(QStringLiteral("green")).toDouble(), c.value(QStringLiteral("blue")).toDouble()); }; shadows = read(saved.value(QStringLiteral("shadows")).toObject(), Qt::black); highlights = read(saved.value(QStringLiteral("highlights")).toObject(), Qt::white); }
    auto *shadowButton = new QPushButton(tr("Choose shadow color…"), &dialog); auto *highlightButton = new QPushButton(tr("Choose highlight color…"), &dialog);
    shadowButton->setObjectName(QStringLiteral("gradientMapShadows")); highlightButton->setObjectName(QStringLiteral("gradientMapHighlights"));
    auto *gradientView = new QLabel(&dialog); gradientView->setFixedHeight(20); gradientView->setMinimumWidth(300); gradientView->setObjectName(QStringLiteral("gradientMapPreview"));
    auto *reverse = new QCheckBox(tr("Reverse"), &dialog);
    auto *previewEnabled = new QCheckBox(tr("Preview"), &dialog); previewEnabled->setChecked(true); previewEnabled->setObjectName(QStringLiteral("filterPreview"));
    const auto value = [&] { const auto color = [](const QColor &c) { return QJsonObject{{QStringLiteral("red"), c.redF()}, {QStringLiteral("green"), c.greenF()}, {QStringLiteral("blue"), c.blueF()}}; }; return QJsonObject{{QStringLiteral("kind"), QStringLiteral("Gradient Map")}, {QStringLiteral("gradientMapSettings"), QJsonObject{{QStringLiteral("shadows"), color(shadows)}, {QStringLiteral("highlights"), color(highlights)}, {QStringLiteral("reversed"), reverse->isChecked()}}}}; };
    if (live) reverse->setChecked(originalAdjustment.value(QStringLiteral("gradientMapSettings")).toObject().value(QStringLiteral("reversed")).toBool());
    session_.beginEdit(live?QStringLiteral("Edit Gradient Map"):QStringLiteral("Gradient Map"));
    const auto preview = [this, live, target, value, originalLayer, &shadows, &highlights, reverse, previewEnabled] { if(!previewEnabled->isChecked()){if(live)session_.previewAdjustment(target,originalLayer.adjustment);else restoreLayer(document_.get(), session_, target, originalLayer);}else if(live)session_.previewAdjustment(target,value());else{restoreLayer(document_.get(), session_, target, originalLayer);session_.applyGradientMap(shadows,highlights,reverse->isChecked());}canvas_->invalidateDocument(); };
    const auto refreshGradient = [&] {
        QImage strip(std::max(1, gradientView->width()), 20, QImage::Format_RGB32); QPainter painter(&strip); QLinearGradient gradient(0, 0, strip.width(), 0); const QColor first = reverse->isChecked() ? highlights : shadows; const QColor last = reverse->isChecked() ? shadows : highlights; gradient.setColorAt(0, first); gradient.setColorAt(1, last); painter.fillRect(strip.rect(), gradient); painter.end(); gradientView->setPixmap(QPixmap::fromImage(strip));
        shadowButton->setStyleSheet(QStringLiteral("text-align:left;padding-left:32px;background:%1;").arg(shadows.name())); highlightButton->setStyleSheet(QStringLiteral("text-align:left;padding-left:32px;background:%1;").arg(highlights.name()));
    };
    const auto pickColor = [&](QColor &color, const QString &title) {
        const QColor original = color; QColorDialog picker(color, &dialog); picker.setWindowTitle(title); picker.setOption(QColorDialog::DontUseNativeDialog); picker.setWindowModality(Qt::NonModal);
        connect(&picker, &QColorDialog::currentColorChanged, &dialog, [&](const QColor &next) { if (!next.isValid()) return; color = next.toRgb(); refreshGradient(); preview(); });
        if (runFloatingDialog(picker) != QDialog::Accepted) { color = original; refreshGradient(); preview(); }
    };
    connect(shadowButton, &QPushButton::clicked, &dialog, [&] { pickColor(shadows, tr("Shadow Color")); });
    connect(highlightButton, &QPushButton::clicked, &dialog, [&] { pickColor(highlights, tr("Highlight Color")); });
    connect(reverse, &QCheckBox::toggled, &dialog, preview);
    connect(reverse, &QCheckBox::toggled, &dialog, refreshGradient); layout->addWidget(gradientView); layout->addWidget(shadowButton); layout->addWidget(highlightButton); layout->addWidget(reverse); layout->addWidget(previewEnabled); connect(previewEnabled, &QCheckBox::toggled, &dialog, preview); refreshGradient();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
    const int result = runFloatingDialog(dialog);
    if(result==QDialog::Accepted){previewEnabled->setChecked(true);preview();}else if(live)session_.previewAdjustment(target,originalAdjustment);else restoreLayer(document_.get(), session_, target, originalLayer);
    session_.endEdit();syncDocumentViews();
}

void MainWindow::grainDialog()
{
    Layer *active = session_.activeLayer(); const bool live = active && active->adjustment.value(QStringLiteral("kind")).toString() == QStringLiteral("Grain");
    if (!active || (!live && active->image.isNull())) return;
    const QUuid target = active->id;
    QDialog dialog(this); dialog.setWindowTitle(tr("Grain")); auto *layout = new QVBoxLayout(&dialog); auto *form = new QFormLayout;
    auto make = [&dialog](double minimum, double maximum, double value, int decimals = 0) { auto *field = new QDoubleSpinBox(&dialog); field->setRange(minimum, maximum); field->setValue(value); field->setDecimals(decimals); return field; };
    auto *amount = make(0, 100, 25, 0); auto *size = make(.5, 20, 1.5, 1); auto *roughness = make(0, 100, 50, 0);
    amount->setObjectName(QStringLiteral("grainAmount")); size->setObjectName(QStringLiteral("grainSize")); roughness->setObjectName(QStringLiteral("grainRoughness"));
    auto *previewEnabled = new QCheckBox(tr("Preview"), &dialog); previewEnabled->setChecked(true); previewEnabled->setObjectName(QStringLiteral("filterPreview"));
    const QJsonObject originalAdjustment = active->adjustment; const Layer originalLayer=*active; quint32 seed = QRandomGenerator::global()->generate();
    if (live) { const QJsonObject saved = originalAdjustment.value(QStringLiteral("grainSettings")).toObject(); amount->setValue(saved.value(QStringLiteral("amount")).toDouble(25)); size->setValue(saved.value(QStringLiteral("size")).toDouble(1.5)); roughness->setValue(saved.value(QStringLiteral("roughness")).toDouble(50)); seed = quint32(saved.value(QStringLiteral("seed")).toDouble(seed)); }
    const auto value = [&] { return QJsonObject{{QStringLiteral("kind"), QStringLiteral("Grain")}, {QStringLiteral("grainSettings"), QJsonObject{{QStringLiteral("amount"), amount->value()}, {QStringLiteral("size"), size->value()}, {QStringLiteral("roughness"), roughness->value()}, {QStringLiteral("seed"), double(seed)}}}}; };
    session_.beginEdit(live?QStringLiteral("Edit Grain"):QStringLiteral("Grain"));const auto preview=[this,live,target,value,originalLayer,amount,size,roughness,seed,previewEnabled]{if(!previewEnabled->isChecked()){if(live)session_.previewAdjustment(target,originalLayer.adjustment);else restoreLayer(document_.get(), session_, target, originalLayer);}else if(live)session_.previewAdjustment(target,value());else{restoreLayer(document_.get(), session_, target, originalLayer);session_.applyGrain(amount->value(),size->value(),roughness->value(),seed);}canvas_->invalidateDocument();};connect(amount,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,preview);connect(size,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,preview);connect(roughness,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,preview);
    addScrubRow(form, tr("Amount"), amount); addScrubRow(form, tr("Size"), size, true); addScrubRow(form, tr("Roughness"), roughness); layout->addLayout(form); layout->addWidget(previewEnabled); connect(previewEnabled,&QCheckBox::toggled,&dialog,preview);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
    const int result = runFloatingDialog(dialog);
    if(result==QDialog::Accepted){previewEnabled->setChecked(true);preview();}else if(live)session_.previewAdjustment(target,originalAdjustment);else restoreLayer(document_.get(), session_, target, originalLayer);
    session_.endEdit();syncDocumentViews();
}

void MainWindow::blackWhiteDialog()
{
    Layer *active = session_.activeLayer();
    const bool live = active && active->adjustment.value(QStringLiteral("kind")).toString() == QStringLiteral("Black & White");
    if (!active || (!live && active->image.isNull())) return;
    const QUuid target = active->id;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Black & White"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;

    auto make = [&dialog](double min, double max, double val) {
        auto *field = new QDoubleSpinBox(&dialog);
        field->setRange(min, max);
        field->setValue(val);
        field->setDecimals(0);
        field->setSuffix(QStringLiteral(" %"));
        return field;
    };

    auto *reds = make(-200, 300, 40);
    auto *yellows = make(-200, 300, 60);
    auto *greens = make(-200, 300, 40);
    auto *cyans = make(-200, 300, 60);
    auto *blues = make(-200, 300, 20);
    auto *magentas = make(-200, 300, 80);

    auto *tint = new QCheckBox(tr("Tint"), &dialog);
    tint->setToolTip(tr("Color the result while keeping its tones, for a sepia or a cyanotype"));

    auto *tintHue = new QDoubleSpinBox(&dialog);
    tintHue->setRange(0, 360);
    tintHue->setValue(40);
    tintHue->setDecimals(0);
    tintHue->setSuffix(QStringLiteral("°"));

    auto *tintSaturation = new QDoubleSpinBox(&dialog);
    tintSaturation->setRange(0, 100);
    tintSaturation->setValue(20);
    tintSaturation->setDecimals(0);
    tintSaturation->setSuffix(QStringLiteral(" %"));

    auto *previewEnabled = new QCheckBox(tr("Preview"), &dialog);
    previewEnabled->setChecked(true);
    previewEnabled->setObjectName(QStringLiteral("filterPreview"));

    const QJsonObject originalAdjustment = active->adjustment;
    const Layer originalLayer = *active;

    if (live) {
        const QJsonObject saved = originalAdjustment.value(QStringLiteral("blackWhiteSettings")).toObject();
        reds->setValue(saved.value(QStringLiteral("reds")).toDouble(40));
        yellows->setValue(saved.value(QStringLiteral("yellows")).toDouble(60));
        greens->setValue(saved.value(QStringLiteral("greens")).toDouble(40));
        cyans->setValue(saved.value(QStringLiteral("cyans")).toDouble(60));
        blues->setValue(saved.value(QStringLiteral("blues")).toDouble(20));
        magentas->setValue(saved.value(QStringLiteral("magentas")).toDouble(80));
        tint->setChecked(saved.value(QStringLiteral("tint")).toBool(false));
        tintHue->setValue(saved.value(QStringLiteral("tintHue")).toDouble(40));
        tintSaturation->setValue(saved.value(QStringLiteral("tintSaturation")).toDouble(20));
    }

    const auto value = [&] {
        return QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("Black & White")},
            {QStringLiteral("blackWhiteSettings"), QJsonObject{
                {QStringLiteral("reds"), reds->value()},
                {QStringLiteral("yellows"), yellows->value()},
                {QStringLiteral("greens"), greens->value()},
                {QStringLiteral("cyans"), cyans->value()},
                {QStringLiteral("blues"), blues->value()},
                {QStringLiteral("magentas"), magentas->value()},
                {QStringLiteral("tint"), tint->isChecked()},
                {QStringLiteral("tintHue"), tintHue->value()},
                {QStringLiteral("tintSaturation"), tintSaturation->value()}
            }}
        };
    };

    session_.beginEdit(live ? QStringLiteral("Edit Black & White") : QStringLiteral("Black & White"));

    const auto preview = [this, live, target, value, originalLayer, reds, yellows, greens, cyans, blues, magentas, tint, tintHue, tintSaturation, previewEnabled] {
        if (!previewEnabled->isChecked()) {
            if (live) session_.previewAdjustment(target, originalLayer.adjustment);
            else {
                restoreLayer(document_.get(), session_, target, originalLayer);
            }
        } else if (live) {
            session_.previewAdjustment(target, value());
        } else {
            restoreLayer(document_.get(), session_, target, originalLayer);
            const float weights[6] = {
                float(reds->value() / 100.0), float(yellows->value() / 100.0), float(greens->value() / 100.0),
                float(cyans->value() / 100.0), float(blues->value() / 100.0), float(magentas->value() / 100.0)
            };
            session_.applyBlackWhite(weights, tint->isChecked(), tintHue->value(), tintSaturation->value() / 100.0);
        }
        canvas_->invalidateDocument();
    };

    connect(reds, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(yellows, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(greens, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(cyans, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(blues, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(magentas, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(tint, &QCheckBox::toggled, &dialog, preview);
    connect(tintHue, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(tintSaturation, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(previewEnabled, &QCheckBox::toggled, &dialog, preview);

    addScrubRow(form, tr("Reds"), reds);
    addScrubRow(form, tr("Yellows"), yellows);
    addScrubRow(form, tr("Greens"), greens);
    addScrubRow(form, tr("Cyans"), cyans);
    addScrubRow(form, tr("Blues"), blues);
    addScrubRow(form, tr("Magentas"), magentas);
    form->addRow(tint);
    auto *tintHueRow = sliderField(tintHue);
    auto *tintSatRow = sliderField(tintSaturation);
    auto *tintHueLabel = new ScrubLabel(tr("Hue"), tintHue, 1.0, std::nullopt, &dialog);
    auto *tintSatLabel = new ScrubLabel(tr("Saturation"), tintSaturation, 1.0, std::nullopt, &dialog);
    form->addRow(tintHueLabel, tintHueRow);
    form->addRow(tintSatLabel, tintSatRow);
    auto updateTintVisibility = [tint, tintHueRow, tintSatRow, tintHueLabel, tintSatLabel] {
        const bool on = tint->isChecked();
        tintHueRow->setEnabled(on);
        tintSatRow->setEnabled(on);
        tintHueLabel->setEnabled(on);
        tintSatLabel->setEnabled(on);
    };
    updateTintVisibility();
    connect(tint, &QCheckBox::toggled, &dialog, updateTintVisibility);

    layout->addLayout(form);
    layout->addWidget(previewEnabled);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    const int result = runFloatingDialog(dialog);
    if (result == QDialog::Accepted) {
        previewEnabled->setChecked(true);
        preview();
    } else if (live) {
        session_.previewAdjustment(target, originalAdjustment);
    } else {
        restoreLayer(document_.get(), session_, target, originalLayer);
    }
    session_.endEdit();
    syncDocumentViews();
}

void MainWindow::colorBalanceDialog()
{
    Layer *active = session_.activeLayer();
    const bool live = active && active->adjustment.value(QStringLiteral("kind")).toString() == QStringLiteral("Color Balance");
    if (!active || (!live && active->image.isNull())) return;
    const QUuid target = active->id;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Color Balance"));
    auto *layout = new QVBoxLayout(&dialog);

    auto make = [&dialog](double val) {
        auto *field = new QDoubleSpinBox(&dialog);
        field->setRange(-100, 100);
        field->setValue(val);
        field->setDecimals(0);
        return field;
    };

    auto *sCR = make(0); auto *sMG = make(0); auto *sYB = make(0);
    auto *mCR = make(0); auto *mMG = make(0); auto *mYB = make(0);
    auto *hCR = make(0); auto *hMG = make(0); auto *hYB = make(0);

    auto *preserveLuminosity = new QCheckBox(tr("Preserve Luminosity"), &dialog);
    preserveLuminosity->setChecked(true);

    auto *previewEnabled = new QCheckBox(tr("Preview"), &dialog);
    previewEnabled->setChecked(true);
    previewEnabled->setObjectName(QStringLiteral("filterPreview"));

    const QJsonObject originalAdjustment = active->adjustment;
    const Layer originalLayer = *active;

    if (live) {
        const QJsonObject saved = originalAdjustment.value(QStringLiteral("colorBalanceSettings")).toObject();
        sCR->setValue(saved.value(QStringLiteral("shadowCyanRed")).toDouble(0));
        sMG->setValue(saved.value(QStringLiteral("shadowMagentaGreen")).toDouble(0));
        sYB->setValue(saved.value(QStringLiteral("shadowYellowBlue")).toDouble(0));
        mCR->setValue(saved.value(QStringLiteral("midCyanRed")).toDouble(0));
        mMG->setValue(saved.value(QStringLiteral("midMagentaGreen")).toDouble(0));
        mYB->setValue(saved.value(QStringLiteral("midYellowBlue")).toDouble(0));
        hCR->setValue(saved.value(QStringLiteral("highlightCyanRed")).toDouble(0));
        hMG->setValue(saved.value(QStringLiteral("highlightMagentaGreen")).toDouble(0));
        hYB->setValue(saved.value(QStringLiteral("highlightYellowBlue")).toDouble(0));
        preserveLuminosity->setChecked(saved.value(QStringLiteral("preserveLuminosity")).toBool(true));
    }

    auto *tabs = new QTabWidget(&dialog);
    auto createTab = [&](QDoubleSpinBox *cr, QDoubleSpinBox *mg, QDoubleSpinBox *yb) {
        auto *tab = new QWidget(&dialog);
        auto *tabForm = new QFormLayout(tab);
        addScrubRow(tabForm, tr("Cyan / Red"), cr);
        addScrubRow(tabForm, tr("Magenta / Green"), mg);
        addScrubRow(tabForm, tr("Yellow / Blue"), yb);
        return tab;
    };
    tabs->addTab(createTab(sCR, sMG, sYB), tr("Shadows"));
    tabs->addTab(createTab(mCR, mMG, mYB), tr("Midtones"));
    tabs->addTab(createTab(hCR, hMG, hYB), tr("Highlights"));
    tabs->setCurrentIndex(1); // Default to Midtones

    const auto value = [&] {
        return QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("Color Balance")},
            {QStringLiteral("colorBalanceSettings"), QJsonObject{
                {QStringLiteral("shadowCyanRed"), sCR->value()},
                {QStringLiteral("shadowMagentaGreen"), sMG->value()},
                {QStringLiteral("shadowYellowBlue"), sYB->value()},
                {QStringLiteral("midCyanRed"), mCR->value()},
                {QStringLiteral("midMagentaGreen"), mMG->value()},
                {QStringLiteral("midYellowBlue"), mYB->value()},
                {QStringLiteral("highlightCyanRed"), hCR->value()},
                {QStringLiteral("highlightMagentaGreen"), hMG->value()},
                {QStringLiteral("highlightYellowBlue"), hYB->value()},
                {QStringLiteral("preserveLuminosity"), preserveLuminosity->isChecked()}
            }}
        };
    };

    session_.beginEdit(live ? QStringLiteral("Edit Color Balance") : QStringLiteral("Color Balance"));

    const auto preview = [this, live, target, value, originalLayer, sCR, sMG, sYB, mCR, mMG, mYB, hCR, hMG, hYB, preserveLuminosity, previewEnabled] {
        if (!previewEnabled->isChecked()) {
            if (live) session_.previewAdjustment(target, originalLayer.adjustment);
            else restoreLayer(document_.get(), session_, target, originalLayer);
        } else if (live) {
            session_.previewAdjustment(target, value());
        } else {
            restoreLayer(document_.get(), session_, target, originalLayer);
            const float shadows[3] = { float(sCR->value() / 100.0), float(sMG->value() / 100.0), float(sYB->value() / 100.0) };
            const float midtones[3] = { float(mCR->value() / 100.0), float(mMG->value() / 100.0), float(mYB->value() / 100.0) };
            const float highlights[3] = { float(hCR->value() / 100.0), float(hMG->value() / 100.0), float(hYB->value() / 100.0) };
            session_.applyColorBalance(shadows, midtones, highlights, preserveLuminosity->isChecked());
        }
        canvas_->invalidateDocument();
    };

    for (auto *f : {sCR, sMG, sYB, mCR, mMG, mYB, hCR, hMG, hYB}) {
        connect(f, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    }
    connect(preserveLuminosity, &QCheckBox::toggled, &dialog, preview);
    connect(previewEnabled, &QCheckBox::toggled, &dialog, preview);

    layout->addWidget(tabs);
    layout->addWidget(preserveLuminosity);
    layout->addWidget(previewEnabled);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    const int result = runFloatingDialog(dialog);
    if (result == QDialog::Accepted) {
        previewEnabled->setChecked(true);
        preview();
    } else if (live) {
        session_.previewAdjustment(target, originalAdjustment);
    } else {
        restoreLayer(document_.get(), session_, target, originalLayer);
    }
    session_.endEdit();
    syncDocumentViews();
}

void MainWindow::cameraRawDialog()
{
    Layer *active = session_.activeLayer();
    if (!active || active->image.isNull()) return;
    const QUuid target = active->id;

    const CanvasWidget::Tool previousTool = canvas_->tool();
    CameraRawDialog dialog(this, session_, target, [this] {
        canvas_->invalidateDocument();
        syncDocumentViews(false);
    });

    if (auto *wbBtn = dialog.whiteBalanceEyedropper()) {
        connect(wbBtn, &QPushButton::toggled, &dialog, [this, &dialog, wbBtn, previousTool](bool checked) {
            canvas_->setTool(checked ? CanvasWidget::Tool::Eyedropper : previousTool);
            if (!checked) {
                colorSampleOverride_ = {};
            } else {
                colorSampleOverride_ = [this, &dialog, wbBtn, previousTool](const QPoint &point) {
                    const auto sampled = session_.levelsSampleAt(point);
                    if (sampled) {
                        dialog.sampleWhiteBalance(*sampled);
                    }
                    wbBtn->setChecked(false);
                    colorSampleOverride_ = {};
                    canvas_->setTool(previousTool);
                };
            }
        });
    }

    runFloatingDialog(dialog);
    colorSampleOverride_ = {};
    canvas_->setTool(previousTool);
    syncDocumentViews();
}

void MainWindow::gaussianBlurDialog()
{
    Layer *active = session_.activeLayer();
    const bool live = active && active->adjustment.value(QStringLiteral("kind")).toString() == QStringLiteral("Gaussian Blur");
    if (!active || (!live && active->image.isNull())) return;
    const QUuid target = active->id;
    const Layer original = *active;
    const QJsonObject originalAdjustment = active->adjustment;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Gaussian Blur"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *radius = new QDoubleSpinBox(&dialog);
    radius->setObjectName(QStringLiteral("gaussianBlurRadius"));
    radius->setDecimals(1);
    radius->setRange(.1, 250);
    radius->setValue(live ? originalAdjustment.value(QStringLiteral("blurRadius")).toDouble(10.0) : 3.0);
    radius->setSuffix(tr(" px"));
    addScrubRow(form, tr("Radius"), radius, true);
    layout->addLayout(form);
    auto *previewEnabled = new QCheckBox(tr("Preview"), &dialog);
    previewEnabled->setChecked(true);
    previewEnabled->setObjectName(QStringLiteral("filterPreview"));
    layout->addWidget(previewEnabled);

    session_.beginEdit(live ? QStringLiteral("Edit Gaussian Blur") : QStringLiteral("Gaussian Blur"));
    const auto value = [&] {
        return QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("Gaussian Blur")},
            {QStringLiteral("blurRadius"), radius->value()}
        };
    };
    const auto preview = [this, live, target, original, value, radius, previewEnabled] {
        if (!previewEnabled->isChecked()) {
            if (live) session_.previewAdjustment(target, original.adjustment);
            else restoreLayer(document_.get(), session_, target, original);
        } else if (live) {
            session_.previewAdjustment(target, value());
        } else {
            restoreLayer(document_.get(), session_, target, original);
            session_.applyGaussianBlur(radius->value());
        }
        canvas_->invalidateDocument();
    };
    connect(radius, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(previewEnabled, &QCheckBox::toggled, &dialog, preview);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (runFloatingDialog(dialog) == QDialog::Accepted) {
        previewEnabled->setChecked(true);
        preview();
    } else {
        if (live) session_.previewAdjustment(target, originalAdjustment);
        else restoreLayer(document_.get(), session_, target, original);
    }
    session_.endEdit();
    syncDocumentViews();
}

void MainWindow::motionBlurDialog()
{
    Layer *active = session_.activeLayer();
    const bool live = active && active->adjustment.value(QStringLiteral("kind")).toString() == QStringLiteral("Motion Blur");
    if (!active || (!live && active->image.isNull())) return;
    const QUuid target = active->id;
    const Layer original = *active;
    const QJsonObject originalAdjustment = active->adjustment;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Motion Blur"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *angle = new QDoubleSpinBox(&dialog);
    angle->setRange(-90, 90);
    angle->setValue(live ? originalAdjustment.value(QStringLiteral("motionAngle")).toDouble(0.0) : 0.0);
    angle->setSuffix(tr("°"));
    auto *distance = new QDoubleSpinBox(&dialog);
    distance->setRange(1, 2000);
    distance->setValue(live ? originalAdjustment.value(QStringLiteral("motionDistance")).toDouble(10.0) : 10.0);
    distance->setSuffix(tr(" px"));
    addScrubRow(form, tr("Angle"), angle);
    addScrubRow(form, tr("Distance"), distance, true);
    layout->addLayout(form);
    auto *previewEnabled = new QCheckBox(tr("Preview"), &dialog);
    previewEnabled->setChecked(true);
    previewEnabled->setObjectName(QStringLiteral("filterPreview"));
    layout->addWidget(previewEnabled);

    session_.beginEdit(live ? QStringLiteral("Edit Motion Blur") : QStringLiteral("Motion Blur"));
    const auto value = [&] {
        return QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("Motion Blur")},
            {QStringLiteral("motionAngle"), angle->value()},
            {QStringLiteral("motionDistance"), distance->value()}
        };
    };
    const auto preview = [this, live, target, original, value, angle, distance, previewEnabled] {
        if (!previewEnabled->isChecked()) {
            if (live) session_.previewAdjustment(target, original.adjustment);
            else restoreLayer(document_.get(), session_, target, original);
        } else if (live) {
            session_.previewAdjustment(target, value());
        } else {
            restoreLayer(document_.get(), session_, target, original);
            session_.applyMotionBlur(angle->value(), distance->value());
        }
        canvas_->invalidateDocument();
    };
    connect(angle, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(distance, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(previewEnabled, &QCheckBox::toggled, &dialog, preview);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (runFloatingDialog(dialog) == QDialog::Accepted) {
        previewEnabled->setChecked(true);
        preview();
    } else {
        if (live) session_.previewAdjustment(target, originalAdjustment);
        else restoreLayer(document_.get(), session_, target, original);
    }
    session_.endEdit();
    syncDocumentViews();
}

void MainWindow::vignetteDialog()
{
    Layer *active = session_.activeLayer();
    if (!active || active->group || (active->image.isNull() && !active->adjustment.isEmpty())) return;
    const QUuid target = active->id;
    const Layer original = *active;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Vignette"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;

    auto *amount = new QDoubleSpinBox(&dialog);
    amount->setRange(0, 100);
    amount->setValue(35);
    amount->setSuffix(tr("%"));
    addScrubRow(form, tr("Amount"), amount);

    QColor edgeColor = Qt::black;
    auto *colorBtn = new QPushButton(&dialog);
    const auto updateSwatch = [&] {
        colorBtn->setStyleSheet(QStringLiteral("text-align:left;padding-left:12px;background:%1;color:%2;font-weight:bold;")
                                .arg(edgeColor.name(), edgeColor.lightness() > 128 ? QStringLiteral("#000") : QStringLiteral("#fff")));
        colorBtn->setText(edgeColor.name().toUpper());
    };
    updateSwatch();
    form->addRow(tr("Color"), colorBtn);

    auto *midpoint = new QDoubleSpinBox(&dialog);
    midpoint->setRange(0, 100);
    midpoint->setValue(50);
    addScrubRow(form, tr("Midpoint"), midpoint);

    auto *roundness = new QDoubleSpinBox(&dialog);
    roundness->setRange(-100, 100);
    roundness->setValue(100);
    addScrubRow(form, tr("Roundness"), roundness);

    auto *feather = new QDoubleSpinBox(&dialog);
    feather->setRange(0, 100);
    feather->setValue(60);
    addScrubRow(form, tr("Feather"), feather);

    auto *highlights = new QDoubleSpinBox(&dialog);
    highlights->setRange(0, 100);
    highlights->setValue(25);
    addScrubRow(form, tr("Highlights"), highlights);

    layout->addLayout(form);

    auto *previewEnabled = new QCheckBox(tr("Preview"), &dialog);
    previewEnabled->setChecked(true);
    previewEnabled->setObjectName(QStringLiteral("filterPreview"));
    layout->addWidget(previewEnabled);

    session_.beginEdit(QStringLiteral("Vignette"));
    const auto preview = [this, target, original, amount, &edgeColor, midpoint, roundness, feather, highlights, previewEnabled] {
        restoreLayer(document_.get(), session_, target, original);
        if (previewEnabled->isChecked()) {
            session_.applyVignette(amount->value(), edgeColor, midpoint->value(), roundness->value(), feather->value(), highlights->value());
        }
        canvas_->invalidateDocument();
    };

    connect(colorBtn, &QPushButton::clicked, &dialog, [&] {
        const QColor originalColor = edgeColor;
        QColorDialog picker(edgeColor, &dialog);
        picker.setWindowTitle(tr("Vignette Edge Color"));
        picker.setOption(QColorDialog::DontUseNativeDialog);
        picker.setWindowModality(Qt::NonModal);
        connect(&picker, &QColorDialog::currentColorChanged, &dialog, [&](const QColor &next) {
            if (!next.isValid()) return;
            edgeColor = next.toRgb();
            updateSwatch();
            preview();
        });
        if (runFloatingDialog(picker) != QDialog::Accepted) {
            edgeColor = originalColor;
            updateSwatch();
            preview();
        }
    });

    connect(amount, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(midpoint, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(roundness, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(feather, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(highlights, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(previewEnabled, &QCheckBox::toggled, &dialog, preview);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    preview();

    if (runFloatingDialog(dialog) == QDialog::Accepted) {
        previewEnabled->setChecked(true);
        preview();
    } else {
        restoreLayer(document_.get(), session_, target, original);
    }
    session_.endEdit();
    syncDocumentViews();
}

void MainWindow::bloomGlowDialog()
{
    Layer *active = session_.activeLayer();
    if (!active || active->group || active->image.isNull()) return;
    const QUuid target = active->id;
    const Layer original = *active;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Bloom / Glow"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;

    auto *amount = new QDoubleSpinBox(&dialog);
    amount->setRange(0, 100);
    amount->setValue(40);
    amount->setSuffix(tr("%"));
    addScrubRow(form, tr("Amount"), amount);

    auto *radius = new QDoubleSpinBox(&dialog);
    radius->setRange(1, 150);
    radius->setValue(24);
    radius->setSuffix(tr(" px"));
    addScrubRow(form, tr("Radius"), radius, true);

    layout->addLayout(form);

    auto *previewEnabled = new QCheckBox(tr("Preview"), &dialog);
    previewEnabled->setChecked(true);
    previewEnabled->setObjectName(QStringLiteral("filterPreview"));
    layout->addWidget(previewEnabled);

    session_.beginEdit(QStringLiteral("Bloom / Glow"));
    const auto preview = [this, target, original, amount, radius, previewEnabled] {
        restoreLayer(document_.get(), session_, target, original);
        if (previewEnabled->isChecked()) {
            session_.applyBloomGlow(amount->value(), radius->value());
        }
        canvas_->invalidateDocument();
    };

    connect(amount, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(radius, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(previewEnabled, &QCheckBox::toggled, &dialog, preview);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    preview();

    if (runFloatingDialog(dialog) == QDialog::Accepted) {
        previewEnabled->setChecked(true);
        preview();
    } else {
        restoreLayer(document_.get(), session_, target, original);
    }
    session_.endEdit();
    syncDocumentViews();
}

void MainWindow::tonalContrastDialog()
{
    Layer *active = session_.activeLayer();
    if (!active || active->group || active->image.isNull()) return;
    const QUuid target = active->id;
    const Layer original = *active;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Tonal Contrast"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;

    auto *amount = new QDoubleSpinBox(&dialog);
    amount->setRange(0, 100);
    amount->setValue(50);
    amount->setSuffix(tr("%"));
    addScrubRow(form, tr("Amount"), amount);

    auto *radius = new QDoubleSpinBox(&dialog);
    radius->setRange(1, 100);
    radius->setValue(16);
    radius->setSuffix(tr(" px"));
    addScrubRow(form, tr("Radius"), radius, true);

    auto *shadows = new QDoubleSpinBox(&dialog);
    shadows->setRange(-100, 100);
    shadows->setValue(40);
    addScrubRow(form, tr("Shadows"), shadows);

    auto *midtones = new QDoubleSpinBox(&dialog);
    midtones->setRange(-100, 100);
    midtones->setValue(60);
    addScrubRow(form, tr("Midtones"), midtones);

    auto *highlights = new QDoubleSpinBox(&dialog);
    highlights->setRange(-100, 100);
    highlights->setValue(30);
    addScrubRow(form, tr("Highlights"), highlights);

    layout->addLayout(form);

    auto *previewEnabled = new QCheckBox(tr("Preview"), &dialog);
    previewEnabled->setChecked(true);
    previewEnabled->setObjectName(QStringLiteral("filterPreview"));
    layout->addWidget(previewEnabled);

    session_.beginEdit(QStringLiteral("Tonal Contrast"));
    const auto preview = [this, target, original, amount, radius, shadows, midtones, highlights, previewEnabled] {
        restoreLayer(document_.get(), session_, target, original);
        if (previewEnabled->isChecked()) {
            session_.applyTonalContrast(amount->value(), radius->value(), shadows->value(), midtones->value(), highlights->value());
        }
        canvas_->invalidateDocument();
    };

    connect(amount, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(radius, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(shadows, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(midtones, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(highlights, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(previewEnabled, &QCheckBox::toggled, &dialog, preview);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    preview();

    if (runFloatingDialog(dialog) == QDialog::Accepted) {
        previewEnabled->setChecked(true);
        preview();
    } else {
        restoreLayer(document_.get(), session_, target, original);
    }
    session_.endEdit();
    syncDocumentViews();
}

void MainWindow::ditherDialog()
{
    Layer *active = session_.activeLayer();
    if (!active || active->group || active->image.isNull()) return;
    const QUuid target = active->id;
    const Layer original = *active;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Dither"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto settings = std::make_shared<DitherSettings>();
    std::vector<std::pair<int, std::function<bool()>>> rows;  // form row, visibility rule
    const auto track = [&](std::function<bool()> visible) { rows.emplace_back(form->rowCount() - 1, std::move(visible)); };

    auto *style = new QComboBox(&dialog);
    style->setObjectName(QStringLiteral("ditherStyle"));
    const auto &groups = DitherInfo::groups();
    for (size_t group = 0; group < groups.size(); ++group) {
        if (group) style->insertSeparator(style->count());
        for (DitherStyle value : groups[group]) style->addItem(DitherInfo::styleName(value), int(value));
    }
    form->addRow(tr("Style"), style);

    const auto spin = [&](const QString &name, const QString &label, double min, double max, double value,
                          const QString &suffix, double DitherSettings::*field, std::function<bool()> visible) {
        auto *box = new QDoubleSpinBox(&dialog);
        box->setObjectName(name);
        box->setDecimals(0);
        box->setRange(min, max);
        box->setValue(value);
        box->setSuffix(suffix);
        addScrubRow(form, label, box);
        track(std::move(visible));
        connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, [settings, field](double v) { (*settings).*field = v; });
        return box;
    };
    const auto style_ = [style] { return DitherStyle(style->currentData().toInt()); };
    const auto always = [] { return true; };
    const DitherSettings d;
    spin(QStringLiteral("ditherPixelSize"), tr("Pixel Size"), DitherSettings::pixelSizeMin, DitherSettings::pixelSizeMax, d.pixelSize, tr(" px"),
         &DitherSettings::pixelSize, [style_] { return DitherInfo::usesPixelSize(style_()); });
    spin(QStringLiteral("ditherTextSize"), tr("Text Size"), DitherSettings::textSizeMin, DitherSettings::textSizeMax, d.textSize, tr(" px"),
         &DitherSettings::textSize, [style_] { return style_() == DitherStyle::Ascii; });
    spin(QStringLiteral("ditherLineSpacing"), tr("Line Spacing"), DitherSettings::lineSpacingMin, DitherSettings::lineSpacingMax, d.lineSpacing, tr(" px"),
         &DitherSettings::lineSpacing, [style_] { return style_() == DitherStyle::Scanlines; });
    spin(QStringLiteral("ditherGlow"), tr("Glow"), 0, 100, d.glow, tr("%"), &DitherSettings::glow,
         [style_] { return style_() == DitherStyle::Scanlines; });
    spin(QStringLiteral("ditherDots"), tr("Dots"), 0, 100, d.dots, tr("%"), &DitherSettings::dots,
         [style_] { return style_() == DitherStyle::Scanlines; });
    spin(QStringLiteral("ditherWobble"), tr("Wobble"), DitherSettings::wobbleMin, DitherSettings::wobbleMax, d.wobble, tr(" px"),
         &DitherSettings::wobble, [style_] { return style_() == DitherStyle::Scanlines; });
    spin(QStringLiteral("ditherCellSize"), tr("Cell Size"), DitherSettings::cellSizeMin, DitherSettings::cellSizeMax, d.cellSize, tr(" px"),
         &DitherSettings::cellSize, [style_] { return DitherInfo::isHalftone(style_()); });
    spin(QStringLiteral("ditherAngle"), tr("Angle"), -90, 90, d.angle, QString::fromUtf8("\xc2\xb0"), &DitherSettings::angle,
         [style_] { return DitherInfo::isHalftone(style_()); });

    auto *characters = new QLineEdit(d.characters, &dialog);
    characters->setObjectName(QStringLiteral("ditherCharacters"));
    form->addRow(tr("Characters"), characters);
    track([style_] { return style_() == DitherStyle::Ascii; });
    connect(characters, &QLineEdit::textChanged, &dialog, [settings](const QString &text) { settings->characters = text; });

    spin(QStringLiteral("ditherLevels"), tr("Tones"), DitherSettings::levelsMin, DitherSettings::levelsMax, d.levels, {},
         &DitherSettings::levels, [style_] { return DitherInfo::hasTones(style_()); });
    spin(QStringLiteral("ditherDiffusion"), tr("Diffusion"), 0, 100, d.diffusion, tr("%"), &DitherSettings::diffusion,
         [style_] { return DitherInfo::diffuses(style_()); });
    spin(QStringLiteral("ditherDensity"), tr("Density"), -100, 100, d.density, {}, &DitherSettings::density, always);
    spin(QStringLiteral("ditherContrast"), tr("Contrast"), -100, 100, d.contrast, {}, &DitherSettings::contrast, always);

    auto *lightOnDark = new QCheckBox(tr("Light marks on dark"), &dialog);
    lightOnDark->setObjectName(QStringLiteral("ditherLightOnDark"));
    lightOnDark->setChecked(d.lightOnDark);
    form->addRow(QString(), lightOnDark);
    track([style_] { return DitherInfo::drawsMarks(style_()); });
    connect(lightOnDark, &QCheckBox::toggled, &dialog, [settings](bool on) { settings->lightOnDark = on; });

    auto *colors = new QComboBox(&dialog);
    colors->setObjectName(QStringLiteral("ditherColors"));
    colors->addItems({tr("Black & White"), tr("Two Colors"), tr("Original")});
    form->addRow(tr("Colors"), colors);

    const auto toColor = [](const DitherColor &c) { return QColor::fromRgbF(c.red, c.green, c.blue); };
    const auto swatchStyle = [toColor](QPushButton *button, const DitherColor &c) {
        button->setStyleSheet(QStringLiteral("background-color: %1;").arg(toColor(c).name()));
    };
    auto *darkButton = new QPushButton(&dialog), *lightButton = new QPushButton(&dialog);
    darkButton->setObjectName(QStringLiteral("ditherDarkColor"));
    lightButton->setObjectName(QStringLiteral("ditherLightColor"));
    swatchStyle(darkButton, d.dark);
    swatchStyle(lightButton, d.light);
    auto *swatches = new QWidget(&dialog);
    auto *swatchLayout = new QHBoxLayout(swatches);
    swatchLayout->setContentsMargins(0, 0, 0, 0);
    swatchLayout->addWidget(new QLabel(tr("Dark"), swatches));
    swatchLayout->addWidget(darkButton);
    swatchLayout->addWidget(new QLabel(tr("Light"), swatches));
    swatchLayout->addWidget(lightButton);
    form->addRow(QString(), swatches);
    track([colors] { return colors->currentIndex() == int(DitherColors::TwoColors); });

    auto *shape = new QComboBox(&dialog);
    shape->setObjectName(QStringLiteral("ditherPixelShape"));
    shape->addItems({tr("Square"), tr("Dot")});
    form->addRow(tr("Pixel Shape"), shape);
    track([settings, style_] { return settings->pixelSize > 1 && DitherInfo::usesPixelSize(style_()); });
    layout->addLayout(form);

    auto *previewEnabled = new QCheckBox(tr("Preview"), &dialog);
    previewEnabled->setChecked(true);
    previewEnabled->setObjectName(QStringLiteral("filterPreview"));
    layout->addWidget(previewEnabled);

    session_.beginEdit(QStringLiteral("Dither"));
    const auto updateRows = [form, rows] {
        for (const auto &[row, visible] : rows) form->setRowVisible(row, visible());
    };
    const auto preview = [this, target, original, settings, previewEnabled] {
        restoreLayer(document_.get(), session_, target, original);
        if (previewEnabled->isChecked()) session_.applyDither(*settings);
        canvas_->invalidateDocument();
    };
    const auto refresh = [updateRows, preview] { updateRows(); preview(); };
    for (auto *box : dialog.findChildren<QDoubleSpinBox *>()) connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, refresh);
    connect(characters, &QLineEdit::textChanged, &dialog, refresh);
    connect(lightOnDark, &QCheckBox::toggled, &dialog, refresh);
    connect(previewEnabled, &QCheckBox::toggled, &dialog, preview);
    connect(style, &QComboBox::currentIndexChanged, &dialog, [style, settings, refresh] {
        settings->style = DitherStyle(style->currentData().toInt());
        refresh();
    });
    connect(colors, &QComboBox::currentIndexChanged, &dialog, [colors, settings, refresh] {
        settings->colors = DitherColors(colors->currentIndex());
        refresh();
    });
    connect(shape, &QComboBox::currentIndexChanged, &dialog, [shape, settings, refresh] {
        settings->pixelShape = shape->currentIndex() == 1 ? DitherPixelShape::Dot : DitherPixelShape::Square;
        refresh();
    });
    const auto pickColor = [&dialog, settings, toColor, swatchStyle, refresh](QPushButton *button, DitherColor DitherSettings::*field) {
        QColorDialog picker(toColor((*settings).*field), &dialog);
        picker.setWindowTitle(field == &DitherSettings::dark ? tr("Dither Dark Color") : tr("Dither Light Color"));
        if (picker.exec() != QDialog::Accepted) return;
        const QColor color = picker.selectedColor();
        (*settings).*field = DitherColor{color.redF(), color.greenF(), color.blueF()};
        swatchStyle(button, (*settings).*field);
        refresh();
    };
    connect(darkButton, &QPushButton::clicked, &dialog, [=] { pickColor(darkButton, &DitherSettings::dark); });
    connect(lightButton, &QPushButton::clicked, &dialog, [=] { pickColor(lightButton, &DitherSettings::light); });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    settings->style = style_();
    updateRows();
    preview();

    if (runFloatingDialog(dialog) == QDialog::Accepted) {
        previewEnabled->setChecked(true);
        preview();
    } else {
        restoreLayer(document_.get(), session_, target, original);
    }
    session_.endEdit();
    syncDocumentViews();
}

void MainWindow::addNoiseDialog()
{
    Layer *active = session_.activeLayer();
    const bool live = active && active->adjustment.value(QStringLiteral("kind")).toString() == QStringLiteral("Add Noise");
    if (!active || (!live && active->image.isNull())) return;
    const QUuid target = active->id;
    const Layer original = *active;
    const QJsonObject originalAdjustment = active->adjustment;
    quint32 seed = live ? quint32(originalAdjustment.value(QStringLiteral("noiseSeed")).toDouble(QRandomGenerator::global()->generate()))
                        : QRandomGenerator::global()->generate();

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Add Noise"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *amount = new QDoubleSpinBox(&dialog);
    amount->setObjectName(QStringLiteral("noiseAmount"));
    amount->setDecimals(1);
    amount->setRange(.1, 400);
    amount->setValue(live ? originalAdjustment.value(QStringLiteral("noiseAmount")).toDouble(10.0) : 10.0);
    amount->setSuffix(tr(" %"));
    auto *distribution = new QComboBox(&dialog);
    distribution->addItems({tr("Uniform"), tr("Gaussian")});
    if (live && originalAdjustment.value(QStringLiteral("noiseGaussian")).toBool(false)) {
        distribution->setCurrentIndex(1);
    }
    auto *monochromatic = new QCheckBox(tr("Monochromatic"), &dialog);
    if (live) monochromatic->setChecked(originalAdjustment.value(QStringLiteral("noiseMonochromatic")).toBool(false));

    addScrubRow(form, tr("Amount"), amount, true);
    form->addRow(tr("Distribution"), distribution);
    layout->addLayout(form);
    layout->addWidget(monochromatic);
    auto *previewEnabled = new QCheckBox(tr("Preview"), &dialog);
    previewEnabled->setChecked(true);
    previewEnabled->setObjectName(QStringLiteral("filterPreview"));
    layout->addWidget(previewEnabled);

    session_.beginEdit(live ? QStringLiteral("Edit Add Noise") : QStringLiteral("Add Noise"));
    const auto value = [&] {
        return QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("Add Noise")},
            {QStringLiteral("noiseAmount"), amount->value()},
            {QStringLiteral("noiseGaussian"), distribution->currentIndex() == 1},
            {QStringLiteral("noiseMonochromatic"), monochromatic->isChecked()},
            {QStringLiteral("noiseSeed"), double(seed)}
        };
    };
    const auto preview = [this, live, target, original, value, seed, amount, distribution, monochromatic, previewEnabled] {
        if (!previewEnabled->isChecked()) {
            if (live) session_.previewAdjustment(target, original.adjustment);
            else restoreLayer(document_.get(), session_, target, original);
        } else if (live) {
            session_.previewAdjustment(target, value());
        } else {
            restoreLayer(document_.get(), session_, target, original);
            session_.addNoiseToActiveLayer(float(amount->value()), distribution->currentIndex() == 1, monochromatic->isChecked(), seed);
        }
        canvas_->invalidateDocument();
    };
    connect(amount, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(distribution, &QComboBox::currentIndexChanged, &dialog, preview);
    connect(monochromatic, &QCheckBox::toggled, &dialog, preview);
    connect(previewEnabled, &QCheckBox::toggled, &dialog, preview);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (runFloatingDialog(dialog) == QDialog::Accepted) {
        previewEnabled->setChecked(true);
        preview();
    } else {
        if (live) session_.previewAdjustment(target, originalAdjustment);
        else restoreLayer(document_.get(), session_, target, original);
    }
    session_.endEdit();
    syncDocumentViews();
}

void MainWindow::layerEffectsDialog(const std::optional<QUuid> &layerId, std::optional<LayerEffectKind> initialKind)
{
    if (!document_ || !session_.canEditEffects()) return;
    const QUuid target = layerId.value_or(document_->activeLayerId.value_or(QUuid()));
    if (target.isNull()) return;
    EffectsDialog dialog(this, session_, target, [this]() {
        canvas_->invalidateDocument();
        canvas_->update();
    }, initialKind);
    runFloatingDialog(dialog);
    syncDocumentViews();
}

void MainWindow::removeBackgroundDialog()
{
    Layer *active = session_.activeLayer();
    if (!active || active->group || active->image.isNull()) return;
    const QUuid target = active->id; const Layer original = *active; const bool originalMaskSelection = session_.isMaskSelected();

    QFutureWatcher<QPair<QImage, QString>> watcher;
    QEventLoop wait;
    QProgressDialog progress(tr("Detecting foreground subjects…"), QString(), 0, 0, this);
    progress.setWindowTitle(tr("Remove Background")); progress.setCancelButton(nullptr); progress.setWindowModality(Qt::WindowModal);
    connect(&watcher, &QFutureWatcher<QPair<QImage, QString>>::finished, &wait, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([image = original.image] {
        QString error; QImage mask = SubjectRemoval::rawMask(image, &error); return qMakePair(mask, error);
    }));
    progress.show(); if (!watcher.isFinished()) wait.exec(); progress.close();
    const auto detected = watcher.result();
    if (detected.first.isNull()) {
        showMessage(this, tr("Remove Background"), detected.second.isEmpty()
            ? tr("No foreground subject was detected in this layer. Try an image with a more distinct subject.") : detected.second);
        return;
    }

    QDialog dialog(this); dialog.setWindowTitle(tr("Remove Background")); auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(tr("Hide the background behind a layer mask, keeping the foreground subjects. The pixels stay, so the background can be painted back at any time."), &dialog);
    description->setWordWrap(true); layout->addWidget(description);
    auto *form = new QFormLayout; auto *quality = new QComboBox(&dialog); quality->addItems({tr("Basic"), tr("Advanced")});
    auto field = [&dialog](double low, double high, double value, const QString &suffix) { auto *box = new QDoubleSpinBox(&dialog); box->setRange(low, high); box->setValue(value); box->setDecimals(0); box->setSuffix(suffix); return box; };
    auto *refine = field(0, 40, 0, tr(" px")); auto *contrast = field(0, 100, 0, tr(" %")); auto *shift = field(-10, 10, 0, tr(" px"));
    form->addRow(tr("Quality"), quality); form->addRow(tr("Refine"), refine); form->addRow(tr("Contrast"), contrast); form->addRow(tr("Shift Edge"), shift); layout->addLayout(form);
    auto *previewEnabled = new QCheckBox(tr("Preview"), &dialog); previewEnabled->setChecked(true); layout->addWidget(previewEnabled);
    const auto settings = [=] { return SubjectRemovalSettings{quality->currentIndex() == 1, refine->value(), contrast->value(), shift->value()}; };
    session_.beginEdit(QStringLiteral("Remove Background"));
    const auto restore = [this, target, original, originalMaskSelection] {
        restoreLayer(document_.get(), session_, target, original);
        session_.selectMaskTarget(originalMaskSelection);
    };
    const auto preview = [this, restore, settings, previewEnabled, raw = detected.first] {
        restore();
        if (previewEnabled->isChecked()) session_.removeBackground(settings(), raw);
        canvas_->invalidateDocument();
    };
    const auto qualityChanged = [=](int index) { const bool advanced = index == 1; refine->setEnabled(advanced); contrast->setEnabled(advanced); shift->setEnabled(advanced); preview(); };
    connect(quality, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, qualityChanged);
    connect(refine, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(contrast, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(shift, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    connect(previewEnabled, &QCheckBox::toggled, &dialog, preview);
    qualityChanged(0);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
    if (runFloatingDialog(dialog) == QDialog::Accepted) { restore(); session_.removeBackground(settings(), detected.first); }
    else restore();
    session_.endEdit(); syncDocumentViews();
}

void MainWindow::cancelActiveSelectionTask()
{
    if (currentSelectionCancelToken_) {
        currentSelectionCancelToken_->store(true);
        currentSelectionCancelToken_.reset();
    }
    ++currentSelectionRequestId_;
}

void MainWindow::selectSubjectAction()
{
    if (!document_) return;

    cancelActiveSelectionTask();

    QString error;
    const auto snapshot = session_.createSelectionSnapshot(true, &error);
    if (!snapshot.valid) {
        if (!error.isEmpty()) showMessage(this, tr("Select Subject"), error);
        return;
    }

    const quint64 requestId = ++currentSelectionRequestId_;
    auto cancelToken = std::make_shared<std::atomic<bool>>(false);
    currentSelectionCancelToken_ = cancelToken;

    QProgressDialog progress(tr("Selecting subject…"), tr("Cancel"), 0, 0, this);
    progress.setWindowTitle(tr("Select Subject"));
    progress.setWindowModality(Qt::WindowModal);
    progress.show();

    QFutureWatcher<EditorSession::SelectionComputationResult> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcher<EditorSession::SelectionComputationResult>::finished, &loop, &QEventLoop::quit);
    connect(&progress, &QProgressDialog::canceled, this, [cancelToken] {
        cancelToken->store(true);
    });

    watcher.setFuture(QtConcurrent::run([snapshot, requestId, cancelToken]() {
        return EditorSession::computeSubjectSelection(snapshot, requestId, cancelToken.get());
    }));

    if (!watcher.isFinished()) loop.exec();
    progress.close();

    if (cancelToken->load()) return;
    if (requestId != currentSelectionRequestId_) return;
    if (!session_.hasDocument() || !document_) return;

    const auto result = watcher.result();
    if (document_->id != result.documentId || session_.documentGeneration() != result.documentGeneration) {
        return;
    }

    QString applyError;
    const bool ok = session_.applySelectionResult(result, SelectionMode::Replace, QStringLiteral("Select Subject"), &applyError);
    if (!ok && !applyError.isEmpty()) {
        showMessage(this, tr("Select Subject"), applyError);
    }
    syncDocumentViews(false);
}

void MainWindow::selectObjectRequested(const QPoint &point, int mode, int edgeOffset, bool smoothEdges, bool sampleAllLayers)
{
    if (!document_) return;

    cancelActiveSelectionTask();

    QString error;
    const auto snapshot = session_.createSelectionSnapshot(sampleAllLayers, &error);
    if (!snapshot.valid) {
        if (!error.isEmpty()) showMessage(this, tr("Object Selection"), error);
        return;
    }

    const quint64 requestId = ++currentSelectionRequestId_;
    auto cancelToken = std::make_shared<std::atomic<bool>>(false);
    currentSelectionCancelToken_ = cancelToken;

    QProgressDialog progress(tr("Selecting object…"), tr("Cancel"), 0, 0, this);
    progress.setWindowTitle(tr("Object Selection"));
    progress.setWindowModality(Qt::WindowModal);
    progress.show();

    QFutureWatcher<EditorSession::SelectionComputationResult> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcher<EditorSession::SelectionComputationResult>::finished, &loop, &QEventLoop::quit);
    connect(&progress, &QProgressDialog::canceled, this, [cancelToken] {
        cancelToken->store(true);
    });

    watcher.setFuture(QtConcurrent::run([snapshot, point, edgeOffset, smoothEdges, requestId, cancelToken]() {
        return EditorSession::computeObjectSelection(snapshot, point, edgeOffset, smoothEdges, requestId, cancelToken.get());
    }));

    if (!watcher.isFinished()) loop.exec();
    progress.close();

    if (cancelToken->load()) return;
    if (requestId != currentSelectionRequestId_) return;
    if (!session_.hasDocument() || !document_) return;

    const auto result = watcher.result();
    if (document_->id != result.documentId || session_.documentGeneration() != result.documentGeneration) {
        return;
    }

    QString applyError;
    const bool ok = session_.applySelectionResult(result, SelectionMode(mode), QStringLiteral("Object Selection"), &applyError);
    if (!ok && !applyError.isEmpty()) {
        showMessage(this, tr("Object Selection"), applyError);
    }
    syncDocumentViews(false);
}

void MainWindow::resizeImageDialog()
{
    if (!document_) return;
    QDialog dialog(this); dialog.setWindowTitle(tr("Image Size"));
    auto *layout = new QVBoxLayout(&dialog); auto *form = new QFormLayout;
    const QSize originalSize = document_->canvasSize;
    struct Draft { double width; double height; double resolution; bool resample = true; bool locked = true; bool syncing = false; };
    auto draft = std::make_shared<Draft>(); draft->width = originalSize.width(); draft->height = originalSize.height(); draft->resolution = document_->resolution;
    auto *units = new QComboBox(&dialog); units->setObjectName(QStringLiteral("imageSizeUnits")); units->addItems({tr("Pixels"), tr("Percent"), tr("Inches"), tr("Centimeters")});
    auto *width = new QDoubleSpinBox(&dialog); width->setObjectName(QStringLiteral("imageSizeWidth")); width->setDecimals(3); width->setRange(.001, 1000000);
    auto *height = new QDoubleSpinBox(&dialog); height->setObjectName(QStringLiteral("imageSizeHeight")); height->setDecimals(3); height->setRange(.001, 1000000);
    auto *resolution = new QDoubleSpinBox(&dialog); resolution->setObjectName(QStringLiteral("imageSizeResolution")); resolution->setDecimals(3); resolution->setRange(1, 9600); resolution->setSuffix(tr(" ppi"));
    auto *sampling = new QComboBox(&dialog); sampling->setObjectName(QStringLiteral("imageSizeSampling")); sampling->addItems({tr("High quality"), tr("Smooth"), tr("Nearest neighbor")});
    auto *constrain = new QCheckBox(tr("Lock aspect ratio"), &dialog); constrain->setObjectName(QStringLiteral("imageSizeLocked")); constrain->setChecked(true);
    auto *resample = new QCheckBox(tr("Resample"), &dialog); resample->setObjectName(QStringLiteral("imageSizeResample")); resample->setChecked(true);
    auto *widthLabel = new ScrubLabel(tr("Width"), width, 1.0, std::nullopt, &dialog); widthLabel->setObjectName(QStringLiteral("imageSizeWidthLabel"));
    auto *heightLabel = new ScrubLabel(tr("Height"), height, 1.0, std::nullopt, &dialog); heightLabel->setObjectName(QStringLiteral("imageSizeHeightLabel"));
    auto *resLabel = new ScrubLabel(tr("Resolution"), resolution, 1.0, std::nullopt, &dialog); resLabel->setObjectName(QStringLiteral("imageSizeResolutionLabel"));
    form->addRow(tr("Units"), units); form->addRow(widthLabel, width); form->addRow(heightLabel, height); form->addRow(resLabel, resolution); form->addRow(tr("Sampling"), sampling);
    layout->addLayout(form); layout->addWidget(constrain); layout->addWidget(resample);
    auto *result = new QLabel(&dialog); result->setObjectName(QStringLiteral("imageSizeResult")); layout->addWidget(result);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
    const auto displayed = [=](bool widthAxis) {
        const double pixels = widthAxis ? draft->width : draft->height;
        const double original = widthAxis ? originalSize.width() : originalSize.height();
        if (units->currentIndex() == 1) return pixels / original * 100;
        if (units->currentIndex() == 2) return pixels / draft->resolution;
        if (units->currentIndex() == 3) return pixels / draft->resolution * 2.54;
        return pixels;
    };
    const auto refresh = [=] {
        draft->syncing = true;
        const QString suffix = units->currentIndex() == 0 ? tr(" px") : units->currentIndex() == 1 ? tr(" %")
            : units->currentIndex() == 2 ? tr(" in") : tr(" cm");
        width->setSuffix(suffix); height->setSuffix(suffix); width->setValue(displayed(true)); height->setValue(displayed(false)); resolution->setValue(draft->resolution);
        draft->syncing = false;
        const int finalWidth = qRound(draft->width), finalHeight = qRound(draft->height);
        const bool valid = finalWidth >= 1 && finalWidth <= 30000 && finalHeight >= 1 && finalHeight <= 30000
            && draft->resolution >= 1 && draft->resolution <= 9600
            && (!draft->resample || qint64(finalWidth) * finalHeight <= 100000000LL);
        result->setText(valid ? tr("Result: %1 × %2 pixels").arg(finalWidth).arg(finalHeight)
                              : tr("Use 1–30,000 pixels per side, up to 100 megapixels, and 1–9,600 ppi."));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
    };
    const auto setDimension = [=](double value, bool widthAxis) {
        if (draft->syncing || value <= 0) return;
        if (!draft->resample) {
            const double pixels = widthAxis ? originalSize.width() : originalSize.height();
            draft->resolution = pixels / value * (units->currentIndex() == 3 ? 2.54 : 1);
            refresh(); return;
        }
        const double original = widthAxis ? originalSize.width() : originalSize.height();
        double pixels = value;
        if (units->currentIndex() == 1) pixels = value / 100 * original;
        else if (units->currentIndex() == 2) pixels = value * draft->resolution;
        else if (units->currentIndex() == 3) pixels = value / 2.54 * draft->resolution;
        if (widthAxis) { draft->width = pixels; if (draft->locked) draft->height = pixels * originalSize.height() / originalSize.width(); }
        else { draft->height = pixels; if (draft->locked) draft->width = pixels * originalSize.width() / originalSize.height(); }
        refresh();
    };
    connect(width, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, [=](double value) { setDimension(value, true); });
    connect(height, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, [=](double value) { setDimension(value, false); });
    connect(units, &QComboBox::currentIndexChanged, &dialog, [=](int index) {
        if (!draft->resample && index < 2) { units->setCurrentIndex(2); return; } refresh();
    });
    connect(resolution, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, [=](double value) {
        if (draft->syncing || value <= 0) return;
        if (draft->resample && units->currentIndex() >= 2 && draft->resolution > 0) {
            const double ratio = value / draft->resolution; draft->width *= ratio; draft->height *= ratio;
        }
        draft->resolution = value; refresh();
    });
    connect(constrain, &QCheckBox::toggled, &dialog, [=](bool value) { draft->locked = value; refresh(); });
    connect(resample, &QCheckBox::toggled, &dialog, [=](bool enabled) {
        draft->resample = enabled; constrain->setEnabled(enabled); sampling->setEnabled(enabled);
        if (auto *model = qobject_cast<QStandardItemModel *>(units->model())) { model->item(0)->setEnabled(enabled); model->item(1)->setEnabled(enabled); }
        if (!enabled) {
            draft->width = originalSize.width(); draft->height = originalSize.height(); draft->locked = true; constrain->setChecked(true);
            if (units->currentIndex() < 2) units->setCurrentIndex(2);
        }
        refresh();
    });
    refresh();
    if (dialog.exec() != QDialog::Accepted) return;
    const Sampling modes[] = {Sampling::HighQuality, Sampling::Smooth, Sampling::Nearest};
    const QSize resultSize = draft->resample ? QSize(qRound(draft->width), qRound(draft->height)) : originalSize;
    if (session_.resizeImage(resultSize, draft->resolution, modes[sampling->currentIndex()])) syncDocumentViews();
}

void MainWindow::resizeCanvasDialog()
{
    if (!document_) return;
    QDialog dialog(this); dialog.setWindowTitle(tr("Canvas Size"));
    auto *layout = new QVBoxLayout(&dialog); auto *form = new QFormLayout;
    const int originalWidth = document_->canvasSize.width(), originalHeight = document_->canvasSize.height();
    const double resolution = document_->resolution;
    struct Draft { double width; double height; bool relative = false; bool locked = false; bool syncing = false; };
    auto draft = std::make_shared<Draft>(); draft->width = originalWidth; draft->height = originalHeight;
    auto *units = new QComboBox(&dialog); units->setObjectName(QStringLiteral("canvasSizeUnits"));
    units->addItems({tr("Pixels"), tr("Percent"), tr("Inches"), tr("Centimeters")});
    auto *width = new QDoubleSpinBox(&dialog); width->setObjectName(QStringLiteral("canvasSizeWidth")); width->setDecimals(3); width->setRange(-1000000, 1000000);
    auto *height = new QDoubleSpinBox(&dialog); height->setObjectName(QStringLiteral("canvasSizeHeight")); height->setDecimals(3); height->setRange(-1000000, 1000000);
    auto *relative = new QCheckBox(tr("Relative to current dimensions"), &dialog); relative->setObjectName(QStringLiteral("canvasSizeRelative"));
    auto *locked = new QCheckBox(tr("Lock original aspect ratio"), &dialog); locked->setObjectName(QStringLiteral("canvasSizeLocked"));
    auto *anchor = new QComboBox(&dialog); anchor->addItems({tr("Top left"), tr("Top"), tr("Top right"), tr("Left"), tr("Center"), tr("Right"), tr("Bottom left"), tr("Bottom"), tr("Bottom right")}); anchor->setCurrentIndex(4);
    auto *fill = new QComboBox(&dialog); fill->setObjectName(QStringLiteral("canvasExtension"));
    fill->addItems({tr("Transparent"), tr("Foreground"), tr("Background"), tr("Black"), tr("White"), tr("Custom")});
    auto *color = new QPushButton(tr("Choose custom color…"), &dialog); QColor extension = Qt::white; color->setEnabled(false);
    connect(fill, &QComboBox::currentIndexChanged, color, [color](int index) { color->setEnabled(index == 5); });
    auto *widthLabel = new ScrubLabel(tr("Width"), width, 1.0, std::nullopt, &dialog); widthLabel->setObjectName(QStringLiteral("canvasSizeWidthLabel"));
    auto *heightLabel = new ScrubLabel(tr("Height"), height, 1.0, std::nullopt, &dialog); heightLabel->setObjectName(QStringLiteral("canvasSizeHeightLabel"));
    form->addRow(tr("Units"), units); form->addRow(widthLabel, width); form->addRow(heightLabel, height); form->addRow(tr("Anchor"), anchor);
    form->addRow(tr("Canvas extension"), fill); layout->addLayout(form); layout->addWidget(color);
    layout->insertWidget(1, relative); layout->insertWidget(2, locked);
    auto *result = new QLabel(&dialog); result->setObjectName(QStringLiteral("canvasSizeResult")); layout->addWidget(result);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
    const auto displayed = [=](bool widthAxis) {
        const double original = widthAxis ? originalWidth : originalHeight;
        const double pixels = (widthAxis ? draft->width : draft->height) - (draft->relative ? original : 0);
        if (units->currentIndex() == 1) return pixels / original * 100;
        if (units->currentIndex() == 2) return pixels / resolution;
        if (units->currentIndex() == 3) return pixels / resolution * 2.54;
        return pixels;
    };
    const auto refresh = [=] {
        draft->syncing = true;
        const QString suffix = units->currentIndex() == 0 ? tr(" px") : units->currentIndex() == 1 ? tr(" %")
            : units->currentIndex() == 2 ? tr(" in") : tr(" cm");
        width->setSuffix(suffix); height->setSuffix(suffix); width->setValue(displayed(true)); height->setValue(displayed(false));
        draft->syncing = false;
        const int finalWidth = qRound(draft->width), finalHeight = qRound(draft->height);
        const bool valid = finalWidth >= 1 && finalWidth <= 30000 && finalHeight >= 1 && finalHeight <= 30000;
        result->setText(valid ? tr("New: %1 × %2 pixels").arg(finalWidth).arg(finalHeight)
                              : tr("Final dimensions must be 1–30,000 pixels per side."));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
    };
    const auto setDimension = [=](double value, bool widthAxis) {
        if (draft->syncing) return;
        const double original = widthAxis ? originalWidth : originalHeight;
        double pixels = value;
        if (units->currentIndex() == 1) pixels = value / 100 * original;
        else if (units->currentIndex() == 2) pixels = value * resolution;
        else if (units->currentIndex() == 3) pixels = value / 2.54 * resolution;
        const double final = pixels + (draft->relative ? original : 0);
        if (widthAxis) { draft->width = final; if (draft->locked) draft->height = final * originalHeight / originalWidth; }
        else { draft->height = final; if (draft->locked) draft->width = final * originalWidth / originalHeight; }
        refresh();
    };
    connect(width, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, [=](double value) { setDimension(value, true); });
    connect(height, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, [=](double value) { setDimension(value, false); });
    connect(units, &QComboBox::currentIndexChanged, &dialog, [=](int) { refresh(); });
    connect(relative, &QCheckBox::toggled, &dialog, [=](bool value) { draft->relative = value; refresh(); });
    connect(locked, &QCheckBox::toggled, &dialog, [=](bool value) {
        draft->locked = value; if (value) draft->height = draft->width * originalHeight / originalWidth; refresh();
    });
    refresh();
    if (dialog.exec() != QDialog::Accepted) return;
    std::optional<QColor> background;
    switch (fill->currentIndex()) {
    case 1: background = foregroundColor_; break;
    case 2: background = backgroundColor_; break;
    case 3: background = QColor(Qt::black); break;
    case 4: background = QColor(Qt::white); break;
    case 5: background = extension; break;
    default: break;
    }
    if (session_.resizeCanvas(QSize(qRound(draft->width), qRound(draft->height)), anchor->currentIndex(), background)) syncDocumentViews();
}

void MainWindow::cropDialog()
{
    if (!document_) return;
    QDialog dialog(this); dialog.setWindowTitle(tr("Crop"));
    auto *layout = new QVBoxLayout(&dialog); auto *form = new QFormLayout;
    auto makeField = [&dialog](int value, int maximum, const QString &name, const QString &label, ScrubLabel **outLabel) {
        auto *field = new QSpinBox(&dialog);
        field->setObjectName(name);
        field->setRange(-1000000, maximum);
        field->setValue(value);
        field->setSuffix(QObject::tr(" px"));
        if (outLabel) {
            *outLabel = new ScrubLabel(label, field, 1.0, 1.0, &dialog);
            (*outLabel)->setObjectName(name + QStringLiteral("Label"));
        }
        return field;
    };
    ScrubLabel *xLabel = nullptr;
    ScrubLabel *yLabel = nullptr;
    ScrubLabel *wLabel = nullptr;
    ScrubLabel *hLabel = nullptr;
    auto *x = makeField(0, 1000000, QStringLiteral("cropX"), tr("X"), &xLabel);
    auto *y = makeField(0, 1000000, QStringLiteral("cropY"), tr("Y"), &yLabel);
    auto *width = makeField(document_->canvasSize.width(), 30000, QStringLiteral("cropWidth"), tr("Width"), &wLabel); width->setMinimum(1);
    auto *height = makeField(document_->canvasSize.height(), 30000, QStringLiteral("cropHeight"), tr("Height"), &hLabel); height->setMinimum(1);
    form->addRow(xLabel, x); form->addRow(yLabel, y); form->addRow(wLabel, width); form->addRow(hLabel, height); layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
    if (dialog.exec() == QDialog::Accepted && session_.crop(QRect(x->value(), y->value(), width->value(), height->value()))) syncDocumentViews();
}

void MainWindow::trimDialog()
{
    if (!document_) return;
    TrimDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted && session_.trim(dialog.options())) {
        syncDocumentViews();
    }
}

void MainWindow::keyboardShortcutsDialog()
{
    KeyboardShortcutsDialog dialog(this);
    dialog.exec();
}

void MainWindow::newProject()
{
    cancelActiveSelectionTask();
    suggestClipboardOnEmpty_ = true;
    installInNewTab(EditorSession(), tr("Untitled %1").arg(workspaceTabs_.size() + 1));
}

void MainWindow::importImages()
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Import Images"), {},
        tr("Images, SVG, Photoshop and Camera RAW Files (*.png *.jpg *.jpeg *.heic *.heif *.tif *.tiff *.svg *.svgz *.psd *.psb *.cr2 *.cr3 *.nef *.arw *.dng *.orf *.rw2 *.pef *.raf);;Vector Graphics (*.svg *.svgz);;Photoshop Files (*.psd *.psb);;Camera RAW Files (*.cr2 *.cr3 *.nef *.arw *.dng *.orf *.rw2 *.pef *.raf);;All Files (*)"));
    importImageFiles(paths);
}

bool MainWindow::importImageFiles(const QStringList &paths, const std::optional<QPointF> &center)
{
    bool imported = false;
    QStringList failures;
    for (const QString &path : paths) {
        if (RawImporter::matches(path)) {
            RawDevelopDialog dialog(this, path);
            if (dialog.exec() == QDialog::Accepted) {
                QImage img = dialog.developedImage();
                if (img.isNull() || !session_.insertImage(img, QFileInfo(path).completeBaseName(), center)) {
                    failures << tr("%1: Failed to insert developed RAW image").arg(QFileInfo(path).fileName());
                    continue;
                }
                imported = true;
            }
            continue;
        }

        const QString suffix = QFileInfo(path).suffix().toLower();
        if (PSDReader::matches(path) || suffix == QStringLiteral("psd") || suffix == QStringLiteral("psb")) {
            qint64 usedPixels = 0;
            if (document_) {
                for (const Layer &l : document_->layers) {
                    if (!l.image.isNull()) usedPixels += (qint64(l.image.width()) * l.image.height());
                    if (!l.mask.isNull()) usedPixels += (qint64(l.mask.width()) * l.mask.height());
                }
            }
            const qint64 remaining = std::max(0LL, 100000000LL - usedPixels);
            PSDImportResult result;
            QString psdError;
            if (!PSDReader::read(path, result, &psdError, remaining)) {
                failures << tr("%1: %2").arg(QFileInfo(path).fileName(), psdError);
                continue;
            }
            if (!result.conversions.isEmpty()) {
                bool confirmed = true;
                if (session_.confirmConversionsCallback()) {
                    confirmed = session_.confirmConversionsCallback()(result.conversions);
                } else {
                    PSDConversionDialog dialog(QFileInfo(path).fileName(), result.conversions, this);
                    confirmed = (dialog.exec() == QDialog::Accepted);
                }
                if (!confirmed) continue;
            }
            if (!session_.insertPhotoshop(result, QFileInfo(path).completeBaseName(), center, &psdError)) {
                failures << tr("%1: %2").arg(QFileInfo(path).fileName(), psdError);
                continue;
            }
            imported = true;
            continue;
        }

        if (SvgImporter::matches(path)) {
            QString svgError;
            if (!session_.insertSvg(path, center, &svgError)) {
                failures << tr("%1: %2").arg(QFileInfo(path).fileName(), svgError.isEmpty() ? tr("Failed to insert SVG") : svgError);
                continue;
            }
            imported = true;
            continue;
        }

        QString readError; QImage image = ImageImporter::read(path, &readError);
        if (image.isNull() || !session_.insertImage(image, QFileInfo(path).completeBaseName(), center)) {
            failures << tr("%1: %2").arg(QFileInfo(path).fileName(), readError);
            continue;
        }
        imported = true;
    }
    if (imported) syncDocumentViews();
    if (!failures.isEmpty()) showMessage(this, tr("Some Images Could Not Be Imported"), failures.join(QLatin1Char('\n')));
    return imported;
}

void MainWindow::finishInlineText()
{
    if (auto *editor = dynamic_cast<InlineTextEditor *>(canvas_->findChild<QTextEdit *>(QStringLiteral("inlineTextEditor"), Qt::FindDirectChildrenOnly))) editor->finish(true);
}

void MainWindow::noteRecentProject(const QString &path)
{
    if (path.isEmpty()) return;
    QSettings settings;
    QStringList recents = settings.value(QStringLiteral("recentProjects")).toStringList();
    recents.removeAll(path);
    recents.prepend(path);
    while (recents.size() > 20) recents.removeLast();
    settings.setValue(QStringLiteral("recentProjects"), recents);
}

QStringList MainWindow::recentProjects() const
{
    QSettings settings;
    return settings.value(QStringLiteral("recentProjects")).toStringList();
}

void MainWindow::finishWriting(const QString &destination)
{
    if (destination.isEmpty()) {
        const auto keys = inFlightSaves_.keys();
        for (const QString &key : keys) {
            if (inFlightSaves_.contains(key)) {
                auto entry = inFlightSaves_.value(key);
                entry.future.waitForFinished();
                if (!entry.completed) {
                    if (entry.completion) entry.completion(entry.future.result());
                }
            }
        }
    } else {
        if (inFlightSaves_.contains(destination)) {
            auto entry = inFlightSaves_.value(destination);
            entry.future.waitForFinished();
            if (!entry.completed) {
                if (entry.completion) entry.completion(entry.future.result());
            }
        }
    }
}

bool MainWindow::hasInFlightSave(int tabIndex) const
{
    if (tabIndex < 0) return !inFlightSaves_.isEmpty();
    for (auto it = inFlightSaves_.cbegin(); it != inFlightSaves_.cend(); ++it) {
        if (it.value().tabIndex == tabIndex) return true;
    }
    if (tabIndex < workspaceTabs_.size() && workspaceTabs_[tabIndex].hasDocument()) {
        const QString path = workspaceTabs_[tabIndex].document()->projectPath;
        if (!path.isEmpty() && inFlightSaves_.contains(path)) return true;
    }
    return false;
}

QFuture<ProjectWriter::SaveResult> MainWindow::saveProjectAsync(int tabIndex, bool asNew, const QString &explicitDestination, bool isAutosave)
{
    if (tabIndex < 0) tabIndex = currentTab_;
    if (tabIndex < 0 || tabIndex >= workspaceTabs_.size()) return {};
    if (tabIndex == currentTab_) {
        workspaceTabs_[tabIndex] = session_;
    }
    EditorSession &tabSession = workspaceTabs_[tabIndex];
    if (!tabSession.hasDocument()) return {};

    const QString originalPath = tabSession.document()->projectPath;
    QString destination = explicitDestination;
    if (destination.isEmpty()) {
        if (!asNew && !originalPath.isEmpty()) {
            destination = originalPath;
        } else {
            QString suggested = originalPath.isEmpty() ? QStringLiteral("Untitled.comp") : originalPath;
            destination = QFileDialog::getSaveFileName(this, tr("Save Compositor Project"), suggested, tr("Compositor projects (*.comp)"));
            if (destination.isEmpty()) return {};
            if (!destination.endsWith(QStringLiteral(".comp"), Qt::CaseInsensitive)) destination += QStringLiteral(".comp");
        }
    }

    const bool isSaveAs = (asNew || (!originalPath.isEmpty() && destination != originalPath));

    // Serialize saves to the same destination
    finishWriting(destination);

    if (tabIndex == currentTab_) {
        finishInlineText();
        if (transformOriginalDocument_) finishPersistentTransform(true);
        canvas_->resolvePendingGradient();
        canvas_->resolvePendingDistortion();
        if (session_.hasFloatingSelection()) session_.commitSelectionTransform();
        workspaceTabs_[tabIndex] = session_;
    }

    // Derive the expected disk state from the actual destination path, never from the tab's previous project path.
    // Represent "destination must be absent" separately from "destination must match this digest."
    ProjectWriter::ExpectedDestinationState expectedState;
    if (!isSaveAs && !originalPath.isEmpty() && destination == originalPath) {
        // Ordinary save to existing project path
        if (tabDigests_.size() > tabIndex && tabDigests_[tabIndex].has_value() && tabDigests_[tabIndex]->isValid()) {
            expectedState = ProjectWriter::ExpectedDestinationState::mustMatch(*tabDigests_[tabIndex]);
        } else if (QFileInfo::exists(destination)) {
            const auto d = ProjectDigest::compute(destination);
            if (d && d->isValid()) {
                expectedState = ProjectWriter::ExpectedDestinationState::mustMatch(*d);
            } else {
                const QString msg = tr("Cannot save to “%1”: destination exists but cannot be fingerprinted (unreadable or malformed).").arg(QFileInfo(destination).fileName());
                if (!isAutosave) {
                    showMessage(this, tr("Could Not Save Project"), msg);
                    statusHint_->setText(tr("Save failed"));
                } else {
                    statusHint_->setText(tr("Autosave failed: %1").arg(msg));
                }
                QPromise<ProjectWriter::SaveResult> promise;
                promise.start();
                promise.addResult(ProjectWriter::SaveResult{ProjectWriter::SaveResult::Status::IoError, msg, std::nullopt});
                promise.finish();
                return promise.future();
            }
        } else {
            expectedState = ProjectWriter::ExpectedDestinationState::mustBeAbsent();
        }
    } else {
        // Save As to a new or chosen path (or first save of untitled)
        const bool destExists = QFileInfo::exists(destination);
        if (!destExists) {
            // For Save As to a new path, verify it remains absent immediately before installation.
            expectedState = ProjectWriter::ExpectedDestinationState::mustBeAbsent();
        } else {
            // For Save As to an existing path, capture that destination's fingerprint after the user chooses it.
            const auto destDigest = ProjectDigest::compute(destination);
            if (destDigest.has_value() && destDigest->isValid()) {
                expectedState = ProjectWriter::ExpectedDestinationState::mustMatch(*destDigest);
            } else {
                const QString msg = tr("Cannot save to “%1”: destination exists but cannot be fingerprinted (unreadable or malformed).").arg(QFileInfo(destination).fileName());
                if (!isAutosave) {
                    showMessage(this, tr("Could Not Save Project"), msg);
                    statusHint_->setText(tr("Save failed"));
                } else {
                    statusHint_->setText(tr("Autosave failed: %1").arg(msg));
                }
                QPromise<ProjectWriter::SaveResult> promise;
                promise.start();
                promise.addResult(ProjectWriter::SaveResult{ProjectWriter::SaveResult::Status::IoError, msg, std::nullopt});
                promise.finish();
                return promise.future();
            }
        }
    }

    // Capture immutable snapshot, tab identity, revision, and destination state on GUI thread
    Document snapshot = *tabSession.document();
    snapshot.projectPath = destination;
    const int capturedTabIndex = tabIndex;
    const QUuid capturedDocId = snapshot.id;
    const QString capturedOriginalPath = originalPath;
    const QString capturedDestination = destination;
    const QUuid capturedRevision = tabSession.currentRevision();

    if (tabWatchers_.size() > tabIndex && tabWatchers_[tabIndex]) {
        tabWatchers_[tabIndex]->setSaving(true);
    }

    auto future = QtConcurrent::run([snapshot, capturedDestination, expectedState]() -> ProjectWriter::SaveResult {
        return ProjectWriter::saveAtomicChecked(snapshot, capturedDestination, expectedState);
    });

    auto *watcher = new QFutureWatcher<ProjectWriter::SaveResult>(this);

    InFlightSave inFlight;
    inFlight.tabIndex = capturedTabIndex;
    inFlight.future = future;
    inFlight.watcher = watcher;
    inFlight.completed = false;
    inFlight.completion = [this, watcher, capturedTabIndex, capturedDocId, capturedOriginalPath, capturedDestination, capturedRevision, isSaveAs, isAutosave](const ProjectWriter::SaveResult &result) {
        if (!inFlightSaves_.contains(capturedDestination)) return;
        if (inFlightSaves_[capturedDestination].watcher != watcher) return;
        auto entry = inFlightSaves_.take(capturedDestination);
        if (entry.watcher) {
            entry.watcher->disconnect();
            entry.watcher->deleteLater();
        }
        onSaveCompleted(capturedTabIndex, capturedDocId, capturedOriginalPath, capturedDestination, capturedRevision, isSaveAs, result, isAutosave);
    };

    inFlightSaves_.insert(capturedDestination, inFlight);

    connect(watcher, &QFutureWatcher<ProjectWriter::SaveResult>::finished, this, [this, watcher, capturedDestination]() {
        if (inFlightSaves_.contains(capturedDestination)) {
            auto &entry = inFlightSaves_[capturedDestination];
            if (entry.watcher == watcher && !entry.completed) {
                entry.completed = true;
                const ProjectWriter::SaveResult result = watcher->result();
                if (entry.completion) entry.completion(result);
            }
        }
    });
    watcher->setFuture(future);

    return future;
}

void MainWindow::onSaveCompleted(int capturedTabIndex, const QUuid &capturedDocId, const QString &capturedOriginalPath, const QString &capturedDestination, const QUuid &capturedRevision, bool isSaveAs, const ProjectWriter::SaveResult &result, bool isAutosave)
{
    Q_UNUSED(isSaveAs);
    const bool tabMatches = (capturedTabIndex >= 0 && capturedTabIndex < workspaceTabs_.size()
        && workspaceTabs_[capturedTabIndex].hasDocument()
        && workspaceTabs_[capturedTabIndex].document()->id == capturedDocId
        && (workspaceTabs_[capturedTabIndex].document()->projectPath == capturedOriginalPath
            || workspaceTabs_[capturedTabIndex].document()->projectPath == capturedDestination));

    if (result.status == ProjectWriter::SaveResult::Status::Success) {
        if (capturedTabIndex >= 0 && capturedTabIndex < tabPendingExternalChange_.size()) {
            tabPendingExternalChange_[capturedTabIndex] = false;
            tabPendingExternalDigest_[capturedTabIndex] = std::nullopt;
            if (tabPendingExternalDoc_.size() > capturedTabIndex) tabPendingExternalDoc_[capturedTabIndex] = nullptr;
        }

        if (tabMatches) {
            workspaceTabs_[capturedTabIndex].document()->projectPath = capturedDestination;
            setupWatcherForTab(capturedTabIndex, capturedDestination);
            if (tabDigests_.size() > capturedTabIndex) tabDigests_[capturedTabIndex] = result.installedDigest;
            if (tabWatchers_.size() > capturedTabIndex && tabWatchers_[capturedTabIndex]) {
                if (result.installedDigest) tabWatchers_[capturedTabIndex]->setKnownDigest(*result.installedDigest);
                tabWatchers_[capturedTabIndex]->setSaving(false);
            }

            noteRecentProject(capturedDestination);

            if (capturedTabIndex == currentTab_) {
                if (document_) document_->projectPath = capturedDestination;
                if (session_.hasDocument()) session_.document()->projectPath = capturedDestination;
                session_.markSaved(capturedRevision);
                workspaceTabs_[capturedTabIndex] = session_;
                removeRecovery(capturedTabIndex);
                refreshTitle();
                if (!isAutosave) {
                    statusHint_->setText(tr("Saved %1").arg(QFileInfo(capturedDestination).fileName()));
                } else {
                    statusHint_->setText(tr("Autosaved %1").arg(QFileInfo(capturedDestination).fileName()));
                }
            } else {
                workspaceTabs_[capturedTabIndex].markSaved(capturedRevision);
                tabs_->setTabText(capturedTabIndex, QFileInfo(capturedDestination).fileName());
                removeRecovery(capturedTabIndex);
            }
        }
    } else if (result.status == ProjectWriter::SaveResult::Status::Conflict) {
        if (tabWatchers_.size() > capturedTabIndex && tabWatchers_[capturedTabIndex]) {
            tabWatchers_[capturedTabIndex]->setSaving(false);
        }
        if (!isAutosave) {
            showMessage(this, tr("Save Conflict"),
                tr("Conflict detected: “%1” was created or modified externally while saving. Save was cancelled to avoid overwriting changes.").arg(QFileInfo(capturedDestination).fileName()));
            statusHint_->setText(tr("Save conflict detected"));
        } else {
            statusHint_->setText(tr("Autosave conflict detected"));
        }

        // Report any genuine external change that arrived during save and survived the save attempt
        if (tabPendingExternalChange_.value(capturedTabIndex, false) && tabPendingExternalDigest_.value(capturedTabIndex).has_value()) {
            const QString path = (workspaceTabs_.size() > capturedTabIndex && workspaceTabs_[capturedTabIndex].hasDocument())
                                 ? workspaceTabs_[capturedTabIndex].document()->projectPath : capturedDestination;
            const ProjectDigest survivingDigest = *tabPendingExternalDigest_[capturedTabIndex];
            const bool isDirty = (capturedTabIndex == currentTab_) ? session_.isModified() : workspaceTabs_[capturedTabIndex].isModified();
            if (isDirty) {
                if (capturedTabIndex == currentTab_ && !isAutosave) {
                    promptExternalChange(capturedTabIndex, path, survivingDigest);
                }
            } else {
                std::shared_ptr<Document> candidateDoc = (tabPendingExternalDoc_.size() > capturedTabIndex)
                                                        ? tabPendingExternalDoc_[capturedTabIndex] : nullptr;
                tabPendingExternalChange_[capturedTabIndex] = false;
                tabPendingExternalDigest_[capturedTabIndex] = std::nullopt;
                if (tabPendingExternalDoc_.size() > capturedTabIndex) tabPendingExternalDoc_[capturedTabIndex] = nullptr;
                if (!candidateDoc) {
                    try {
                        candidateDoc = std::make_shared<Document>(ProjectReader::load(path));
                        candidateDoc->projectPath = path;
                    } catch (...) {}
                }
                if (candidateDoc) {
                    if (capturedTabIndex == currentTab_) {
                        session_.setDocument(std::make_shared<Document>(*candidateDoc), false);
                        session_.markSaved();
                        workspaceTabs_[capturedTabIndex] = session_;
                        if (tabWatchers_.size() > capturedTabIndex && tabWatchers_[capturedTabIndex]) {
                            tabWatchers_[capturedTabIndex]->setKnownDigest(survivingDigest);
                        }
                        tabDigests_[capturedTabIndex] = survivingDigest;
                        syncDocumentViews();
                        refreshTitle();
                    } else {
                        workspaceTabs_[capturedTabIndex].setDocument(std::make_shared<Document>(*candidateDoc), false);
                        workspaceTabs_[capturedTabIndex].markSaved();
                        if (tabWatchers_.size() > capturedTabIndex && tabWatchers_[capturedTabIndex]) {
                            tabWatchers_[capturedTabIndex]->setKnownDigest(survivingDigest);
                        }
                        tabDigests_[capturedTabIndex] = survivingDigest;
                    }
                }
            }
        }
    } else {
        if (tabWatchers_.size() > capturedTabIndex && tabWatchers_[capturedTabIndex]) {
            tabWatchers_[capturedTabIndex]->setSaving(false);
        }
        if (!isAutosave) {
            showMessage(this, tr("Could Not Save Project"), result.errorMessage);
            statusHint_->setText(tr("Save failed"));

            if (tabPendingExternalChange_.value(capturedTabIndex, false) && tabPendingExternalDigest_.value(capturedTabIndex).has_value()) {
                const QString path = (workspaceTabs_.size() > capturedTabIndex && workspaceTabs_[capturedTabIndex].hasDocument())
                                     ? workspaceTabs_[capturedTabIndex].document()->projectPath : capturedDestination;
                const ProjectDigest survivingDigest = *tabPendingExternalDigest_[capturedTabIndex];
                if (capturedTabIndex == currentTab_) {
                    promptExternalChange(capturedTabIndex, path, survivingDigest);
                }
            }
        } else {
            statusHint_->setText(tr("Autosave failed: %1").arg(result.errorMessage));
        }
    }
}

bool MainWindow::saveProject(bool wait, const QString &explicitDestination)
{
    if (!session_.hasDocument()) return true;
    if (session_.document()->projectPath.isEmpty()) return saveProjectAs(explicitDestination, wait);
    const QString dest = explicitDestination.isEmpty() ? session_.document()->projectPath : explicitDestination;
    auto future = saveProjectAsync(currentTab_, false, dest);
    if (wait) {
        finishWriting(dest);
        return future.result().status == ProjectWriter::SaveResult::Status::Success;
    }
    return true;
}

bool MainWindow::saveProjectAs(const QString &explicitDestination, bool wait)
{
    if (!session_.hasDocument()) return true;
    QString path = explicitDestination;
    if (path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, tr("Save Compositor Project"),
            session_.document()->projectPath.isEmpty() ? QStringLiteral("Untitled.comp") : session_.document()->projectPath,
            tr("Compositor projects (*.comp)"));
        if (path.isEmpty()) return false;
    }
    if (!path.endsWith(QStringLiteral(".comp"), Qt::CaseInsensitive)) path += QStringLiteral(".comp");
    auto future = saveProjectAsync(currentTab_, true, path);
    if (wait) {
        finishWriting(path);
        return future.result().status == ProjectWriter::SaveResult::Status::Success;
    }
    return true;
}

bool MainWindow::saveProjectAs(bool wait)
{
    return saveProjectAs(QString(), wait);
}

bool MainWindow::confirmReplacement()
{
    finishWriting();
    if (!document_ || !session_.isModified()) return true;
    QString name = QFileInfo(document_->projectPath).fileName(); if (name.isEmpty()) name = tr("Untitled");
    const auto choice = showMessage(this, tr("Unsaved Changes"), tr("Save changes to %1?").arg(name),
        tr("Your changes will be lost if you don’t save them."),
        QMessageBox::Save | QMessageBox::Cancel | QMessageBox::Discard, QMessageBox::Save);
    if (choice == QMessageBox::Save) return saveProject(true);
    return choice == QMessageBox::Discard;
}

void MainWindow::setupWatcherForTab(int tabIndex, const QString &path)
{
    if (tabIndex < 0 || tabIndex >= workspaceTabs_.size()) return;

    while (tabWatchers_.size() <= tabIndex) tabWatchers_.push_back(nullptr);
    while (tabDigests_.size() <= tabIndex) tabDigests_.push_back(std::nullopt);
    while (tabPendingExternalChange_.size() <= tabIndex) tabPendingExternalChange_.push_back(false);
    while (tabPendingExternalDigest_.size() <= tabIndex) tabPendingExternalDigest_.push_back(std::nullopt);
    while (tabPendingExternalDoc_.size() <= tabIndex) tabPendingExternalDoc_.push_back(nullptr);

    if (tabWatchers_[tabIndex]) {
        tabWatchers_[tabIndex]->stop();
        tabWatchers_[tabIndex]->deleteLater();
        tabWatchers_[tabIndex] = nullptr;
    }

    if (path.isEmpty() || !QFileInfo::exists(path)) {
        tabDigests_[tabIndex] = std::nullopt;
        return;
    }

    auto *watcher = new ProjectWatcher(path, this);
    const auto initialDigest = ProjectDigest::compute(path);
    tabDigests_[tabIndex] = initialDigest;
    if (initialDigest) {
        watcher->setKnownDigest(*initialDigest);
    }

    const QUuid capturedDocId = (workspaceTabs_.size() > tabIndex && workspaceTabs_[tabIndex].hasDocument())
                                ? workspaceTabs_[tabIndex].document()->id : QUuid();

    connect(watcher, &ProjectWatcher::packageValidatedExternally, this,
        [this, tabIndex, capturedDocId](uint64_t reqId, const QString &changedPath, const std::optional<ProjectDigest> &expectedDigest, const ProjectDigest &newDigest, const std::shared_ptr<Document> &loadedDoc) {
            handleExternalChangeValidated(tabIndex, capturedDocId, reqId, changedPath, expectedDigest, newDigest, loadedDoc);
    });
    connect(watcher, &ProjectWatcher::packageRemovedExternally, this, [this, tabIndex, path](const QString &removedPath) {
        handleExternalRemoval(tabIndex, removedPath);
    });

    tabWatchers_[tabIndex] = watcher;
}

ProjectWatcher *MainWindow::tabWatcher(int tabIndex) const
{
    if (tabIndex >= 0 && tabIndex < tabWatchers_.size()) return tabWatchers_.at(tabIndex);
    return nullptr;
}

void MainWindow::handleExternalRemoval(int tabIndex, const QString &path)
{
    if (tabIndex < 0 || tabIndex >= workspaceTabs_.size()) return;
    if (tabIndex == currentTab_) {
        statusHint_->setText(tr("“%1” was removed from disk").arg(QFileInfo(path).fileName()));
        refreshTitle();
    }
}

void MainWindow::handleExternalChange(int tabIndex, const QString &path, const ProjectDigest &newDigest)
{
    if (tabIndex < 0 || tabIndex >= workspaceTabs_.size()) return;
    if (!workspaceTabs_[tabIndex].hasDocument()) return;
    const QUuid docId = workspaceTabs_[tabIndex].document()->id;
    const uint64_t reqId = (tabWatchers_.size() > tabIndex && tabWatchers_[tabIndex]) ? tabWatchers_[tabIndex]->currentRequestId() : 0;
    const auto expDigest = (tabDigests_.size() > tabIndex) ? tabDigests_[tabIndex] : std::nullopt;
    handleExternalChangeValidated(tabIndex, docId, reqId, path, expDigest, newDigest, nullptr);
}

void MainWindow::handleExternalChangeValidated(int tabIndex, const QUuid &capturedDocId, uint64_t requestId, const QString &path, const std::optional<ProjectDigest> &expectedDigest, const ProjectDigest &newDigest, const std::shared_ptr<Document> &loadedDoc)
{
    // Apply results only if the tab, path, request identity, and current known digest still match.
    if (tabIndex < 0 || tabIndex >= workspaceTabs_.size()) return;
    if (!workspaceTabs_[tabIndex].hasDocument()) return;
    if (workspaceTabs_[tabIndex].document()->id != capturedDocId) return;
    if (workspaceTabs_[tabIndex].document()->projectPath != path) return;
    if (tabWatchers_.size() <= tabIndex || !tabWatchers_[tabIndex] || tabWatchers_[tabIndex]->currentRequestId() != requestId) return;
    if (tabDigests_.size() <= tabIndex || tabDigests_[tabIndex] != expectedDigest) return;

    std::shared_ptr<Document> candidateDoc = loadedDoc;
    if (!candidateDoc) {
        try {
            candidateDoc = std::make_shared<Document>(ProjectReader::load(path));
            candidateDoc->projectPath = path;
        } catch (...) {
            return;
        }
    }

    const bool hasSaveInFlight = hasInFlightSave(tabIndex);
    if (hasSaveInFlight) {
        // While a save is active on this tab, inspection cannot reload the document,
        // open a conflict prompt, or change the save’s captured revision or disk baseline.
        // Record it as pending for this tab so it is not lost.
        while (tabPendingExternalChange_.size() <= tabIndex) tabPendingExternalChange_.push_back(false);
        while (tabPendingExternalDigest_.size() <= tabIndex) tabPendingExternalDigest_.push_back(std::nullopt);
        while (tabPendingExternalDoc_.size() <= tabIndex) tabPendingExternalDoc_.push_back(nullptr);

        tabPendingExternalChange_[tabIndex] = true;
        tabPendingExternalDigest_[tabIndex] = newDigest;
        tabPendingExternalDoc_[tabIndex] = candidateDoc;
        return;
    }

    const bool isDirty = (tabIndex == currentTab_) ? session_.isModified() : workspaceTabs_[tabIndex].isModified();
    if (isDirty && tabIndex == currentTab_) {
        workspaceTabs_[tabIndex] = session_;
    }
    if (!isDirty) {
        // Clean tab: reload in place preserving viewport, selection, and active layer
        if (tabIndex == currentTab_) {
            const double zoom = canvas_->zoom();
            const QPointF pan = canvas_->panOffset();
            const std::optional<QUuid> activeId = document_ ? document_->activeLayerId : std::nullopt;
            const std::optional<QImage> sel = document_ ? document_->selection : std::nullopt;

            auto docPtr = std::make_shared<Document>(*candidateDoc);
            if (sel.has_value()) {
                docPtr->selection = *sel;
            }
            session_.setDocument(docPtr, false);
            workspaceTabs_[tabIndex] = session_;

            if (activeId) {
                for (const auto &l : docPtr->layers) {
                    if (l.id == *activeId) {
                        session_.selectLayer(*activeId);
                        break;
                    }
                }
            }
            session_.markSaved();
            if (tabWatchers_.size() > tabIndex && tabWatchers_[tabIndex]) {
                tabWatchers_[tabIndex]->setKnownDigest(newDigest);
            }
            tabDigests_[tabIndex] = newDigest;
            tabPendingExternalChange_[tabIndex] = false;
            tabPendingExternalDigest_[tabIndex] = std::nullopt;
            if (tabPendingExternalDoc_.size() > tabIndex) tabPendingExternalDoc_[tabIndex] = nullptr;

            syncDocumentViews();
            canvas_->setZoom(zoom);
            canvas_->setPanOffset(pan);
            refreshTitle();
            statusHint_->setText(tr("Reloaded %1 (external change)").arg(QFileInfo(path).fileName()));
        } else {
            auto docPtr = std::make_shared<Document>(*candidateDoc);
            workspaceTabs_[tabIndex].setDocument(docPtr, false);
            workspaceTabs_[tabIndex].markSaved();
            if (tabWatchers_.size() > tabIndex && tabWatchers_[tabIndex]) {
                tabWatchers_[tabIndex]->setKnownDigest(newDigest);
            }
            tabDigests_[tabIndex] = newDigest;
            tabPendingExternalChange_[tabIndex] = false;
            tabPendingExternalDigest_[tabIndex] = std::nullopt;
            if (tabPendingExternalDoc_.size() > tabIndex) tabPendingExternalDoc_[tabIndex] = nullptr;
            tabs_->setTabText(tabIndex, QFileInfo(path).fileName());
        }
    } else {
        // Dirty tab: defer prompt if inactive tab or in-progress edits
        tabPendingExternalChange_[tabIndex] = true;
        tabPendingExternalDigest_[tabIndex] = newDigest;
        if (tabPendingExternalDoc_.size() > tabIndex) tabPendingExternalDoc_[tabIndex] = candidateDoc;

        const bool isFrontmost = (tabIndex == currentTab_);
        const bool isBusy = transformOriginalDocument_.has_value() || session_.isPainting() || canvas_->editorInteractionBlocked();
        if (!isFrontmost || isBusy) {
            return;
        }
        promptExternalChange(tabIndex, path, newDigest);
    }
}

void MainWindow::promptExternalChange(int tabIndex, const QString &path, const ProjectDigest &newDigest)
{
    if (tabIndex < 0 || tabIndex >= workspaceTabs_.size()) return;
    if (tabIndex != currentTab_) return;

    const QString fileName = QFileInfo(path).fileName();
    const auto choice = showMessage(this,
        tr("“%1” was changed on disk.").arg(fileName),
        tr("Another app changed this project. You can revert to the version on disk, losing your unsaved changes, or keep what you have."),
        QString(),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);

    tabPendingExternalChange_[tabIndex] = false;
    tabPendingExternalDigest_[tabIndex] = std::nullopt;

    if (choice == QMessageBox::Yes) {
        // Revert: reload candidate from disk or use pre-loaded candidateDoc
        std::shared_ptr<Document> candidateDoc = (tabPendingExternalDoc_.size() > tabIndex) ? tabPendingExternalDoc_[tabIndex] : nullptr;
        if (!candidateDoc) {
            try {
                candidateDoc = std::make_shared<Document>(ProjectReader::load(path));
                candidateDoc->projectPath = path;
            } catch (...) {
                return;
            }
        }
        if (tabPendingExternalDoc_.size() > tabIndex) tabPendingExternalDoc_[tabIndex] = nullptr;

        const double zoom = canvas_->zoom();
        const QPointF pan = canvas_->panOffset();
        const std::optional<QUuid> activeId = document_ ? document_->activeLayerId : std::nullopt;
        const std::optional<QImage> sel = document_ ? document_->selection : std::nullopt;

        auto docPtr = std::make_shared<Document>(*candidateDoc);
        if (sel.has_value()) {
            docPtr->selection = *sel;
        }
        session_.setDocument(docPtr, false);
        workspaceTabs_[tabIndex] = session_;

        if (activeId) {
            for (const auto &l : docPtr->layers) {
                if (l.id == *activeId) {
                    session_.selectLayer(*activeId);
                    break;
                }
            }
        }
        session_.markSaved();
        if (tabWatchers_.size() > tabIndex && tabWatchers_[tabIndex]) {
            tabWatchers_[tabIndex]->setKnownDigest(newDigest);
        }
        tabDigests_[tabIndex] = newDigest;
        syncDocumentViews();
        canvas_->setZoom(zoom);
        canvas_->setPanOffset(pan);
        refreshTitle();
        statusHint_->setText(tr("Reverted to %1 from disk").arg(fileName));
    } else {
        // Keep Mine: retain local document and dirty state, update knownDigest to candidate so we don't prompt again for same change
        if (tabWatchers_.size() > tabIndex && tabWatchers_[tabIndex]) {
            tabWatchers_[tabIndex]->setKnownDigest(newDigest);
        }
        tabDigests_[tabIndex] = newDigest;
        if (tabPendingExternalDoc_.size() > tabIndex) tabPendingExternalDoc_[tabIndex] = nullptr;
    }
}

QString MainWindow::recoveryPath(int tabIndex) const
{
    if (tabIndex < 0) tabIndex = currentTab_;
    if (tabIndex >= 0 && tabIndex < tabRecoveryPaths_.size()) return tabRecoveryPaths_.at(tabIndex);
    return {};
}

QString MainWindow::recoveryDirectory() const
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("recovery"));
}

QString MainWindow::newRecoveryPath() const
{
    return QDir(recoveryDirectory()).filePath(QStringLiteral("Untitled-%1.comp").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
}

void MainWindow::removeRecovery(int tabIndex)
{
    const QString path = recoveryPath(tabIndex);
    if (!path.isEmpty()) finishWriting(path);
    const QString root = QDir(recoveryDirectory()).absolutePath() + QLatin1Char('/');
    if (!path.isEmpty() && QFileInfo(path).isDir() && QFileInfo(path).absoluteFilePath().startsWith(root)) QDir(path).removeRecursively();
}

QFuture<ProjectWriter::SaveResult> MainWindow::saveRecoveryAsync(int tabIndex)
{
    if (tabIndex < 0) tabIndex = currentTab_;
    if (tabIndex < 0 || tabIndex >= workspaceTabs_.size()) return {};
    if (tabIndex == currentTab_) {
        workspaceTabs_[tabIndex] = session_;
    }
    EditorSession &tabSession = workspaceTabs_[tabIndex];
    if (!tabSession.hasDocument()) return {};
    const QString recoveryDest = tabRecoveryPaths_.value(tabIndex);
    if (recoveryDest.isEmpty()) return {};
    if (inFlightSaves_.contains(recoveryDest)) return {};

    if (tabIndex == currentTab_) {
        finishInlineText();
        if (transformOriginalDocument_) finishPersistentTransform(true);
        canvas_->resolvePendingGradient();
        canvas_->resolvePendingDistortion();
        if (session_.hasFloatingSelection()) session_.commitSelectionTransform();
        workspaceTabs_[tabIndex] = session_;
    }

    QDir().mkpath(recoveryDirectory());

    Document snapshot = *tabSession.document();
    snapshot.projectPath = recoveryDest;

    auto future = QtConcurrent::run([snapshot, recoveryDest]() -> ProjectWriter::SaveResult {
        return ProjectWriter::saveAtomicChecked(snapshot, recoveryDest, ProjectWriter::ExpectedDestinationState::any());
    });

    auto *watcher = new QFutureWatcher<ProjectWriter::SaveResult>(this);

    InFlightSave inFlight;
    inFlight.tabIndex = tabIndex;
    inFlight.future = future;
    inFlight.watcher = watcher;
    inFlight.completed = false;
    inFlight.completion = [this, watcher, recoveryDest](const ProjectWriter::SaveResult &result) {
        if (!inFlightSaves_.contains(recoveryDest)) return;
        if (inFlightSaves_[recoveryDest].watcher != watcher) return;
        auto entry = inFlightSaves_.take(recoveryDest);
        if (entry.watcher) {
            entry.watcher->disconnect();
            entry.watcher->deleteLater();
        }
        if (result.status != ProjectWriter::SaveResult::Status::Success) {
            statusHint_->setText(tr("Autosave failed: %1").arg(result.errorMessage));
        }
    };

    inFlightSaves_.insert(recoveryDest, inFlight);

    connect(watcher, &QFutureWatcher<ProjectWriter::SaveResult>::finished, this, [this, watcher, recoveryDest]() {
        if (inFlightSaves_.contains(recoveryDest)) {
            auto &entry = inFlightSaves_[recoveryDest];
            if (entry.watcher == watcher && !entry.completed) {
                entry.completed = true;
                const ProjectWriter::SaveResult result = watcher->result();
                if (entry.completion) entry.completion(result);
            }
        }
    });
    watcher->setFuture(future);
    return future;
}

void MainWindow::autosave()
{
    stashCurrentTab();
    for (int i = 0; i < workspaceTabs_.size(); ++i) {
        EditorSession &candidate = workspaceTabs_[i];
        const auto &doc = candidate.document();
        if (!doc || !candidate.isModified() || candidate.isPainting()) continue;
        if (doc->projectPath.isEmpty()) {
            saveRecoveryAsync(i);
        } else {
            const QString dest = doc->projectPath;
            if (inFlightSaves_.contains(dest)) continue;
            saveProjectAsync(i, false, dest, /*isAutosave=*/true);
        }
    }
}

void MainWindow::offerRecovery()
{
    if (document_) return;
    QDir root(recoveryDirectory());
    const QStringList recoveries = root.entryList({QStringLiteral("*.comp")}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    if (recoveries.isEmpty()) return;
    const auto choice = showMessage(this, tr("Recover Unsaved Project"),
        tr("CompositorLX found %n unsaved recovery copy/copies. Restore them?", nullptr, recoveries.size()),
        QString(), QMessageBox::Yes | QMessageBox::Discard, QMessageBox::Yes);
    if (choice != QMessageBox::Yes) {
        for (const QString &entry : recoveries) QDir(root.filePath(entry)).removeRecursively();
        return;
    }
    int restored = 0;
    QStringList failures;
    for (const QString &entry : recoveries) {
        const QString path = root.filePath(entry);
        try {
            auto recovered = std::make_shared<Document>(ProjectReader::load(path)); recovered->projectPath.clear();
            EditorSession recoveredSession; recoveredSession.setDocument(std::move(recovered), false);
            if (restored == 0 && workspaceTabs_.size() == 1 && !document_) {
                session_ = std::move(recoveredSession); workspaceTabs_[0] = session_; tabRecoveryPaths_[0] = path; syncDocumentViews();
            } else {
                installInNewTab(std::move(recoveredSession), tr("Untitled")); tabRecoveryPaths_[currentTab_] = path;
            }
            ++restored;
        } catch (const ProjectError &error) {
            failures << tr("%1: %2").arg(entry, error.message());
        }
    }
    if (!failures.isEmpty()) showMessage(this, tr("Some Projects Could Not Be Recovered"), failures.join(QLatin1Char('\n')));
    if (restored > 0) statusHint_->setText(tr("Recovered %n unsaved project(s)", nullptr, restored));
}

void MainWindow::chooseProject()
{
    const QString path = QFileDialog::getExistingDirectory(this, tr("Open Compositor Project"), {},
                                                            QFileDialog::ShowDirsOnly);
    if (!path.isEmpty()) openProject(path);
}

void MainWindow::exportPng(bool jpegDefault)
{
    finishInlineText();
    if (!document_) {
        showMessage(this, tr("Export Image"), tr("Open a project before exporting."));
        return;
    }
    QImage image = LayerRenderer::flattened(*document_);
    if (image.isNull()) {
        showMessage(this, tr("Export Failed"), tr("The canvas is too large to allocate for export."));
        return;
    }
    QByteArray jpegData;
    if (jpegDefault) {
        QSettings settings; const int quality = std::clamp(settings.value(QStringLiteral("jpegExportQuality"), 85).toInt(), 0, 100);
        QDialog dialog(this); dialog.setObjectName(QStringLiteral("jpegExportDialog")); dialog.setWindowTitle(tr("Export JPEG"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *preview = new QLabel(&dialog); preview->setObjectName(QStringLiteral("jpegPreview")); preview->setFixedSize(560, 330);
        preview->setAlignment(Qt::AlignCenter); preview->setStyleSheet(QStringLiteral("background:#1e2024;")); layout->addWidget(preview);
        auto *form = new QFormLayout;
        auto *qualityField = new SnapSlider(Qt::Horizontal, &dialog); qualityField->setObjectName(QStringLiteral("jpegQuality")); qualityField->setRange(0, 100); qualityField->setValue(quality);
        auto *qualityValue = new QLabel(QStringLiteral("%1%").arg(quality), &dialog); auto *qualityRow = new QHBoxLayout; qualityRow->addWidget(qualityField, 1); qualityRow->addWidget(qualityValue);
        auto *background = new QPushButton(tr("Choose…"), &dialog); background->setObjectName(QStringLiteral("jpegMatte")); QColor matte = Qt::white;
        form->addRow(tr("Quality"), qualityRow); form->addRow(tr("Background for transparency"), background); layout->addLayout(form);
        layout->addWidget(new QLabel(QStringLiteral("%1 × %2 px · sRGB").arg(image.width()).arg(image.height()), &dialog));
        auto *resultText = new QLabel(&dialog); resultText->setObjectName(QStringLiteral("jpegResult")); layout->addWidget(resultText);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog); buttons->button(QDialogButtonBox::Ok)->setText(tr("Export…"));
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
        std::optional<JpegResult> encoded;
        const auto updatePreview = [&] {
            QString error; encoded = ImageExporter::jpeg(image, qualityField->value(), matte, document_->resolution, &error);
            buttons->button(QDialogButtonBox::Ok)->setEnabled(encoded.has_value());
            if (!encoded) { preview->clear(); resultText->setText(error); return; }
            preview->setPixmap(QPixmap::fromImage(encoded->preview).scaled(preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            resultText->setText(tr("%1 · encoded preview, fitted to window").arg(QLocale().formattedDataSize(encoded->data.size())));
        };
        QTimer previewTimer(&dialog); previewTimer.setSingleShot(true); previewTimer.setInterval(200);
        connect(&previewTimer, &QTimer::timeout, &dialog, updatePreview);
        connect(qualityField, &QSlider::valueChanged, &dialog, [=, &previewTimer](int value) { qualityValue->setText(QStringLiteral("%1%").arg(value)); previewTimer.start(); });
        connect(background, &QPushButton::clicked, &dialog, [this, &matte, &previewTimer] { const QColor chosen = QColorDialog::getColor(matte, this, tr("Background for Transparency")); if (chosen.isValid()) { matte = chosen; previewTimer.start(); } });
        updatePreview();
        if (dialog.exec() != QDialog::Accepted || !encoded) return;
        settings.setValue(QStringLiteral("jpegExportQuality"), qualityField->value()); jpegData = std::move(encoded->data);
    }

    QString base = QFileInfo(document_->projectPath).completeBaseName(); if (base.isEmpty()) base = tr("Untitled");
    const QString suggested = base + (jpegDefault ? QStringLiteral(".jpg") : QStringLiteral(".png"));
    const QString path = QFileDialog::getSaveFileName(this, jpegDefault ? tr("Export JPEG") : tr("Export PNG"), suggested,
                                                       jpegDefault ? tr("JPEG images (*.jpg *.jpeg)") : tr("PNG images (*.png)"));
    if (path.isEmpty()) return;
    const int dotsPerMeter = qRound(document_->resolution / .0254); image.setDotsPerMeterX(dotsPerMeter); image.setDotsPerMeterY(dotsPerMeter);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        showMessage(this, tr("Export Failed"), file.errorString());
        return;
    }
    if (jpegDefault) {
        if (file.write(jpegData) != jpegData.size() || !file.commit()) { showMessage(this, tr("Export Failed"), file.errorString()); return; }
        statusBar()->showMessage(tr("Exported %1").arg(QFileInfo(path).fileName()), 6000); return;
    }
    QImageWriter writer(&file, "PNG"); writer.setText(QStringLiteral("Software"), QStringLiteral("CompositorLX"));
    if (!writer.write(image) || !file.commit()) {
        showMessage(this, tr("Export Failed"),
                              writer.errorString().isEmpty() ? file.errorString() : writer.errorString());
        return;
    }
    statusBar()->showMessage(tr("Exported %1").arg(QFileInfo(path).fileName()), 6000);
}

bool MainWindow::openProject(const QString &path)
{
    cancelActiveSelectionTask();
    if (RawImporter::matches(path)) {
        RawDevelopDialog dialog(this, path);
        if (dialog.exec() != QDialog::Accepted) return false;
        QImage image = dialog.developedImage();
        if (image.isNull()) {
            showMessage(this, tr("Could Not Open RAW File"), tr("Failed to develop RAW image."));
            return false;
        }
        EditorSession opened;
        if (!opened.insertImage(image, QFileInfo(path).completeBaseName(), std::nullopt)) {
            showMessage(this, tr("Could Not Open RAW File"), tr("Failed to create document from RAW image."));
            return false;
        }
        if (workspaceTabs_.size() == 1 && !document_) {
            session_ = std::move(opened); workspaceTabs_[0] = session_; syncDocumentViews();
        } else {
            installInNewTab(std::move(opened), QFileInfo(path).completeBaseName());
        }
        statusBar()->showMessage(tr("Opened %1 · %2 × %3 · %4 layers")
                                     .arg(QFileInfo(path).fileName())
                                     .arg(document_->canvasSize.width())
                                     .arg(document_->canvasSize.height())
                                     .arg(document_->layers.size()), 6000);
        return true;
    }

    const QString suffix = QFileInfo(path).suffix().toLower();
    if (PSDReader::matches(path) || suffix == QStringLiteral("psd") || suffix == QStringLiteral("psb")) {
        PSDImportResult result;
        QString psdError;
        if (!PSDReader::read(path, result, &psdError)) {
            showMessage(this, tr("Could Not Open Photoshop File"), psdError);
            return false;
        }
        if (!result.conversions.isEmpty()) {
            bool confirmed = true;
            if (session_.confirmConversionsCallback()) {
                confirmed = session_.confirmConversionsCallback()(result.conversions);
            } else {
                PSDConversionDialog dialog(QFileInfo(path).fileName(), result.conversions, this);
                confirmed = (dialog.exec() == QDialog::Accepted);
            }
            if (!confirmed) return false;
        }
        EditorSession opened;
        if (!opened.insertPhotoshop(result, QFileInfo(path).completeBaseName(), std::nullopt, &psdError)) {
            showMessage(this, tr("Could Not Open Photoshop File"), psdError);
            return false;
        }
        if (workspaceTabs_.size() == 1 && !document_) {
            session_ = std::move(opened); workspaceTabs_[0] = session_; syncDocumentViews();
        } else {
            installInNewTab(std::move(opened), QFileInfo(path).completeBaseName());
        }
        statusBar()->showMessage(tr("Opened %1 · %2 × %3 · %4 layers")
                                     .arg(QFileInfo(path).fileName())
                                     .arg(document_->canvasSize.width())
                                     .arg(document_->canvasSize.height())
                                     .arg(document_->layers.size()), 6000);
        return true;
    }

    if (SvgImporter::matches(path)) {
        EditorSession opened;
        QString svgError;
        if (!opened.insertSvg(path, std::nullopt, &svgError)) {
            showMessage(this, tr("Could Not Open SVG File"), svgError.isEmpty() ? tr("Failed to read SVG file.") : svgError);
            return false;
        }
        if (workspaceTabs_.size() == 1 && !document_) {
            session_ = std::move(opened); workspaceTabs_[0] = session_; syncDocumentViews();
        } else {
            installInNewTab(std::move(opened), QFileInfo(path).completeBaseName());
        }
        statusBar()->showMessage(tr("Opened %1 · %2 × %3 · %4 layers")
                                     .arg(QFileInfo(path).fileName())
                                     .arg(document_->canvasSize.width())
                                     .arg(document_->canvasSize.height())
                                     .arg(document_->layers.size()), 6000);
        return true;
    }

    try {
        ProjectWriter::recoverInterruptedPackage(path);
        auto loaded = std::make_shared<Document>(ProjectReader::load(path));
        EditorSession opened; opened.setDocument(std::move(loaded));
        if (workspaceTabs_.size() == 1 && !document_) {
            session_ = std::move(opened); workspaceTabs_[0] = session_; syncDocumentViews();
        } else {
            installInNewTab(std::move(opened), QFileInfo(path).completeBaseName());
        }
        setupWatcherForTab(currentTab_, path);
        noteRecentProject(path);
        statusBar()->showMessage(tr("Opened %1 · %2 × %3 · %4 layers")
                                     .arg(QFileInfo(path).fileName())
                                     .arg(document_->canvasSize.width())
                                     .arg(document_->canvasSize.height())
                                     .arg(document_->layers.size()), 6000);
        return true;
    } catch (const ProjectError &error) {
        showMessage(this, tr("Could Not Open Project"), error.message());
        return false;
    } catch (const std::exception &error) {
        showMessage(this, tr("Could Not Open Project"), QString::fromUtf8(error.what()));
        return false;
    }
}

void MainWindow::refreshTitle()
{
    QString fallback = tabs_ && currentTab_ >= 0 && currentTab_ < tabs_->count() ? tabs_->tabText(currentTab_) : tr("Untitled");
    fallback.remove(QStringLiteral(" •"));
    QString name = document_ && !document_->projectPath.isEmpty() ? QFileInfo(document_->projectPath).completeBaseName() : fallback;
    if (name.isEmpty()) name = tr("Untitled");
    const QString displayed = document_ && session_.isModified() ? name + QStringLiteral(" •") : name;
    if (tabs_ && currentTab_ >= 0 && currentTab_ < tabs_->count()) tabs_->setTabText(currentTab_, displayed);
    if (!document_ && statusDimensions_) {
        statusDimensions_->setText(tr("No document"));
        if (layerCount_) layerCount_->setText(QStringLiteral("0"));
        if (statusHint_) statusHint_->setText(tr("Open or drop a Compositor project to begin"));
    }
    setWindowTitle(QStringLiteral("%1 — CompositorLX").arg(displayed));
}

void MainWindow::stashCurrentTab()
{
    if (currentTab_ >= 0 && currentTab_ < workspaceTabs_.size()) workspaceTabs_[currentTab_] = session_;
}

void MainWindow::activateTab(int index)
{
    cancelActiveSelectionTask();
    if (index < 0 || index >= workspaceTabs_.size() || index == currentTab_) return;
    finishInlineText();
    if (transformOriginalDocument_) finishPersistentTransform(true);
    canvas_->resolvePendingGradient(); canvas_->resolvePendingDistortion(); canvas_->resolvePendingCrop(false);
    if (session_.hasFloatingSelection()) session_.commitSelectionTransform();
    stashCurrentTab(); currentTab_ = index; session_ = workspaceTabs_.at(index); syncDocumentViews();
    if (tabPendingExternalChange_.value(index, false) && tabPendingExternalDigest_.value(index).has_value() && session_.document()) {
        promptExternalChange(index, session_.document()->projectPath, *tabPendingExternalDigest_[index]);
    }
}

void MainWindow::installTabCloseButton(int index)
{
    QWidget *container = createTabCloseButton(tabs_);
    auto *button = container->findChild<QToolButton *>(QStringLiteral("documentTabClose"));
    tabs_->setTabButton(index, QTabBar::RightSide, container);
    connect(button, &QToolButton::clicked, this, [this, container] {
        for (int candidate = 0; candidate < tabs_->count(); ++candidate) {
            if (tabs_->tabButton(candidate, QTabBar::RightSide) == container) {
                closeTab(candidate);
                return;
            }
        }
    });
}

void MainWindow::installInNewTab(EditorSession session, const QString &title)
{
    finishInlineText();
    if (transformOriginalDocument_) finishPersistentTransform(true);
    canvas_->resolvePendingGradient(); canvas_->resolvePendingDistortion(); canvas_->resolvePendingCrop(false);
    if (session_.hasFloatingSelection()) session_.commitSelectionTransform();
    stashCurrentTab(); workspaceTabs_.push_back(std::move(session));
    tabRecoveryPaths_.push_back(newRecoveryPath());
    tabWatchers_.push_back(nullptr);
    tabDigests_.push_back(std::nullopt);
    tabPendingExternalChange_.push_back(false);
    tabPendingExternalDigest_.push_back(std::nullopt);
    tabPendingExternalDoc_.push_back(nullptr);
    currentTab_ = workspaceTabs_.size() - 1;
    tabs_->blockSignals(true); tabs_->addTab(title); installTabCloseButton(currentTab_); tabs_->setCurrentIndex(currentTab_); tabs_->blockSignals(false);
    session_ = workspaceTabs_.at(currentTab_); syncDocumentViews();
}

bool MainWindow::copyLayersToTab(const QVector<QUuid> &ids, int targetIndex, bool newTab)
{
    if (ids.isEmpty()) return false;
    stashCurrentTab();
    const Document *source = nullptr; int sourceIndex = -1;
    for (int candidateIndex = 0; candidateIndex < workspaceTabs_.size(); ++candidateIndex) {
        const EditorSession &candidate = workspaceTabs_.at(candidateIndex);
        if (!candidate.document()) continue;
        const bool found = std::any_of(candidate.document()->layers.cbegin(), candidate.document()->layers.cend(),
            [&ids](const Layer &layer) { return ids.contains(layer.id); });
        if (found) { source = candidate.document().get(); sourceIndex = candidateIndex; break; }
    }
    if (!source) return false;
    if (!newTab && sourceIndex == targetIndex) return false;
    const Document sourceSnapshot = *source;
    QSet<QUuid> included(ids.cbegin(), ids.cend());
    bool grew = true;
    while (grew) {
        grew = false;
        for (const Layer &layer : sourceSnapshot.layers) if (layer.parentId && included.contains(*layer.parentId) && !included.contains(layer.id)) {
            included.insert(layer.id); grew = true;
        }
    }
    QVector<Layer> copied;
    qint64 addedPixels = 0;
    for (const Layer &layer : sourceSnapshot.layers) if (included.contains(layer.id)) {
        copied.push_back(layer);
        addedPixels += qint64(layer.image.width()) * layer.image.height();
        addedPixels += qint64(layer.mask.width()) * layer.mask.height();
    }
    if (copied.isEmpty()) return false;

    if (newTab) {
        installInNewTab(EditorSession(), tr("Untitled %1").arg(workspaceTabs_.size() + 1));
        targetIndex = currentTab_;
    } else {
        if (targetIndex < 0 || targetIndex >= workspaceTabs_.size()) return false;
        activateTab(targetIndex);
    }
    if (!session_.document()) session_.createDocument(sourceSnapshot.canvasSize.width(), sourceSnapshot.canvasSize.height());
    if (!session_.document()) return false;
    qint64 usedPixels = 0;
    for (const Layer &layer : session_.document()->layers) {
        usedPixels += qint64(layer.image.width()) * layer.image.height();
        usedPixels += qint64(layer.mask.width()) * layer.mask.height();
    }
    if (usedPixels + addedPixels > 100000000LL) {
        showMessage(this, tr("Layers Too Large"), tr("The copied layers exceed this project's 100-megapixel limit."));
        return false;
    }
    const auto root = std::find_if(sourceSnapshot.layers.cbegin(), sourceSnapshot.layers.cend(), [&ids](const Layer &layer) { return ids.contains(layer.id); });
    const QPointF anchor = root == sourceSnapshot.layers.cend() ? QPointF(sourceSnapshot.canvasSize.width() / 2.0, sourceSnapshot.canvasSize.height() / 2.0) : root->transform.center();
    const QPointF destination(session_.document()->canvasSize.width() / 2.0, session_.document()->canvasSize.height() / 2.0);
    const QPointF offset = destination - anchor;
    QHash<QUuid, QUuid> mapping;
    for (const Layer &layer : copied) mapping.insert(layer.id, QUuid::createUuid());
    for (Layer &layer : copied) {
        const QUuid old = layer.id; layer.id = mapping.value(old);
        layer.parentId = layer.parentId && mapping.contains(*layer.parentId) ? std::optional<QUuid>(mapping.value(*layer.parentId)) : std::nullopt;
        layer.maskSourceId = layer.maskSourceId && mapping.contains(*layer.maskSourceId) ? std::optional<QUuid>(mapping.value(*layer.maskSourceId)) : std::nullopt;
        layer.transform.origin += offset;
        if (layer.maskPlacement) layer.maskPlacement->origin += offset;
    }
    session_.beginEdit(copied.size() > 1 ? tr("Copy Layers from Project") : tr("Copy Layer from Project"));
    session_.document()->layers += copied;
    const QUuid primary = mapping.value(ids.constFirst(), copied.constLast().id);
    session_.selectLayer(primary);
    session_.endEdit();
    syncDocumentViews();
    return true;
}

void MainWindow::closeTab(int index)
{
    cancelActiveSelectionTask();
    if (index < 0 || index >= workspaceTabs_.size()) return;
    finishInlineText();
    activateTab(index);
    for (const auto &key : inFlightSaves_.keys()) {
        if (inFlightSaves_.contains(key) && inFlightSaves_[key].tabIndex == index) {
            finishWriting(key);
        }
    }
    if (workspaceTabs_[index].hasDocument()) {
        const QString path = workspaceTabs_[index].document()->projectPath;
        if (!path.isEmpty()) finishWriting(path);
    }
    if (!confirmReplacement()) return;
    if (workspaceTabs_.size() == 1) {
        removeRecovery(index); session_ = EditorSession(); workspaceTabs_[0] = session_; tabRecoveryPaths_[0] = newRecoveryPath();
        if (tabWatchers_.size() > 0 && tabWatchers_[0]) {
            tabWatchers_[0]->stop();
            tabWatchers_[0]->deleteLater();
            tabWatchers_[0] = nullptr;
        }
        tabDigests_[0] = std::nullopt;
        tabPendingExternalChange_[0] = false;
        tabPendingExternalDigest_[0] = std::nullopt;
        tabPendingExternalDoc_[0] = nullptr;
        tabs_->setTabText(0, tr("Untitled")); syncDocumentViews(); return;
    }
    removeRecovery(index);
    stashCurrentTab(); workspaceTabs_.removeAt(index);
    tabRecoveryPaths_.removeAt(index);
    if (tabWatchers_.size() > index) {
        if (tabWatchers_[index]) {
            tabWatchers_[index]->stop();
            tabWatchers_[index]->deleteLater();
        }
        tabWatchers_.removeAt(index);
    }
    if (tabDigests_.size() > index) tabDigests_.removeAt(index);
    if (tabPendingExternalChange_.size() > index) tabPendingExternalChange_.removeAt(index);
    if (tabPendingExternalDigest_.size() > index) tabPendingExternalDigest_.removeAt(index);
    if (tabPendingExternalDoc_.size() > index) tabPendingExternalDoc_.removeAt(index);

    tabs_->blockSignals(true); tabs_->removeTab(index); tabs_->blockSignals(false);
    currentTab_ = -1; const int next = std::min(index, int(workspaceTabs_.size()) - 1);
    tabs_->setCurrentIndex(next); activateTab(next);
}

void MainWindow::selectLayer(const QModelIndex &index)
{
    if (!document_ || !index.isValid()) return;
    const auto id = layerModel_->layerId(index); if (!id) return;
    canvas_->resolvePendingGradient(); canvas_->resolvePendingDistortion();
    session_.selectLayer(*id);
    updateInspector();
    canvas_->update();
}

void MainWindow::updateInspector()
{
    if (!document_ || !document_->activeLayerId) {
        const std::array<QWidget *, 14> fields = {xLabel_, xField_, yLabel_, yField_, widthLabel_, widthField_, heightLabel_, heightField_, scaleField_, rotationLabel_, rotationField_, sampling_, blendMode_, opacitySlider_};
        for (QWidget *field : fields) {
            if (field) field->setEnabled(false);
        }
        updateCommandStates();
        return;
    }
    const auto it = std::find_if(document_->layers.cbegin(), document_->layers.cend(), [this](const Layer &layer) {
        return layer.id == *document_->activeLayerId;
    });
    if (it == document_->layers.cend()) return;
    const std::array<QWidget *, 14> fields = {xLabel_, xField_, yLabel_, yField_, widthLabel_, widthField_, heightLabel_, heightField_, scaleField_, rotationLabel_, rotationField_, sampling_, blendMode_, opacitySlider_};
    for (QWidget *field : fields) if (field) field->setEnabled(true);
    const QSignalBlocker bx(xField_), by(yField_), bw(widthField_), bh(heightField_), bs(scaleField_), br(rotationField_), bo(opacitySlider_), bb(blendMode_), bsample(sampling_);
    const QRectF groupBounds = session_.selectedLayerIds().size() > 1 ? session_.selectedLayersBounds() : QRectF();
    if (!groupBounds.isEmpty()) {
        xField_->setValue(groupBounds.x()); yField_->setValue(groupBounds.y()); widthField_->setValue(groupBounds.width()); heightField_->setValue(groupBounds.height());
        scaleField_->setValue(100); rotationField_->setValue(0);
        scaleField_->setEnabled(false); sampling_->setEnabled(false);
    } else {
        const LayerTransform shown=session_.isMaskSelected()&&!it->maskLinked?it->maskPlacement.value_or(it->transform):it->transform;
        xField_->setValue(shown.origin.x()); yField_->setValue(shown.origin.y()); widthField_->setValue(shown.size.width()); heightField_->setValue(shown.size.height());
        const QSize pixels=session_.isMaskSelected()&&!it->mask.isNull()?it->mask.size().expandedTo(QSize(1,1)):it->image.isNull()?shown.size.toSize().expandedTo(QSize(1,1)):it->image.size();
        scaleField_->setValue(shown.size.width()/std::max(1,pixels.width())*100); rotationField_->setValue(shown.rotation);
        scaleField_->setEnabled(true); sampling_->setEnabled(true);
    }
    sampling_->setCurrentIndex(2-int(it->transform.sampling));
    opacitySlider_->setValue(qRound(it->opacity * 100));
    const int blendIndex = std::clamp(int(it->blendMode), 0, blendMode_->count() - 1);
    blendMode_->setCurrentIndex(blendIndex);
    updateCommandStates();
}

void MainWindow::updateCommandStates()
{
    const auto action = [this](const char *name) { return findChild<QAction *>(QString::fromLatin1(name)); };
    const auto enabled = [&action](const char *name, bool value) { if (QAction *item = action(name)) item->setEnabled(value); };
    const bool text = textEditorHasFocus();
    const bool hasDocument = bool(document_);
    const Layer *active = session_.activeLayer();
    const bool hasActive = active != nullptr;
    const bool pixelTarget = active && !active->group && (session_.isMaskSelected() ? !active->mask.isNull() : !active->image.isNull());
    const bool hasSelection = document_ && document_->selection.has_value();
    const bool canCopy = pixelTarget;
    bool canTransformLayer = false;
    if (document_) for (const Layer &layer : document_->layers) {
        if (!session_.selectedLayerIds().contains(layer.id)) continue;
        if (!layer.group && !layer.image.isNull()) { canTransformLayer = true; break; }
        if (layer.group) {
            const QSet<QUuid> descendants = session_.descendantIds(layer.id);
            if (std::any_of(document_->layers.cbegin(), document_->layers.cend(), [&descendants](const Layer &child) { return descendants.contains(child.id) && !child.group && !child.image.isNull(); })) { canTransformLayer = true; break; }
        }
    }
    enabled("commandUndo", text || session_.canUndo()); enabled("commandRedo", text || session_.canRedo());
    enabled("commandSave", hasDocument); enabled("commandSaveAs", hasDocument); enabled("commandExportPng", hasDocument); enabled("commandExportJpeg", hasDocument);
    enabled("imageTrimAction", hasDocument); enabled("imageCropAction", hasDocument);
    enabled("commandCut", text || (hasSelection && canCopy)); enabled("commandCopy", text || canCopy); enabled("commandCopyMerged", hasDocument && hasSelection);
    enabled("commandPaste", text || !clipboardImage_.isNull() || !QGuiApplication::clipboard()->image().isNull());
    enabled("commandDuplicate", hasActive && (hasSelection ? canCopy : !active->group)); enabled("commandDelete", text || hasActive || selectedEffect_.has_value());
    enabled("commandTransform", hasSelection ? canCopy : canTransformLayer);
    enabled("commandNewLayer", hasDocument); enabled("commandNewFolder", hasDocument); enabled("commandGroupLayers", hasDocument); enabled("commandUngroupLayers", hasDocument && session_.canUngroupLayers());
    enabled("commandMoveOut", active && active->parentId.has_value()); enabled("commandRenameLayer", hasActive); enabled("commandVisibility", hasActive);
    enabled("commandMerge", session_.canMergeLayers()); enabled("commandMoveUp", session_.canMoveActiveLayer(1)); enabled("commandMoveDown", session_.canMoveActiveLayer(-1));
    enabled("commandNewAdjustment", hasDocument); enabled("commandEditAdjustment", active && !active->adjustment.isEmpty() && active->adjustment.value(QStringLiteral("kind")).toString() != QStringLiteral("Invert"));
    enabled("commandLayerEffects", session_.canEditEffects());
    enabled("commandSelectAll", text || hasDocument); enabled("commandDeselect", hasSelection); enabled("commandInverseSelection", hasSelection);

    if (QAction *item = action("commandTransform")) item->setText(hasSelection ? tr("Transform Selection") : tr("Transform Layer"));
    if (QAction *item = action("commandDuplicate")) item->setText(hasSelection ? tr("Layer via Copy") : tr("Duplicate Layer"));
    if (QAction *item = action("commandDelete")) item->setText(selectedEffect_ ? tr("Delete Effect")
        : session_.isMaskSelected() && active && !active->mask.isNull() ? tr("Delete Layer Mask")
        : session_.selectedLayerIds().size() > 1 ? tr("Delete Layers") : tr("Delete Layer"));
    if (QAction *item = action("commandVisibility")) item->setText(active && !active->visible ? tr("Show Layer") : tr("Hide Layer"));
    if (QAction *item = action("commandClipping")) {
        item->setText(active && active->maskSourceId ? tr("Release Clipping Mask") : tr("Create Clipping Mask"));
        bool canClip = active && !active->group;
        if (canClip && !active->maskSourceId) {
            int below = -1;
            for (int i = 0; i < document_->layers.size() && document_->layers.at(i).id != active->id; ++i) if (document_->layers.at(i).parentId == active->parentId) below = i;
            canClip = below >= 0 && !document_->layers.at(below).group;
        }
        item->setEnabled(canClip);
    }
    if (QAction *item = action("commandMerge")) item->setText(session_.mergeTitle());

    if (auto *btn = findChild<QToolButton *>(QStringLiteral("addEffectButton"))) {
        btn->setEnabled(session_.canEditEffects());
    }
    if (auto *btn = findChild<QToolButton *>(QStringLiteral("deleteLayerButton"))) {
        btn->setEnabled(hasActive || selectedEffect_.has_value());
        btn->setToolTip(selectedEffect_ ? tr("Delete selected effect")
                        : session_.isMaskSelected() && active && !active->mask.isNull() ? tr("Delete layer mask")
                        : session_.selectedLayerIds().size() > 1 ? tr("Delete selected layers") : tr("Delete selected layer"));
    }

    const auto setActionChecked = [](QAction *action, bool checked) {
        if (!action) return;
        const QSignalBlocker blocker(action);
        action->setChecked(checked);
    };
    setActionChecked(actionShowGrid_, session_.showsGrid());
    setActionChecked(actionShowGuides_, session_.showsGuides());
    setActionChecked(actionShowRulers_, session_.showsRulers());
    setActionChecked(actionSnap_, session_.snapEnabled());
    setActionChecked(actionSnapToGuides_, session_.snapToGuides());
    setActionChecked(actionSnapToGrid_, session_.snapToGrid());
    setActionChecked(actionSnapToLayers_, session_.snapToLayers());
    setActionChecked(actionSnapToDocumentBounds_, session_.snapToDocumentBounds());
    setActionChecked(actionLockGuides_, session_.locksGuides());
    if (actionClearGuides_) actionClearGuides_->setEnabled(session_.canClearGuides());
}

void MainWindow::updateRulerVisibility()
{
    const bool show = session_.showsRulers() && session_.hasDocument();
    if (rulerCorner_) rulerCorner_->setVisible(show);
    if (horizontalRuler_) {
        horizontalRuler_->setVisible(show);
        if (show) horizontalRuler_->update();
    }
    if (verticalRuler_) {
        verticalRuler_->setVisible(show);
        if (show) verticalRuler_->update();
    }
}

InlineTextEditor *MainWindow::inlineTextEditor() const
{
    return canvas_ ? canvas_->findChild<InlineTextEditor *>(QStringLiteral("inlineTextEditor"), Qt::FindDirectChildrenOnly) : nullptr;
}

bool MainWindow::isMenuBarVisible() const
{
    if (!menuBar() || menuBar()->isHidden()) {
        return false;
    }
    if (const auto *act = findChild<QAction *>(QStringLiteral("showMenuBar"))) {
        return act->isChecked();
    }
    return true;
}

void MainWindow::updateMenuRestoreButton()
{
    if (!menuRestoreButton_) return;
    const bool visible = isMenuBarVisible();
    if (visible) {
        menuRestoreButton_->setToolTip(tr("Hide menu bar (Ctrl+Shift+M)"));
        menuRestoreButton_->setAccessibleName(tr("Hide menu bar"));
        if (quickFileMenu_ && quickFileMenu_->isVisible()) {
            quickFileMenu_->close();
        }
    } else {
        menuRestoreButton_->setToolTip(tr("Show menu bar (Ctrl+Shift+M) · Right-click for menu bar"));
        menuRestoreButton_->setAccessibleName(tr("Show menu bar"));
    }
}

void MainWindow::syncDocumentViews(bool compositeChanged)
{
    const bool enteringCanvas = session_.document() && canvasStack_->currentWidget() != canvas_;
    const QPointer<QWidget> focusBeforeEntry = enteringCanvas ? QApplication::focusWidget() : nullptr;
    document_ = session_.document();
    canvasStack_->setCurrentWidget(document_ ? static_cast<QWidget *>(canvas_) : canvasStack_->widget(0));
    if (enteringCanvas) QTimer::singleShot(0, canvas_, [this, focusBeforeEntry] {
        QWidget *current = QApplication::focusWidget();
        if (canvas_->isVisible() && (!current || current == focusBeforeEntry || !current->isVisible())) canvas_->setFocus(Qt::OtherFocusReason);
    });
    if (!document_ && suggestClipboardOnEmpty_) {
        suggestClipboardOnEmpty_ = false;
        const QImage clipboard = QGuiApplication::clipboard()->image();
        if (!clipboard.isNull() && clipboard.width() <= 30000 && clipboard.height() <= 30000
            && qint64(clipboard.width()) * clipboard.height() <= 100000000LL) {
            newCanvasWidth_->setValue(clipboard.width()); newCanvasHeight_->setValue(clipboard.height());
        } else { newCanvasWidth_->setValue(1920); newCanvasHeight_->setValue(1080); }
        newCanvasWidth_->setFocus(); newCanvasWidth_->selectAll();
    }
    canvas_->setDocument(document_, compositeChanged);
    canvas_->setCloneTracking(session_.cloneSource(), session_.cloneOffset());
    canvas_->setSelectedLayerIds(session_.selectedLayerIds());
    canvas_->setTransformMask(session_.isMaskSelected() && session_.activeLayer() && !session_.activeLayer()->maskLinked);
    canvas_->setMaskTarget(session_.isMaskSelected());
    canvas_->setFloatingTransform(session_.hasFloatingSelection());
    layerModel_->setDocument(document_, compositeChanged);
    if (selectedEffect_) {
        bool stillExists = false;
        if (document_) {
            for (const Layer &l : document_->layers) {
                if (l.id == selectedEffect_->layerId && l.effects.has_value() && l.effects->contains(selectedEffect_->kind)) {
                    stillExists = true;
                    break;
                }
            }
        }
        if (!stillExists) selectedEffect_ = std::nullopt;
    }
    {
        const QSignalBlocker blocker(layerView_->selectionModel());
        layerView_->selectionModel()->clearSelection();
        QModelIndex primaryIndex;
        if (document_) {
            for (int row = 0; row < layerModel_->rowCount(); ++row) {
                const QModelIndex index = layerModel_->index(row, 0);
                const auto id = layerModel_->layerId(index); if (!id) continue;
                if (selectedEffect_ && layerModel_->isEffect(index)) {
                    if (*id == selectedEffect_->layerId && layerModel_->effectKind(index) == selectedEffect_->kind) {
                        layerView_->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
                        primaryIndex = index;
                    }
                } else if (!selectedEffect_ && !layerModel_->isEffect(index)) {
                    if (session_.selectedLayerIds().contains(*id)) {
                        layerView_->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
                    }
                    if (document_->activeLayerId && *id == *document_->activeLayerId) primaryIndex = index;
                }
            }
        }
        layerView_->selectionModel()->setCurrentIndex(primaryIndex, QItemSelectionModel::NoUpdate);
    }
    layerCount_->setText(document_ ? QString::number(document_->layers.size()) : QStringLiteral("0"));
    if (document_) {
        statusDimensions_->setText(QStringLiteral("%1 × %2 px").arg(document_->canvasSize.width()).arg(document_->canvasSize.height()));
        if (canvas_ && canvas_->tool() == CanvasWidget::Tool::Blur) {
            updateSmearStatusHint();
        } else {
            statusHint_->setText(tr("Drag to move · Handles to resize · Circle to rotate · 1–0 layer opacity · Space to pan"));
        }
    }
    updateInspector();
    updateRulerVisibility();
    refreshTitle();
}

void MainWindow::showAbout()
{
    showMessage(this, tr("About CompositorLX"),
                tr("CompositorLX %1").arg(QCoreApplication::applicationVersion()),
                tr("Linux port of Compositor · Qt 6 Widgets + C++20"));
}

void MainWindow::checkForUpdates()
{
    auto *network = new QNetworkAccessManager(this);
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/ClaudiuJitea/compositorLX/releases/latest")));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("CompositorLX/%1").arg(QCoreApplication::applicationVersion()));
    QNetworkReply *reply = network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, network] {
        const QByteArray payload = reply->readAll(); const QString error = reply->errorString(); const bool ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater(); network->deleteLater();
        if (!ok) { showMessage(this, tr("Could Not Check for Updates"), tr("The release service could not be reached."), error); return; }
        const QJsonObject release = QJsonDocument::fromJson(payload).object();
        const QString tag = release.value(QStringLiteral("tag_name")).toString(); const QUrl page(release.value(QStringLiteral("html_url")).toString());
        if (tag.isEmpty()) { showMessage(this, tr("Could Not Check for Updates"), tr("The release response was not recognized.")); return; }
        const QVersionNumber current = QVersionNumber::fromString(QCoreApplication::applicationVersion());
        const QVersionNumber available = QVersionNumber::fromString(tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag);
        if (!available.isNull() && QVersionNumber::compare(available, current) > 0) {
            if (showMessage(this, tr("Update Available"), tr("%1 is available. You have %2.").arg(tag, QCoreApplication::applicationVersion()),
                            tr("Open the release page to see the available downloads."), QMessageBox::Open | QMessageBox::Cancel, QMessageBox::Open) == QMessageBox::Open)
                QDesktopServices::openUrl(page);
        } else showMessage(this, tr("CompositorLX Is Up to Date"), tr("You are using the latest available version (%1).").arg(QCoreApplication::applicationVersion()));
    });
}

void MainWindow::deleteLayersWithMaskChoice()
{
    if (selectedEffect_) {
        const auto sel = *selectedEffect_;
        selectedEffect_ = std::nullopt;
        session_.removeLayerEffect(sel.layerId, sel.kind);
        syncDocumentViews();
        return;
    }
    if (session_.selectedDeletionLiveMaskDependents().isEmpty()) { session_.deleteSelectedLayers(); syncDocumentViews(); return; }
    QMessageBox box(QMessageBox::Question, tr("This layer supplies a live mask"),
                    tr("Bake keeps the current masked appearance. Remove Links reveals the dependent layers' pixels."),
                    QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, this);
    box.button(QMessageBox::Yes)->setText(tr("Bake and Delete")); box.button(QMessageBox::No)->setText(tr("Remove Links and Delete"));
    const int choice = box.exec(); if (choice == QMessageBox::Cancel) return;
    session_.deleteSelectedLayers(choice == QMessageBox::Yes); syncDocumentViews();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls() || event->mimeData()->hasImage()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (event->mimeData()->hasImage()) {
        const QImage image = qvariant_cast<QImage>(event->mimeData()->imageData());
        const QPoint canvasPoint = canvas_->mapFrom(this, event->position().toPoint());
        if (!image.isNull() && session_.insertImage(image, tr("Dropped Image"), canvas_->documentPointAt(canvasPoint))) { syncDocumentViews(); event->acceptProposedAction(); }
        return;
    }
    if (!event->mimeData()->hasUrls()) return;
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.size() == 1 && QFileInfo(urls.constFirst().toLocalFile()).isDir()) {
        if (openProject(urls.constFirst().toLocalFile())) event->acceptProposedAction();
        return;
    }
    QStringList paths;
    for (const QUrl &url : urls) if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) paths << url.toLocalFile();
    const QPoint canvasPoint = canvas_->mapFrom(this, event->position().toPoint());
    if (importImageFiles(paths, canvas_->documentPointAt(canvasPoint))) event->acceptProposedAction();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    cancelActiveSelectionTask();
    finishInlineText();
    if (transformOriginalDocument_) finishPersistentTransform(true);
    canvas_->resolvePendingGradient(); canvas_->resolvePendingDistortion(); canvas_->resolvePendingCrop(false);
    if (session_.hasFloatingSelection()) session_.commitSelectionTransform();
    finishWriting();
    stashCurrentTab();
    const int initiallyActive = currentTab_;
    QVector<int> order; order.push_back(initiallyActive);
    for (int i = 0; i < workspaceTabs_.size(); ++i) if (i != initiallyActive) order.push_back(i);
    for (int index : order) {
        if (index < 0 || index >= workspaceTabs_.size() || !workspaceTabs_.at(index).isModified()) continue;
        tabs_->setCurrentIndex(index);
        activateTab(index);
        if (!confirmReplacement()) { event->ignore(); return; }
        stashCurrentTab();
    }
    for (const QString &path : std::as_const(tabRecoveryPaths_)) {
        const QString root = QDir(recoveryDirectory()).absolutePath() + QLatin1Char('/');
        if (QFileInfo(path).isDir() && QFileInfo(path).absoluteFilePath().startsWith(root)) QDir(path).removeRecursively();
    }
    event->accept();
}

// While Control is held the Auto Select box shows flipped, and while Shift is held the aspect lock does, as what a
// press will do (mac bca8f13). Keys held while typing in a field don't count, so shortcuts there don't flicker the bar.
void MainWindow::syncHeldModifiers()
{
    auto *autoSelect = findChild<QCheckBox *>(QStringLiteral("transformAutoSelect"));
    auto *lock = findChild<QToolButton *>(QStringLiteral("transformRatioLock"));
    if (!autoSelect || !lock) return;
    const QWidget *focus = QApplication::focusWidget();
    const bool typing = focus && (qobject_cast<const QLineEdit *>(focus) || qobject_cast<const QAbstractSpinBox *>(focus) || qobject_cast<const QTextEdit *>(focus) || qobject_cast<const QPlainTextEdit *>(focus));
    const Qt::KeyboardModifiers held = typing ? Qt::KeyboardModifiers() : QApplication::queryKeyboardModifiers();
    const bool ctrl = held.testFlag(Qt::ControlModifier), shift = held.testFlag(Qt::ShiftModifier);
    if (autoSelectFlipped_ != ctrl) {
        autoSelectFlipped_ = ctrl;
        const QSignalBlocker blocker(autoSelect);
        autoSelect->setChecked(canvas_->transformAutoSelects() != ctrl);
    }
    if (ratioLockFlipped_ != shift) {
        ratioLockFlipped_ = shift;
        const QSignalBlocker blocker(lock);
        lock->setChecked(canvas_->locksTransformRatio() != shift);
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease || event->type() == QEvent::ApplicationStateChange
        || event->type() == QEvent::WindowActivate || event->type() == QEvent::MouseButtonPress)
        QTimer::singleShot(0, this, &MainWindow::syncHeldModifiers);
    if (event->type() == QEvent::Show) {
        if (auto *dialog = qobject_cast<QDialog *>(watched)) {
            dialog->setWindowIcon(QIcon());
            if (auto *message = qobject_cast<QMessageBox *>(dialog)) message->setIcon(QMessageBox::NoIcon);
            const auto clearButtonIcons = [dialog] {
                for (QAbstractButton *button : dialog->findChildren<QAbstractButton *>()) button->setIcon(QIcon());
            };
            clearButtonIcons();
            QTimer::singleShot(0, dialog, clearButtonIcons);
        }
    }
    if (layerView_ && (watched == layerView_ || watched == layerView_->viewport())) {
        // Select > All offers Select All to the focused view first; with the Layers panel just clicked, its list took it
        // and selected every layer. The canvas gets it instead (mac 451281e); a layer's name being edited still gets it first.
        if (event->type() == QEvent::ShortcutOverride && static_cast<QKeyEvent *>(event)->matches(QKeySequence::SelectAll)) {   // a name being edited has the focus itself, not the list
            event->ignore();
            return true;
        }
        if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) {
                deleteLayersWithMaskChoice();
                return true;
            }
        }
        const auto toggleSwipeRow = [this](const QPoint &point) {
            const QModelIndex index = layerView_->indexAt(point); const auto id = layerModel_->layerId(index);
            if (!id || visibilitySwipeVisited_.contains(*id)) return;
            const auto it = std::find_if(document_->layers.cbegin(), document_->layers.cend(), [&](const Layer &layer){return layer.id==*id;});
            if (it == document_->layers.cend()) return;
            if (it->visible != visibilitySwipeValue_) session_.toggleLayerVisibility(*id);
            visibilitySwipeVisited_.insert(*id); syncDocumentViews();
        };
        if (event->type() == QEvent::MouseButtonDblClick) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            const QModelIndex index = layerView_->indexAt(mouse->position().toPoint());
            if (layerModel_->isEffect(index)) {
                const auto id = layerModel_->layerId(index);
                const auto kind = layerModel_->effectKind(index);
                if (id && kind) {
                    layerEffectsDialog(*id, *kind);
                    return true;
                }
            }
        }
        if (event->type() == QEvent::MouseButtonPress) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            const QModelIndex index = layerView_->indexAt(mouse->position().toPoint()); const auto id = layerModel_->layerId(index);
            if (mouse->button() == Qt::LeftButton && id) {
                if (layerModel_->isEffect(index)) {
                    const auto kind = layerModel_->effectKind(index);
                    const int depth = index.data(Qt::UserRole + 1).toInt();
                    const int indent = std::min(depth, 8) * 18;
                    const int eyeX = 38 + indent;
                    if (kind && mouse->position().x() >= eyeX - 6 && mouse->position().x() <= eyeX + 22) {
                        session_.toggleLayerEffect(*id, *kind);
                        syncDocumentViews();
                        return true;
                    }
                    selectedEffect_ = EffectSelection{*id, *kind};
                    session_.selectLayer(*id);
                    syncDocumentViews(false);
                    return false;
                } else {
                    const int depth = index.data(Qt::UserRole + 1).toInt();
                    const int indent = std::min(depth, 8) * 18;
                    const int thumbX = 34 + indent;
                    const QImage maskImg = index.data(Qt::UserRole + 3).value<QImage>();
                    if (!maskImg.isNull() && mouse->position().x() >= thumbX + 41 && mouse->position().x() <= thumbX + 78) {
                        pendingMaskDrag_ = true;
                        maskDragStartPos_ = mouse->position().toPoint();
                        maskDragLayerId_ = *id;
                    }
                    if (layerModel_->data(index, LayerListModel::HasEffectsRole).toBool()) {
                        const QRect rowRect = layerView_->visualRect(index);
                        if (mouse->position().x() >= rowRect.right() - 36 && mouse->position().x() <= rowRect.right()) {
                            layerModel_->toggleEffectsExpanded(*id);
                            syncDocumentViews();
                            return true;
                        }
                    }
                    if (mouse->position().x() < 29) {
                        const auto it=std::find_if(document_->layers.cbegin(),document_->layers.cend(),[&](const Layer&l){return l.id==*id;});
                        if(it!=document_->layers.cend()){visibilitySwipeActive_=true;visibilitySwipeValue_=!it->visible;visibilitySwipeVisited_.clear();session_.beginEdit(tr("Layer Visibility"));toggleSwipeRow(mouse->position().toPoint());return true;}
                    }
                }
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            pendingMaskDrag_ = false;
            if (visibilitySwipeActive_) {
                visibilitySwipeActive_=false; session_.endEdit(); visibilitySwipeVisited_.clear(); refreshTitle(); return true;
            }
        } else if (event->type() == QEvent::Leave) layerView_->viewport()->setCursor(Qt::ArrowCursor);
        else if (event->type() == QEvent::MouseMove) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (pendingMaskDrag_ && (mouse->buttons() & Qt::LeftButton)) {
                if ((mouse->position().toPoint() - maskDragStartPos_).manhattanLength() >= QApplication::startDragDistance()) {
                    pendingMaskDrag_ = false;
                    const QModelIndex dragIndex = layerView_->indexAt(maskDragStartPos_);
                    const QImage maskThumb = dragIndex.data(Qt::UserRole + 3).value<QImage>();
                    auto *drag = new QDrag(layerView_);
                    auto *mime = new QMimeData;
                    const QByteArray data = maskDragLayerId_.toString(QUuid::WithoutBraces).toUtf8();
                    mime->setData(QStringLiteral("application/x-compositor-layer-mask"), data);
                    mime->setData(QStringLiteral("com.compositor.layer-mask"), data);
                    drag->setMimeData(mime);
                    if (!maskThumb.isNull()) {
                        drag->setPixmap(QPixmap::fromImage(maskThumb));
                        drag->setHotSpot(QPoint(maskThumb.width() / 2, maskThumb.height() / 2));
                    }
                    drag->exec(Qt::CopyAction);
                    return true;
                }
            }
            if (visibilitySwipeActive_ && (mouse->buttons() & Qt::LeftButton)) { toggleSwipeRow(mouse->position().toPoint()); return true; }
            const QModelIndex index = layerView_->indexAt(mouse->position().toPoint());
            Qt::CursorShape shape = Qt::ArrowCursor;
            if (index.isValid()) {
                if (layerModel_->isEffect(index)) {
                    const int depth = index.data(Qt::UserRole + 1).toInt();
                    const int indent = std::min(depth, 8) * 18;
                    const int eyeX = 38 + indent;
                    if (mouse->position().x() >= eyeX - 6 && mouse->position().x() <= eyeX + 22) {
                        shape = Qt::PointingHandCursor;
                    } else if (mouse->modifiers().testFlag(Qt::AltModifier)) {
                        shape = Qt::DragCopyCursor;
                    }
                } else {
                    const int depth = index.data(Qt::UserRole + 1).toInt();
                    const int indent = std::min(depth, 8) * 18;
                    const int thumbnail = 34 + indent;
                    const bool overMask = !index.data(Qt::UserRole + 3).value<QImage>().isNull()
                        && mouse->position().x() >= thumbnail + 41 && mouse->position().x() <= thumbnail + 78;
                    const bool overThumb = mouse->position().x() >= thumbnail && mouse->position().x() < thumbnail + 36;
                    if (mouse->modifiers().testFlag(Qt::AltModifier)) {
                        shape = Qt::DragCopyCursor;
                    } else if ((overThumb || overMask) && mouse->modifiers().testFlag(Qt::ControlModifier)) {
                        shape = Qt::PointingHandCursor;
                    }
                }
            }
            layerView_->viewport()->setCursor(shape);
        }
    }
    if ((watched == tabs_ || watched->property("newTabDropTarget").toBool())
        && (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove || event->type() == QEvent::Drop)) {
        auto *drop = static_cast<QDropEvent *>(event);
        const bool layerDrop = drop->mimeData()->hasFormat(QStringLiteral("application/x-compositor-layers"));
        const bool fileDrop = drop->mimeData()->hasUrls();
        const bool imageDrop = drop->mimeData()->hasImage();
        if (!layerDrop && !fileDrop && !imageDrop) return false;
        if (event->type() == QEvent::Drop) {
            const bool newTab = watched != tabs_;
            const int target = newTab ? -1 : tabs_->tabAt(drop->position().toPoint());
            if (layerDrop) {
                QVector<QUuid> ids;
                for (const QByteArray &part : drop->mimeData()->data(QStringLiteral("application/x-compositor-layers")).split('\n')) {
                    const QUuid id(QString::fromUtf8(part)); if (!id.isNull() && !ids.contains(id)) ids.push_back(id);
                }
                if (!ids.isEmpty() && (newTab || target >= 0)) copyLayersToTab(ids, target, newTab);
            } else if (imageDrop) {
                const QImage image = qvariant_cast<QImage>(drop->mimeData()->imageData());
                if (!image.isNull()) {
                    if (newTab) installInNewTab(EditorSession(), tr("Untitled %1").arg(workspaceTabs_.size() + 1));
                    else if (target >= 0) activateTab(target);
                    if (session_.insertImage(image, tr("Dropped Image"))) syncDocumentViews();
                }
            } else {
                QStringList paths; for (const QUrl &url : drop->mimeData()->urls()) if (url.isLocalFile()) paths << url.toLocalFile();
                if (newTab) {
                    for (const QString &path : paths) {
                        if (path.endsWith(QStringLiteral(".comp"), Qt::CaseInsensitive)) { openProject(path); continue; }
                        if (!(workspaceTabs_.size() == 1 && !document_)) installInNewTab(EditorSession(), tr("Untitled %1").arg(workspaceTabs_.size() + 1));
                        importImageFiles({path});
                    }
                } else if (target >= 0) {
                    activateTab(target);
                    QStringList images;
                    for (const QString &path : paths) {
                        if (path.endsWith(QStringLiteral(".comp"), Qt::CaseInsensitive)) openProject(path); else images << path;
                    }
                    if (!images.isEmpty()) importImageFiles(images);
                }
            }
        }
        drop->setDropAction(Qt::CopyAction); drop->accept(); return true;
    }
    if (blendMode_ && watched == blendMode_->view()
        && (event->type() == QEvent::Hide || event->type() == QEvent::Close)) {
        canvas_->setBlendModePreview(std::nullopt, std::nullopt);
    }
    if ((watched == foregroundSwatch_ || watched == backgroundSwatch_) && event->type() == QEvent::MouseButtonPress) {
        openColorPicker(watched == backgroundSwatch_);
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

} // namespace compositor
