#include "core/EditorSession.h"
#include "core/GifPlayback.h"
#include "io/GifIO.h"
#include "io/ProjectReader.h"
#include "io/ProjectWriter.h"
#include "rendering/DeepImage.h"
#include "rendering/LensEffects.h"
#include "rendering/RgbSplit.h"
#include "ui/GifTimeline.h"
#include "ui/MainWindow.h"
#include "ui/SegmentedControl.h"
#include <QtTest>
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <gif_lib.h>

using namespace compositor;
// Compositor 1.5.7 and 1.5.8: Filter › Lens Effects (LensEffectsTests.swift, with the Mac's coordinates and limits),
// RGB Split's centered smear, GIF playback on a schedule (GIFPreviewPlaybackTests.swift), one GIF reader kept open, the
// delay label's milliseconds, and the project versions the reader accepts (ProjectTests.swift).
class TestUpstream158 : public QObject {
    Q_OBJECT
    static constexpr int width = 400, height = 240;

    // A dark picture with one small white light in the middle (LensEffectsTests.night).
    static QImage night()
    {
        QImage image(width, height, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor::fromRgbF(.05f, .06f, .1f));
        QPainter p(&image); p.fillRect(QRect(190, 110, 20, 20), Qt::white);
        return image;
    }
    static QImage scene(QColor background, QRect light, QColor color)
    {
        QImage image(width, height, QImage::Format_RGBA8888_Premultiplied);
        image.fill(background);
        QPainter p(&image); p.fillRect(light, color);
        return image;
    }
    static QImage run(const std::function<void(LensEffectSettings &)> &change, const QImage &image = night())
    {
        LensEffectSettings settings; change(settings);
        return settings.apply(image);
    }
    // Red, green and blue at a pixel, rows top-down.
    static std::array<int, 3> color(const QImage &image, int x, int y)
    {
        const QColor c = image.convertToFormat(QImage::Format_RGBA8888).pixelColor(x, y);
        return {c.red(), c.green(), c.blue()};
    }
    static QString text(const std::array<int, 3> &c) { return QStringLiteral("(%1, %2, %3)").arg(c[0]).arg(c[1]).arg(c[2]); }

    // GIFPreviewPlaybackTests' TestClock: sleeping until a deadline moves the clock there (never back), and showing a
    // frame takes `decode` microseconds. Stops after `limit` frames, or when playing once ends.
    struct Played { QVector<int> frames; QVector<qint64> times; qint64 now = 0; int sleeps = 0; };
    static Played play(const QVector<qint64> &durations, int initial, bool loops, qint64 decode, int limit = 100)
    {
        Played r;
        GifPlayback playback(durations, initial, loops);
        if (!playback.valid()) return r;
        while (r.sleeps < 100) {
            ++r.sleeps;
            r.now = std::max(r.now, playback.next());
            const auto step = playback.advance(r.now);
            if (step.show >= 0) {
                r.frames.append(step.show); r.times.append(r.now); r.now += decode;
                if (r.frames.size() == limit) break;
            }
            if (step.finished) break;
        }
        return r;
    }
    static QVector<qint64> micro(std::initializer_list<double> seconds)
    {
        QVector<qint64> out;
        for (double s : seconds) out.append(qint64(std::llround(s * 1e6)));
        return out;
    }

