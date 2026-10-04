#include "ui/MainWindow.h"
#include "ui/EditorStyle.h"
#include "ui/SliderJumpStyle.h"
#include "rendering/SubjectRemoval.h"

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
    QApplication::setApplicationName(QStringLiteral("CompositorLX"));
    QApplication::setApplicationVersion(QStringLiteral("0.4.5"));
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
    parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("A .comp project directory to open, or images to import."));
    parser.process(application);

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
    if (!parser.positionalArguments().isEmpty()) window.receiveFiles(parser.positionalArguments());
    return application.exec();
}
