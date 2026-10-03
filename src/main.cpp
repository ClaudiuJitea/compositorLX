#include "ui/MainWindow.h"
#include "ui/EditorStyle.h"
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
    QApplication::setApplicationVersion(QStringLiteral("0.4.0"));
    QApplication::setOrganizationName(QStringLiteral("Compositor"));
    QApplication::setDesktopFileName(QStringLiteral("compositor-lx"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/compositor-lx.png")));
    application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
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
    application.setStyleSheet(compositor::editorStyleSheet());

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
    window.show();
    if (!parser.positionalArguments().isEmpty()) window.receiveFiles(parser.positionalArguments());
    return application.exec();
}