    // A 16 × 16 GIF whose later frames are only the 4 × 4 patch that changed (disposal: keep), 40 ms each.
    static QString partialGif(QTemporaryDir &dir)
    {
        const QString path = dir.filePath(QStringLiteral("partial.gif"));
        int error = 0;
        GifFileType *gif = EGifOpenFileName(path.toLocal8Bit().constData(), false, &error);
        if (!gif) return {};
        GifColorType colors[4] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
        ColorMapObject *map = GifMakeMapObject(4, colors);
        EGifSetGifVersion(gif, true);
        EGifPutScreenDesc(gif, 16, 16, 2, 0, map);
        const auto frame = [&](int left, int top, int side, GifPixelType index) {
            GraphicsControlBlock control{DISPOSE_DO_NOT, false, 4, NO_TRANSPARENT_COLOR};
            GifByteType extension[4];
            EGifGCBToExtension(&control, extension);
            EGifPutExtension(gif, GRAPHICS_EXT_FUNC_CODE, 4, extension);
            EGifPutImageDesc(gif, left, top, side, side, false, nullptr);
            std::vector<GifPixelType> line(size_t(side), index);
            for (int y = 0; y < side; ++y) EGifPutLine(gif, line.data(), side);
        };
        frame(0, 0, 16, 0);
        frame(0, 0, 4, 1);
        frame(6, 6, 4, 2);
        frame(12, 12, 4, 3);
        EGifCloseFile(gif, &error);
        GifFreeMapObject(map);
        return path;
    }
    static QString colorsGif(QTemporaryDir &dir)
    {
        const QString path = dir.filePath("colors.gif"); QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(QByteArray::fromBase64("R0lGODlhCAAIAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQIBAAAACwAAAAACAAIAAAIDwABCBxIsKDBgwgTKkwYEAAh+QQICAAAACwAAAAACAAIAIEAgAAAAAAAAAAAAAAIDwABCBxIsKDBgwgTKkwYEAAh+QQIDAAAACwAAAAACAAIAIEAAP8AAAAAAAAAAAAIDwABCBxIsKDBgwgTKkwYEAA7"));
        return path;
    }

private slots:
    void initTestCase() { QCoreApplication::setOrganizationName("compositor-tests"); QCoreApplication::setApplicationName("upstream158"); }

    // ---- Lens Effects (6edbaba, 393bcd7, c7ff16d, 97d28a3) -------------------------------------------------------

    void anamorphicFlareStreaksSidewaysInBlue()
    {
        const QImage original = night(), flared = run([](auto &s) { s.effect = LensEffect::AnamorphicFlare; s.flareAmount = 80; });
        const auto beside = color(flared, 300, 120), above = color(flared, 200, 30);
        QVERIFY2(beside[2] > color(original, 300, 120)[2] + 10, qPrintable("the streak reaches across: " + text(beside)));
        QVERIFY2(beside[2] > beside[0], qPrintable("and is blue: " + text(beside)));
        QVERIFY2(above == color(original, 200, 30), qPrintable("but not up: " + text(above)));
    }

    void anamorphicFlareKeepsTheLightsColorWithoutBlue()
    {
        // A light just short of white, red-tinged, as a hot tail light is.
        const QImage light = scene(QColor::fromRgbF(.02f, .02f, .03f), QRect(192, 112, 16, 16), QColor::fromRgbF(1, .95f, .95f));
        const auto beside = color(run([](auto &s) { s.effect = LensEffect::AnamorphicFlare; s.flareBlue = 0; s.flareAmount = 80; }, light), 280, 120);
        QVERIFY2(beside[0] > 20 && beside[0] >= beside[2], qPrintable("a streak the light's color, not blue: " + text(beside)));
    }

    void anamorphicThresholdDecidesWhichLightsFlare()
    {
        // A light short of white, as a backlight or a bulb in daylight is.
        const QImage light = scene(QColor::fromRgbF(.02f, .02f, .03f), QRect(192, 112, 16, 16), QColor::fromRgbF(.85f, .85f, .85f));
        const auto streak = [&](double threshold) {
            return color(run([threshold](auto &s) { s.effect = LensEffect::AnamorphicFlare; s.threshold = threshold; }, light), 280, 120)[2]
                   - color(light, 280, 120)[2];
        };
        QVERIFY2(streak(75) > 8, "below the light: it flares");
        QVERIFY2(streak(95) <= 1, "above it: nothing");
    }

    // c7ff16d: past 95, Threshold weighs how big and solid a light is, down to the strongest one or two.
    void thresholdPastNinetyFiveKeepsOnlyTheStrongestLight()
    {
        QImage image = scene(QColor::fromRgbF(.02f, .02f, .03f), QRect(40, 100, 40, 40), Qt::white);
        { QPainter p(&image); p.fillRect(QRect(300, 30, 4, 4), Qt::white); }
        const auto blue = [&](double threshold, int x, int y) {
            return color(run([threshold](auto &s) { s.effect = LensEffect::AnamorphicFlare; s.threshold = threshold; s.flareAmount = 100; }, image), x, y)[2]
                   - color(image, x, y)[2];
        };
        const int smallAt90 = blue(90, 340, 32), smallAt100 = blue(100, 340, 32);
        QVERIFY2(smallAt90 > 4, qPrintable(QString::number(smallAt90)));
        QVERIFY2(smallAt100 * 4 < smallAt90, qPrintable(QStringLiteral("%1 vs %2").arg(smallAt100).arg(smallAt90)));
        QVERIFY2(blue(100, 160, 120) > 8, "the big light still flares");
        LensEffectSettings s; s.threshold = 120; QCOMPARE(s.normalized().threshold, 100.0);
    }

