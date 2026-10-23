#include "ui/MainWindow.h"
#include "ui/EditorStyle.h"
#include "ui/SliderJumpStyle.h"
#include "rendering/SubjectRemoval.h"
#include "rendering/GpuOperations.h"
#include "rendering/RendererSelfCheck.h"
#include "rendering/RasterOperations.h"
#include "rendering/DeepImage.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QThreadPool>
#include <QTimer>
#include <cstdio>
#include <cmath>
#include <algorithm>

#include <QApplication>
#include <QCommandLineParser>
#include <QPalette>
#include <QStyleFactory>
#include <QIcon>
#include <QPainter>

int main(int argc, char *argv[])
{
    Q_INIT_RESOURCE(resources);
    QApplication application(argc, argv);
    QObject::connect(&application,&QCoreApplication::aboutToQuit,[]{QThreadPool::globalInstance()->waitForDone();});
    QApplication::setApplicationName(QStringLiteral("CompositorLX"));
    QApplication::setApplicationVersion(QStringLiteral(COMPOSITOR_VERSION));
    QApplication::setOrganizationName(QStringLiteral("Compositor"));
    QApplication::setDesktopFileName(QStringLiteral("compositor-lx"));
    // The installed theme icon (hicolor, or one the icon theme provides), else the bundled vector one.
    QIcon fallbackIcon(QStringLiteral(":/icons/compositor-lx.svg"));
    fallbackIcon.addFile(QStringLiteral(":/icons/compositor-lx.png"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("compositor-lx"), fallbackIcon));
    compositor::applyEditorTheme(application);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Compositor image editor for Linux"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption subjectModelCheck(QStringLiteral("check-subject-model"), QStringLiteral("Validate the bundled foreground-removal model and exit."));
    parser.addOption(subjectModelCheck);
    QCommandLineOption rendererCheck(QStringLiteral("check-renderer"),QStringLiteral("Run a renderer self-check and print diagnostics as JSON."));
    parser.addOption(rendererCheck);
    parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("A .comp project directory to open, or images to import."));
    parser.process(application);

    if(parser.isSet(rendererCheck)) {
        const QJsonObject json=compositor::rendererSelfCheck();
        const auto output=QJsonDocument(json).toJson(QJsonDocument::Indented);std::fwrite(output.constData(),1,size_t(output.size()),stdout);return json.value("valid").toBool()?0:2;
    }

    if (parser.isSet(subjectModelCheck)) {
        QImage source(96, 64, QImage::Format_RGBA8888_Premultiplied); source.fill(QColor(15, 25, 35));
        QPainter painter(&source); painter.setPen(Qt::NoPen); painter.setBrush(Qt::white); painter.drawEllipse(QRect(28, 8, 40, 50)); painter.end();
        QString error; const QImage mask = compositor::SubjectRemoval::rawMask(source, &error);
        if (mask.isNull() || mask.size() != source.size()) { qCritical().noquote() << error; return 2; }
        return 0;
    }

    compositor::MainWindow window;
    window.restoreWindowState();
    window.show();
    // Compile the GPU pipelines on a worker once the window is up, so the first filter does not pay for it.
    QTimer::singleShot(0, [] { compositor::GpuRenderer::warmUp(); });
    if (!parser.positionalArguments().isEmpty()) window.receiveFiles(parser.positionalArguments());
    return application.exec();
}
