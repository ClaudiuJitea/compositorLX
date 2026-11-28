// Developer tool: renders the README's screenshots from the real editor, offscreen.
//   readme_shots <output-dir> <forest-photo> <leaf-photo> [movie]
// The README uses mendhak's "Bluebells Suspended In Time" and "Red Acer" from the Ubuntu wallpapers (CC BY-SA 4.0,
// /usr/share/backgrounds). Writes volumetric-light.jpg (the editor with the Volumetric Light panel), new-filters.jpg
// (Volumetric Light and Rim Light before and after) and, given a movie, movie-layers.jpg (the movie scrubbed on the
// canvas, with its bar). The README's movie is a slow zoom over "Red Acer", made with ffmpeg's zoompan.
#include "core/EditorSession.h"
#include "rendering/RimLight.h"
#include "rendering/VolumetricLight.h"
#include "ui/CanvasWidget.h"
#include "ui/EditorStyle.h"
#include "ui/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QTest>
#include <QTimer>
#include <cstdio>

using namespace compositor;

namespace {
// A top-level window and any visible panel over it, as they'd be seen on screen.
QImage composite(QWidget &window)
{
    QImage shot = window.grab().toImage().convertToFormat(QImage::Format_RGB32);
    QPainter p(&shot);
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (w == &window || !w->isVisible() || !qobject_cast<QDialog *>(w)) continue;
        const QPixmap panel = w->grab();
        const QPoint at = w->geometry().topLeft() - window.geometry().topLeft();
        p.fillRect(QRect(at + QPoint(2, 3), panel.size() / panel.devicePixelRatio()), QColor(0, 0, 0, 90)); // a soft shadow
        p.drawPixmap(at, panel);
    }
    return shot;
}

QAction *action(QWidget &window, const QString &text)
{
    for (QAction *a : window.findChildren<QAction *>())
        if (a->text().remove(QLatin1Char('&')).startsWith(text) && a->isEnabled() && !a->menu()) return a;
    return nullptr;
}

QDialog *visibleDialog(const QString &title)
{
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *d = qobject_cast<QDialog *>(w); d && d->isVisible() && d->windowTitle() == title) return d;
    return nullptr;
}

