#include "ui/RenderingSettingsDialog.h"
#include "rendering/GpuRenderer.h"
#include "rendering/SubjectRemoval.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace compositor {
bool showRenderingSettings(QWidget *parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Rendering"));
    dialog.setObjectName("renderingSettingsDialog");
    dialog.resize(520, 360);
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(QObject::tr("Choose how image effects are rendered. Automatic uses the GPU where it "
                                               "helps and keeps CPU fallback available."),
                                   &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *form = new QFormLayout;
    auto *backend = new QComboBox(&dialog);
    backend->setObjectName("renderingBackend");
    backend->addItems({QObject::tr("Automatic"), QObject::tr("CPU"), QObject::tr("GPU (with CPU fallback)")});
    backend->setCurrentIndex(int(GpuRenderer::backend()));
    form->addRow(QObject::tr("Renderer"), backend);
    auto *memory = new QComboBox(&dialog);
    memory->setObjectName("renderingMemoryBudget");
    for (int size : {64, 128, 256, 512, 1024, 2048})
        memory->addItem(QObject::tr("%1 MiB").arg(size), size);
    memory->setCurrentIndex(memory->findData(GpuRenderer::memoryBudgetMiB()));
    form->addRow(QObject::tr("GPU working memory"), memory);
    layout->addLayout(form);
    if (qEnvironmentVariableIsSet("COMPOSITOR_RENDER_BACKEND") ||
        qEnvironmentVariableIsSet("COMPOSITOR_GPU_MEMORY_MIB")) {
        auto *override = new QLabel(
            QObject::tr(
                "An environment override is active. Saved preferences take effect when that override is removed."),
            &dialog);
        override->setWordWrap(true);
        layout->addWidget(override);
    }
    const auto diagnostics = [] {
        return GpuRenderer::diagnostics() +
               QObject::tr("\nRemove Background runtime providers: %1").arg(SubjectRemoval::executionProviders());
    };
    auto *status = new QPlainTextEdit(diagnostics(), &dialog);
    status->setObjectName("renderingDiagnostics");
    status->setReadOnly(true);
    layout->addWidget(status);
    auto *refresh = new QPushButton(QObject::tr("Refresh diagnostics"), &dialog);
    layout->addWidget(refresh);
    QObject::connect(refresh, &QPushButton::clicked, &dialog,
                     [status, diagnostics] { status->setPlainText(diagnostics()); });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    GpuRenderer::setBackend(RenderBackend(backend->currentIndex()));
    GpuRenderer::setMemoryBudgetMiB(memory->currentData().toInt());
    return true;
}
} // namespace compositor