    void starburstSpikesAlongItsAngle()
    {
        const QImage original = night();
        const QImage starred = run([](auto &s) { s.effect = LensEffect::Starburst; s.starAngle = 0; s.starPoints = 4; s.starAmount = 100; });
        // Four points at 0°: spikes left, right, up and down; none on the diagonal.
        QVERIFY(color(starred, 240, 120)[1] > color(original, 240, 120)[1] + 10);
        QVERIFY(color(starred, 200, 70)[1] > color(original, 200, 70)[1] + 10);
        QVERIFY(color(starred, 240, 70)[1] <= color(original, 240, 70)[1] + 2);
    }

    // Core Image's angles turn counterclockwise with y up; on screen a 45° spike runs up and to the right.
    void starburstAngleTurnsCounterclockwiseOnScreen()
    {
        const QImage original = night();
        const QImage starred = run([](auto &s) { s.effect = LensEffect::Starburst; s.starAngle = 45; s.starPoints = 4; s.starAmount = 100; });
        QVERIFY(color(starred, 240, 80)[1] > color(original, 240, 80)[1] + 10);
        QVERIFY(color(starred, 160, 80)[1] > color(original, 160, 80)[1] + 10); // the second spoke, at 135°
        QVERIFY(color(starred, 260, 120)[1] <= color(original, 260, 120)[1] + 2);
    }

    void mistGlowsAroundTheLightWithoutBrighteningThePicture()
    {
        const QImage original = night(), misted = run([](auto &s) { s.effect = LensEffect::Mist; s.mistStrength = 80; });
        QVERIFY2(color(misted, 225, 120)[1] > color(original, 225, 120)[1] + 5, "a halo beside the light");
        QVERIFY2(color(misted, 10, 10)[1] <= color(original, 10, 10)[1] + 3, "far corners barely lift");
    }

    void prismGhostsASharpCopy()
    {
        const QImage original = night();
        // Pushed left by a quarter of the width: the light's copy lands 100 pixels left of it, and stays crisp.
        const QImage ghosted = run([](auto &s) { s.effect = LensEffect::Prism; s.prismAngle = 180; s.prismOffset = 25; s.prismCopies = 1; s.prismAmount = 100; });
        QVERIFY(color(ghosted, 100, 120)[1] > color(original, 100, 120)[1] + 20);
        QVERIFY2(color(ghosted, 70, 120) == color(original, 70, 120), "a sharp copy, not a smear");
        QVERIFY2(color(ghosted, 300, 120) == color(original, 300, 120), "the other side is untouched");
    }

    void prismBothWaysEchoesOnEachSide()
    {
        const QImage original = night();
        const auto echo = [&](bool bothWays) {
            const QImage image = run([bothWays](auto &s) { s.effect = LensEffect::Prism; s.prismAngle = 180; s.prismOffset = 25; s.prismCopies = 1; s.prismAmount = 100; s.prismBothWays = bothWays; });
            return std::pair{color(image, 100, 120)[1] - color(original, 100, 120)[1], color(image, 300, 120)[1] - color(original, 300, 120)[1]};
        };
        const auto one = echo(false), both = echo(true);
        QVERIFY(one.first > 20 && one.second == 0);
        QVERIFY(both.first > 20 && both.second > 20);
    }

    void prismCopiesEchoFartherOut()
    {
        const QImage original = night();
        // Copies 40 pixels apart, leftwards from the light at 190–210: the third lands around 80–90.
        const auto echo = [&](double copies) {
            return color(run([copies](auto &s) { s.effect = LensEffect::Prism; s.prismCopies = copies; s.prismOffset = 10; s.prismAmount = 100; }), 85, 120)[1]
                   - color(original, 85, 120)[1];
        };
        QVERIFY(echo(1) <= 2);
        QVERIFY(echo(3) > 10);
    }

