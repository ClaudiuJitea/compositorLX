#include "ui/MainWindow.h"
#include "ui/CanvasWidget.h"
#include "ui/ShortcutManager.h"
#include "ui/ColorPickerDialog.h"
#include "ui/SegmentedControl.h"
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include "rendering/LayerRenderer.h"
#include "rendering/DeepImage.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QPainter>
#include <QRegularExpression>
#include <QTimer>
#include <QShortcut>
#include <QLineF>
#include <QVBoxLayout>

namespace compositor
{
namespace
{
class ExportPreviewView final : public QGraphicsView
{
  public:
    using QGraphicsView::QGraphicsView;
    std::function<void()> resized;

  protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QGraphicsView::resizeEvent(event);
        if (resized)
            resized();
    }
};
} // namespace
void MainWindow::searchCommands()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("commandPalette"));
    dialog.setWindowTitle(tr("Search Commands"));
    dialog.resize(520, 390);
    auto *layout = new QVBoxLayout(&dialog);
    auto *search = new QLineEdit(&dialog);
    search->setObjectName(QStringLiteral("commandSearch"));
    search->setPlaceholderText(tr("Search commands and tools…"));
    layout->addWidget(search);
    auto *list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("commandResults"));
    layout->addWidget(list, 1);
    struct Command {
        QString title;
        QPointer<QAction> action;
        QString key; // a tool's key, shown where a command's shortcut would be
        int score = 0;
        int order = 0;
    };
    QVector<Command> commands;
    const auto clean = [](QString title) {
        title.replace(QStringLiteral("&&"), QStringLiteral("\x01"));
        title.remove('&');
        title.replace(QChar(1), '&');
        return title;
    };
    // Brought up to date as the menus would be when opened: states, titles and menus rebuilt on aboutToShow.
    updateCommandStates();
    // Left out as on macOS: the palette itself and Help.
    const QStringList skipped{tr("Search Commands…"), tr("Help")};
    std::function<void(QMenu *, const QString &)> collect = [&](QMenu *menu, const QString &prefix) {
        emit menu->aboutToShow();
        for (QAction *action : menu->actions()) {
            if (action->isSeparator() || !action->isVisible())
                continue;
            const QString name = clean(action->text());
            if (name.isEmpty() || skipped.contains(name))
                continue;
            if (action->menu()) {
                collect(action->menu(), prefix + name + QStringLiteral(" › "));
                continue;
            }
            commands.push_back({prefix + name, action});
        }
    };
    for (QAction *action : menuBar()->actions())
        if (action->menu() && !skipped.contains(clean(action->text())))
            collect(action->menu(), clean(action->text()) + QStringLiteral(" › "));
    // Commands that live on buttons rather than in the menu bar: adding a layer mask, as the mask button does.
    const bool hasSelection = document_ && document_->selection.has_value();
    const bool canAddMask = session_.canEditMask() && session_.activeLayer() && session_.activeLayer()->mask.isNull();
    for (bool revealing : {true, false}) {
        auto *action = new QAction(&dialog);
        action->setEnabled(canAddMask);
        connect(action, &QAction::triggered, this, [this, revealing] { if (session_.addLayerMask(revealing)) syncDocumentViews(); });
        commands.push_back({revealing ? (hasSelection ? tr("Layer › Add Layer Mask from Selection") : tr("Layer › Add Layer Mask"))
                                      : (hasSelection ? tr("Layer › Add Layer Mask Hiding Selection") : tr("Layer › Add Layer Mask (Hide All)")),
                            action});
    }
    // Every tool, and each of a tool's modes on its own, by the name someone would search for: Liquify is a mode of
    // the Blur tool and Polygonal Lasso a mode of the Lasso, neither in a menu. Disabled with no canvas to use them on.
    using Tool = CanvasWidget::Tool;
    struct ToolEntry { QString name; QString shortcutId; Tool tool; std::function<void()> setup; };
    const auto smear = [this](int index) { return [this, index] { if (smearMode_) { smearMode_->setCurrentIndex(index); updateSmearStatusHint(); } }; };
    const QVector<ToolEntry> tools{
        {tr("Move / Transform"), QStringLiteral("Move / Transform tool"), Tool::Move, {}},
        {tr("Rectangular Marquee"), QStringLiteral("Marquee / cycle shape"), Tool::Marquee, [this] { canvas_->setMarqueeElliptical(false); }},
        {tr("Elliptical Marquee"), QStringLiteral("Marquee / cycle shape"), Tool::Marquee, [this] { canvas_->setMarqueeElliptical(true); }},
        {tr("Lasso"), QStringLiteral("Lasso / cycle mode"), Tool::Lasso, [this] { canvas_->setPolygonalLasso(false); }},
        {tr("Polygonal Lasso"), QStringLiteral("Lasso / cycle mode"), Tool::Lasso, [this] { canvas_->setPolygonalLasso(true); }},
        {tr("Magic Wand"), QStringLiteral("Magic"), Tool::Wand, [this] { canvas_->setWandMode(CanvasWidget::WandMode::Wand); }},
        {tr("Object Selection"), QStringLiteral("Magic"), Tool::Wand, [this] { canvas_->setWandMode(CanvasWidget::WandMode::Object); }},
        {tr("Crop"), QStringLiteral("Crop tool"), Tool::Crop, {}},
        {tr("Brush"), QStringLiteral("Brush tool"), Tool::Brush, {}},
        {tr("Eraser"), QStringLiteral("Eraser"), Tool::Eraser, {}},
        {tr("Spot Healing Brush"), QStringLiteral("Spot Healing"), Tool::Healing, {}},
        {tr("Clone Stamp"), QStringLiteral("Clone Stamp"), Tool::Clone, {}},
        {tr("Liquify"), QStringLiteral("Blur / Smudge / Liquify"), Tool::Blur, smear(0)},
        {tr("Blur"), QStringLiteral("Blur / Smudge / Liquify"), Tool::Blur, smear(1)},
        {tr("Smudge"), QStringLiteral("Blur / Smudge / Liquify"), Tool::Blur, smear(2)},
        {tr("Gradient"), QStringLiteral("Gradient tool"), Tool::Gradient, {}},
        {tr("Rectangle"), QStringLiteral("Shape tool"), Tool::Shape, [this] { canvas_->setShapeKind(ShapeKind::Rectangle); }},
        {tr("Ellipse"), QStringLiteral("Shape tool"), Tool::Shape, [this] { canvas_->setShapeKind(ShapeKind::Ellipse); }},
        {tr("Line"), QStringLiteral("Shape tool"), Tool::Shape, [this] { canvas_->setShapeKind(ShapeKind::Line); }},
        {tr("Type"), QStringLiteral("Type tool"), Tool::Text, {}},
        {tr("Eyedropper"), QStringLiteral("Eyedropper tool"), Tool::Eyedropper, {}},
        {tr("Hand"), QStringLiteral("Hand tool"), Tool::Hand, {}},
        {tr("Zoom"), QStringLiteral("Zoom tool"), Tool::Zoom, {}},
    };
    for (const ToolEntry &entry : tools) {
        auto *action = new QAction(&dialog);
        action->setEnabled(session_.hasDocument());
        connect(action, &QAction::triggered, this, [this, tool = entry.tool, setup = entry.setup] {
            canvas_->setTool(tool);
            // Only once the tool is in hand, as its own Tab or key would switch it.
            if (canvas_->tool() == tool && setup) setup();
        });
        const QKeySequence key = ShortcutManager::instance().shortcut(QStringLiteral("Canvas & Layers:") + entry.shortcutId);
        commands.push_back({tr("Tool › %1").arg(entry.name), action, key.toString(QKeySequence::NativeText)});
    }
    for (int i = 0; i < commands.size(); ++i)
        commands[i].order = i;
    // Fuzzy matching as launchers do it (mac CommandPaletteSearch): the query's letters in order, scored more at a
    // word's start and right after the previous match; then ranked by how the typed text was found, best first.
    const auto wordsStartWords = [](const QStringList &words, const QString &title) {
        qsizetype from = 0;
        for (const QString &word : words) {
            qsizetype search = from, found = -1;
            for (qsizetype at; (at = title.indexOf(word, search)) >= 0; search = at + 1)
                if (at == 0 || !title[at - 1].isLetter()) { found = at + word.size(); break; }
            if (found < 0) return false;
            from = found;
        }
        return true;
    };
    const auto score = [&](const QString &query, const QString &text) -> std::optional<int> {
        QString wanted = query.toLower();
        wanted.remove(QRegularExpression(QStringLiteral("\\s")));
        if (wanted.isEmpty()) return 0;
        const QString title = text.toLower();
        int total = 0, previous = -2;
        qsizetype from = 0;
        for (QChar character : wanted) {
            const qsizetype found = title.indexOf(character, from);
            if (found < 0) return std::nullopt;
            const bool wordStart = found == 0 || !title[found - 1].isLetterOrNumber();
            total += 1 + (wordStart ? 8 : 0) + (found == previous + 1 ? 5 : 0);
            previous = int(found);
            from = found + 1;
        }
        const QString phrase = query.toLower().trimmed();
        const QStringList words = phrase.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QString compact = title; compact.remove(QLatin1Char(' '));
        const int tier = wordsStartWords({phrase}, title) ? 5
            : words.size() > 1 && wordsStartWords(words, title) ? 4
            : title.contains(phrase) ? 3
            : compact.contains(wanted) ? 2 : 1;
        return tier * 1'000'000 + (tier == 1 ? total * 100 : 0) - int(title.size()) / 4;
    };
    QVector<Command> matches;
    const auto refresh = [&] {
        matches.clear();
        list->clear();
        const QString query = search->text();
        for (auto command : commands) {
            if (!command.action) continue;
            if (const auto value = score(query, command.title)) {
                command.score = *value;
                matches.push_back(command);
            }
        }
        // Enabled ahead of disabled; with no query, menu order.
        std::sort(matches.begin(), matches.end(), [&](const Command &a, const Command &b) {
            const bool ae = a.action->isEnabled(), be = b.action->isEnabled();
            if (ae != be) return ae;
            if (query.trimmed().isEmpty() || a.score == b.score) return a.order < b.order;
            return a.score > b.score;
        });
        for (const auto &c : matches) {
            QAction *a = c.action;
            const QString key = !c.key.isEmpty() ? c.key : a->shortcut().toString(QKeySequence::NativeText);
            auto *row = new QListWidgetItem(
                (a->isCheckable() ? (a->isChecked() ? QStringLiteral("✓ ") : QStringLiteral("   ")) : QString())
                    + c.title + (key.isEmpty() ? QString() : QStringLiteral("    ") + key),
                list);
            if (!a->isEnabled())
                row->setFlags(row->flags() & ~Qt::ItemIsEnabled);
        }
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->flags().testFlag(Qt::ItemIsEnabled)) {
                list->setCurrentRow(i);
                break;
            }
    };
    QPointer<QAction> chosen;
    const auto choose = [&] {
        const int row = list->currentRow();
        if (row >= 0 && row < matches.size() && matches[row].action && matches[row].action->isEnabled()) {
            chosen = matches[row].action;
            dialog.accept();
        }
    };
    connect(search, &QLineEdit::textChanged, &dialog, refresh);
    connect(search, &QLineEdit::returnPressed, &dialog, choose);
    connect(list, &QListWidget::itemActivated, &dialog, [&] { choose(); });
    for (int direction : {-1, 1}) {
        auto *key = new QShortcut(QKeySequence(direction < 0 ? Qt::Key_Up : Qt::Key_Down), &dialog);
        connect(key, &QShortcut::activated, &dialog, [&, direction] {
            int row = list->currentRow();
            for (int count = 0; count < list->count(); ++count) {
                row = (row + direction + list->count()) % list->count();
                if (list->item(row)->flags().testFlag(Qt::ItemIsEnabled)) {
                    list->setCurrentRow(row);
                    break;
                }
            }
        });
    }
    refresh();
    search->setFocus();
    if (dialog.exec() == QDialog::Accepted && chosen)
        chosen->trigger();
}
void MainWindow::scanlinesDialog()
{
    const auto *layer = session_.activeLayer();
    if (!layer || layer->image.isNull())
        return;
    const Layer original = *layer;
    const QUuid id = layer->id;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("scanlinesDialog"));
    dialog.setWindowTitle(tr("Scanlines"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    ScanlinesSettings settings = scanlinesSettings_;
    const auto field = [&](const QString &title, const QString &name, double &value, double lo, double hi) {
        auto *spin = new QDoubleSpinBox(&dialog);
        spin->setObjectName(name);
        spin->setRange(lo, hi);
        spin->setValue(value);
        form->addRow(title, spin);
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog,
                [&value](double v) { value = v; });
        return spin;
    };
    field(tr("Spacing"), QStringLiteral("scanSpacing"), settings.lineSpacing, 2, 32);
    field(tr("Thickness"), QStringLiteral("scanThickness"), settings.thickness, 5, 100);
    field(tr("Black Level"), QStringLiteral("scanBlackLevel"), settings.blackLevel, 0, 100);
    field(tr("Glow"), QStringLiteral("scanGlow"), settings.glow, 0, 100);
    field(tr("Dots by Tone"), QStringLiteral("scanDots"), settings.dots, 0, 100);
    field(tr("Wobble"), QStringLiteral("scanWobble"), settings.wobble, 0, 64);
    field(tr("Displace"), QStringLiteral("scanDisplace"), settings.displace, -100, 100);
    field(tr("Smoothness"), QStringLiteral("scanSmoothness"), settings.smoothness, 0, 100);
    field(tr("Threshold"), QStringLiteral("scanThreshold"), settings.threshold, 0, 100);
    field(tr("Color Split"), QStringLiteral("scanSplit"), settings.split, 0, 16);
    field(tr("Density"), QStringLiteral("scanDensity"), settings.density, -100, 100);
    field(tr("Contrast"), QStringLiteral("scanContrast"), settings.contrast, -100, 100);
    auto *colors = new QComboBox(&dialog);
    colors->addItems({tr("Black & White"), tr("Two Colors"), tr("Original Colors")});
    colors->setCurrentIndex(int(settings.colors));
    form->addRow(tr("Colors"), colors);
    settings.dark = {session_.foregroundColor().redF(), session_.foregroundColor().greenF(),
                     session_.foregroundColor().blueF()};
    settings.light = {session_.backgroundColor().redF(), session_.backgroundColor().greenF(),
                      session_.backgroundColor().blueF()};
    connect(colors, &QComboBox::currentIndexChanged, &dialog,
            [&](int i) { settings.colors = DitherColors(i); });
    auto *enabled = new QCheckBox(tr("Preview"), &dialog);
    enabled->setChecked(true);
    layout->addWidget(enabled);
    auto *timer = new QTimer(&dialog);
    timer->setSingleShot(true);
    timer->setInterval(60);
    session_.beginEdit(QStringLiteral("Scanlines"));
    const auto preview = [&] {
        if (enabled->isChecked())
            previewFilter(&dialog, id, original, [value = settings](EditorSession &s) { return s.applyScanlines(value); });
        else
            session_.rollbackLayer(id, original);
        canvas_->invalidateDocument();
    };
    connect(timer, &QTimer::timeout, &dialog, preview);
    for (auto *spin : dialog.findChildren<QDoubleSpinBox *>())
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), timer, qOverload<>(&QTimer::start));
    connect(enabled, &QCheckBox::toggled, &dialog, [&] { timer->start(); });
    connect(colors, &QComboBox::currentIndexChanged, &dialog, [&] { timer->start(); });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    preview();
    const int result = runPortDialog(dialog);
    timer->stop();
    session_.rollbackLayer(id, original);
    if (result == QDialog::Accepted) {
        session_.applyScanlines(settings);
        scanlinesSettings_ = settings;
    }
    session_.endEdit();
    syncDocumentViews();
}
void MainWindow::progressiveBlurDialog()
{
    if (session_.activeLayer() && session_.activeLayer()->adjustment.value("kind").toString() == "Progressive Blur") { filterLayerDialog("Progressive Blur"); return; }
    const auto *layer = session_.activeLayer();
    if (!layer || layer->image.isNull())
        return;
    const Layer original = *layer;
    const QUuid id = layer->id;
    ProgressiveBlurSettings settings;
    const auto box = layer->transform;
    settings.start = box.origin + QPointF(box.size.width() / 2, box.size.height() * .35);
    settings.end = box.origin + QPointF(box.size.width() / 2, box.size.height());
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("progressiveBlurDialog"));
    dialog.setWindowTitle(tr("Progressive Blur"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *from = new QDoubleSpinBox(&dialog), *to = new QDoubleSpinBox(&dialog);
    from->setObjectName(QStringLiteral("progressiveFrom"));
    to->setObjectName(QStringLiteral("progressiveTo"));
    for (auto *spin : {from, to})
        spin->setRange(0, 250);
    to->setValue(20);
    form->addRow(tr("From"), from);
    form->addRow(tr("To"), to);
    auto *radial = new QCheckBox(tr("Radial"), &dialog), *inverted = new QCheckBox(tr("Invert"), &dialog),
         *enabled = new QCheckBox(tr("Preview"), &dialog);
    enabled->setChecked(true);
    layout->addWidget(radial);
    layout->addWidget(inverted);
    layout->addWidget(enabled);
    auto *timer = new QTimer(&dialog);
    timer->setSingleShot(true);
    timer->setInterval(70);
    session_.beginEdit(QStringLiteral("Progressive Blur"));
    const auto preview = [&] {
        settings.from = from->value();
        settings.to = to->value();
        settings.radial = radial->isChecked();
        settings.inverted = inverted->isChecked();
        if (enabled->isChecked())
            previewFilter(&dialog, id, original, [value = settings](EditorSession &s) { return s.applyProgressiveBlur(value); });
        else
            session_.rollbackLayer(id, original);
        canvas_->invalidateDocument();
    };
    connect(timer, &QTimer::timeout, &dialog, preview);
    for (auto *spin : {from, to})
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), timer, qOverload<>(&QTimer::start));
    for (auto *check : {radial, inverted, enabled})
        connect(check, &QCheckBox::toggled, &dialog, [&] { timer->start(); });
    canvas_->setProbeMode(true);
    canvas_->probeOverlay = [&](QPainter &p, const QRectF &rect, double zoom) {
        const QPointF a = rect.topLeft() + settings.start * zoom, b = rect.topLeft() + settings.end * zoom;
        p.setPen(QPen(Qt::white, 2));
        p.setBrush(Qt::NoBrush);
        p.drawLine(a, b);
        if (settings.radial)
            p.drawEllipse(a, QLineF(a, b).length(), QLineF(a, b).length());
        p.drawEllipse(a, 5, 5);
        p.drawEllipse(b, 5, 5);
    };
    bool movesStart = false;
    const auto press = connect(canvas_, &CanvasWidget::probePressed, &dialog, [&](QPointF point) {
        const double threshold = 10 / std::max(.001, canvas_->zoom());
        const double toStart = QLineF(point, settings.start).length(),
                     toEnd = QLineF(point, settings.end).length();
        movesStart = toStart <= threshold && toStart < toEnd;
        if (toStart > threshold && toEnd > threshold) {
            settings.start = point;
            settings.end = point + QPointF(0, 1);
            movesStart = false;
        } else if (movesStart)
            settings.start = point;
        else
            settings.end = point;
        canvas_->update();
        timer->start();
    });
    const auto move = connect(canvas_, &CanvasWidget::probeMoved, &dialog, [&](QPointF point, bool dragging) {
        if (dragging) {
            if (movesStart)
                settings.start = point;
            else
                settings.end = point;
            canvas_->update();
            timer->start();
        }
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    preview();
    const int result = runPortDialog(dialog);
    timer->stop();
    disconnect(press);
    disconnect(move);
    canvas_->setProbeMode(false);
    canvas_->probeOverlay = {};
    session_.rollbackLayer(id, original);
    if (result == QDialog::Accepted)
        session_.applyProgressiveBlur(settings);
    session_.endEdit();
    syncDocumentViews();
}
} // namespace compositor