void label(QPainter &p, const QRect &tile, const QString &text)
{
    QFont font = p.font(); font.setPixelSize(22); font.setWeight(QFont::DemiBold); p.setFont(font);
    const QRect box = QFontMetrics(font).boundingRect(text).adjusted(-12, -6, 12, 6);
    const QRect at(tile.left() + 16, tile.bottom() - 16 - box.height(), box.width(), box.height());
    p.setPen(Qt::NoPen); p.setBrush(QColor(0, 0, 0, 150)); p.drawRoundedRect(at, 8, 8);
    p.setPen(Qt::white); p.drawText(at, Qt::AlignCenter, text);
}
} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    Q_INIT_RESOURCE(resources);
    QApplication app(argc, argv);
    compositor::applyEditorTheme(app, compositor::themeModeFromKey(QStringLiteral("dark")));
    QApplication::setOrganizationName(QStringLiteral("readme-shots"));
    if (argc < 4) { std::fprintf(stderr, "usage: readme_shots <output-dir> <forest-photo> <leaf-photo> [movie]\n"); return 1; }
    const QString out = QString::fromLocal8Bit(argv[1]), forestPath = QString::fromLocal8Bit(argv[2]), leafPath = QString::fromLocal8Bit(argv[3]);
    const QString moviePath = argc > 4 ? QString::fromLocal8Bit(argv[4]) : QString();
    QDir().mkpath(out);
    const QImage forest = QImage(forestPath).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const QImage leaf = QImage(leafPath).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (forest.isNull() || leaf.isNull()) { std::fprintf(stderr, "could not read the photos\n"); return 1; }

    VolumetricLightSettings volumetric;
    volumetric.color = QColor::fromRgbF(1, .9f, .72f);
    volumetric.intensity = 65; volumetric.length = 75; volumetric.haze = 30; volumetric.size = 25; volumetric.shafts = 50;
    volumetric.distance = 55;
    const QPointF light(forest.width() * .47, forest.height() * .08); // the gap in the canopy above the path
    RimLightSettings rim;
    rim.lights[0].angle = 60; rim.lights[0].color = QColor::fromRgbF(1, .82f, .55f); rim.lights[0].strength = 100; rim.lights[0].width = 60;
    RimLight cool = rim.nextLight(); cool.color = QColor::fromRgbF(.45f, .7f, 1); cool.strength = 90; cool.width = 50;
    rim.lights.push_back(cool);
    rim.glow = 45;

    // 1. The editor with the Volumetric Light panel over the forest.
    {
        MainWindow window;
        window.resize(1866, 1000);
        window.show();
        window.importImageFiles({forestPath});
        window.session().lastVolumetricLight = volumetric;
        window.syncDocumentViews();
        QTest::qWait(800);
        QTimer::singleShot(500, &window, [&] {
            if (auto *canvas = window.findChild<CanvasWidget *>()) emit canvas->filterPointMoved(light);
            // The depth model and the first preview take a few seconds at 3840 × 2160.
            for (int i = 0; i < 120; ++i) QTest::qWait(250);
            composite(window).save(QDir(out).filePath(QStringLiteral("volumetric-light.jpg")), "JPG", 90);
            if (QDialog *d = visibleDialog(QStringLiteral("Volumetric Light"))) d->reject();
        });
        if (QAction *a = action(window, QStringLiteral("Volumetric Light"))) a->trigger();
        else { std::fprintf(stderr, "no Volumetric Light action\n"); return 2; }
    }

    // 2. Before and after: Volumetric Light on the forest, Rim Light on the leaf.
    {
        const QImage lit = VolumetricLighting::apply(forest, volumetric, light);
        QString error;
        const QImage rimmed = RimLighting::apply(leaf, rim, RenderBackend::Auto, nullptr, &error);
        if (lit.isNull() || rimmed.isNull()) { std::fprintf(stderr, "filters failed: %s\n", qPrintable(error)); return 3; }
        const QSize tile(960, 540);
        QImage sheet(tile.width() * 2 + 12, tile.height() * 2 + 12, QImage::Format_RGB32);
        sheet.fill(QColor(30, 30, 32));
        QPainter p(&sheet);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setRenderHint(QPainter::Antialiasing);
        const QImage tiles[] = {forest, lit, leaf, rimmed};
        const QString names[] = {QStringLiteral("Original"), QStringLiteral("Volumetric Light"), QStringLiteral("Original"), QStringLiteral("Rim Light")};
        for (int i = 0; i < 4; ++i) {
            const QRect at((i % 2) * (tile.width() + 12), (i / 2) * (tile.height() + 12), tile.width(), tile.height());
            p.drawImage(at, tiles[i].scaled(tile, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
            label(p, at, names[i]);
        }
        p.end();
        sheet.save(QDir(out).filePath(QStringLiteral("new-filters.jpg")), "JPG", 90);
    }

    // 3. A movie layer: the clip opened as a layer and scrubbed part way, its frame on the canvas, the movie bar under it
    // and "MOV" in the Layers panel.
    if (!moviePath.isEmpty()) {
        MainWindow window;
        window.resize(1866, 1000);
        window.show();
        if (!window.importImageFiles({moviePath})) { std::fprintf(stderr, "could not open the movie\n"); return 4; }
        window.syncDocumentViews();
        QTest::qWait(600);
        auto *playhead = window.findChild<QSlider *>(QStringLiteral("moviePlayhead"));
        if (!playhead) { std::fprintf(stderr, "no movie bar\n"); return 5; }
        playhead->setSliderDown(true);
        playhead->setValue(int(playhead->maximum() * .62));
        playhead->setSliderDown(false);
        QTest::qWait(2500);   // the exact frame is decoded and taken
        composite(window).save(QDir(out).filePath(QStringLiteral("movie-layers.jpg")), "JPG", 90);
        window.session().markSaved();
    }
    return 0;
}