    // Prism at 90° steps up the screen, as Core Image's angle does with y up; Blur softens the copy.
    void prismAngleAndBlur()
    {
        const QImage original = night();
        const QImage up = run([](auto &s) { s.effect = LensEffect::Prism; s.prismAngle = 90; s.prismOffset = 20; s.prismCopies = 1; s.prismAmount = 100; });
        QVERIFY(color(up, 200, 40)[1] > color(original, 200, 40)[1] + 20);
        QVERIFY(color(up, 200, 200) == color(original, 200, 200));
        const QImage soft = run([](auto &s) { s.effect = LensEffect::Prism; s.prismAngle = 180; s.prismOffset = 25; s.prismCopies = 1; s.prismAmount = 100; s.prismBlur = 100; });
        QVERIFY2(color(soft, 85, 120)[1] > color(original, 85, 120)[1] + 2, "blurred, the copy spreads past its edge");
    }

    void bokehOpensTheLightIntoABrightDisc()
    {
        const QImage original = night();
        // A radius of 3% of 400 pixels, 12: inside it the light spreads evenly; well past it, nothing.
        const QImage blurred = run([](auto &s) { s.effect = LensEffect::Bokeh; s.bokehRadius = 3; s.bokehRing = 0; });
        QVERIFY2(color(blurred, 200, 135)[1] > color(original, 200, 135)[1] + 40, "the disc reaches past the light");
        QVERIFY2(color(blurred, 200, 160)[1] <= color(original, 200, 160)[1] + 2, "but not far past its radius");
    }

    void ovalBokehIsTallerThanWide()
    {
        const QImage original = night();
        const QImage oval = run([](auto &s) { s.effect = LensEffect::Bokeh; s.bokehRadius = 4; s.bokehShape = BokehShape::Oval; });
        // Radius 16 from a 20-pixel light at 190–210, 110–130: 34 pixels out sideways is past it, but not upwards.
        const int up = color(oval, 200, 120 - 34)[1] - color(original, 200, 120 - 34)[1];
        const int side = color(oval, 200 + 34, 120)[1] - color(original, 200 + 34, 120)[1];
        QVERIFY2(up > side + 10, qPrintable(QStringLiteral("up %1, sideways %2").arg(up).arg(side)));
    }

    // A big disc runs on a smaller copy (macOS MetalBokeh.largestRadius), every shape included.
    void largeBokehShapesStayBounded()
    {
        const QImage original = night();
        for (BokehShape shape : LensEffectSettings::shapes) {
            const QImage image = run([shape](auto &s) { s.effect = LensEffect::Bokeh; s.bokehRadius = 6; s.bokehShape = shape; });
            QCOMPARE(image.size(), original.size());
            QVERIFY(color(image, 200, 150)[1] > color(original, 200, 150)[1] + 10);
            QVERIFY(color(image, 10, 10)[1] <= color(original, 10, 10)[1] + 2);
        }
    }

    void prismWithoutOffsetStillApplies()
    {
        LensEffectSettings settings; settings.effect = LensEffect::Prism; settings.prismOffset = 0;
        // Its copies still fringe the picture, as the preview shows, so OK has to keep them.
        QVERIFY(!settings.isIdentity());
    }

    void noAmountIsNothingToDo()
    {
        for (LensEffect effect : LensEffectSettings::effects) {
            LensEffectSettings settings; settings.effect = effect;
            settings.flareAmount = 0; settings.starAmount = 0; settings.mistStrength = 0; settings.prismAmount = 0;
            QVERIFY2(settings.isIdentity() == (effect != LensEffect::Bokeh), "Bokeh is always a full lens blur");
            settings.flareAmount = 50; settings.starAmount = 50; settings.mistStrength = 50; settings.prismAmount = 50;
            QVERIFY(!settings.isIdentity());
        }
    }

