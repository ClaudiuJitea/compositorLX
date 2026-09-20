#include "ui/MainWindow.h"
#include "io/ImageExporter.h"

#include "io/ProjectReader.h"
#include "io/ProjectWriter.h"
#include "io/ImageImporter.h"
#include "rendering/LayerRenderer.h"
#include "rendering/TextLayout.h"
#include "rendering/RasterOperations.h"
#include "rendering/SubjectRemoval.h"
#include "ui/CanvasWidget.h"
#include "ui/LayerListModel.h"
#include "ui/ToolOptionsLayout.h"
#include "ui/EditorIcons.h"

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
#include <QHBoxLayout>
#include <QFormLayout>
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
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

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

class InlineTextEditor final : public QTextEdit {
public:
    explicit InlineTextEditor(QWidget *parent) : QTextEdit(parent)
    {
        setObjectName(QStringLiteral("inlineTextEditor"));
        setAcceptRichText(false);
        setFrameShape(QFrame::NoFrame);
        setAutoFillBackground(false); viewport()->setAutoFillBackground(false);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        parent->installEventFilter(this);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *grip = new QLabel(QStringLiteral("⌟"), this);
        grip->setObjectName(QStringLiteral("inlineTextGrip"));
        grip->setAlignment(Qt::AlignCenter);
        grip->setAttribute(Qt::WA_TransparentForMouseEvents);
        grip_ = grip;
    }
    std::function<void(bool)> finished;
    QRectF canvasBox;
    bool areaText = true;
    void syncCanvasGeometry()
    {
        if (canvasBox.isEmpty() || finishing_) return;
        auto *canvas = static_cast<CanvasWidget *>(parentWidget());
        const qreal zoom = canvas->zoom();
        const QRect geometry = canvas->widgetRectForDocumentRect(canvasBox).toAlignedRect();
        const QMargins margins(qRound(4 * zoom), qRound(3 * zoom), qRound(4 * zoom), qRound(3 * zoom));
        if (viewportMargins() != margins) setViewportMargins(margins);
        setGeometry(geometry);
        const int wrapWidth = std::max(1, qRound((canvasBox.width() - 8) * zoom));
        if (areaText && lineWrapColumnOrWidth() != wrapWidth) setLineWrapColumnOrWidth(wrapWidth);
    }
    void growPointText()
    {
        if (areaText || wasResized_ || finishing_) return;
        const qreal zoom = static_cast<CanvasWidget *>(parentWidget())->zoom();
        const QSizeF content = document()->size();
        canvasBox.setSize(QSizeF(std::max(40.0, document()->idealWidth() / zoom + 8),
                                std::max(20.0, content.height() / zoom + 6)));
        syncCanvasGeometry();
    }
    [[nodiscard]] bool wasResized() const { return wasResized_; }
    void finish(bool commit)
    {
        if (finishing_) return;
        finishing_ = true;
        // A replacement editor may be created before deferred deletion runs.
        setObjectName(QString()); hide();
        if (parentWidget()) parentWidget()->setFocus(Qt::OtherFocusReason);
        if (finished) finished(commit);
        deleteLater();
    }
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget() && (event->type() == QEvent::Resize || event->type() == QEvent::Paint)) syncCanvasGeometry();
        return QTextEdit::eventFilter(watched, event);
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) { finish(false); event->accept(); return; }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && event->modifiers().testFlag(Qt::ControlModifier)) { finish(true); event->accept(); return; }
        QTextEdit::keyPressEvent(event);
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (QRect(width() - 18, height() - 18, 18, 18).contains(event->position().toPoint())) {
            resizing_ = true; resizeStart_ = event->globalPosition(); originalSize_ = size(); setCursor(Qt::SizeFDiagCursor); event->accept(); return;
        }
        QTextEdit::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (resizing_) {
            const QPointF delta = event->globalPosition() - resizeStart_;
            resize(std::max(120, originalSize_.width() + qRound(delta.x())), std::max(48, originalSize_.height() + qRound(delta.y())));
            event->accept(); return;
        }
        setCursor(QRect(width() - 18, height() - 18, 18, 18).contains(event->position().toPoint()) ? Qt::SizeFDiagCursor : Qt::IBeamCursor);
        QTextEdit::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (resizing_) {
            resizing_ = false; wasResized_ = true; areaText = true;
            canvasBox.setSize(static_cast<CanvasWidget *>(parentWidget())->documentRectForWidgetRect(geometry()).size());
            setWordWrapMode(QTextOption::WordWrap); setLineWrapMode(QTextEdit::FixedPixelWidth); syncCanvasGeometry();
            setCursor(Qt::IBeamCursor); event->accept(); return;
        }
        QTextEdit::mouseReleaseEvent(event);
    }
    void resizeEvent(QResizeEvent *event) override
    {
        QTextEdit::resizeEvent(event);
        if (grip_) grip_->setGeometry(width() - 18, height() - 18, 16, 16);
    }
    void paintEvent(QPaintEvent *event) override
    {
        QTextEdit::paintEvent(event);
        QPainter painter(viewport()); painter.setPen(QPen(QColor(94, 167, 242), 1));
        painter.drawRect(viewport()->rect().adjusted(0, 0, -1, -1));
    }
private:
    QLabel *grip_ = nullptr;
    bool resizing_ = false;
    bool wasResized_ = false;
    bool finishing_ = false;
    QPointF resizeStart_;
    QSize originalSize_;
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

QMessageBox::StandardButton showMessage(QWidget *parent, const QString &title, const QString &text,
                                        const QString &detail = QString(),
                                        QMessageBox::StandardButtons buttons = QMessageBox::Ok,
                                        QMessageBox::StandardButton defaultButton = QMessageBox::NoButton)
{
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
        default: return QObject::tr("OK");
        }
    };
    if (defaultButton == QMessageBox::NoButton) {
        if (buttons.testFlag(QMessageBox::Save)) defaultButton = QMessageBox::Save;
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

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {220, 54}; }

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
        p->setPen(QColor(235, 237, 240));
        QFont mainFont = option.font; mainFont.setPixelSize(11); p->setFont(mainFont);
        p->drawText(QRect(textX, r.top() + 8, r.right() - textX - 7, 21), Qt::AlignVCenter | Qt::AlignLeft, index.data().toString());
        p->setPen(QColor(145, 148, 153));
        QFont smallFont = option.font; smallFont.setPixelSize(10); p->setFont(smallFont);
        p->drawText(QRect(textX, r.top() + 28, r.right() - textX - 7, 17), Qt::AlignVCenter | Qt::AlignLeft, index.data(Qt::UserRole).toString());
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

QDoubleSpinBox *numberField(QWidget *parent, const QString &prefix, double maximum = 1000000.0)
{
    auto *field = new QDoubleSpinBox(parent);
    field->setPrefix(prefix + QStringLiteral("  "));
    field->setRange(-maximum, maximum);
    field->setDecimals(0);
    field->setButtonSymbols(QAbstractSpinBox::NoButtons);
    field->setMinimumWidth(84);
    field->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    field->setKeyboardTracking(false);
    return field;
}

QJsonObject levelsAdjustment(const LevelsSettings &settings)
{
    QJsonArray ranges; for (const LevelRange &range : settings.ranges) ranges.append(QJsonObject{{QStringLiteral("black"), range.black}, {QStringLiteral("gamma"), range.gamma},
        {QStringLiteral("white"), range.white}, {QStringLiteral("outputBlack"), range.outputBlack}, {QStringLiteral("outputWhite"), range.outputWhite}});
    return {{QStringLiteral("kind"), QStringLiteral("Levels")}, {QStringLiteral("levels"), QJsonObject{{QStringLiteral("channel"), QStringLiteral("RGB")}, {QStringLiteral("ranges"), ranges}}}};
}

QJsonObject curvesAdjustment(const CurvesSettings &settings)
{
    QJsonArray channels; for (const auto &curve : settings.channels) { QJsonArray points; for (const CurvePoint &point : curve) points.append(QJsonObject{{QStringLiteral("x"), point.x}, {QStringLiteral("y"), point.y}}); channels.append(points); }
    return {{QStringLiteral("kind"), QStringLiteral("Curves")}, {QStringLiteral("curves"), QJsonObject{{QStringLiteral("channel"), QStringLiteral("RGB")}, {QStringLiteral("channels"), channels}}}};
}

