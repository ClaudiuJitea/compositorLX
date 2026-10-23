#include "ui/RenderingSettingsDialog.h"
#include "rendering/GpuRenderer.h"
#include "rendering/RendererSelfCheck.h"
#include "rendering/SubjectRemoval.h"
#include "ui/GpuViewport.h"
#include "ui/PanelDialog.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QSettings>
#include <QtConcurrent>

namespace compositor {
namespace {
QString diagnosticsText() {
    return GpuRenderer::diagnostics() +
           QObject::tr("\nRemove Background runtime providers: %1").arg(SubjectRemoval::executionProviders());
}
// Shows `work()`'s text in `view` once it is ready, without blocking the dialog (it may open the GPU device).
void loadAsync(QWidget *owner, QPlainTextEdit *view, std::function<QString()> work, bool append = false) {
    auto *watcher = new QFutureWatcher<QString>(owner);
    QObject::connect(watcher, &QFutureWatcherBase::finished, owner, [watcher, view, append] {
        if (append)
            view->appendPlainText(watcher->result());
        else
            view->setPlainText(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run(std::move(work)));
}
} // namespace

bool showRenderingSettings(QWidget *parent) {
    PanelDialog dialog(parent, QObject::tr("Rendering"));
    dialog.setProperty("panelKind", QStringLiteral("rendering")); // Keeps the messageDialog style name.
    dialog.panel()->setMinimumWidth(560);
    dialog.addLabel(QObject::tr("Rendering"), QStringLiteral("messageTitle"), 0);
    dialog.addLabel(QObject::tr("Choose how image effects and the canvas are rendered. Automatic uses the GPU where "
                                "it measurably helps; every GPU path keeps a CPU fallback. Exports, merges and other "
                                "committed results always match the CPU reference unless GPU is chosen."),
                    QStringLiteral("messageText"), 8);
    auto *form = new QFormLayout;
    dialog.content()->addSpacing(14);
    dialog.content()->addLayout(form);
    QStringList overrides;

    // The saved preference is shown and saved; an environment override is reported and never written back (A7).
    auto *backend = new QComboBox(dialog.panel());
    backend->setObjectName("renderingBackend");
    backend->addItems({QObject::tr("Automatic"), QObject::tr("CPU"), QObject::tr("GPU (with CPU fallback)")});
    backend->setCurrentIndex(int(GpuRenderer::savedBackend()));
    if (GpuRenderer::backendOverridden()) {
        backend->setEnabled(false);
        overrides << QStringLiteral("COMPOSITOR_RENDER_BACKEND=%1").arg(qEnvironmentVariable("COMPOSITOR_RENDER_BACKEND"));
    }
    form->addRow(QObject::tr("Renderer"), backend);

    auto *memory = new QComboBox(dialog.panel());
    memory->setObjectName("renderingMemoryBudget");
    for (int size : {64, 128, 256, 512, 1024, 2048})
        memory->addItem(QObject::tr("%1 MiB").arg(size), size);
    const int savedMemory = GpuRenderer::savedMemoryBudgetMiB();
    if (memory->findData(savedMemory) < 0)
        memory->addItem(QObject::tr("%1 MiB").arg(savedMemory), savedMemory);
    memory->setCurrentIndex(memory->findData(savedMemory));
    if (GpuRenderer::memoryBudgetOverridden()) {
        memory->setEnabled(false);
        overrides << QStringLiteral("COMPOSITOR_GPU_MEMORY_MIB=%1").arg(qEnvironmentVariable("COMPOSITOR_GPU_MEMORY_MIB"));
    }
    form->addRow(QObject::tr("GPU working memory"), memory);

    auto *canvas = new QComboBox(dialog.panel());
    canvas->setObjectName("renderingCanvas");
    canvas->addItems({QObject::tr("Automatic (OpenGL on Mesa and Intel drivers)"), QObject::tr("Raster (CPU)"),
                      QObject::tr("OpenGL")});
    canvas->setCurrentIndex(int(savedCanvasPresentation()));
    if (canvasPresentationOverridden()) {
        canvas->setEnabled(false);
        overrides << QStringLiteral("COMPOSITOR_CANVAS_BACKEND=%1").arg(qEnvironmentVariable("COMPOSITOR_CANVAS_BACKEND"));
    }
    form->addRow(QObject::tr("Canvas presentation"), canvas);

    // OD6: integrated GPU first on hybrid laptops; a discrete GPU can be chosen.
    auto *device = new QComboBox(dialog.panel());
    device->setObjectName("renderingDevice");
    device->addItem(QObject::tr("Automatic (integrated GPU first)"), QString());
    const QString savedDevice = QSettings().value("rendering/device").toString();
    const auto info = vulkanDeviceInfo(false);
    for (const QString &name : info.devices)
        device->addItem(name, name);
    if (!savedDevice.isEmpty() && device->findData(savedDevice) < 0)
        device->addItem(savedDevice, savedDevice);
    device->setCurrentIndex(std::max(0, device->findData(savedDevice)));
    if (qEnvironmentVariableIsSet("COMPOSITOR_GPU_DEVICE")) {
        device->setEnabled(false);
        overrides << QStringLiteral("COMPOSITOR_GPU_DEVICE=%1").arg(qEnvironmentVariable("COMPOSITOR_GPU_DEVICE"));
    }
    if (device->count() > 2 || !savedDevice.isEmpty())
        form->addRow(QObject::tr("GPU device"), device);
    else
        device->hide();

    if (!overrides.isEmpty()) {
        auto *note = dialog.addLabel(QObject::tr("An environment override is active (%1). The settings it replaces "
                                                 "are shown as saved and take effect once it is removed.")
                                         .arg(overrides.join(QStringLiteral(", "))),
                                     QStringLiteral("messageText"), 10);
        note->setObjectName("renderingOverride");
    }

    dialog.content()->addSpacing(12);
    auto *status = new QPlainTextEdit(QObject::tr("Loading diagnostics…"), dialog.panel());
    status->setObjectName("renderingDiagnostics");
    status->setReadOnly(true);
    status->setMinimumHeight(200);
    dialog.content()->addWidget(status);
    loadAsync(&dialog, status, diagnosticsText);
    auto *tools = new QHBoxLayout;
    auto *refresh = new QPushButton(QObject::tr("Refresh"), dialog.panel());
    refresh->setObjectName("renderingRefresh");
    auto *check = new QPushButton(QObject::tr("Run self-check"), dialog.panel());
    check->setObjectName("renderingSelfCheck");
    auto *copy = new QPushButton(QObject::tr("Copy diagnostics"), dialog.panel());
    copy->setObjectName("renderingCopyDiagnostics");
    for (auto *button : {refresh, check, copy}) {
        button->setAutoDefault(false);
        tools->addWidget(button);
    }
    tools->addStretch();
    dialog.content()->addSpacing(12);
    dialog.content()->addLayout(tools);
    QObject::connect(refresh, &QPushButton::clicked, &dialog, [&dialog, status] { loadAsync(&dialog, status, diagnosticsText); });
    QObject::connect(check, &QPushButton::clicked, &dialog, [&dialog, status] {
        status->appendPlainText(QObject::tr("\nRunning self-check…"));
        loadAsync(
            &dialog, status,
            [] { return QString::fromUtf8(QJsonDocument(rendererSelfCheck()).toJson(QJsonDocument::Indented)); }, true);
    });
    QObject::connect(copy, &QPushButton::clicked, &dialog,
                     [status] { QApplication::clipboard()->setText(status->toPlainText()); });
    dialog.addButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, QDialogButtonBox::Ok);
    const int result = dialog.exec();
    if (result != QDialog::Accepted && result != int(QDialogButtonBox::Ok))
        return false;
    if (backend->isEnabled())
        GpuRenderer::setBackend(RenderBackend(backend->currentIndex()));
    if (memory->isEnabled())
        GpuRenderer::setMemoryBudgetMiB(memory->currentData().toInt());
    if (canvas->isEnabled())
        setCanvasPresentation(CanvasPresentation(canvas->currentIndex()));
    if (device->isEnabled() && device->currentData().toString() != savedDevice) {
        QSettings().setValue("rendering/device", device->currentData().toString());
        GpuRenderer::release(); // The next GPU graph opens the chosen device.
    }
    return true;
}
} // namespace compositor