    void settingsNormalizeAsOnMac()
    {
        LensEffectSettings s;
        QCOMPARE(s.threshold, 90.0); QCOMPARE(s.flareLength, 60.0); QCOMPARE(s.starPoints, 6); QCOMPARE(s.prismAngle, 180.0);
        QCOMPARE(s.bokehRing, 100.0); QCOMPARE(s.bokehShape, BokehShape::Round);
        s.threshold = 10; s.flareLength = 0; s.starLength = 99; s.starPoints = 5; s.prismCopies = 3.6; s.prismOffset = -4;
        s.bokehRadius = 9; s.mistSize = qQNaN(); s.starAngle = 400;
        const auto n = s.normalized();
        QCOMPARE(n.threshold, 50.0); QCOMPARE(n.flareLength, 1.0); QCOMPARE(n.starLength, 40.0); QCOMPARE(n.starPoints, 6);
        QCOMPARE(n.prismCopies, 4.0); QCOMPARE(n.prismOffset, 0.0); QCOMPARE(n.bokehRadius, 6.0); QCOMPARE(n.mistSize, 30.0);
        QCOMPARE(n.starAngle, 180.0);
    }

    // Transparency goes back on as it was; 16-bit layers stay 16-bit.
    void keepsAlphaAndDepth()
    {
        QImage image = night();
        { QPainter p(&image); p.setCompositionMode(QPainter::CompositionMode_Source); p.fillRect(QRect(0, 0, 50, 50), Qt::transparent); }
        const QImage flared = run([](auto &s) { s.effect = LensEffect::Mist; }, image);
        QCOMPARE(flared.pixelColor(10, 10).alpha(), 0);
        QCOMPARE(flared.pixelColor(300, 200).alpha(), 255);
        const QImage deep = image.convertToFormat(QImage::Format_RGBA64_Premultiplied);
        const QImage deepFlared = run([](auto &s) { s.effect = LensEffect::AnamorphicFlare; s.flareAmount = 80; }, deep);
        QVERIFY(DeepImage::isDeep(deepFlared) && !DeepImage::isHDR(deepFlared));
        QVERIFY(color(deepFlared, 300, 120)[2] > color(image, 300, 120)[2] + 10);
    }

    void sessionUndoNameAndLastFilter()
    {
        EditorSession s; s.createDocument(width, height);
        QVERIFY(s.insertImage(night(), "Night"));
        const QImage before = s.activeLayer()->image;
        LensEffectSettings none; none.flareAmount = 0;
        QVERIFY2(!s.applyLensEffects(none), "nothing to do leaves no edit");
        LensEffectSettings flare; flare.flareAmount = 80;
        QVERIFY(s.applyLensEffects(flare));
        QCOMPARE(s.history().undoName(), QStringLiteral("Lens Effects"));
        QCOMPARE(s.lastFilterName(), QStringLiteral("Lens Effects"));
        const QImage once = s.activeLayer()->image;
        QVERIFY(s.applyLastFilter());
        QVERIFY(s.activeLayer()->image != once);
        s.undo(); s.undo();
        QCOMPARE(s.activeLayer()->image, before);
    }