QJsonObject hueAdjustment(const HueSaturationSettings &settings)
{
    static const QStringList names{QStringLiteral("Master"),QStringLiteral("Reds"),QStringLiteral("Yellows"),QStringLiteral("Greens"),QStringLiteral("Cyans"),QStringLiteral("Blues"),QStringLiteral("Magentas")};
    QJsonArray adjustments,bands;
    for(int i=0;i<names.size();++i){const RangeAdjustment &a=settings.adjustments[size_t(i)];adjustments.append(names[i]);adjustments.append(QJsonObject{{QStringLiteral("hue"),a.hue},{QStringLiteral("saturation"),a.saturation},{QStringLiteral("lightness"),a.lightness}});const HueBand &b=settings.bands[size_t(i)];bands.append(names[i]);bands.append(QJsonObject{{QStringLiteral("falloffStart"),b.falloffStart},{QStringLiteral("rangeStart"),b.rangeStart},{QStringLiteral("rangeEnd"),b.rangeEnd},{QStringLiteral("falloffEnd"),b.falloffEnd}});}
    const QJsonObject hsv{{QStringLiteral("range"),names.at(int(settings.range))},{QStringLiteral("colorize"),settings.colorize},{QStringLiteral("invertRange"),settings.invertRange},{QStringLiteral("adjustments"),adjustments},{QStringLiteral("bands"),bands}};
    const RangeAdjustment &master=settings.adjustments[size_t(ColorRange::Master)];
    return {{QStringLiteral("kind"),QStringLiteral("Hue/Saturation")},{QStringLiteral("hue"),master.hue},{QStringLiteral("saturation"),master.saturation},{QStringLiteral("lightness"),master.lightness},{QStringLiteral("colorize"),settings.colorize},{QStringLiteral("hsvSettings"),hsv}};
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
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
    auto *menuRestoreButton = new QToolButton(tabBar);
    menuRestoreButton->setText(QStringLiteral("≡"));
    menuRestoreButton->setToolTip(tr("Show or hide menu bar (Ctrl+Shift+M) · Right-click for all commands"));
    menuRestoreButton->setAccessibleName(tr("Show or hide menu bar"));
    menuRestoreButton->setObjectName(QStringLiteral("menuRestoreButton"));
    menuRestoreButton->setFixedSize(25, 25);
    tabLayout->addWidget(menuRestoreButton);
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
    auto *smearMode = new QComboBox(transformBar); smearMode->addItems({tr("Liquify"), tr("Blur"), tr("Smudge")}); smearMode->setVisible(false); smearMode->setFixedWidth(100);
    auto *cloneAligned=new QCheckBox(tr("Aligned"),transformBar);cloneAligned->setChecked(true);cloneAligned->setVisible(false);
    auto *cloneSample=new QComboBox(transformBar);cloneSample->addItems({tr("This Layer"),tr("All Layers")});cloneSample->setVisible(false);cloneSample->setFixedWidth(105);
    auto *healingMode=new QComboBox(transformBar);healingMode->addItems({tr("Content-Aware"),tr("Create Texture"),tr("Proximity Match")});healingMode->setVisible(false);healingMode->setFixedWidth(145);
    auto *brushMode = new QComboBox(transformBar); brushMode->addItems({tr("Paint"), tr("Erase")}); brushMode->setVisible(false);
    auto *shapeRadius = numberField(transformBar, QStringLiteral("Radius"), 5000); shapeRadius->setRange(0, 5000); shapeRadius->setVisible(false);
    auto *shapeKind = new QComboBox(transformBar); shapeKind->addItems({tr("Rectangle"), tr("Ellipse")}); shapeKind->setVisible(false);
    auto *textFont = new QFontComboBox(transformBar); textFont->setObjectName(QStringLiteral("textFont")); textFont->setFixedWidth(170); textFont->setEditable(true); textFont->setInsertPolicy(QComboBox::NoInsert); textFont->setMaxVisibleItems(16); textFont->setToolTip(tr("Type to search or open the font list")); textFont->setVisible(false);
    if (textFont->completer()) { textFont->completer()->setCaseSensitivity(Qt::CaseInsensitive); textFont->completer()->setCompletionMode(QCompleter::PopupCompletion); }
    if (textFont->lineEdit()) { textFont->lineEdit()->setPlaceholderText(tr("Search fonts")); textFont->lineEdit()->setClearButtonEnabled(false); }
    auto *textSize = new QSpinBox(transformBar); textSize->setObjectName(QStringLiteral("textSize")); textSize->setRange(4, 1000); textSize->setValue(48); textSize->setSuffix(tr(" px")); textSize->setFixedWidth(88); textSize->setVisible(false);
    auto *textBold = new QToolButton(transformBar); textBold->setObjectName(QStringLiteral("textBold")); textBold->setText(tr("B")); textBold->setCheckable(true); textBold->setToolTip(tr("Bold")); textBold->setFixedSize(30, 29); textBold->setVisible(false);
    auto *textItalic = new QToolButton(transformBar); textItalic->setObjectName(QStringLiteral("textItalic")); textItalic->setText(tr("I")); textItalic->setCheckable(true); textItalic->setToolTip(tr("Italic")); textItalic->setFixedSize(30, 29); textItalic->setVisible(false);
    auto *textUnderline = new QToolButton(transformBar); textUnderline->setObjectName(QStringLiteral("textUnderline")); textUnderline->setText(tr("U")); textUnderline->setCheckable(true); textUnderline->setToolTip(tr("Underline")); textUnderline->setFixedSize(30, 29); textUnderline->setVisible(false);
    auto *textAlignment = new QComboBox(transformBar); textAlignment->setObjectName(QStringLiteral("textAlignment")); textAlignment->addItems({tr("Left"), tr("Center"), tr("Right")}); textAlignment->setFixedWidth(82); textAlignment->setVisible(false);
    auto *textCancel = new QPushButton(tr("Cancel"), transformBar); textCancel->setObjectName(QStringLiteral("textCancel")); textCancel->setVisible(false);
    auto *textDone = new QPushButton(tr("Done"), transformBar); textDone->setObjectName(QStringLiteral("textDone")); textDone->setVisible(false);
    brushMode->setObjectName(QStringLiteral("brushMode")); shapeKind->setObjectName(QStringLiteral("shapeKind"));
    auto *gradientShape = new QComboBox(transformBar); gradientShape->addItems({tr("Linear"), tr("Radial")}); gradientShape->setVisible(false);
    auto *gradientStyle = new QComboBox(transformBar); gradientStyle->addItems({tr("Foreground to Transparent"), tr("Foreground to Background")}); gradientStyle->setVisible(false); gradientStyle->setFixedWidth(190);
    auto *gradientReverse = new QCheckBox(tr("Reverse"), transformBar); gradientReverse->setVisible(false);
    gradientOpacityField_ = numberField(transformBar, QStringLiteral("Opacity"), 100); gradientOpacityField_->setObjectName(QStringLiteral("gradientOpacity")); gradientOpacityField_->setRange(1, 100); gradientOpacityField_->setValue(100); gradientOpacityField_->setSuffix(QStringLiteral(" %")); gradientOpacityField_->setVisible(false);
    auto *cropRatio = new QComboBox(transformBar); cropRatio->addItems({tr("Unconstrained"), tr("Original Ratio"), tr("1:1"), tr("4:3"), tr("16:9")}); cropRatio->setVisible(false);
    auto *marqueeKind = new QComboBox(transformBar); marqueeKind->addItems({tr("Rectangle"), tr("Ellipse")}); marqueeKind->setVisible(false);
    auto *lassoKind = new QComboBox(transformBar); lassoKind->addItems({tr("Freehand"), tr("Polygonal")}); lassoKind->setVisible(false);
    auto *selectionMode = new QComboBox(transformBar); selectionMode->addItems({tr("New"), tr("Add"), tr("Subtract")}); selectionMode->setVisible(false);
    auto *selectionAntialias = new QCheckBox(tr("Anti-alias"), transformBar); selectionAntialias->setChecked(true); selectionAntialias->setVisible(false);
    auto *wandTolerance = new QSpinBox(transformBar); wandTolerance->setRange(0, 255); wandTolerance->setValue(32); wandTolerance->setPrefix(tr("Tolerance ")); wandTolerance->setVisible(false);
    auto *wandSampleSize = new QComboBox(transformBar); wandSampleSize->addItems({tr("Point Sample"), tr("3 by 3 Average"), tr("5 by 5 Average")}); wandSampleSize->setVisible(false);
    auto *wandSample = new QComboBox(transformBar); wandSample->addItems({tr("This Layer"), tr("All Layers")}); wandSample->setVisible(false);
    auto *wandContiguous = new QCheckBox(tr("Contiguous"), transformBar); wandContiguous->setChecked(true); wandContiguous->setVisible(false);
    auto *selectionAmount = new QSpinBox(transformBar); selectionAmount->setRange(1, 500); selectionAmount->setValue(1); selectionAmount->setSuffix(tr(" px")); selectionAmount->setVisible(false);
    auto *expandSelection = new QPushButton(tr("Expand"), transformBar); expandSelection->setVisible(false);
    auto *contractSelection = new QPushButton(tr("Contract"), transformBar); contractSelection->setVisible(false);
    marqueeKind->setObjectName(QStringLiteral("marqueeKind")); lassoKind->setObjectName(QStringLiteral("lassoKind"));
    selectionMode->setObjectName(QStringLiteral("selectionMode")); selectionAntialias->setObjectName(QStringLiteral("selectionAntialias"));
    wandTolerance->setObjectName(QStringLiteral("wandTolerance")); wandSampleSize->setObjectName(QStringLiteral("wandSampleSize"));
    wandSample->setObjectName(QStringLiteral("wandSample")); wandContiguous->setObjectName(QStringLiteral("wandContiguous"));
    brushSizeField_ = numberField(transformBar, QStringLiteral("Size"), 2000); brushSizeField_->setRange(1, 2000); brushSizeField_->setValue(brushDiameter_); brushSizeField_->setSuffix(QStringLiteral(" px")); brushSizeField_->setVisible(false);
    brushHardnessField_ = numberField(transformBar, QStringLiteral("Hardness"), 100); brushHardnessField_->setRange(0, 100); brushHardnessField_->setValue(100); brushHardnessField_->setSuffix(QStringLiteral(" %")); brushHardnessField_->setVisible(false);
    brushOpacityField_ = numberField(transformBar, QStringLiteral("Opacity"), 100); brushOpacityField_->setRange(1, 100); brushOpacityField_->setValue(100); brushOpacityField_->setSuffix(QStringLiteral(" %")); brushOpacityField_->setVisible(false);
    brushSizeField_->setObjectName(QStringLiteral("brushSize")); brushHardnessField_->setObjectName(QStringLiteral("brushHardness")); brushOpacityField_->setObjectName(QStringLiteral("brushOpacity"));
    auto *autoSelect = new QCheckBox(tr("Auto Select"), transformBar); autoSelect->setObjectName(QStringLiteral("transformAutoSelect"));
    showTransformControls_ = new QCheckBox(tr("Show Controls"), transformBar); showTransformControls_->setObjectName(QStringLiteral("transformShowControls")); showTransformControls_->setChecked(true);
    xField_ = numberField(transformBar, QStringLiteral("X"));
    yField_ = numberField(transformBar, QStringLiteral("Y"));
    widthField_ = numberField(transformBar, QStringLiteral("W"));
    heightField_ = numberField(transformBar, QStringLiteral("H"));
    xField_->setObjectName(QStringLiteral("transformX")); yField_->setObjectName(QStringLiteral("transformY"));
    widthField_->setObjectName(QStringLiteral("transformWidth")); heightField_->setObjectName(QStringLiteral("transformHeight"));
    xField_->setFixedWidth(78); yField_->setFixedWidth(78); widthField_->setFixedWidth(82); heightField_->setFixedWidth(82);
    auto *link = new QToolButton(transformBar); link->setObjectName(QStringLiteral("transformRatioLock")); link->setIcon(editorIcon(16)); link->setIconSize(QSize(18, 18)); link->setCheckable(true); link->setChecked(true); link->setToolTip(tr("Keep width and height proportional")); link->setFixedSize(29, 29);
    scaleField_ = numberField(transformBar, QStringLiteral("Scale"), 3200); scaleField_->setObjectName(QStringLiteral("transformScale")); scaleField_->setSuffix(QStringLiteral(" %")); scaleField_->setRange(0.1, 3200); scaleField_->setValue(100); scaleField_->setFixedWidth(105);
    rotationField_ = numberField(transformBar, QStringLiteral("°"), 360); rotationField_->setObjectName(QStringLiteral("transformRotation")); rotationField_->setRange(-360, 360); rotationField_->setFixedWidth(72); rotationField_->setToolTip(tr("Rotation"));
    sampling_ = new QComboBox(transformBar); sampling_->setObjectName(QStringLiteral("transformSampling")); sampling_->addItems({tr("High quality"), tr("Smooth"), tr("Nearest")}); sampling_->setFixedWidth(112); sampling_->setToolTip(tr("Resampling quality"));
    auto *flipH = new QPushButton(tr("Flip H"), transformBar); flipH->setObjectName(QStringLiteral("transformFlipHorizontal")); flipH->setFixedWidth(57); flipH->setToolTip(tr("Flip horizontally"));
    auto *flipV = new QPushButton(tr("Flip V"), transformBar); flipV->setObjectName(QStringLiteral("transformFlipVertical")); flipV->setFixedWidth(57); flipV->setToolTip(tr("Flip vertically"));
    transformCancel_ = new QPushButton(tr("Cancel"), transformBar); transformCancel_->setObjectName(QStringLiteral("transformCancel")); transformCancel_->setEnabled(false);
    transformApply_ = new QPushButton(tr("Apply"), transformBar); transformApply_->setObjectName(QStringLiteral("primaryButton")); transformApply_->setEnabled(false);
    transformLayout->addGroup({transformTitle});
    transformLayout->addGroup({autoSelect, showTransformControls_});
    transformLayout->addGroup({xField_, yField_});
    transformLayout->addGroup({widthField_, link, heightField_});
    transformLayout->addGroup({scaleField_, rotationField_});
    transformLayout->addGroup({flipH, flipV, sampling_});
    transformLayout->addGroup({brushMode, smearMode, healingMode, shapeKind, shapeRadius, cropRatio});
    transformLayout->addGroup({brushSizeField_, brushHardnessField_, brushOpacityField_});
    transformLayout->addGroup({cloneAligned, cloneSample});
    transformLayout->addGroup({textFont, textSize});
    textFont->setFixedWidth(210); textSize->setFixedWidth(100); textAlignment->setFixedWidth(104);
    transformLayout->addGroup({textBold, textItalic, textUnderline, textAlignment});
    transformLayout->addGroup({gradientShape, gradientStyle});
    transformLayout->addGroup({gradientOpacityField_, gradientReverse});
    transformLayout->addGroup({marqueeKind, lassoKind, selectionMode, selectionAntialias});
    transformLayout->addGroup({wandTolerance, wandContiguous});
    transformLayout->addGroup({wandSampleSize, wandSample});
    transformLayout->addGroup({selectionAmount, expandSelection, contractSelection});
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
        control->setFixedHeight(30);
        if (auto *field = qobject_cast<QDoubleSpinBox *>(control)) {
            field->setAccessibleName(field->prefix().trimmed());
        }
        // Fixed widths predate the themed padding and arrows. Let Qt measure
        // the complete control, including every option or the numeric range.
        if (auto *combo = qobject_cast<QComboBox *>(control)) combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        if (qobject_cast<QComboBox *>(control) || qobject_cast<QAbstractSpinBox *>(control) || qobject_cast<QPushButton *>(control)) {
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
            canvas_->setTool(tool);
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
    connect(brushSizeField_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) { brushDiameter_ = value; canvas_->setBrushDiameter(value); });
    connect(brushHardnessField_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) { brushHardness_ = value / 100.0; });
    connect(brushOpacityField_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) { brushOpacity_ = value / 100.0; });
    connect(brushMode, &QComboBox::currentIndexChanged, canvas_, [this](int index) { canvas_->setTool(index == 1 ? CanvasWidget::Tool::Eraser : CanvasWidget::Tool::Brush); });
    connect(shapeKind, &QComboBox::currentIndexChanged, canvas_, [this](int index) { canvas_->setEllipticalShape(index == 1); });
    connect(canvas_, &CanvasWidget::shapeKindChanged, shapeKind, [shapeKind](bool elliptical) { shapeKind->setCurrentIndex(elliptical ? 1 : 0); });
    connect(marqueeKind, &QComboBox::currentIndexChanged, canvas_, [this](int index) { canvas_->setMarqueeElliptical(index == 1); });
    connect(lassoKind, &QComboBox::currentIndexChanged, canvas_, [this](int index) { canvas_->setPolygonalLasso(index == 1); });
    connect(canvas_, &CanvasWidget::marqueeKindChanged, marqueeKind, [marqueeKind](bool elliptical) { marqueeKind->setCurrentIndex(elliptical ? 1 : 0); });
    connect(canvas_, &CanvasWidget::lassoKindChanged, lassoKind, [lassoKind](bool polygonal) { lassoKind->setCurrentIndex(polygonal ? 1 : 0); });
    connect(selectionMode, &QComboBox::currentIndexChanged, canvas_, &CanvasWidget::setSelectionMode);
    connect(selectionAntialias, &QCheckBox::toggled, canvas_, &CanvasWidget::setSelectionAntialiased);
    connect(expandSelection, &QPushButton::clicked, this, [this, selectionAmount] { if (session_.expandSelection(selectionAmount->value())) syncDocumentViews(false); });
    connect(contractSelection, &QPushButton::clicked, this, [this, selectionAmount] { if (session_.contractSelection(selectionAmount->value())) syncDocumentViews(false); });
    connect(cropRatio, &QComboBox::currentIndexChanged, this, [this, cropRatio](int index) {
        const double ratios[] = {0, document_ ? double(document_->canvasSize.width()) / document_->canvasSize.height() : 0, 1, 4.0/3.0, 16.0/9.0};
        canvas_->setCropRatio(ratios[std::clamp(index, 0, 4)]);
    });
    const std::array<QWidget *, 12> moveControls{autoSelect, showTransformControls_, xField_, yField_, widthField_, heightField_, link,
                                                scaleField_, rotationField_, sampling_, flipH, flipV};
    connect(canvas_, &CanvasWidget::toolChanged, this, [this, transformTitle, cropRatio, smearMode, cloneAligned, cloneSample,
            healingMode, brushMode, shapeKind, shapeRadius, textFont, textSize, textBold, textItalic, textUnderline, textAlignment, textCancel, textDone, gradientShape, gradientStyle, gradientReverse, marqueeKind, lassoKind,
            selectionMode, selectionAntialias, wandTolerance, wandSampleSize, wandSample, wandContiguous, expandSelection,
            contractSelection, selectionAmount, moveControls, toolButtons](CanvasWidget::Tool tool) {
        if (tool != CanvasWidget::Tool::Text)
            if (auto *editor = dynamic_cast<InlineTextEditor *>(canvas_->findChild<QTextEdit *>(QStringLiteral("inlineTextEditor"), Qt::FindDirectChildrenOnly))) editor->finish(true);
        if (tool != CanvasWidget::Tool::Move && transformOriginalDocument_) finishPersistentTransform(true);
        const bool brush = tool == CanvasWidget::Tool::Brush || tool == CanvasWidget::Tool::Eraser || tool == CanvasWidget::Tool::Healing
            || tool == CanvasWidget::Tool::Clone || tool == CanvasWidget::Tool::Blur;
        const bool move = tool == CanvasWidget::Tool::Move;
        const bool selection = tool == CanvasWidget::Tool::Marquee || tool == CanvasWidget::Tool::Lasso || tool == CanvasWidget::Tool::Wand;
        for (QWidget *control : moveControls) control->setVisible(move);
        brushSizeField_->setVisible(brush); brushHardnessField_->setVisible(brush); brushOpacityField_->setVisible(brush);
        brushMode->setVisible(tool == CanvasWidget::Tool::Brush || tool == CanvasWidget::Tool::Eraser);
        if (tool == CanvasWidget::Tool::Brush || tool == CanvasWidget::Tool::Eraser) { const QSignalBlocker blocker(brushMode); brushMode->setCurrentIndex(tool == CanvasWidget::Tool::Eraser ? 1 : 0); }
        cropRatio->setVisible(tool == CanvasWidget::Tool::Crop);
        smearMode->setVisible(tool == CanvasWidget::Tool::Blur);
        cloneAligned->setVisible(tool == CanvasWidget::Tool::Clone); cloneSample->setVisible(tool == CanvasWidget::Tool::Clone);
        healingMode->setVisible(tool == CanvasWidget::Tool::Healing);
        shapeKind->setVisible(tool == CanvasWidget::Tool::Shape); shapeRadius->setVisible(tool == CanvasWidget::Tool::Shape && shapeKind->currentIndex() == 0);
        const bool text = tool == CanvasWidget::Tool::Text;
        textFont->setVisible(text); textSize->setVisible(text); textBold->setVisible(text); textItalic->setVisible(text); textUnderline->setVisible(text); textAlignment->setVisible(text);
        textCancel->setVisible(text); textDone->setVisible(text);
        transformCancel_->setVisible(move || tool == CanvasWidget::Tool::Crop);
        transformApply_->setVisible(move || tool == CanvasWidget::Tool::Crop);
        gradientShape->setVisible(tool == CanvasWidget::Tool::Gradient); gradientStyle->setVisible(tool == CanvasWidget::Tool::Gradient);
        gradientReverse->setVisible(tool == CanvasWidget::Tool::Gradient); gradientOpacityField_->setVisible(tool == CanvasWidget::Tool::Gradient);
        marqueeKind->setVisible(tool == CanvasWidget::Tool::Marquee); lassoKind->setVisible(tool == CanvasWidget::Tool::Lasso);
        selectionMode->setVisible(selection); selectionAntialias->setVisible(tool == CanvasWidget::Tool::Lasso || tool == CanvasWidget::Tool::Wand
                                                                             || (tool == CanvasWidget::Tool::Marquee && marqueeKind->currentIndex() == 1));
        wandTolerance->setVisible(tool == CanvasWidget::Tool::Wand); wandSampleSize->setVisible(tool == CanvasWidget::Tool::Wand);
        wandSample->setVisible(tool == CanvasWidget::Tool::Wand); wandContiguous->setVisible(tool == CanvasWidget::Tool::Wand);
        expandSelection->setVisible(selection); contractSelection->setVisible(selection); selectionAmount->setVisible(selection);
        if (brush) transformTitle->setText(tool == CanvasWidget::Tool::Healing ? tr("Spot Healing") : tool == CanvasWidget::Tool::Clone ? tr("Clone Stamp")
            : tool == CanvasWidget::Tool::Blur ? tr("Smear") : tool == CanvasWidget::Tool::Eraser ? tr("Eraser") : tr("Brush"));
        else if (selection) transformTitle->setText(tool == CanvasWidget::Tool::Marquee ? tr("Marquee") : tool == CanvasWidget::Tool::Lasso ? tr("Lasso") : tr("Magic Wand"));
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
    connect(marqueeKind, &QComboBox::currentIndexChanged, this, [this, selectionAntialias, marqueeKind](int) {
        selectionAntialias->setVisible(canvas_->tool() == CanvasWidget::Tool::Marquee && marqueeKind->currentIndex() == 1);
    });
    connect(shapeKind, &QComboBox::currentIndexChanged, this, [this, shapeRadius, shapeKind](int) {
        shapeRadius->setVisible(canvas_->tool() == CanvasWidget::Tool::Shape && shapeKind->currentIndex() == 0);
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
    workspaceLayout->addWidget(canvasStack_, 1);

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
    blendMode_ = new QComboBox(appearance); blendMode_->addItems({tr("Normal"), tr("Multiply"), tr("Screen"), tr("Overlay"), tr("Darken"), tr("Lighten"), tr("Difference"),tr("Color Dodge"),tr("Color Burn"),tr("Hue"),tr("Saturation"),tr("Color"),tr("Luminosity")});
    blendMode_->setObjectName(QStringLiteral("blendMode"));
    blendMode_->view()->installEventFilter(this);
    blendRow->addWidget(blendLabel); blendRow->addWidget(blendMode_, 1); appearanceLayout->addLayout(blendRow);
    auto *opacityRow = new QHBoxLayout; auto *opacityLabel = new QLabel(tr("Opacity"), appearance);
    opacitySlider_ = new SnapSlider(Qt::Horizontal, appearance); opacitySlider_->setObjectName(QStringLiteral("layerOpacity")); opacitySlider_->setRange(0, 100); opacitySlider_->setValue(100);
    auto *opacityValue = new QLabel(QStringLiteral("100  %"), appearance); opacityValue->setMinimumWidth(43); opacityValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    opacityRow->addWidget(opacityLabel); opacityRow->addWidget(opacitySlider_, 1); opacityRow->addWidget(opacityValue); appearanceLayout->addLayout(opacityRow);
    inspectorLayout->addWidget(appearance);
    layerView_ = new QListView(inspector);
    layerView_->setObjectName(QStringLiteral("layerList"));
    layerView_->setModel(layerModel_);
    layerView_->setItemDelegate(new LayerDelegate(layerView_));
    layerView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    layerView_->setDragEnabled(true); layerView_->setAcceptDrops(true); layerView_->setDropIndicatorShown(true);
    layerView_->setDragDropMode(QAbstractItemView::DragDrop); layerView_->setDefaultDropAction(Qt::MoveAction);
    layerView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layerView_->viewport()->setMouseTracking(true);
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
        const auto found = std::find_if(document_->layers.cbegin(), document_->layers.cend(), [&](const Layer &candidate){ return candidate.id == *id; });
        if (found == document_->layers.cend()) return;
        const Layer &layer = *found;
        const int depth = index.data(Qt::UserRole + 1).toInt();
        const int thumbnail = 34 + std::min(depth, 8) * 18;
        const int x = layerView_->viewport()->mapFromGlobal(QCursor::pos()).x();
        if (layer.group && x >= thumbnail - 9 && x < thumbnail + 8) { layerModel_->toggleExpanded(layer.id); return; }
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
    connect(layerView_, &QListView::doubleClicked, this, [this](const QModelIndex &) {
        const Layer *layer = session_.activeLayer(); if (!layer) return;
        if (layer->adjustment.isEmpty()) { layerView_->edit(layerView_->currentIndex()); return; }
        const QString kind = layer->adjustment.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("Hue/Saturation")) hueSaturationDialog(); else if (kind == QStringLiteral("Levels")) levelsDialog();
        else if (kind == QStringLiteral("Curves")) curvesDialog(); else if (kind == QStringLiteral("Exposure")) exposureDialog();
        else if (kind == QStringLiteral("Gradient Map")) gradientMapDialog(); else if (kind == QStringLiteral("Grain")) grainDialog();
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
        if (link->isChecked() && (sender() == widthField_ || sender() == heightField_)) {
            QRectF current = session_.selectedLayerIds().size() > 1 ? session_.selectedLayersBounds() : QRectF();
            if (current.isEmpty()) {
                if (const Layer *layer = session_.activeLayer()) current = QRectF(layer->transform.origin, layer->transform.size);
            }
            if (current.width() > 0 && current.height() > 0) {
                if (sender() == widthField_) {
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
    connect(sampling_, &QComboBox::currentIndexChanged, this, [this](int index) {
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
        currentTab_ = tabs_->currentIndex();
        session_ = workspaceTabs_.at(currentTab_);
        syncDocumentViews();
    });
    connect(addLayer, &QToolButton::clicked, this, [this] { session_.addBlankLayer(); syncDocumentViews(); });
    connect(addGroup, &QToolButton::clicked, this, [this] { session_.addGroup(); syncDocumentViews(); });
    connect(duplicate, &QToolButton::clicked, this, [this] { session_.duplicateActiveLayer(); syncDocumentViews(); });
    connect(addMask, &QToolButton::clicked, this, [this] { if (session_.addLayerMask()) syncDocumentViews(); });
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
    connect(canvas_, &CanvasWidget::cropRequested, this, [this](const QRect &rect) {
        if (session_.crop(rect)) syncDocumentViews();
    });
    connect(canvas_, &CanvasWidget::brushStrokeStarted, this, [this](const QPointF &point, bool erasing) {
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
    connect(canvas_, &CanvasWidget::cloneStrokeStarted, this, [this,cloneAligned,cloneSample](const QPointF &point) {
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
    connect(canvas_, &CanvasWidget::healingStrokeStarted, this, [this,healingMode](const QPointF &point) {
        session_.beginHealingStroke(point, brushDiameter_, brushHardness_, brushOpacity_, healingMode->currentIndex(), QRandomGenerator::global()->generate()); canvas_->invalidateDocument();
    });
    connect(canvas_, &CanvasWidget::blurStrokeStarted, this, [this, smearMode](const QPointF &point) {
        if (smearMode->currentIndex() == 1) session_.beginBlurStroke(point, brushDiameter_, brushHardness_, brushOpacity_);
        else session_.beginWarpStroke(point, smearMode->currentIndex() == 0 ? 0 : 1, brushDiameter_, brushHardness_, brushOpacity_);
        canvas_->invalidateDocument();
    });
    connect(canvas_, &CanvasWidget::gradientRequested, this, [this,gradientShape,gradientStyle,gradientReverse](const QPointF &start, const QPointF &end) {
        previewGradient(start, end, gradientShape->currentIndex() == 1, gradientStyle->currentIndex() == 0,
                        gradientReverse->isChecked(), gradientOpacityField_->value() / 100.0);
    });
    connect(canvas_, &CanvasWidget::gradientCommitRequested, this, [this] { finishGradient(true); });
    connect(canvas_, &CanvasWidget::gradientCancelRequested, this, [this] { finishGradient(false); });
    connect(gradientShape, &QComboBox::currentIndexChanged, canvas_, &CanvasWidget::refreshPendingGradient);
    connect(gradientStyle, &QComboBox::currentIndexChanged, canvas_, &CanvasWidget::refreshPendingGradient);
    connect(gradientReverse, &QCheckBox::toggled, canvas_, &CanvasWidget::refreshPendingGradient);
    connect(gradientOpacityField_, &QDoubleSpinBox::valueChanged, canvas_, &CanvasWidget::refreshPendingGradient);
    connect(canvas_, &CanvasWidget::shapeRequested, this, [this](const QRectF &rect, bool ellipse, double cornerRadius) {
        if (session_.addShape(rect, foregroundColor_, Qt::transparent, 0, ellipse, cornerRadius)) syncDocumentViews();
    });
    const auto applyInlineTextFormat = [this, textFont, textSize, textBold, textItalic, textUnderline, textAlignment] {
        auto *editor = dynamic_cast<InlineTextEditor *>(canvas_->findChild<QTextEdit *>(QStringLiteral("inlineTextEditor"), Qt::FindDirectChildrenOnly));
        if (!editor) return;
        QFont font = textFont->currentFont(); font.setPixelSize(std::max(1, qRound(textSize->value() * canvas_->zoom()))); font.setBold(textBold->isChecked()); font.setItalic(textItalic->isChecked()); font.setUnderline(textUnderline->isChecked());
        const QTextCursor originalCursor = editor->textCursor();
        formatTextDocument(*editor->document(), font, foregroundColor_, textAlignment->currentIndex(), editor->areaText);
        editor->setTextCursor(originalCursor);
        editor->setCurrentFont(font);
        editor->setTextColor(foregroundColor_);
        editor->growPointText();
    };
    connect(textFont, &QFontComboBox::currentFontChanged, this, [applyInlineTextFormat](const QFont &) { applyInlineTextFormat(); });
    connect(textSize, &QSpinBox::valueChanged, this, [applyInlineTextFormat](int) { applyInlineTextFormat(); });
    connect(textBold, &QToolButton::toggled, this, [applyInlineTextFormat](bool) { applyInlineTextFormat(); });
    connect(textItalic, &QToolButton::toggled, this, [applyInlineTextFormat](bool) { applyInlineTextFormat(); });
    connect(textUnderline, &QToolButton::toggled, this, [applyInlineTextFormat](bool) { applyInlineTextFormat(); });
    connect(textAlignment, &QComboBox::currentIndexChanged, this, [applyInlineTextFormat](int) { applyInlineTextFormat(); });
    const auto beginInlineText = [this, textFont, textSize, textBold, textItalic, textUnderline, textAlignment, textDone, textCancel, applyInlineTextFormat]
        (const QRectF &box, const QString &initialText, bool areaText, const std::optional<QUuid> &layerId, const QColor &color) {
        if (auto *existing = dynamic_cast<InlineTextEditor *>(canvas_->findChild<QTextEdit *>(QStringLiteral("inlineTextEditor"), Qt::FindDirectChildrenOnly))) existing->finish(true);
        auto *editor = new InlineTextEditor(canvas_);
        editor->canvasBox = box; editor->areaText = areaText;
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
                        textItalic->isChecked(), textUnderline->isChecked(), textAlignment->currentIndex(), foregroundColor_, fixedBox)
                    : session_.addText(editor->toPlainText(), documentBox, textFont->currentFont().family(), textSize->value(), textBold->isChecked(),
                        textItalic->isChecked(), textUnderline->isChecked(), textAlignment->currentIndex(), foregroundColor_, fixedBox);
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
    connect(canvas_, &CanvasWidget::textLayerEditRequested, this, [this, textFont, textSize, textBold, textItalic, textUnderline, textAlignment, beginInlineText](const QUuid &id) {
        // Commit using the previous layer's controls before loading the next layer's style.
        if (auto *existing = dynamic_cast<InlineTextEditor *>(canvas_->findChild<QTextEdit *>(QStringLiteral("inlineTextEditor"), Qt::FindDirectChildrenOnly))) existing->finish(true);
        if (!document_) return;
        auto it = std::find_if(document_->layers.begin(), document_->layers.end(), [&id](const Layer &layer) { return layer.id == id; });
        if (it == document_->layers.end()) return;
        const QJsonObject metadata = it->shape;
        const QSignalBlocker fontBlock(textFont), sizeBlock(textSize), boldBlock(textBold), italicBlock(textItalic), underlineBlock(textUnderline), alignmentBlock(textAlignment);
        textFont->setCurrentFont(QFont(metadata.value(QStringLiteral("fontFamily")).toString()));
        textSize->setValue(metadata.value(QStringLiteral("pixelSize")).toInt(48));
        textBold->setChecked(metadata.value(QStringLiteral("bold")).toBool()); textItalic->setChecked(metadata.value(QStringLiteral("italic")).toBool());
        textUnderline->setChecked(metadata.value(QStringLiteral("underline")).toBool()); textAlignment->setCurrentIndex(std::clamp(metadata.value(QStringLiteral("alignment")).toInt(), 0, 2));
        const QColor color(metadata.value(QStringLiteral("fill")).toString());
        if (color.isValid()) { foregroundColor_ = color; canvas_->setPaletteForeground(color); foregroundSwatch_->setStyleSheet(QStringLiteral("background:%1;border:1px solid white;border-radius:4px;").arg(color.name(QColor::HexRgb))); }
        const QRectF box(it->transform.origin, it->transform.size); const QString text = metadata.value(QStringLiteral("text")).toString(); const bool areaText = metadata.value(QStringLiteral("areaText")).toBool();
        session_.selectLayer(id); canvas_->setTextEditingLayer(id);
        beginInlineText(box, text, areaText, id, color.isValid() ? color : foregroundColor_);
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
    auto *quickFileMenu = new QMenu(menuRestoreButton);
    quickFileMenu->setObjectName(QStringLiteral("quickFileMenu"));
    quickFileMenu->setTitle(tr("Application menu"));
    if (QAction *open = findChild<QAction *>(QStringLiteral("commandOpen"))) quickFileMenu->addAction(open);
    quickFileMenu->addSeparator();
    if (QAction *save = findChild<QAction *>(QStringLiteral("commandSave"))) quickFileMenu->addAction(save);
    if (QAction *saveAs = findChild<QAction *>(QStringLiteral("commandSaveAs"))) quickFileMenu->addAction(saveAs);
    auto *quickExportMenu = quickFileMenu->addMenu(tr("Export"));
    quickExportMenu->setObjectName(QStringLiteral("quickExportMenu"));
    if (QAction *png = findChild<QAction *>(QStringLiteral("commandExportPng"))) quickExportMenu->addAction(png);
    if (QAction *jpeg = findChild<QAction *>(QStringLiteral("commandExportJpeg"))) quickExportMenu->addAction(jpeg);
    quickFileMenu->addSeparator();
    // Reuse the same menus and actions so shortcuts, enabled states, and dynamic
    // command labels remain identical with the menu bar shown or hidden.
    for (QAction *category : menuBar()->actions())
        if (QMenu *menu = category->menu()) quickFileMenu->addMenu(menu);
    quickFileMenu->addSeparator();
    if (QAction *showMenuBar = findChild<QAction *>(QStringLiteral("showMenuBar"))) quickFileMenu->addAction(showMenuBar);
    if (QAction *showMenuBar = findChild<QAction *>(QStringLiteral("showMenuBar"))) {
        connect(menuRestoreButton, &QToolButton::clicked, showMenuBar, [showMenuBar] { showMenuBar->toggle(); });
    }
    menuRestoreButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(menuRestoreButton, &QWidget::customContextMenuRequested, this, [menuRestoreButton, quickFileMenu] {
        quickFileMenu->popup(menuRestoreButton->mapToGlobal(QPoint(0, menuRestoreButton->height() + 4)));
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
    connect(save, &QAction::triggered, this, &MainWindow::saveProject);
    auto *saveAs = file->addAction(tr("Save &As…"));
    saveAs->setObjectName(QStringLiteral("commandSaveAs"));
    saveAs->setShortcut(QKeySequence::SaveAs);
    connect(saveAs, &QAction::triggered, this, &MainWindow::saveProjectAs);
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
    auto *toggleMask = maskMenu->addAction(tr("Enable/Disable"));
    auto *linkMask = maskMenu->addAction(tr("Link/Unlink from Layer"));
    auto *invertMask = maskMenu->addAction(tr("Invert Mask"));
    auto *loadMask = maskMenu->addAction(tr("Load as Selection"));
    auto *deleteMask = maskMenu->addAction(tr("Delete Mask"));
    connect(revealMask, &QAction::triggered, this, [this] { if (session_.addLayerMask(true, false)) syncDocumentViews(); });
    connect(hideMask, &QAction::triggered, this, [this] { if (session_.addLayerMask(false, false)) syncDocumentViews(); });
    connect(toggleMask, &QAction::triggered, this, [this] { if (session_.toggleLayerMask()) syncDocumentViews(); });
    connect(linkMask, &QAction::triggered, this, [this] { if (session_.toggleMaskLink()) syncDocumentViews(); });
    connect(invertMask, &QAction::triggered, this, [this] { if (session_.invertLayerMask()) syncDocumentViews(); });
    connect(loadMask, &QAction::triggered, this, [this] { if (session_.loadMaskAsSelection()) syncDocumentViews(false); });
    connect(deleteMask, &QAction::triggered, this, [this] { if (session_.deleteLayerMask()) syncDocumentViews(); });
    auto *adjustments = new QMenu(tr("New Adjustment Layer"), this);
    adjustments->menuAction()->setObjectName(QStringLiteral("commandNewAdjustment"));
    const QStringList adjustmentKinds{tr("Hue/Saturation"), tr("Levels"), tr("Curves"), tr("Exposure"), tr("Gradient Map"), tr("Grain")};
    for (const QString &kind : adjustmentKinds) connect(adjustments->addAction(kind), &QAction::triggered, this, [this, kind] {
        QJsonObject settings;
        if (kind == QStringLiteral("Gradient Map")) {
            const auto color = [](const QColor &value) { return QJsonObject{{QStringLiteral("red"), value.redF()},
                {QStringLiteral("green"), value.greenF()}, {QStringLiteral("blue"), value.blueF()}}; };
            settings.insert(QStringLiteral("gradientMapSettings"), QJsonObject{{QStringLiteral("shadows"), color(foregroundColor_)},
                {QStringLiteral("highlights"), color(backgroundColor_)}, {QStringLiteral("reversed"), false}});
        }
        if (!session_.addAdjustment(kind, settings)) return;
        syncDocumentViews();
        if (kind == QStringLiteral("Hue/Saturation")) hueSaturationDialog(); else if (kind == QStringLiteral("Levels")) levelsDialog();
        else if (kind == QStringLiteral("Curves")) curvesDialog(); else if (kind == QStringLiteral("Exposure")) exposureDialog();
        else if (kind == QStringLiteral("Gradient Map")) gradientMapDialog(); else grainDialog();
    });
    layerMenuActions->addMenu(adjustments);
    auto *editAdjustment = layerMenuActions->addAction(tr("Edit Adjustment…"));
    editAdjustment->setObjectName(QStringLiteral("commandEditAdjustment"));
    connect(editAdjustment, &QAction::triggered, this, [this] {
        const Layer *layer = session_.activeLayer(); if (!layer || layer->adjustment.isEmpty()) return;
        const QString kind = layer->adjustment.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("Hue/Saturation")) hueSaturationDialog(); else if (kind == QStringLiteral("Levels")) levelsDialog();
        else if (kind == QStringLiteral("Curves")) curvesDialog(); else if (kind == QStringLiteral("Exposure")) exposureDialog();
        else if (kind == QStringLiteral("Gradient Map")) gradientMapDialog(); else if (kind == QStringLiteral("Grain")) grainDialog();
    });
    layerMenuActions->addSeparator();
    layerMenuActions->addAction(transformSelection);
    layerMenuButton_->setMenu(adjustments);

    auto *select = menuBar()->addMenu(tr("&Select"));
    auto *selectAll = select->addAction(tr("Select &All")); selectAll->setShortcut(QKeySequence::SelectAll);
    auto *deselect = select->addAction(tr("&Deselect")); deselect->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    auto *inverse = select->addAction(tr("&Inverse")); inverse->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I));
    selectAll->setObjectName(QStringLiteral("commandSelectAll")); deselect->setObjectName(QStringLiteral("commandDeselect")); inverse->setObjectName(QStringLiteral("commandInverseSelection"));
    connect(selectAll, &QAction::triggered, this, [this] { if (dispatchTextEditCommand(TextEditCommand::SelectAll)) return; session_.selectAll(); syncDocumentViews(false); });
    connect(deselect, &QAction::triggered, this, [this] { session_.deselect(); syncDocumentViews(false); });
    connect(inverse, &QAction::triggered, this, [this] { session_.invertSelection(); syncDocumentViews(false); });
    auto *layerPixels = select->addAction(tr("Layer's Pixels"));
    auto *maskPixels = select->addAction(tr("Mask's Black Areas"));
    connect(layerPixels, &QAction::triggered, this, [this] { if (session_.loadLayerAsSelection()) syncDocumentViews(false); });
    connect(maskPixels, &QAction::triggered, this, [this] { if (session_.loadMaskAsSelection()) syncDocumentViews(false); });
    select->addSeparator();
    auto *expand = select->addAction(tr("Expand…"));
    auto *contract = select->addAction(tr("Contract…"));
    connect(expand, &QAction::triggered, this, [this] {
        bool ok = false; const int amount = QInputDialog::getInt(this, tr("Expand Selection"), tr("Pixels"), 1, 1, 500, 1, &ok);
        if (ok && session_.expandSelection(amount)) syncDocumentViews(false);
    });
    connect(contract, &QAction::triggered, this, [this] {
        bool ok = false; const int amount = QInputDialog::getInt(this, tr("Contract Selection"), tr("Pixels"), 1, 1, 500, 1, &ok);
        if (ok && session_.contractSelection(amount)) syncDocumentViews(false);
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
    connect(imageSize, &QAction::triggered, this, &MainWindow::resizeImageDialog);
    connect(canvasSize, &QAction::triggered, this, &MainWindow::resizeCanvasDialog);
    connect(cropAction, &QAction::triggered, this, &MainWindow::cropDialog);
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
    auto *noise = image->addAction(tr("Add Noise…"));
    connect(noise, &QAction::triggered, this, [this] {
        Layer *active=session_.activeLayer();if(!active||active->image.isNull())return;const QUuid target=active->id;const Layer original=*active;const quint32 seed=QRandomGenerator::global()->generate();
        QDialog dialog(this);dialog.setWindowTitle(tr("Add Noise"));auto *layout=new QVBoxLayout(&dialog);auto *form=new QFormLayout;auto *amount=new QDoubleSpinBox(&dialog);amount->setRange(.1,400);amount->setValue(10);amount->setSuffix(tr(" %"));auto *distribution=new QComboBox(&dialog);distribution->addItems({tr("Uniform"),tr("Gaussian")});auto *monochromatic=new QCheckBox(tr("Monochromatic"),&dialog);form->addRow(tr("Amount"),sliderField(amount,true));form->addRow(tr("Distribution"),distribution);layout->addLayout(form);layout->addWidget(monochromatic);auto *previewEnabled=new QCheckBox(tr("Preview"),&dialog);previewEnabled->setChecked(true);previewEnabled->setObjectName(QStringLiteral("filterPreview"));layout->addWidget(previewEnabled);
        session_.beginEdit(QStringLiteral("Add Noise"));const auto preview=[this,target,original,seed,amount,distribution,monochromatic,previewEnabled]{for(Layer &layer:document_->layers)if(layer.id==target){layer=original;break;}if(previewEnabled->isChecked())session_.addNoiseToActiveLayer(float(amount->value()),distribution->currentIndex()==1,monochromatic->isChecked(),seed);canvas_->invalidateDocument();};connect(amount,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,preview);connect(distribution,&QComboBox::currentIndexChanged,&dialog,preview);connect(monochromatic,&QCheckBox::toggled,&dialog,preview);connect(previewEnabled,&QCheckBox::toggled,&dialog,preview);
        auto *buttons=new QDialogButtonBox(QDialogButtonBox::Cancel|QDialogButtonBox::Ok,&dialog);connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);layout->addWidget(buttons);if(runFloatingDialog(dialog)==QDialog::Accepted){previewEnabled->setChecked(true);preview();}else for(Layer &layer:document_->layers)if(layer.id==target){layer=original;break;}session_.endEdit();syncDocumentViews();
    });
    auto *lens = image->addAction(tr("Lens Correction…"));
    connect(lens, &QAction::triggered, this, [this] {
        Layer *active=session_.activeLayer();if(!active||active->image.isNull())return;const QUuid target=active->id;const Layer original=*active;
        QDialog dialog(this);dialog.setWindowTitle(tr("Lens Correction"));auto *layout=new QVBoxLayout(&dialog);auto *amount=new QDoubleSpinBox(&dialog);amount->setRange(-100,100);amount->setValue(0);amount->setDecimals(0);layout->addWidget(new QLabel(tr("Remove Distortion"),&dialog));layout->addWidget(sliderField(amount));auto *hint=new QLabel(tr("Positive straightens barrel distortion; negative straightens pincushion distortion."),&dialog);hint->setWordWrap(true);layout->addWidget(hint);auto *previewEnabled=new QCheckBox(tr("Preview"),&dialog);previewEnabled->setChecked(true);previewEnabled->setObjectName(QStringLiteral("filterPreview"));layout->addWidget(previewEnabled);
        session_.beginEdit(QStringLiteral("Lens Correction"));const auto preview=[this,target,original,amount,previewEnabled]{for(Layer &layer:document_->layers)if(layer.id==target){layer=original;break;}if(previewEnabled->isChecked())session_.distortActiveLayer(amount->value());canvas_->invalidateDocument();};connect(amount,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,preview);connect(previewEnabled,&QCheckBox::toggled,&dialog,preview);auto *buttons=new QDialogButtonBox(QDialogButtonBox::Cancel|QDialogButtonBox::Ok,&dialog);connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);layout->addWidget(buttons);if(runFloatingDialog(dialog)==QDialog::Accepted){previewEnabled->setChecked(true);preview();}else for(Layer &layer:document_->layers)if(layer.id==target){layer=original;break;}session_.endEdit();syncDocumentViews();
    });

    auto *filterMenu = menuBar()->addMenu(tr("&Filter"));
    auto *gaussianBlur = filterMenu->addAction(tr("Gaussian Blur…"));
    auto *motionBlur = filterMenu->addAction(tr("Motion Blur…"));
    auto *removeBackground = filterMenu->addAction(tr("Remove Background…"));
    auto *contentFill = filterMenu->addAction(tr("Content-Aware Fill"));
    contentFill->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Delete));
    connect(gaussianBlur, &QAction::triggered, this, &MainWindow::gaussianBlurDialog);
    connect(motionBlur, &QAction::triggered, this, &MainWindow::motionBlurDialog);
    connect(removeBackground, &QAction::triggered, this, &MainWindow::removeBackgroundDialog);
    connect(contentFill, &QAction::triggered, this, [this] {
        if (!session_.contentAwareFill()) showMessage(this, tr("Content-Aware Fill"), tr("Not enough unselected opaque pixels surround this selection."));
        else syncDocumentViews();
    });
    filterMenu->addAction(noise); filterMenu->addAction(lens);

    auto *brushTool = new QAction(this); brushTool->setShortcut(QKeySequence(Qt::Key_B));
    auto *moveTool = new QAction(this); moveTool->setShortcut(QKeySequence(Qt::Key_V));
    auto *wandTool = new QAction(this); wandTool->setShortcut(QKeySequence(Qt::Key_W));
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
    connect(blurToolAction, &QAction::triggered, this, [this] { if (!textEditorHasFocus()) canvas_->setTool(CanvasWidget::Tool::Blur); });
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

    auto *help = menuBar()->addMenu(tr("&Help"));
    connect(help->addAction(tr("Check for Updates…")), &QAction::triggered, this, &MainWindow::checkForUpdates);
    connect(help->addAction(tr("About CompositorLX")), &QAction::triggered, this, &MainWindow::showAbout);

    addAction(newAction); addAction(open); addAction(importAction); addAction(save); addAction(saveAs); addAction(exportAction); addAction(exportJpegAction); addAction(closeProject); addAction(undo); addAction(redo);
    addAction(cut); addAction(copy); addAction(copyMerged); addAction(paste);
    addAction(duplicate); addAction(remove); addAction(newLayerAction); addAction(selectAll); addAction(deselect); addAction(inverse);
    addAction(levels); addAction(hueSaturation); addAction(curves); addAction(imageSize); addAction(canvasSize); addAction(invert);
    addAction(brushTool); addAction(eraserTool); addAction(marqueeTool); addAction(toggleMarquee); addAction(lassoTool); addAction(toggleLasso);
    addAction(moveTool); addAction(wandTool); addAction(cropTool); addAction(handTool); addAction(zoomTool); addAction(idleTool);
    addAction(gradientTool); addAction(shapeTool); addAction(toggleShape); addAction(textTool);
    addAction(cloneTool);
    addAction(healingTool);
    addAction(blurToolAction);
    addAction(eyedropperTool);
    addAction(smallerBrush); addAction(largerBrush); addAction(softerBrush); addAction(harderBrush);
    addAction(nextBlend); addAction(previousBlend);
    addAction(swapColors); addAction(resetColors);
    addAction(showMenuBar);
    menuBar()->setVisible(showMenuBar->isChecked());
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
    }
    addAction(contentFill); addAction(transformSelection); addAction(fillForeground); addAction(fillBackground);
    addAction(groupLayers); addAction(moveUp); addAction(moveDown);
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
    form->addRow(tr("Channel"), channel); form->addRow(tr("Input black"), black); form->addRow(tr("Gamma"), gamma); form->addRow(tr("Input white"), white);
    form->addRow(tr("Output black"), outputBlack); form->addRow(tr("Output white"), outputWhite); layout->addLayout(form);
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
            else for (Layer &layer : document_->layers) if (layer.id == target) { layer = originalLayer; break; }
        } else if (live) session_.previewAdjustment(target, levelsAdjustment(settings));
        else { for (Layer &layer : document_->layers) if (layer.id == target) { layer = originalLayer; break; } session_.applyLevels(settings); }
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
    else for (Layer &layer : document_->layers) if (layer.id == target) { layer = originalLayer; break; }
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
    const auto preview = [this, live, target, value, originalLayer, stops, offset, gamma, previewEnabled] { if (!previewEnabled->isChecked()) { if (live) session_.previewAdjustment(target, originalLayer.adjustment); else for (Layer &layer : document_->layers) if (layer.id == target) { layer=originalLayer;break; } } else if (live) session_.previewAdjustment(target, value()); else { for (Layer &layer : document_->layers) if (layer.id == target) { layer=originalLayer;break; } session_.applyExposure(stops->value(),offset->value(),gamma->value()); } canvas_->invalidateDocument(); };
    connect(stops, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview); connect(offset, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview); connect(gamma, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, preview);
    form->addRow(tr("Exposure (stops)"), sliderField(stops)); form->addRow(tr("Offset"), sliderField(offset)); form->addRow(tr("Gamma"), sliderField(gamma, true)); layout->addLayout(form); layout->addWidget(previewEnabled); connect(previewEnabled, &QCheckBox::toggled, &dialog, preview);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
    const int result = runFloatingDialog(dialog);
    if(result==QDialog::Accepted){previewEnabled->setChecked(true);preview();}else if(live)session_.previewAdjustment(target,originalAdjustment);else for(Layer &layer:document_->layers)if(layer.id==target){layer=originalLayer;break;}
    session_.endEdit();syncDocumentViews();
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
    auto *hueControl = sliderField(hue); auto *saturationControl = sliderField(saturation); auto *lightnessControl = sliderField(lightness);
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
    form->addRow(tr("Range"), range); form->addRow(tr("Hue"), hueControl); form->addRow(tr("Saturation"), saturationControl); form->addRow(tr("Lightness"), lightnessControl);
    const auto liveValue = [&] { store(); return hueAdjustment(settings); };
    session_.beginEdit(live ? QStringLiteral("Edit Hue/Saturation") : QStringLiteral("Hue/Saturation"));
    const auto preview = [this, live, target, liveValue, originalLayer, &settings, previewEnabled] {
        if (!previewEnabled->isChecked()) {
            if (live) session_.previewAdjustment(target, originalLayer.adjustment);
            else for (Layer &layer : document_->layers) if (layer.id == target) { layer = originalLayer; break; }
        } else if(live) session_.previewAdjustment(target,liveValue());
        else { for(Layer &layer:document_->layers)if(layer.id==target){layer=originalLayer;break;} liveValue(); session_.applyHueSaturation(settings); }
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
    if(result==QDialog::Accepted) { previewEnabled->setChecked(true); preview(); } else if(live)session_.previewAdjustment(target,originalAdjustment);else for(Layer &layer:document_->layers)if(layer.id==target){layer=originalLayer;break;}
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
    form->addRow(tr("Input"), input); form->addRow(tr("Output"), output);
    auto *hint = new QLabel(tr("Click to add a point. Drag to adjust."), &dialog); auto *pointButtons = new QHBoxLayout;
    auto *status = new QLabel(&dialog); status->setObjectName(QStringLiteral("curvePointStatus")); auto *remove = new QPushButton(tr("Remove point"), &dialog); auto *reset = new QPushButton(tr("Reset curve"), &dialog);
    remove->setObjectName(QStringLiteral("removeCurvePoint")); reset->setObjectName(QStringLiteral("resetCurve")); pointButtons->addWidget(status); pointButtons->addStretch(); pointButtons->addWidget(remove);
    auto *previewEnabled = new QCheckBox(tr("Preview"), &dialog); previewEnabled->setChecked(true); previewEnabled->setObjectName(QStringLiteral("filterPreview"));
    layout->addWidget(channel); layout->addWidget(graph, 1); layout->addWidget(hint); layout->addLayout(form); layout->addLayout(pointButtons); layout->addWidget(reset); layout->addWidget(previewEnabled);
    CurvesSettings settings; const QJsonObject originalAdjustment=active->adjustment; const Layer originalLayer=*active;
    if(live){const QJsonArray saved=originalAdjustment.value(QStringLiteral("curves")).toObject().value(QStringLiteral("channels")).toArray();for(int c=0;c<std::min(4,int(saved.size()));++c){QVector<CurvePoint> curve;for(const QJsonValue &entry:saved[c].toArray()){const QJsonObject point=entry.toObject();curve.push_back({point.value(QStringLiteral("x")).toDouble(),point.value(QStringLiteral("y")).toDouble()});}if(!curve.isEmpty())settings.channels[size_t(c)]=curve;}}
    graph->settings = &settings;
    const auto preview=[this,live,target,&settings,originalLayer,previewEnabled]{if(!previewEnabled->isChecked()){if(live)session_.previewAdjustment(target,originalLayer.adjustment);else for(Layer &layer:document_->layers)if(layer.id==target){layer=originalLayer;break;}}else if(live)session_.previewAdjustment(target,curvesAdjustment(settings));else{for(Layer &layer:document_->layers)if(layer.id==target){layer=originalLayer;break;}session_.applyCurves(settings);}canvas_->invalidateDocument();};
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
    if(result==QDialog::Accepted){previewEnabled->setChecked(true);preview();}else if(live)session_.previewAdjustment(target,originalAdjustment);else for(Layer &layer:document_->layers)if(layer.id==target){layer=originalLayer;break;}
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
    const auto preview = [this, live, target, value, originalLayer, &shadows, &highlights, reverse, previewEnabled] { if(!previewEnabled->isChecked()){if(live)session_.previewAdjustment(target,originalLayer.adjustment);else for(Layer &layer:document_->layers)if(layer.id==target){layer=originalLayer;break;}}else if(live)session_.previewAdjustment(target,value());else{for(Layer &layer:document_->layers)if(layer.id==target){layer=originalLayer;break;}session_.applyGradientMap(shadows,highlights,reverse->isChecked());}canvas_->invalidateDocument(); };
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
    if(result==QDialog::Accepted){previewEnabled->setChecked(true);preview();}else if(live)session_.previewAdjustment(target,originalAdjustment);else for(Layer &layer:document_->layers)if(layer.id==target){layer=originalLayer;break;}
    session_.endEdit();syncDocumentViews();
}

void MainWindow::grainDialog()
{
    Layer *active = session_.activeLayer(); const bool live = active && active->adjustment.value(QStringLiteral("kind")).toString() == QStringLiteral("Grain");
    if (!active || (!live && active->image.isNull())) return;
    const QUuid target = active->id;
    QDialog dialog(this); dialog.setWindowTitle(tr("Grain")); auto *layout = new QVBoxLayout(&dialog); auto *form = new QFormLayout;
    auto make = [&dialog](double minimum, double maximum, double value) { auto *field = new QDoubleSpinBox(&dialog); field->setRange(minimum, maximum); field->setValue(value); field->setDecimals(1); return field; };
    auto *amount = make(0, 100, 25); auto *size = make(.5, 20, 1.5); auto *roughness = make(0, 100, 50);
    auto *previewEnabled = new QCheckBox(tr("Preview"), &dialog); previewEnabled->setChecked(true); previewEnabled->setObjectName(QStringLiteral("filterPreview"));
    const QJsonObject originalAdjustment = active->adjustment; const Layer originalLayer=*active; quint32 seed = QRandomGenerator::global()->generate();
    if (live) { const QJsonObject saved = originalAdjustment.value(QStringLiteral("grainSettings")).toObject(); amount->setValue(saved.value(QStringLiteral("amount")).toDouble(25)); size->setValue(saved.value(QStringLiteral("size")).toDouble(1.5)); roughness->setValue(saved.value(QStringLiteral("roughness")).toDouble(50)); seed = quint32(saved.value(QStringLiteral("seed")).toDouble(seed)); }
    const auto value = [&] { return QJsonObject{{QStringLiteral("kind"), QStringLiteral("Grain")}, {QStringLiteral("grainSettings"), QJsonObject{{QStringLiteral("amount"), amount->value()}, {QStringLiteral("size"), size->value()}, {QStringLiteral("roughness"), roughness->value()}, {QStringLiteral("seed"), double(seed)}}}}; };
    session_.beginEdit(live?QStringLiteral("Edit Grain"):QStringLiteral("Grain"));const auto preview=[this,live,target,value,originalLayer,amount,size,roughness,seed,previewEnabled]{if(!previewEnabled->isChecked()){if(live)session_.previewAdjustment(target,originalLayer.adjustment);else for(Layer &layer:document_->layers)if(layer.id==target){layer=originalLayer;break;}}else if(live)session_.previewAdjustment(target,value());else{for(Layer &layer:document_->layers)if(layer.id==target){layer=originalLayer;break;}session_.applyGrain(amount->value(),size->value(),roughness->value(),seed);}canvas_->invalidateDocument();};connect(amount,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,preview);connect(size,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,preview);connect(roughness,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,preview);
    form->addRow(tr("Amount"), sliderField(amount)); form->addRow(tr("Size"), sliderField(size, true)); form->addRow(tr("Roughness"), sliderField(roughness)); layout->addLayout(form); layout->addWidget(previewEnabled); connect(previewEnabled,&QCheckBox::toggled,&dialog,preview);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
    const int result = runFloatingDialog(dialog);
    if(result==QDialog::Accepted){previewEnabled->setChecked(true);preview();}else if(live)session_.previewAdjustment(target,originalAdjustment);else for(Layer &layer:document_->layers)if(layer.id==target){layer=originalLayer;break;}
    session_.endEdit();syncDocumentViews();
}

void MainWindow::gaussianBlurDialog()
{
    Layer *active=session_.activeLayer();if(!active||active->image.isNull())return;const QUuid target=active->id;const Layer original=*active;
    QDialog dialog(this);dialog.setWindowTitle(tr("Gaussian Blur"));auto *layout=new QVBoxLayout(&dialog);auto *form=new QFormLayout;auto *radius=new QDoubleSpinBox(&dialog);radius->setRange(.1,250);radius->setValue(3);radius->setSuffix(tr(" px"));form->addRow(tr("Radius"),sliderField(radius,true));layout->addLayout(form);auto *previewEnabled=new QCheckBox(tr("Preview"),&dialog);previewEnabled->setChecked(true);previewEnabled->setObjectName(QStringLiteral("filterPreview"));layout->addWidget(previewEnabled);
    session_.beginEdit(QStringLiteral("Gaussian Blur"));const auto preview=[this,target,original,radius,previewEnabled]{for(Layer &layer:document_->layers)if(layer.id==target){layer=original;break;}if(previewEnabled->isChecked())session_.applyGaussianBlur(radius->value());canvas_->invalidateDocument();};connect(radius,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,preview);connect(previewEnabled,&QCheckBox::toggled,&dialog,preview);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Cancel|QDialogButtonBox::Ok,&dialog);connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);layout->addWidget(buttons);
    if(runFloatingDialog(dialog)==QDialog::Accepted){previewEnabled->setChecked(true);preview();}else for(Layer &layer:document_->layers)if(layer.id==target){layer=original;break;}session_.endEdit();syncDocumentViews();
}

void MainWindow::motionBlurDialog()
{
    Layer *active=session_.activeLayer();if(!active||active->image.isNull())return;const QUuid target=active->id;const Layer original=*active;
    QDialog dialog(this); dialog.setWindowTitle(tr("Motion Blur")); auto *layout = new QVBoxLayout(&dialog); auto *form = new QFormLayout;
    auto *angle = new QDoubleSpinBox(&dialog); angle->setRange(-90, 90); angle->setSuffix(tr("°"));
    auto *distance = new QDoubleSpinBox(&dialog); distance->setRange(1, 2000); distance->setValue(10); distance->setSuffix(tr(" px"));
    form->addRow(tr("Angle"), sliderField(angle)); form->addRow(tr("Distance"), sliderField(distance,true)); layout->addLayout(form);auto *previewEnabled=new QCheckBox(tr("Preview"),&dialog);previewEnabled->setChecked(true);previewEnabled->setObjectName(QStringLiteral("filterPreview"));layout->addWidget(previewEnabled);
    session_.beginEdit(QStringLiteral("Motion Blur"));const auto preview=[this,target,original,angle,distance,previewEnabled]{for(Layer &layer:document_->layers)if(layer.id==target){layer=original;break;}if(previewEnabled->isChecked())session_.applyMotionBlur(angle->value(),distance->value());canvas_->invalidateDocument();};connect(angle,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,preview);connect(distance,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,preview);connect(previewEnabled,&QCheckBox::toggled,&dialog,preview);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
    if(runFloatingDialog(dialog)==QDialog::Accepted){previewEnabled->setChecked(true);preview();}else for(Layer &layer:document_->layers)if(layer.id==target){layer=original;break;}session_.endEdit();syncDocumentViews();
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
        for (Layer &layer : document_->layers) if (layer.id == target) { layer = original; break; }
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
    form->addRow(tr("Units"), units); form->addRow(tr("Width"), width); form->addRow(tr("Height"), height); form->addRow(tr("Resolution"), resolution); form->addRow(tr("Sampling"), sampling);
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
    connect(color, &QPushButton::clicked, &dialog, [this, &extension] { const QColor chosen = QColorDialog::getColor(extension, this, tr("Canvas Extension Color")); if (chosen.isValid()) extension = chosen; });
    form->addRow(tr("Units"), units); form->addRow(tr("Width"), width); form->addRow(tr("Height"), height); form->addRow(tr("Anchor"), anchor);
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
    auto makeField = [&dialog](int value, int maximum) { auto *field = new QSpinBox(&dialog); field->setRange(-1000000, maximum); field->setValue(value); field->setSuffix(QObject::tr(" px")); return field; };
    auto *x = makeField(0, 1000000); auto *y = makeField(0, 1000000);
    auto *width = makeField(document_->canvasSize.width(), 30000); width->setMinimum(1);
    auto *height = makeField(document_->canvasSize.height(), 30000); height->setMinimum(1);
    form->addRow(tr("X"), x); form->addRow(tr("Y"), y); form->addRow(tr("Width"), width); form->addRow(tr("Height"), height); layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
    if (dialog.exec() == QDialog::Accepted && session_.crop(QRect(x->value(), y->value(), width->value(), height->value()))) syncDocumentViews();
}

void MainWindow::newProject()
{
    suggestClipboardOnEmpty_ = true;
    installInNewTab(EditorSession(), tr("Untitled %1").arg(workspaceTabs_.size() + 1));
}

void MainWindow::importImages()
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Import Images"), {},
        tr("Images (*.png *.jpg *.jpeg *.heic *.heif *.tif *.tiff)"));
    importImageFiles(paths);
}

bool MainWindow::importImageFiles(const QStringList &paths, const std::optional<QPointF> &center)
{
    bool imported = false;
    QStringList failures;
    for (const QString &path : paths) {
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

bool MainWindow::saveProject()
{
    finishInlineText();
    if (!document_) return true;
    if (transformOriginalDocument_) finishPersistentTransform(true);
    canvas_->resolvePendingGradient(); canvas_->resolvePendingDistortion();
    if (session_.hasFloatingSelection()) session_.commitSelectionTransform();
    if (document_->projectPath.isEmpty()) return saveProjectAs();
    try {
        ProjectWriter::save(*document_, document_->projectPath);
        session_.markSaved();
        removeRecovery();
        statusHint_->setText(tr("Saved %1").arg(QFileInfo(document_->projectPath).fileName()));
        refreshTitle();
        return true;
    } catch (const ProjectWriteError &error) {
        showMessage(this, tr("Could Not Save Project"), error.message());
        return false;
    }
}

bool MainWindow::saveProjectAs()
{
    if (!document_) return true;
    QString path = QFileDialog::getSaveFileName(this, tr("Save Compositor Project"),
        document_->projectPath.isEmpty() ? QStringLiteral("Untitled.comp") : document_->projectPath,
        tr("Compositor projects (*.comp)"));
    if (path.isEmpty()) return false;
    if (!path.endsWith(QStringLiteral(".comp"), Qt::CaseInsensitive)) path += QStringLiteral(".comp");
    const QString previous = document_->projectPath;
    document_->projectPath = path;
    if (saveProject()) return true;
    document_->projectPath = previous;
    return false;
}

bool MainWindow::confirmReplacement()
{
    if (!document_ || !session_.isModified()) return true;
    QString name = QFileInfo(document_->projectPath).fileName(); if (name.isEmpty()) name = tr("Untitled");
    const auto choice = showMessage(this, tr("Unsaved Changes"), tr("Save changes to %1?").arg(name),
        tr("Your changes will be lost if you don’t save them."),
        QMessageBox::Save | QMessageBox::Cancel | QMessageBox::Discard, QMessageBox::Save);
    if (choice == QMessageBox::Save) return saveProject();
    return choice == QMessageBox::Discard;
}

QString MainWindow::recoveryPath() const
{
    if (currentTab_ >= 0 && currentTab_ < tabRecoveryPaths_.size()) return tabRecoveryPaths_.at(currentTab_);
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

void MainWindow::removeRecovery()
{
    const QString path = recoveryPath();
    const QString root = QDir(recoveryDirectory()).absolutePath() + QLatin1Char('/');
    if (!path.isEmpty() && QFileInfo(path).isDir() && QFileInfo(path).absoluteFilePath().startsWith(root)) QDir(path).removeRecursively();
}

void MainWindow::autosave()
{
    stashCurrentTab();
    int saved = 0;
    QString failure;
    for (int i = 0; i < workspaceTabs_.size(); ++i) {
        EditorSession &candidate = workspaceTabs_[i];
        const auto &doc = candidate.document();
        if (!doc || !candidate.isModified() || candidate.isPainting()) continue;
        try {
            if (!doc->projectPath.isEmpty()) {
                ProjectWriter::save(*doc, doc->projectPath);
                candidate.markSaved();
            } else {
                QDir().mkpath(recoveryDirectory());
                ProjectWriter::save(*doc, tabRecoveryPaths_.at(i));
            }
            ++saved;
        } catch (const ProjectWriteError &error) {
            failure = error.message();
        }
    }
    if (currentTab_ >= 0 && currentTab_ < workspaceTabs_.size()) session_ = workspaceTabs_.at(currentTab_);
    refreshTitle();
    if (!failure.isEmpty()) statusHint_->setText(tr("Autosave failed: %1").arg(failure));
    else if (saved > 0) statusHint_->setText(tr("Autosaved %n document(s)", nullptr, saved));
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
    try {
        auto loaded = std::make_shared<Document>(ProjectReader::load(path));
        EditorSession opened; opened.setDocument(std::move(loaded));
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
    if (index < 0 || index >= workspaceTabs_.size() || index == currentTab_) return;
    finishInlineText();
    if (transformOriginalDocument_) finishPersistentTransform(true);
    canvas_->resolvePendingGradient(); canvas_->resolvePendingDistortion(); canvas_->resolvePendingCrop(false);
    if (session_.hasFloatingSelection()) session_.commitSelectionTransform();
    stashCurrentTab(); currentTab_ = index; session_ = workspaceTabs_.at(index); syncDocumentViews();
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
    if (index < 0 || index >= workspaceTabs_.size()) return;
    finishInlineText();
    activateTab(index);
    if (!confirmReplacement()) return;
    if (workspaceTabs_.size() == 1) {
        removeRecovery(); session_ = EditorSession(); workspaceTabs_[0] = session_; tabRecoveryPaths_[0] = newRecoveryPath();
        tabs_->setTabText(0, tr("Untitled")); syncDocumentViews(); return;
    }
    removeRecovery();
    stashCurrentTab(); workspaceTabs_.removeAt(index);
    tabRecoveryPaths_.removeAt(index);
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
        const std::array<QWidget *, 9> fields = {xField_, yField_, widthField_, heightField_, scaleField_, rotationField_, sampling_, blendMode_, opacitySlider_};
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
    const std::array<QWidget *, 9> fields = {xField_, yField_, widthField_, heightField_, scaleField_, rotationField_, sampling_, blendMode_, opacitySlider_};
    for (QWidget *field : fields) field->setEnabled(true);
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
    enabled("commandCut", text || (hasSelection && canCopy)); enabled("commandCopy", text || canCopy); enabled("commandCopyMerged", hasDocument && hasSelection);
    enabled("commandPaste", text || !clipboardImage_.isNull() || !QGuiApplication::clipboard()->image().isNull());
    enabled("commandDuplicate", hasActive && (hasSelection ? canCopy : !active->group)); enabled("commandDelete", text || hasActive);
    enabled("commandTransform", hasSelection ? canCopy : canTransformLayer);
    enabled("commandNewLayer", hasDocument); enabled("commandNewFolder", hasDocument); enabled("commandGroupLayers", hasDocument);
    enabled("commandMoveOut", active && active->parentId.has_value()); enabled("commandRenameLayer", hasActive); enabled("commandVisibility", hasActive);
    enabled("commandMerge", session_.canMergeLayers()); enabled("commandMoveUp", session_.canMoveActiveLayer(1)); enabled("commandMoveDown", session_.canMoveActiveLayer(-1));
    enabled("commandNewAdjustment", hasDocument); enabled("commandEditAdjustment", active && !active->adjustment.isEmpty());
    enabled("commandSelectAll", text || hasDocument); enabled("commandDeselect", hasSelection); enabled("commandInverseSelection", hasSelection);

    if (QAction *item = action("commandTransform")) item->setText(hasSelection ? tr("Transform Selection") : tr("Transform Layer"));
    if (QAction *item = action("commandDuplicate")) item->setText(hasSelection ? tr("Layer via Copy") : tr("Duplicate Layer"));
    if (QAction *item = action("commandDelete")) item->setText(session_.isMaskSelected() && active && !active->mask.isNull() ? tr("Delete Layer Mask")
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
    {
        const QSignalBlocker blocker(layerView_->selectionModel());
        layerView_->selectionModel()->clearSelection();
        QModelIndex primaryIndex;
        if (document_) {
            for (int row = 0; row < layerModel_->rowCount(); ++row) {
                const QModelIndex index = layerModel_->index(row, 0);
                const auto id = layerModel_->layerId(index); if (!id) continue;
                if (session_.selectedLayerIds().contains(*id)) {
                    layerView_->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
                }
                if (document_->activeLayerId && *id == *document_->activeLayerId) primaryIndex = index;
            }
        }
        layerView_->selectionModel()->setCurrentIndex(primaryIndex, QItemSelectionModel::NoUpdate);
    }
    layerCount_->setText(document_ ? QString::number(document_->layers.size()) : QStringLiteral("0"));
    if (document_) {
        statusDimensions_->setText(QStringLiteral("%1 × %2 px").arg(document_->canvasSize.width()).arg(document_->canvasSize.height()));
        statusHint_->setText(tr("Drag to move · Handles to resize · Circle to rotate · 1–0 layer opacity · Space to pan"));
    }
    updateInspector();
    refreshTitle();
}

void MainWindow::showAbout()
{
    showMessage(this, tr("About CompositorLX"), tr("CompositorLX 0.1"),
                tr("Linux port of Compositor · Qt 6 Widgets + C++20"));
}

void MainWindow::checkForUpdates()
{
    auto *network = new QNetworkAccessManager(this);
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/robbietilton/Compositor/releases/latest")));
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
    finishInlineText();
    if (transformOriginalDocument_) finishPersistentTransform(true);
    canvas_->resolvePendingGradient(); canvas_->resolvePendingDistortion(); canvas_->resolvePendingCrop(false);
    if (session_.hasFloatingSelection()) session_.commitSelectionTransform();
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

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
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
    if (layerView_ && watched == layerView_->viewport()) {
        const auto toggleSwipeRow = [this](const QPoint &point) {
            const QModelIndex index = layerView_->indexAt(point); const auto id = layerModel_->layerId(index);
            if (!id || visibilitySwipeVisited_.contains(*id)) return;
            const auto it = std::find_if(document_->layers.cbegin(), document_->layers.cend(), [&](const Layer &layer){return layer.id==*id;});
            if (it == document_->layers.cend()) return;
            if (it->visible != visibilitySwipeValue_) session_.toggleLayerVisibility(*id);
            visibilitySwipeVisited_.insert(*id); syncDocumentViews();
        };
        if (event->type() == QEvent::MouseButtonPress) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            const QModelIndex index = layerView_->indexAt(mouse->position().toPoint()); const auto id = layerModel_->layerId(index);
            if (mouse->button() == Qt::LeftButton && mouse->position().x() < 29 && id) {
                const auto it=std::find_if(document_->layers.cbegin(),document_->layers.cend(),[&](const Layer&l){return l.id==*id;});
                if(it!=document_->layers.cend()){visibilitySwipeActive_=true;visibilitySwipeValue_=!it->visible;visibilitySwipeVisited_.clear();session_.beginEdit(tr("Layer Visibility"));toggleSwipeRow(mouse->position().toPoint());return true;}
            }
        } else if (event->type() == QEvent::MouseButtonRelease && visibilitySwipeActive_) {
            visibilitySwipeActive_=false; session_.endEdit(); visibilitySwipeVisited_.clear(); refreshTitle(); return true;
        } else if (event->type() == QEvent::Leave) layerView_->viewport()->setCursor(Qt::ArrowCursor);
        else if (event->type() == QEvent::MouseMove) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (visibilitySwipeActive_ && (mouse->buttons() & Qt::LeftButton)) { toggleSwipeRow(mouse->position().toPoint()); return true; }
            const QModelIndex index = layerView_->indexAt(mouse->position().toPoint());
            Qt::CursorShape shape = Qt::ArrowCursor;
            if (index.isValid()) {
                const int thumbnail = 34 + std::min(index.data(Qt::UserRole + 1).toInt(), 8) * 18;
                const bool overThumbnail = mouse->position().x() >= thumbnail && mouse->position().x() <= thumbnail + 80;
                if (mouse->modifiers().testFlag(Qt::AltModifier) && !overThumbnail) shape = Qt::DragCopyCursor;
                else if (overThumbnail && (mouse->modifiers() & (Qt::AltModifier | Qt::ControlModifier))) shape = Qt::PointingHandCursor;
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