#include "io/ImageExporter.h"
#include "io/GifIO.h"
#include "io/ChannelPack.h"
#include "io/PrintExporter.h"
#include "io/HdrImage.h"
#include "io/CMYK.h"
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QPushButton>
#include <QFileInfo>
#include <QSettings>

namespace compositor
{
void MainWindow::setupCMYK(QMenu *view)
{
    auto *menu = view->addMenu(tr("CMYK"));
    auto *proof = menu->addAction(tr("Proof Colors"));
    proof->setObjectName(QStringLiteral("cmdProofColors"));
    proof->setCheckable(true);
    proof->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Y));
    addAction(proof);
    auto *gamut = menu->addAction(tr("Gamut Warning"));
    gamut->setObjectName(QStringLiteral("cmdGamutWarning"));
    gamut->setCheckable(true);
    gamut->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Y));
    addAction(gamut);
    const auto apply = [this, proof, gamut] {
        canvas_->setGamutWarning(gamut->isChecked(), proof->isChecked());
        canvas_->setProofProfile(proof->isChecked() || gamut->isChecked() ? CMYK::chosen() : std::nullopt);
    };
    connect(proof, &QAction::toggled, this, [apply] { apply(); });
    connect(gamut, &QAction::toggled, this, [apply] { apply(); });
    ShortcutManager::instance().registerAction(QStringLiteral("Menus:Proof Colors"), proof);
    ShortcutManager::instance().registerAction(QStringLiteral("Menus:Gamut Warning"), gamut);
    auto *profiles = menu->addMenu(tr("Proof Profile"));
    const auto refresh = [profiles, apply] {
        profiles->clear();
        const auto chosen = CMYK::chosen();
        for (const auto &p : CMYK::installed()) {
            auto *action = profiles->addAction(p.name);
            action->setCheckable(true);
            action->setChecked(chosen && chosen->data == p.data);
            QObject::connect(action, &QAction::triggered, profiles, [p, apply] {
                CMYK::choose(p);
                apply();
            });
        }
    };
    connect(profiles, &QMenu::aboutToShow, this, refresh);
    auto *load = menu->addAction(tr("Load Profile…"));
    load->setObjectName(QStringLiteral("cmdLoadCMYKProfile"));
    connect(load, &QAction::triggered, this, [this, apply] {
        const QString path =
            QFileDialog::getOpenFileName(this, tr("Load CMYK Profile"), {}, tr("ICC Profiles (*.icc *.icm)"));
        if (path.isEmpty())
            return;
        if (!CMYK::load(path))
            QMessageBox::warning(this, tr("CMYK Profile"), tr("Choose a valid CMYK ICC profile."));
        else
            apply();
    });
}
void MainWindow::exportAsDialog(bool jpegDefault)
{
    finishInlineText();
    if (!document_)
        return;
    // Flattened at the document's depth: in floats when it has a 16-bit or HDR layer. Written at 8 bits (dithered)
    // unless a 16-bit PNG is asked for, or an EXR, which is offered only with an HDR layer, as on macOS.
    const QImage deepOriginal = LayerRenderer::flattened(*document_);
    if (deepOriginal.isNull())
        return;
    const bool hasDeep = DeepImage::isDeep(deepOriginal);
    const bool hasHDR = std::any_of(document_->layers.cbegin(), document_->layers.cend(),
                                    [](const Layer &layer) { return DeepImage::isHDR(layer.image); });
    const QImage original = hasDeep ? DeepImage::dithered(deepOriginal) : deepOriginal;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("exportAsDialog"));
    dialog.setWindowTitle(tr("Export As"));
    dialog.resize(660, 610);
    auto *layout = new QVBoxLayout(&dialog);
    auto *preview = new ExportPreviewView(&dialog);
    preview->setObjectName(QStringLiteral("exportPreview"));
    preview->setMinimumSize(500, 250);
    preview->setDragMode(QGraphicsView::ScrollHandDrag);
    auto *scene = new QGraphicsScene(preview);
    preview->setScene(scene);
    QImage checks(16, 16, QImage::Format_RGB32);
    {
        QPainter p(&checks);
        p.fillRect(checks.rect(), QColor(228, 228, 228));
        p.fillRect(0, 0, 8, 8, QColor(188, 188, 188));
        p.fillRect(8, 8, 8, 8, QColor(188, 188, 188));
    }
    preview->setBackgroundBrush(QBrush(QPixmap::fromImage(checks)));
    layout->addWidget(preview, 1);
    auto *zoomRow = new QHBoxLayout;
    bool fitPreview = true;
    preview->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    preview->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    preview->resized = [&] {
        if (fitPreview)
            preview->fitInView(scene->sceneRect(), Qt::KeepAspectRatio);
    };
    for (const QString &label : {tr("Fit"), tr("100%"), QStringLiteral("−"), QStringLiteral("+")}) {
        auto *button = new QPushButton(label, &dialog);
        zoomRow->addWidget(button);
        connect(button, &QPushButton::clicked, &dialog, [&, label] {
            if (label == tr("Fit")) {
                fitPreview = true;
                preview->fitInView(scene->sceneRect(), Qt::KeepAspectRatio);
            } else if (label == tr("100%")) {
                fitPreview = false;
                preview->resetTransform();
            } else {
                fitPreview = false;
                preview->scale(label == QStringLiteral("+") ? 1.5 : 1 / 1.5,
                               label == QStringLiteral("+") ? 1.5 : 1 / 1.5);
            }
        });
    }
    zoomRow->addStretch();
    layout->addLayout(zoomRow);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *format = new QComboBox(&dialog);
    format->setObjectName(QStringLiteral("exportFormat"));
    enum Kind { Png, Jpeg, Pdf, Exr, Tiff, Gif, Pack };
    // JPEG and PNG first, as on macOS, then TIFF, PDF and (with an HDR layer) EXR.
    format->addItem(tr("JPEG"), Jpeg);
    format->addItem(tr("PNG"), Png);
    format->addItem(tr("TIFF"), Tiff);
    format->addItem(tr("Channel Pack (PNG)"), Pack);
    format->addItem(tr("PDF"), Pdf);
    format->addItem(tr("GIF"), Gif);
    if (hasHDR)
        format->addItem(tr("OpenEXR"), Exr);
    // The format the last export used, offered first next time; Export JPEG… always starts on JPEG.
    static int lastFormat = Png;
    const int startFormat = jpegDefault ? Jpeg : lastFormat == Exr && !hasHDR ? Png : lastFormat;
    format->setCurrentIndex(std::max(0, format->findData(startFormat)));
    const auto kind = [format] { return format->currentData().toInt(); };
    form->addRow(tr("Format"), format);
    auto *gifLoop = new QCheckBox(tr("Loop forever"), &dialog); gifLoop->setChecked(true);
    auto *gifTransparent = new QCheckBox(tr("Transparency"), &dialog); gifTransparent->setChecked(true);
    form->addRow(gifLoop); form->addRow(gifTransparent);
    std::array<QComboBox *, 4> channels{};
    const QStringList channelNames{"Red", "Green", "Blue", "Alpha"};
    for (int c = 0; c < 4; ++c) {
        auto *combo = new QComboBox(&dialog); channels[c] = combo; combo->setObjectName("pack" + channelNames[c]);
        combo->addItem(tr("Black"), QStringLiteral("black")); combo->addItem(tr("White"), QStringLiteral("white")); combo->setCurrentIndex(c == 3 ? 1 : 0);
        for (const auto &layer : document_->layers) {
            combo->addItem(layer.name, layer.id.toString());
            const QString name = layer.name.trimmed().toLower();
            if (name == channelNames[c].toLower() || name == channelNames[c].left(1).toLower()) combo->setCurrentIndex(combo->count() - 1);
        }
        form->addRow(channelNames[c], combo);
    }
    const auto pack = [&](const QSize &size) {
        std::array<ChannelSource, 4> sources;
        for (int c = 0; c < 4; ++c) { const auto id = channels[c]->currentData().toString(); sources[c] = {QUuid(id), id == "white"}; }
        return ChannelPack::render(*document_, sources, size);
    };
    auto *width = new QSpinBox(&dialog), *height = new QSpinBox(&dialog);
    width->setObjectName(QStringLiteral("exportWidth"));
    height->setObjectName(QStringLiteral("exportHeight"));
    for (auto *spin : {width, height})
        spin->setRange(1, DocumentLimits::maxSide);
    width->setValue(original.width());
    height->setValue(original.height());
    form->addRow(tr("Width"), width);
    form->addRow(tr("Height"), height);
    auto *lock = new QCheckBox(tr("Keep Aspect Ratio"), &dialog);
    lock->setChecked(true);
    auto *scale = new QComboBox(&dialog);
    scale->setObjectName(QStringLiteral("exportScale"));
    scale->setToolTip(tr("Scale the export: the document stays as it is"));
    for (int percent : {25, 50, 75, 100, 200})
        scale->addItem(QStringLiteral("%1%").arg(percent), percent);
    scale->setCurrentIndex(3);
    auto *sizeRow = new QHBoxLayout;
    sizeRow->addWidget(lock);
    sizeRow->addStretch();
    sizeRow->addWidget(scale);
    form->addRow(sizeRow);
    connect(width, qOverload<int>(&QSpinBox::valueChanged), &dialog, [&](int value) {
        if (lock->isChecked()) {
            QSignalBlocker block(height);
            height->setValue(std::max(1, qRound(double(value) * original.height() / original.width())));
        }
    });
    connect(height, qOverload<int>(&QSpinBox::valueChanged), &dialog, [&](int value) {
        if (lock->isChecked()) {
            QSignalBlocker block(width);
            width->setValue(std::max(1, qRound(double(value) * original.width() / original.height())));
        }
    });
    auto *quality = new QSpinBox(&dialog);
    quality->setObjectName(QStringLiteral("exportQuality"));
    quality->setRange(0, 100);
    // JPEG's quality from the last export, which the next one starts from.
    quality->setValue(std::clamp(QSettings().value(QStringLiteral("jpegExportQuality"), 85).toInt(), 0, 100));
    form->addRow(tr("Quality"), quality);
    // A PNG at 16 bits a channel: off by default, it's about twice the size.
    auto *sixteenBit = new QCheckBox(tr("16 bits per channel"), &dialog);
    sixteenBit->setObjectName(QStringLiteral("exportSixteenBit"));
    sixteenBit->setToolTip(tr("Keep the 16-bit layers' full precision, in a file about twice the size"));
    form->addRow(tr("Depth"), sixteenBit);
    auto *color = new QComboBox(&dialog);
    color->setObjectName(QStringLiteral("exportColor"));
    color->addItems({tr("RGB"), tr("CMYK")});
    form->addRow(tr("Color"), color);
    auto *profiles = new QComboBox(&dialog);
    auto available = CMYK::installed();
    const auto chosen = CMYK::chosen();
    if (chosen && std::none_of(available.cbegin(), available.cend(),
                               [&](const auto &p) { return p.data == chosen->data; }))
        available.prepend(*chosen);
    for (const auto &p : available)
        profiles->addItem(p.name);
    form->addRow(tr("Profile"), profiles);
    QColor matte = Qt::white;
    auto *matteButton = new QPushButton(tr("Background Color…"), &dialog);
    form->addRow(tr("Matte"), matteButton);
    auto *status = new QLabel(&dialog);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *summary = new QLabel(&dialog);
    summary->setObjectName(QStringLiteral("exportSummary"));
    layout->addWidget(summary);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *timer = new QTimer(&dialog);
    timer->setSingleShot(true);
    timer->setInterval(80);
    const auto source = [&] { return kind() == Exr || (kind() == Png && sixteenBit->isChecked()) ? deepOriginal : original; };
    const auto setRow = [form](QWidget *field, bool visible) { form->setRowVisible(field, visible); };
    QString fileSize;
    const auto refresh = [&] {
        fileSize.clear();
        const int f = kind();
        for (auto *channel : channels) setRow(channel, f == Pack);
        setRow(gifLoop, f == Gif); setRow(gifTransparent, f == Gif);
        const bool supportsCMYK = f == Jpeg || f == Pdf || f == Tiff;
        setRow(quality, f == Jpeg);
        setRow(sixteenBit, f == Png && hasDeep);
        color->setEnabled(supportsCMYK);
        if (!supportsCMYK)
            color->setCurrentIndex(0);
        profiles->setEnabled(color->currentIndex() == 1);
        quality->setEnabled(f == Jpeg);
        const bool valid = qint64(width->value()) * height->value() <= DocumentLimits::maxSurfacePixels &&
                           (color->currentIndex() != 1 || !available.isEmpty());
        buttons->button(QDialogButtonBox::Save)->setEnabled(valid);
        status->setText(
            valid ? QString()
                  : tr("Choose dimensions within the surface limit and a CMYK profile when exporting CMYK."));
        const QSize requested(width->value(), height->value());
        const QSize size = requested.width() > 8192 || requested.height() > 8192
                               ? requested.scaled(8192, 8192, Qt::KeepAspectRatio)
                               : requested;
        QImage shown = f == Pack ? pack(size) : source().scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (f == Pack) for (int y = 0; y < shown.height(); ++y) for (int x = 0; x < shown.width(); ++x) shown.scanLine(y)[x * 4 + 3] = 255;
        if (shown.isNull()) { status->setText(tr("Could not render export preview.")); buttons->button(QDialogButtonBox::Save)->setEnabled(false); return; }
        if (f == Jpeg || f == Pdf || color->currentIndex() == 1) {
            QImage flat(shown.size(), QImage::Format_RGB32);
            flat.fill(matte);
            {
                QPainter p(&flat);
                p.drawImage(0, 0, shown);
            }
            shown = flat;
        }
        if (color->currentIndex() == 1 && !available.isEmpty())
            shown = CMYK::proof(shown, available[profiles->currentIndex()]);
        if (f == Jpeg && color->currentIndex() == 0) {
            const auto encoded = ImageExporter::jpeg(shown, quality->value(), matte, document_->resolution);
            if (encoded)
                shown = encoded->preview;
            // The preview is the full export up to 8192 pixels a side, so its size is the file's.
            if (encoded && size == requested)
                fileSize = QLocale().formattedDataSize(encoded->data.size());
        }
        summary->setText(f == Exr ? tr("Brighter than white kept · Transparency kept")
                         : color->currentIndex() == 1 ? tr("CMYK – %1").arg(profiles->currentText())
                         : f == Png || f == Tiff ? tr("Transparency kept · sRGB") : tr("sRGB")
                         + (fileSize.isEmpty() ? QString() : QStringLiteral("    ") + fileSize));
        scene->clear();
        auto *item = scene->addPixmap(QPixmap::fromImage(DeepImage::dithered(shown)));
        item->setScale(double(requested.width()) / shown.width());
        scene->setSceneRect(QRectF(QPointF(), QSizeF(requested)));
        if (fitPreview)
            preview->fitInView(scene->sceneRect(), Qt::KeepAspectRatio);
        matteButton->setEnabled(f == Jpeg || f == Pdf || color->currentIndex() == 1);
        setRow(matteButton, f == Jpeg || f == Pdf || color->currentIndex() == 1);
    };
    connect(scale, &QComboBox::activated, &dialog, [&] {
        const double factor = scale->currentData().toInt() / 100.0;
        const QSignalBlocker blockWidth(width), blockHeight(height);
        width->setValue(std::max(1, qRound(original.width() * factor)));
        height->setValue(std::max(1, qRound(original.height() * factor)));
        timer->start();
    });
    for (auto *channel : channels) connect(channel, &QComboBox::currentIndexChanged, timer, qOverload<>(&QTimer::start));
    connect(sixteenBit, &QCheckBox::toggled, timer, qOverload<>(&QTimer::start));
    connect(timer, &QTimer::timeout, &dialog, refresh);
    connect(matteButton, &QPushButton::clicked, &dialog, [&] {
        const QColor color = ColorPickerDialog::getColor(matte, &dialog, tr("Export Background Color"));
        if (color.isValid()) {
            matte = color;
            timer->start();
        }
    });
    for (auto *spin : {width, height, quality})
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), timer, qOverload<>(&QTimer::start));
    for (auto *combo : {format, color, profiles})
        connect(combo, &QComboBox::currentIndexChanged, timer, qOverload<>(&QTimer::start));
    refresh();
    const int accepted = dialog.exec();
    preview->resized = {};
    if (accepted != QDialog::Accepted)
        return;
    timer->stop();
    lastFormat = kind();
    if (kind() == Jpeg)
        QSettings().setValue(QStringLiteral("jpegExportQuality"), quality->value());
    const QStringList extensions{QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("pdf"),
                                 QStringLiteral("exr"), QStringLiteral("tiff"), QStringLiteral("gif"), QStringLiteral("png")};
    const QString ext = extensions[kind()];
    // Named after the project, as on macOS.
    const QString baseName = document_->projectPath.isEmpty() ? QStringLiteral("Untitled")
                                                              : QFileInfo(document_->projectPath).completeBaseName();
    QString path = QFileDialog::getSaveFileName(this, tr("Export As"), baseName + QLatin1Char('.') + ext,
                                                tr("%1 files (*.%2)").arg(ext.toUpper(), ext));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QLatin1Char('.') + ext;
    if (kind() == Pack) {
        const auto bytes = ChannelPack::png(pack(QSize(width->value(), height->value())), document_->resolution);
        QString error;
        if (bytes.isEmpty() || !ImageExporter::writeAtomically(bytes, path, &error)) QMessageBox::warning(this, tr("Could Not Export Channel Pack"), error.isEmpty() ? tr("Could not render channel pack.") : error);
        return;
    }
    if (kind() == Gif) {
        QString error;
        if (!GifIO::write(*document_, path, QSize(width->value(), height->value()), gifLoop->isChecked(), gifTransparent->isChecked(), &error))
            QMessageBox::warning(this, tr("Could Not Export GIF"), error);
        return;
    }
    QImage image =
        source().scaled(width->value(), height->value(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const std::optional<CMYKProfile> profile =
        color->currentIndex() == 1 && !available.isEmpty()
            ? std::optional<CMYKProfile>(available[profiles->currentIndex()])
            : std::nullopt;
    if (kind() == Jpeg || kind() == Pdf || profile) {
        QImage flat(image.size(), QImage::Format_RGB32);
        flat.fill(matte);
        {
            QPainter p(&flat);
            p.drawImage(0, 0, image);
        }
        image = flat;
    }
    QString error;
    std::optional<QByteArray> bytes;
    switch (kind()) {
    case Png:
        bytes = ImageExporter::png(image, document_->resolution, &error);
        break;
    case Jpeg:
        if (profile)
            bytes = PrintExporter::jpeg(image, document_->resolution, quality->value(), *profile, &error);
        else {
            const auto jpeg =
                ImageExporter::jpeg(image, quality->value(), matte, document_->resolution, &error);
            if (jpeg)
                bytes = jpeg->data;
        }
        break;
    case Pdf:
        bytes = PrintExporter::pdf(image, document_->resolution, profile, &error);
        break;
    case Exr:
        bytes = HdrImage::encode(image, &error);
        break;
    case Tiff:
        bytes = PrintExporter::tiff(image, document_->resolution, profile, &error);
        break;
    }
    if (!bytes || !ImageExporter::writeAtomically(*bytes, path, &error))
        QMessageBox::warning(this, tr("Export Failed"), error);
}
} // namespace compositor