    void filterMenuAndPanel()
    {
        MainWindow w; w.resize(1000, 760); w.show();
        w.session().createDocument(width, height); QVERIFY(w.session().insertImage(night(), "Night")); w.syncDocumentViews();
        auto *action = w.findChild<QAction *>("cmdLensEffects"); QVERIFY(action);
        QCOMPARE(action->text(), QStringLiteral("Lens Effects…"));
        // In the Filter menu right after RGB Split, as macOS lists FilterKind.
        QMenu *filter = nullptr;
        for (QAction *top : w.menuBar()->actions()) if (top->text().remove('&') == QStringLiteral("Filter")) filter = top->menu();
        QVERIFY(filter);
        const auto items = filter->actions();
        const int at = items.indexOf(action);
        QVERIFY(at > 0); QCOMPARE(items[at - 1]->objectName(), QStringLiteral("cmdRgbSplit"));
        emit filter->aboutToShow(); QVERIFY(action->isEnabled());
        bool checked = false;
        QTimer::singleShot(0, &w, [&] {
            auto *d = w.findChild<QDialog *>("lensEffectsDialog"); QVERIFY(d);
            // A failed check below returns early: close the panel so the test reports instead of waiting.
            const auto closing = qScopeGuard([&] { if (!checked) d->reject(); });
            QCOMPARE(d->windowTitle(), QStringLiteral("Lens Effects"));
            auto *effect = d->findChild<QComboBox *>("lensEffect");
            QStringList names; for (int i = 0; i < effect->count(); ++i) names << effect->itemText(i);
            QCOMPARE(names, (QStringList{"Anamorphic Flare", "Starburst", "Mist", "Prism", "Bokeh"}));
            QVERIFY(d->findChild<QDoubleSpinBox *>("lensFlareAmount")->isVisibleTo(d));
            QVERIFY(!d->findChild<QDoubleSpinBox *>("lensPrismAmount")->isVisibleTo(d));
            QCOMPARE(d->findChild<QDoubleSpinBox *>("lensFlareThreshold")->maximum(), 100.0);
            effect->setCurrentIndex(3);
            QVERIFY(d->findChild<QDoubleSpinBox *>("lensPrismAmount")->isVisibleTo(d));
            QVERIFY(d->findChild<QWidget *>("lensPrismDirection")->isVisibleTo(d));
            QVERIFY(!d->findChild<QDoubleSpinBox *>("lensFlareAmount")->isVisibleTo(d));
            QCOMPARE(d->findChild<QDoubleSpinBox *>("lensPrismBlur")->toolTip(), QStringLiteral("Soften the copies, more the farther they're pushed"));
            effect->setCurrentIndex(4);
            auto *shape = d->findChild<QComboBox *>("lensBokehShape"); QVERIFY(shape->isVisibleTo(d));
            QCOMPARE(shape->itemText(3), QStringLiteral("Cat's Eye"));
            QCOMPARE(d->findChild<QDoubleSpinBox *>("lensBokehSize")->decimals(), 1);
            effect->setCurrentIndex(3);
            d->findChild<QDoubleSpinBox *>("lensPrismOffset")->setValue(25);
            static_cast<SegmentedControl *>(d->findChild<QWidget *>("lensPrismDirection"))->setCurrentIndex(1);
            checked = true;
            d->accept();
        });
        action->trigger();
        QVERIFY(checked);
        QCOMPARE(w.session().history().undoName(), QStringLiteral("Lens Effects"));
        QCOMPARE(w.session().lastLensEffects.effect, LensEffect::Prism);
        QVERIFY(w.session().lastLensEffects.prismBothWays);
        const QImage result = w.session().activeLayer()->image;
        QVERIFY(color(result, 100, 120)[1] > color(night(), 100, 120)[1] + 20);
        QVERIFY(color(result, 300, 120)[1] > color(night(), 300, 120)[1] + 20);
        w.session().markSaved();
    }

    // ---- RGB Split (c51a2f2) --------------------------------------------------------------------------------------

    void rgbSplitBlurSmearsBothWays()
    {
        QImage image(121, 21, QImage::Format_RGBA8888_Premultiplied); image.fill(Qt::black);
        { QPainter p(&image); p.fillRect(QRect(58, 0, 5, 21), Qt::white); }
        RgbSplitSettings s; s.distance = 0; s.blur = 40; s.angle = 0;
        const QImage smeared = s.applyCpu(image, 15);
        const auto centroid = [](const QImage &i) {
            double sum = 0, weighted = 0;
            for (int x = 0; x < i.width(); ++x) { const double v = i.pixelColor(x, 10).greenF(); sum += v; weighted += v * (x + .5); }
            return weighted / sum;
        };
        QVERIFY2(std::abs(centroid(smeared) - centroid(image)) < 1, qPrintable(QStringLiteral("%1 vs %2").arg(centroid(smeared)).arg(centroid(image))));
        QVERIFY(smeared.pixelColor(45, 10).greenF() > .05 && smeared.pixelColor(75, 10).greenF() > .05);
    }

    // ---- GIF playback (a971eb6, 64097c9, 6078d7e, a5c900d) --------------------------------------------------------

