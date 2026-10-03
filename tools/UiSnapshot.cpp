// Developer tool: renders the main window offscreen under several tools and writes PNGs, so UI polish is judged on
// what is actually drawn.   QT_QPA_PLATFORM=offscreen ui_snapshot <output-dir> [device-pixel-ratio]
#include "core/EditorSession.h"
#include "ui/EditorStyle.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QAction>
#include <QDialog>
#include <QDir>
#include <QTimer>
#include <QAbstractItemView>
#include <QPainter>
#include <QTest>
#include <QToolButton>

using namespace compositor;

static QImage gradientLayer(int w, int h, QColor a, QColor b)
{
    QImage image(w, h, QImage::Format_RGBA8888_Premultiplied);
    QPainter p(&image);
    QLinearGradient g(0, 0, w, h); g.setColorAt(0, a); g.setColorAt(1, b);
    p.fillRect(image.rect(), g);
    return image;
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    if (argc > 2) qputenv("QT_SCALE_FACTOR", argv[2]);
    Q_INIT_RESOURCE(resources);
    QApplication app(argc, argv);
    compositor::applyEditorTheme(app);
    QApplication::setOrganizationName(QStringLiteral("ui-snapshot"));
    const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    QDir().mkpath(out);

    MainWindow window;
    window.resize(1600, 900);
    window.show();
    EditorSession &s = window.session();
    s.createDocument(1600, 900, false);
    s.insertImage(gradientLayer(1600, 900, QColor(60, 30, 120), QColor(230, 120, 60)), QStringLiteral("Sky"));
    s.insertImage(gradientLayer(800, 400, QColor(30, 110, 190), QColor(20, 40, 90)), QStringLiteral("Mountains"));
    s.addLayerMask(true, false);
    s.insertImage(gradientLayer(300, 300, QColor(240, 220, 200), QColor(200, 120, 100)), QStringLiteral("Sun"));
    s.addGroup();
    s.addBlankLayer();
    window.syncDocumentViews();
    QApplication::processEvents();

    const auto grab = [&](const QString &name) {
        QTest::qWait(400);   // thumbnails and the canvas render in the background
        window.grab().save(QDir(out).filePath(name + QStringLiteral(".png")));
    };
    QAbstractItemView *layerList = nullptr;
    for (auto *candidate : window.findChildren<QAbstractItemView *>()) if (candidate->objectName() == QLatin1String("layerList") || candidate->model()->metaObject()->className() == QLatin1String("compositor::LayerListModel")) { layerList = candidate; break; }
    if (auto *view = layerList; view && view->model()) {
        for (int row = 0; row < view->model()->rowCount(); ++row) {
            const QIcon icon = qvariant_cast<QIcon>(view->model()->index(row, 0).data(Qt::DecorationRole));
            qInfo() << "row" << row << view->model()->index(row, 0).data().toString() << "icon null:" << icon.isNull()
                    << "sizes:" << icon.availableSizes();
            if (!icon.isNull()) icon.pixmap(36, 36).save(QDir(out).filePath(QStringLiteral("thumb-%1.png").arg(row)));
        }
    }
    const auto rail = window.findChild<QWidget *>(QStringLiteral("toolRail"));
    const auto buttons = rail ? rail->findChildren<QToolButton *>() : QList<QToolButton *>();
    const std::pair<int, const char *> tools[] = {{0, "move"}, {1, "marquee"}, {4, "crop"}, {5, "brush"}, {8, "blur"}, {9, "gradient"}, {10, "shape"}, {11, "type"}, {14, "zoom"}};
    for (const auto &[index, name] : tools) {
        if (index < buttons.size()) buttons.at(index)->click();
        grab(QStringLiteral("tool-") + QLatin1String(name));
    }
    // A mask targeted with the Brush: the black/white mask palette control.
    for (const Layer &layer : s.document()->layers)
        if (layer.name == QLatin1String("Mountains")) { s.selectLayer(layer.id); s.selectMaskTarget(true); break; }
    window.syncDocumentViews();
    if (5 < buttons.size()) buttons.at(5)->click();
    grab(QStringLiteral("brush-on-mask"));
    // Effects and live text on other layers.
    for (const Layer &layer : s.document()->layers)
        if (layer.name == QLatin1String("Sun")) { s.selectLayer(layer.id); s.selectMaskTarget(false); s.addLayerEffect(layer.id, LayerEffectKind::DropShadow); s.addLayerEffect(layer.id, LayerEffectKind::Stroke); break; }
    TextStyle text; text.content = QStringLiteral("CompositorLX"); text.fontName = QStringLiteral("Helvetica"); text.fontSize = 96; text.red = text.green = text.blue = 1;
    s.addText(text, QRectF(120, 120, 900, 200), false, false, false, false);
    window.syncDocumentViews();
    if (0 < buttons.size()) buttons.at(0)->click();
    grab(QStringLiteral("layers-effects-text"));
    // Dialogs: open each through its menu action, grab it while it runs, close it.
    for (const Layer &layer : s.document()->layers) if (layer.name == QLatin1String("Sky")) { s.selectLayer(layer.id); s.selectMaskTarget(false); break; }
    s.deselect();
    window.syncDocumentViews();
    const char *dialogs[] = {"commandGridSettings", "commandColorRange", "cmdDither", "cmdLevels", "cmdHueSaturation", "cmdGaussianBlur", "cmdCameraRaw", "cmdBlackWhite"};
    for (const char *name : dialogs) {
        QAction *action = window.findChild<QAction *>(QLatin1String(name));
        if (!action) { qWarning() << "no action" << name; continue; }
        QTimer::singleShot(1200, [&, name] {
            QWidget *dialog = nullptr;
            for (QWidget *top : QApplication::topLevelWidgets()) if (top->isVisible() && top != &window && qobject_cast<QDialog *>(top)) dialog = top;
            if (dialog) { QTest::qWait(300);
            else qWarning() << "no dialog for" << name;
        });
        action->trigger();
        QApplication::processEvents();
    }
    return 0;
}
