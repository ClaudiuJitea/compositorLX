#include "ui/MainWindow.h"
#include "ui/CanvasWidget.h"
#include "rendering/RgbSplit.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace compositor {
void MainWindow::filterLayerDialog(const QString &kind)
{
    const Layer *active = session_.activeLayer();
    if (!active) return;
    const bool live = active->adjustment.value("kind").toString() == kind;
    if (!live && (!session_.canPaint() || session_.isMaskSelected())) return;
    const Layer original = *active;
    const QUuid id = active->id;
    QJsonObject settings = live ? active->adjustment.value("filterLayerSettings").toObject()
                               : QJsonObject::fromVariantMap(session_.filterMemory.value(kind).toMap());
    const auto box = active->transform;
    if (!settings.contains("rgbSplitCenter")) settings.insert("rgbSplitCenter", QJsonArray{box.center().x(), box.center().y()});
    if (!settings.contains("rgbSplitReach")) settings.insert("rgbSplitReach", std::max(1.0, std::hypot(box.size.width(), box.size.height()) / 2));
    if (!settings.contains("progressiveStart")) settings.insert("progressiveStart", QJsonArray{box.center().x(), box.origin.y() + box.size.height() * .35});
    if (!settings.contains("progressiveEnd")) settings.insert("progressiveEnd", QJsonArray{box.center().x(), box.origin.y() + box.size.height()});
    QDialog dialog(this); dialog.setWindowTitle(kind); dialog.setObjectName("filterLayerDialog");
    auto *layout = new QVBoxLayout(&dialog); auto *form = new QFormLayout; layout->addLayout(form);
    auto *timer = new QTimer(&dialog); timer->setSingleShot(true); timer->setInterval(90);
    const auto number = [&](const QString &label, const QString &key, double lo, double hi, double fallback) {
        auto *field = new QDoubleSpinBox(&dialog); field->setObjectName(key); field->setDecimals(1); field->setRange(lo, hi); field->setValue(settings.value(key).toDouble(fallback)); settings.insert(key, field->value()); form->addRow(label, field);
        connect(field, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, [&, key](double v) { settings.insert(key, v); timer->start(); });
        return field;
    };
    const auto toggle = [&](const QString &label, const QString &key) {
        auto *field = new QCheckBox(label, &dialog); field->setObjectName(key); field->setChecked(settings.value(key).toBool()); form->addRow(field);
        connect(field, &QCheckBox::toggled, &dialog, [&, key](bool v) { settings.insert(key, v); timer->start(); });
    };
    const auto point = [&](const QString &label, const QString &key) {
        const auto p = settings.value(key).toArray();
        for (int axis = 0; axis < 2; ++axis) {
            auto *field = new QDoubleSpinBox(&dialog); field->setRange(-1000000, 1000000); field->setValue(p[axis].toDouble()); form->addRow(label + (axis ? tr(" Y") : tr(" X")), field);
            connect(field, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, [&, axis, key](double v) { auto p = settings.value(key).toArray(); p[axis] = v; settings.insert(key, p); timer->start(); });
        }
    };
    if (kind == "RGB Split") {
        number(tr("Distance"), "rgbSplitDistance", 0, 200, 10); number(tr("Blur streaks"), "rgbSplitBlur", 0, 400, 0);
        number(tr("Angle"), "rgbSplitAngle", -180, 180, 0); toggle(tr("Radial"), "rgbSplitRadial");
        point(tr("Center"), "rgbSplitCenter"); number(tr("Reach"), "rgbSplitReach", 1, 1000000, 1000);
        if (!live) {
            auto *split = new QPushButton(tr("Split to Layers"), &dialog); split->setObjectName("splitToLayers"); layout->addWidget(split);
            connect(split, &QPushButton::clicked, &dialog, [&] { dialog.done(2); });
        }
    } else if (kind == "Bloom / Glow") {
        number(tr("Amount"), "bloomAmount", 0, 100, 40); number(tr("Radius"), "bloomRadius", 1, 150, 24);
    } else {
        number(tr("From"), "progressiveFrom", 0, 250, 0); number(tr("To"), "progressiveTo", 0, 250, 20);
        toggle(tr("Radial"), "progressiveRadial"); toggle(tr("Invert"), "progressiveInverted");
        point(tr("Start"), "progressiveStart"); point(tr("End"), "progressiveEnd");
    }
    auto *enabled = new QCheckBox(tr("Preview"), &dialog); enabled->setChecked(true); layout->addWidget(enabled);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    session_.beginEdit(live ? tr("Edit %1").arg(kind) : kind);
    const auto rgbSplit = [&] {
        RgbSplitSettings s; s.distance = settings.value("rgbSplitDistance").toDouble(); s.blur = settings.value("rgbSplitBlur").toDouble();
        s.angle = settings.value("rgbSplitAngle").toDouble(); s.radial = settings.value("rgbSplitRadial").toBool(); s.reach = settings.value("rgbSplitReach").toDouble();
        const auto p = settings.value("rgbSplitCenter").toArray(); s.center = QPointF(p[0].toDouble(), p[1].toDouble());
        return s;
    };
    const auto apply = [&] {
        if (live) { auto adjustment = original.adjustment; adjustment.insert("filterLayerSettings", settings); session_.previewAdjustment(id, adjustment); }
        else if (kind == "RGB Split") session_.applyRgbSplit(rgbSplit());
    };
    const auto preview = [&] {
        if (!enabled->isChecked()) session_.rollbackLayer(id, original);
        else if (!live && kind == "RGB Split") previewFilter(&dialog, id, original, [value = rgbSplit()](EditorSession &s) { return s.applyRgbSplit(value); });
        else { session_.rollbackLayer(id, original); apply(); }
        canvas_->invalidateDocument();
    };
    connect(timer, &QTimer::timeout, &dialog, preview); connect(enabled, &QCheckBox::toggled, timer, qOverload<>(&QTimer::start));
    preview();
    const int result = runPortDialog(dialog);
    timer->stop(); session_.rollbackLayer(id, original);
    if (result == QDialog::Accepted) { apply(); if (!live) session_.filterMemory.insert(kind, settings.toVariantMap()); }
    session_.endEdit();
    if (result == 2) session_.splitChannels();
    syncDocumentViews();
}
}