    void decodingDoesNotAccumulateDelayAtAnyPlaybackSpeed_data()
    {
        QTest::addColumn<double>("speed");
        for (double speed : {.25, 1.0, 4.0}) QTest::newRow(qPrintable(QString::number(speed))) << speed;
    }
    void decodingDoesNotAccumulateDelayAtAnyPlaybackSpeed()
    {
        QFETCH(double, speed);
        const auto r = play(micro({.1 / speed, .2 / speed, .15 / speed}), 0, false, qint64(std::llround(30000 / speed)));
        QCOMPARE(r.frames, (QVector<int>{1, 2}));
        QCOMPARE(r.times, (QVector<qint64>{qint64(std::llround(1e5 / speed)), qint64(std::llround(3e5 / speed))}));
        QCOMPARE(r.now, qint64(std::llround(4.5e5 / speed)));
    }
    void slowDecodingSkipsExpiredIntervalsAndOnceEndsOnTheLastFrame_data()
    {
        QTest::addColumn<double>("delay");
        QTest::newRow("0.25") << .25; QTest::newRow("0.5") << .5;
    }
    void slowDecodingSkipsExpiredIntervalsAndOnceEndsOnTheLastFrame()
    {
        QFETCH(double, delay);
        const auto r = play(micro({.1, .1, .1, .1}), 0, false, qint64(delay * 1e6));
        QCOMPARE(r.frames, (QVector<int>{1, 3}));
        QCOMPARE(r.sleeps, delay < .4 ? 3 : 2);
    }
    void resumeAndLoopKeepMixedFrameDurations()
    {
        const auto r = play(micro({.1, .2, .3}), 1, true, 20000, 3);
        QCOMPARE(r.frames, (QVector<int>{2, 0, 1}));
        QCOMPARE(r.times, micro({.2, .5, .6}));
    }
    void emptySingleFrameAndInvalidTimingDoNotStartPlayback()
    {
        for (const auto &durations : {QVector<qint64>{}, micro({.1}), micro({.1, 0}), QVector<qint64>{100000, -1}})
            QVERIFY(!GifPlayback(durations, 0).valid());
        QVERIFY(!GifPlayback(micro({.1, .2}), 2).valid());
        QCOMPARE(play(micro({.1}), 0, true, 0).sleeps, 0);
    }
    // GIFTimeline.play: frame identities [0, 4, 2, 3] from the one shown (4), on the layer's saved delays.
    void timelineKeepsSavedTimingAndStableFrameIdentities_data()
    {
        QTest::addColumn<bool>("uniform");
        QTest::newRow("perFrame") << false; QTest::newRow("uniform") << true;
    }
    void timelineKeepsSavedTimingAndStableFrameIdentities()
    {
        QFETCH(bool, uniform);
        const QVector<int> identities{0, 4, 2, 3};
        const auto r = play(uniform ? micro({.1, .1, .1, .1}) : micro({.04, .16, .08, .24}), 1, true, 30000, 3);
        QVector<int> shown; for (int position : r.frames) shown.append(identities[position]);
        QCOMPARE(shown, (QVector<int>{2, 3, 0}));
        QCOMPARE(r.times, uniform ? micro({.1, .2, .3}) : micro({.16, .24, .48}));
    }
    void timelineSkipsExpiredFramesAfterSlowDecode()
    {
        const QVector<int> identities{0, 4, 2, 3};
        const auto r = play(micro({.1, .1, .1, .1}), 0, true, 250000, 2);
        QVector<int> shown; for (int position : r.frames) shown.append(identities[position]);
        QCOMPARE(shown, (QVector<int>{4, 3}));
        QCOMPARE(r.times, micro({.1, .35}));
    }

    void framesReadInAnyOrderMatchAFreshDecode()
    {
        QTemporaryDir dir; const QString path = partialGif(dir); QVERIFY(!path.isEmpty());
        QString error; auto animation = GifIO::readFile(path, &error); QVERIFY2(animation, qPrintable(error));
        QCOMPARE(animation->frames.size(), 4);
        const auto fresh = [&](int index) {
            QImageReader reader(path, "gif"); QImage image;
            for (int i = 0; i <= index; ++i) image = reader.read();
            return image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        };
        for (int index : {0, 1, 2, 3, 3, 1, 2, 0, 3}) QCOMPARE(animation->frame(index), fresh(index));
        // The last frame is every patch over the first: red, then green, blue and white squares.
        const QImage last = animation->frame(3);
        QCOMPARE(last.pixelColor(1, 1), QColor(Qt::green)); QCOMPARE(last.pixelColor(7, 7), QColor(Qt::blue));
        QCOMPARE(last.pixelColor(13, 13), QColor(Qt::white)); QCOMPARE(last.pixelColor(15, 0), QColor(Qt::red));
        // A copy of the layer's animation shares the one reader with its source.
        const GifAnimation copy = *animation;
        QCOMPARE(copy.frame(2), fresh(2));
        QVERIFY(copy.source->reader && copy.source->reader == animation->source->reader);
    }

    void delayLabelShowsTheGifsOwnMilliseconds()
    {
        QTemporaryDir dir; EditorSession s; s.createDocument(8, 8); QVERIFY(s.insertGif(colorsGif(dir)));
        const GifAnimation &a = *s.activeLayer()->gif;
        QCOMPARE(GifTimeline::originalDelays(a), QStringLiteral("40–120 ms"));
        QCOMPARE(GifTimeline::originalDelays(a, false), QStringLiteral("40–120ms"));
        GifAnimation same = a; for (const auto &f : same.frames) same.delays.insert(f.index, 70);
        QCOMPARE(GifTimeline::originalDelays(same), QStringLiteral("70 ms"));
        // The GIF's own delays, even while one delay plays for all.
        same.sameDelay = true; same.uniformDelay = 500;
        QCOMPARE(GifTimeline::originalDelays(same, false), QStringLiteral("70ms"));
    }

    void timelineMenuAndLabelInWindow()
    {
        QTemporaryDir dir; MainWindow w; w.resize(1100, 700); w.show(); QVERIFY(w.openProject(colorsGif(dir))); QCoreApplication::processEvents();
        auto *delay = w.findChild<QToolButton *>("gifDelay"); QCOMPARE(delay->text(), QStringLiteral("40–120ms"));
        auto *original = w.findChild<QAction *>("gifDelayOriginal"); QCOMPARE(original->text(), QStringLiteral("40–120 ms · Original"));
        w.findChild<QAction *>("gifDelay50")->trigger();
        QCOMPARE(delay->text(), QStringLiteral("50ms"));
        QCOMPARE(original->text(), QStringLiteral("40–120 ms · Original"));
        auto *timeline = static_cast<GifTimeline *>(w.findChild<QWidget *>("gifTimeline")); QVERIFY(timeline);
        // A paused timeline decodes nothing; playing advances through the frames on their delays and wraps around.
        QVERIFY(!timeline->isPlaying());
        QTest::qWait(150); QCOMPARE(w.session().activeLayer()->gif->selected, 0);
        auto *play = w.findChild<QToolButton *>("gifPlay"); play->click(); QVERIFY(timeline->isPlaying());
        QSet<int> seen;
        QTRY_VERIFY_WITH_TIMEOUT((seen.insert(w.session().activeLayer()->gif->selected), seen.size() == 3), 2000);
        play->click(); QVERIFY(!timeline->isPlaying());
        const int stopped = w.session().activeLayer()->gif->selected; QTest::qWait(150);
        QCOMPARE(w.session().activeLayer()->gif->selected, stopped);
        w.session().markSaved();
    }

    // ---- Project versions (b35ac38) ------------------------------------------------------------------------------

    void projectsFromEarlierReleasesStillOpen()
    {
        QTemporaryDir dir; EditorSession session; session.createDocument(100, 80, true); session.addBlankLayer();
        const QString url = dir.filePath("Earlier.comp");
        ProjectWriter::save(*session.document(), url);
        const QString metadata = url + "/manifest.json";
        QFile file(metadata); QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject manifest = QJsonDocument::fromJson(file.readAll()).object(); file.close();
        const auto load = [&](int version) {
            QJsonObject earlier = manifest; earlier.insert("version", version);
            QFile out(metadata); if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QStringLiteral("unwritable");
            out.write(QJsonDocument(earlier).toJson()); out.close();
            try { const Document document = ProjectReader::load(url); return QStringLiteral("v%1").arg(document.formatVersion); }
            catch (const ProjectError &error) { return error.message(); }
        };
        QCOMPARE(load(15), QStringLiteral("v15"));
        QCOMPARE(load(16), QStringLiteral("v16"));
        // 27, from the macOS GIF branch's own builds, opens and saves again as the current format.
        QCOMPARE(load(27), QStringLiteral("v17"));
        for (int version : {0, 18, 26, 28})
            QCOMPARE(load(version), QStringLiteral("This project uses format version %1. This app supports versions 1–17 and 27.").arg(version));
    }
};
QTEST_MAIN(TestUpstream158)
#include "TestUpstream158.moc"
