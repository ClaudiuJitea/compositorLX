#include "core/EditorSession.h"
#include "io/MovieDecoder.h"
#include "io/ProjectReader.h"
#include "io/ProjectWriter.h"
#include "rendering/LayerRenderer.h"
#include "ui/CanvasWidget.h"
#include "ui/EditorStyle.h"
#include "ui/GifTimeline.h"
#include "ui/MainWindow.h"
#include "ui/MovieTimeline.h"
#include "ui/SliderJumpStyle.h"
#include "ui/SliderTracks.h"

#include <QtTest>
#include <QAbstractItemView>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>
#include <QToolButton>
#include <QVBoxLayout>
#include <QDialog>

using namespace compositor;

// Compositor 1.7.1 – 1.7.6, each case naming the macOS commit (and test) it mirrors.
class TestUpstream176 : public QObject {
    Q_OBJECT

    static QString movie(const QString &name) { return QStringLiteral(FIXTURES_DIR "/movies/") + name; }
    static bool reddish(QRgb pixel) { return qRed(pixel) > 150 && qBlue(pixel) < 100; }
    static bool bluish(QRgb pixel) { return qBlue(pixel) > 150 && qRed(pixel) < 100; }

    static QJsonObject manifest(const QString &project)
    {
        QFile file(project + "/manifest.json"); file.open(QIODevice::ReadOnly);
        return QJsonDocument::fromJson(file.readAll()).object();
    }
    static void writeManifest(const QString &project, const QJsonObject &object)
    {
        QFile file(project + "/manifest.json"); file.open(QIODevice::WriteOnly | QIODevice::Truncate);
        file.write(QJsonDocument(object).toJson());
    }
    static QString colorsGif(QTemporaryDir &dir)
    {
        const QString path = dir.filePath("colors.gif"); QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(QByteArray::fromBase64("R0lGODlhCAAIAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQIBAAAACwAAAAACAAIAAAIDwABCBxIsKDBgwgTKkwYEAAh+QQICAAAACwAAAAACAAIAIEAgAAAAAAAAAAAAAAIDwABCBxIsKDBgwgTKkwYEAAh+QQIDAAAACwAAAAACAAIAIEAAP8AAAAAAAAAAAAIDwABCBxIsKDBgwgTKkwYEAA7"));
        return path;
    }

private slots:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName("compositor-tests"); QCoreApplication::setApplicationName("upstream176");
        QSettings().clear();
        QSettings().setValue("moviePlaybackMuted", true);
    }

    // ---- Movie frames (b469ec1 MovieFrameTests) ------------------------------------------------------------------

    // capturesTheSelectedFrameUprightIncludingAtTheEnd: a two-second movie, red then blue at two frames a second, with
    // a portrait rotation tag.
    void capturesTheSelectedFrameUprightIncludingAtTheEnd()
    {
        QString error;
        auto decoder = MovieDecoder::open(movie("frame-picker-portrait.mov"), &error);
        QVERIFY2(decoder, qPrintable(error));
        QCOMPARE(decoder->info().size, QSize(48, 80));
        QCOMPARE(decoder->info().duration, 2.0);
        for (const auto &[seconds, red] : {std::pair{0.99, true}, std::pair{1.0, false}, std::pair{2.0, false}, std::pair{0.0, true}, std::pair{5.0, false}}) {
            const QImage frame = decoder->frame(seconds, &error);
            QVERIFY2(!frame.isNull(), qPrintable(error));
            QCOMPARE(frame.size(), QSize(48, 80));
            const QRgb pixel = frame.pixel(10, 10);
            QVERIFY2(red ? reddish(pixel) : bluish(pixel), qPrintable(QStringLiteral("%1 s: %2").arg(seconds).arg(QColor(pixel).name())));
        }
        // A preview frame is decoded no larger than asked, keeping its shape.
        QCOMPARE(decoder->frame(1.2, &error, QSize(24, 24)).size(), QSize(14, 24));
    }

    // appliesPreferredTrackTransform: each display matrix turns the picture as FFmpeg's (and AVFoundation's) players do.
    // The clips have a red square at the source's top left; where it shows was read from ffmpeg's own autorotate.
    void framesAreTurnedUprightByTheTrackMatrix()
    {
        const struct { const char *file; QSize size; QPoint red; } cases[] = {
            {"rot90.mov", {32, 64}, {4, 59}}, {"rot270.mov", {32, 64}, {27, 4}},
            {"rot180.mov", {64, 32}, {59, 27}}, {"hflip.mov", {64, 32}, {59, 4}},
        };
        for (const auto &c : cases) {
            auto decoder = MovieDecoder::open(movie(c.file));
            QVERIFY(decoder);
            const QImage frame = decoder->frame(0);
            QCOMPARE(frame.size(), c.size);
            QVERIFY2(reddish(frame.pixel(c.red)), c.file);
            // And the opposite corner isn't.
            QVERIFY2(bluish(frame.pixel(c.size.width() - 1 - c.red.x(), c.size.height() - 1 - c.red.y())), c.file);
        }
    }

    // c0d4446 / 590c386: an HDR movie's frames are mapped to SDR the same way however they're asked for.
    void hdrMoviesAreToneMappedConsistently()
    {
        auto decoder = MovieDecoder::open(movie("pq.mp4"));
        QVERIFY(decoder);
        QVERIFY(decoder->info().hdr);
        const QImage full = decoder->frame(0.2), preview = decoder->frame(0.2, nullptr, QSize(16, 16));
        // PQ code 0.5 is about 94 nits: 0.46 of reference white, about 181 in sRGB.
        const QColor gray = full.pixelColor(16, 16);
        QVERIFY2(gray.red() > 168 && gray.red() < 194, qPrintable(gray.name()));
        QVERIFY(std::abs(gray.red() - gray.green()) <= 3 && std::abs(gray.green() - gray.blue()) <= 3);
        QVERIFY(std::abs(preview.pixelColor(8, 8).red() - gray.red()) <= 2);
        // The tone curve itself: reference white lands near the top without clipping, black stays black.
        const quint16 white[3] = {37650, 37650, 37650}, black[3] = {0, 0, 0};
        const QColor mappedWhite = MovieDecoder::toneMapBt2020(white, 1, 1, 3, false).pixelColor(0, 0);
        QVERIFY2(mappedWhite.red() > 225 && mappedWhite.red() < 255, qPrintable(mappedWhite.name()));
        QCOMPARE(MovieDecoder::toneMapBt2020(black, 1, 1, 3, false).pixelColor(0, 0), QColor(Qt::black));
    }

    void playheadTimeReadsAsMacOSFormatsIt()
    {
        QCOMPARE(formatMovieTime(0), QStringLiteral("0:00.00"));
        QCOMPARE(formatMovieTime(65.25), QStringLiteral("1:05.25"));
        QCOMPARE(formatMovieTime(2), QStringLiteral("0:02.00"));
        QCOMPARE(formatMovieTime(0.125), QStringLiteral("0:00.12"));   // half to even
        QCOMPARE(formatMovieTime(-1), QStringLiteral("0:00.00"));
    }

    void bookmarksNameTheMovie()
    {
        const QString path = movie("frame-picker-portrait.mov");
        const QByteArray bookmark = MovieBookmark::make(path);
        QVERIFY(bookmark.startsWith("book"));
        QCOMPARE(MovieBookmark::path(bookmark), QFileInfo(path).absoluteFilePath());
        QCOMPARE(MovieBookmark::path(MovieBookmark::make(QStringLiteral("/tmp/Ünïcode clip.mov"))), QStringLiteral("/tmp/Ünïcode clip.mov"));
        QVERIFY(MovieBookmark::path(QByteArray("not a bookmark")).isEmpty());
        QByteArray damaged = bookmark; damaged.truncate(60);
        QVERIFY(MovieBookmark::path(damaged).isEmpty());
    }

    // linkedMovieRoundTripsWithItsStill, and ProjectStore's version 23 gates.
    void linkedMovieRoundTripsWithItsStill()
    {
        QTemporaryDir dir;
        EditorSession session;
        QString error;
        QVERIFY2(session.insertMovie(movie("frame-picker-portrait.mov"), std::nullopt, &error), qPrintable(error));
        const Layer original = *session.activeLayer();
        QVERIFY(original.movie);
        QCOMPARE(original.movie->duration, 2.0);
        QCOMPARE(original.movie->seconds, 0.0);
        QCOMPARE(original.name, QStringLiteral("frame-picker-portrait"));
        QCOMPARE(session.document()->canvasSize, QSize(48, 80));
        const QString file = dir.filePath("movie.comp");
        ProjectWriter::save(*session.document(), file);
        QCOMPARE(manifest(file).value("version").toInt(), 23);
        const QJsonObject record = manifest(file).value("layers").toArray().first().toObject().value("movie").toObject();
        QCOMPARE(record.value("duration").toDouble(), 2.0);
        QVERIFY(record.value("bookmark").isString());
        const Document reopened = ProjectReader::load(file);
        const Layer &restored = reopened.layers.first();
        QVERIFY(restored.movie);
        QCOMPARE(*restored.movie, *original.movie);
        QCOMPARE(restored.image.width(), 48);
        QCOMPARE(EditorSession::moviePath(*restored.movie), QFileInfo(movie("frame-picker-portrait.mov")).absoluteFilePath());
    }

    void readerAcceptsMovieLinksOnlyWhereMacOSDoes_data()
    {
        QTest::addColumn<QString>("change");
        QTest::newRow("version 22") << "version22";
        QTest::newRow("version 27") << "version27";
        QTest::newRow("seconds past the end") << "late";
        QTest::newRow("no length") << "empty";
        QTest::newRow("a folder") << "group";
        QTest::newRow("not base64") << "bookmark";
    }
    void readerAcceptsMovieLinksOnlyWhereMacOSDoes()
    {
        QFETCH(QString, change);
        QTemporaryDir dir;
        EditorSession session;
        QVERIFY(session.insertMovie(movie("frame-picker-portrait.mov")));
        const QString file = dir.filePath("movie.comp");
        ProjectWriter::save(*session.document(), file);
        QJsonObject object = manifest(file);
        QJsonArray layers = object.value("layers").toArray();
        QJsonObject layer = layers.first().toObject(), link = layer.value("movie").toObject();
        if (change == "version22") object.insert("version", 22);
        if (change == "version27") object.insert("version", 27);
        if (change == "late") link.insert("seconds", 2.5);
        if (change == "empty") link.insert("duration", 0);
        if (change == "group") layer.insert("isGroup", true);
        if (change == "bookmark") link.insert("bookmark", "***");
        layer.insert("movie", link); layers.replace(0, layer); object.insert("layers", layers);
        writeManifest(file, object);
        QVERIFY_THROWS_EXCEPTION(ProjectError, ProjectReader::load(file));
    }

    void selectingGrabbingAndRelinkingFrames()
    {
        EditorSession session;
        QVERIFY(session.insertMovie(movie("frame-picker-portrait.mov")));
        const QUuid id = session.activeLayer()->id;
        auto decoder = MovieDecoder::open(movie("frame-picker-portrait.mov"));
        QVERIFY(session.setMovieFrame(id, decoder->frame(1.5), 1.5));
        QCOMPARE(session.history().undoName(), QStringLiteral("Select Movie Frame"));
        QCOMPARE(session.activeLayer()->movie->seconds, 1.5);
        QVERIFY(bluish(session.activeLayer()->image.pixel(10, 10)));
        session.undo();
        QCOMPARE(session.activeLayer()->movie->seconds, 0.0);
        QVERIFY(reddish(session.activeLayer()->image.pixel(10, 10)));
        session.redo();
        // Grab Frame: a still just above, named for its time, as large and where the movie is; the movie stays active.
        QVERIFY(session.grabMovieFrame(id));
        QCOMPARE(session.history().undoName(), QStringLiteral("Grab Movie Frame"));
        QCOMPARE(session.document()->layers.size(), 2);
        const Layer &still = session.document()->layers.at(1);
        QCOMPARE(still.name, QStringLiteral("frame-picker-portrait · 0:01.50"));
        QVERIFY(!still.movie);
        QCOMPARE(still.transform, session.document()->layers.at(0).transform);
        QCOMPARE(session.activeLayer()->id, id);
        // A frame isn't taken onto a layer that isn't the one being edited.
        session.selectLayer(still.id);
        QVERIFY(!session.setMovieFrame(id, decoder->frame(0), 0));
        session.selectLayer(id);
        // Relink: another movie, its first frame.
        QVERIFY(session.relinkMovie(id, movie("rot90.mov")));
        QCOMPARE(session.history().undoName(), QStringLiteral("Relink Movie"));
        QCOMPARE(session.activeLayer()->image.size(), QSize(32, 64));
        QCOMPARE(EditorSession::moviePath(*session.activeLayer()->movie), QFileInfo(movie("rot90.mov")).absoluteFilePath());
    }

    // e234dd4: distorting a movie layer leaves plain pixels; moving it keeps the link.
    void distortionTurnsAMovieLayerIntoPixels()
    {
        EditorSession session;
        QVERIFY(session.insertMovie(movie("frame-picker-portrait.mov")));
        session.document()->canvasSize = QSize(200, 200);
        QVERIFY(session.activeLayer()->movie);
        const QRectF box(session.activeLayer()->transform.origin, session.activeLayer()->transform.size);
        QVERIFY(session.distortSelectedLayers({box.topLeft() + QPointF(4, 0), box.topRight(), box.bottomRight(), box.bottomLeft()}));
        QVERIFY(!session.activeLayer()->movie);
        session.undo();
        QVERIFY(session.activeLayer()->movie);
    }

    void movieLayersSayMOV()
    {
        MainWindow window; window.resize(1000, 700); window.show();
        QVERIFY(window.openProject(movie("frame-picker-portrait.mov")));
        QCoreApplication::processEvents();
        bool found = false;
        for (auto *view : window.findChildren<QAbstractItemView *>()) {
            if (!view->model()) continue;
            for (int row = 0; row < view->model()->rowCount(); ++row)
                if (view->model()->data(view->model()->index(row, 0), Qt::UserRole).toString().endsWith(QStringLiteral("· MOV"))) found = true;
        }
        QVERIFY(found);
        QCOMPARE(window.session().sourceImagePath, QFileInfo(movie("frame-picker-portrait.mov")).absoluteFilePath());
        window.session().markSaved();
    }

    // The movie bar: a scrub shows frames on the canvas without changing the document; letting go takes the frame.
    void scrubbingPreviewsAndReleasingSettles()
    {
        MainWindow window; window.resize(1000, 700); window.show();
        QVERIFY(window.openProject(movie("frame-picker-portrait.mov")));
        QCoreApplication::processEvents();
        auto *timeline = static_cast<MovieTimeline *>(window.findChild<QWidget *>("movieTimeline"));
        QVERIFY(timeline && timeline->isVisible());
        QCOMPARE(timeline->height(), layersFooterHeight);
        auto *playhead = timeline->findChild<QSlider *>("moviePlayhead");
        auto *time = timeline->findChild<QLabel *>("movieTime");
        auto *canvas = window.findChild<CanvasWidget *>();
        QVERIFY(playhead && time && canvas);
        QCOMPARE(playhead->maximum(), 2000);
        QCOMPARE(time->text(), QStringLiteral("0:00.00"));
        const int undo = window.session().history().undoCount();
        playhead->setSliderDown(true);
        playhead->setValue(1500);
        QTRY_VERIFY(canvas->hasMoviePreview());
        QCOMPARE(time->text(), QStringLiteral("0:01.50"));
        QCOMPARE(window.session().history().undoCount(), undo);
        QVERIFY(reddish(window.session().activeLayer()->image.pixel(10, 10)));
        playhead->setSliderDown(false);
        QTRY_COMPARE(window.session().activeLayer()->movie->seconds, 1.5);
        QTRY_VERIFY(!timeline->isBusy());
        QVERIFY(bluish(window.session().activeLayer()->image.pixel(10, 10)));
        QCOMPARE(window.session().history().undoName(), QStringLiteral("Select Movie Frame"));
        QVERIFY(!canvas->hasMoviePreview());
        // Grab Frame from the bar.
        timeline->grabFrame();
        QCOMPARE(window.session().document()->layers.size(), 2);
        window.session().markSaved();
    }

    void playingAdvancesAndPausingSettles()
    {
        MainWindow window; window.resize(1000, 700); window.show();
        QVERIFY(window.openProject(movie("sound.mp4")));
        QCoreApplication::processEvents();
        auto *timeline = static_cast<MovieTimeline *>(window.findChild<QWidget *>("movieTimeline"));
        auto *play = timeline->findChild<QToolButton *>("moviePlay");
        auto *mute = timeline->findChild<QToolButton *>("movieMute");
        QVERIFY(play && mute);
        QCOMPARE(mute->isVisible(), MovieTimeline::soundAvailable());
        play->click();
        QVERIFY(timeline->isPlaying());
        QCOMPARE(play->toolTip(), QStringLiteral("Pause"));
        QTRY_VERIFY_WITH_TIMEOUT(timeline->shownSeconds() > 0.3, 3000);
        play->click();
        QVERIFY(!timeline->isPlaying());
        QTRY_VERIFY(!timeline->isBusy());
        QTRY_VERIFY(window.session().activeLayer()->movie->seconds > 0.25);
        QCOMPARE(window.session().history().undoName(), QStringLiteral("Select Movie Frame"));
        // Playing on to the end settles on the last frame.
        play->click();
        QTRY_VERIFY_WITH_TIMEOUT(!timeline->isPlaying(), 5000);
        QTRY_VERIFY(!timeline->isBusy());
        QCOMPARE(window.session().activeLayer()->movie->seconds, window.session().activeLayer()->movie->duration);
        window.session().markSaved();
    }

    void aMissingMovieAsksToBeRelinked()
    {
        QTemporaryDir dir;
        const QString copy = dir.filePath("gone.mov");
        QVERIFY(QFile::copy(movie("frame-picker-portrait.mov"), copy));
        MainWindow window; window.resize(1000, 700); window.show();
        QVERIFY(window.openProject(copy));
        QFile::remove(copy);
        auto *timeline = static_cast<MovieTimeline *>(window.findChild<QWidget *>("movieTimeline"));
        timeline->findChild<QToolButton *>("moviePlay")->click();
        QCOMPARE(timeline->failure(), QStringLiteral("Movie unavailable. Right-click the timeline to relink and play."));
        timeline->relink(movie("frame-picker-portrait.mov"));
        QVERIFY(timeline->failure().isEmpty());
        QCOMPARE(window.session().history().undoName(), QStringLiteral("Relink Movie"));
        window.session().markSaved();
    }

    // Timings for the sync doc: MOVIE_BENCH=<clip> runs it (skipped otherwise).
    void movieDecodingTimings()
    {
        const QString clip = qEnvironmentVariable("MOVIE_BENCH");
        if (clip.isEmpty()) QSKIP("set MOVIE_BENCH to a movie to time");
        const auto median = [](QVector<qint64> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
        QElapsedTimer timer;
        QVector<qint64> open, first, cold, sequential, preview;
        for (int run = 0; run < 3; ++run) {
            timer.start(); auto decoder = MovieDecoder::open(clip); open << timer.elapsed(); QVERIFY(decoder);
            timer.start(); QVERIFY(!decoder->frame(0).isNull()); first << timer.elapsed();
            timer.start(); QVERIFY(!decoder->frame(2.9).isNull()); cold << timer.elapsed();
            timer.start(); for (int i = 1; i <= 30; ++i) QVERIFY(!decoder->frame(i / 30.0, nullptr, QSize(MovieTimeline::previewSide, MovieTimeline::previewSide)).isNull());
            sequential << timer.elapsed() / 30;
            timer.start(); for (double t : {3.5, 0.7, 2.2, 1.4, 3.1}) QVERIFY(!decoder->frame(t, nullptr, QSize(MovieTimeline::previewSide, MovieTimeline::previewSide)).isNull());
            preview << timer.elapsed() / 5;
        }
        qInfo().noquote() << QStringLiteral("open %1 ms · first frame %2 ms · exact seek 2.9 s %3 ms · playback frame %4 ms · scrub frame %5 ms")
                                 .arg(median(open)).arg(median(first)).arg(median(cold)).arg(median(sequential)).arg(median(preview));
    }

    // ---- GIF playback bar (8c6ebeb) ------------------------------------------------------------------------------

    void gifTimelineHasAScrubberAndFrameCounter()
    {
        QTemporaryDir dir; MainWindow w; w.resize(1100, 700); w.show(); QVERIFY(w.openProject(colorsGif(dir))); QCoreApplication::processEvents();
        auto *timeline = static_cast<GifTimeline *>(w.findChild<QWidget *>("gifTimeline"));
        auto *scrubber = timeline->findChild<QSlider *>("gifScrubber");
        auto *counter = timeline->findChild<QLabel *>("gifFrameCounter");
        QVERIFY(scrubber && counter);
        QCOMPARE(counter->text(), QStringLiteral("01 / 03"));
        QCOMPARE(scrubber->minimum(), 1); QCOMPARE(scrubber->maximum(), 3);
        scrubber->setValue(3);
        QTRY_COMPARE(w.session().activeLayer()->gif->selected, w.session().activeLayer()->gif->frames[2].index);
        QTRY_COMPARE(counter->text(), QStringLiteral("03 / 03"));
        // Scrubbing stops playback.
        auto *play = timeline->findChild<QToolButton *>("gifPlay"); play->click(); QVERIFY(timeline->isPlaying());
        scrubber->setValue(1); QVERIFY(!timeline->isPlaying());
        QCOMPARE(GifTimeline::frameCounter(7, 120), QStringLiteral("07 / 120"));
        w.session().markSaved();
    }

    // GIFLayerTests: the Original label keeps the GIF's own delays while one delay plays for all.
    void originalDelaysStayTheGifsOwnUnderAUniformDelay()
    {
        QTemporaryDir dir; EditorSession s; s.createDocument(8, 8); QVERIFY(s.insertGif(colorsGif(dir)));
        QVERIFY(s.setGifTiming(100));
        QCOMPARE(GifTimeline::originalDelays(*s.activeLayer()->gif), QStringLiteral("40–120 ms"));
    }

    // ---- Gradient on a mask placed apart (a242ce6) -----------------------------------------------------------------

    void aGradientOnAMaskPlacedApartShowsWhileDragging()
    {
        EditorSession s; s.createDocument(200, 160);
        QImage red(200, 160, QImage::Format_RGBA8888_Premultiplied); red.fill(Qt::red);
        QVERIFY(s.insertImage(red, "Red"));
        QVERIFY(s.addLayerMask(true, false));
        Layer *layer = s.activeLayer();
        LayerTransform placement = layer->transform; placement.origin.ry() -= 6; placement.size.rheight() += 14;
        layer->maskPlacement = placement;
        s.selectMaskTarget(true);
        const QImage before = LayerRenderer::flattened(*s.document(), RenderPurpose::Display);
        QVERIFY(s.applyGradient(QPointF(20, 20), QPointF(180, 140), Qt::black, Qt::white));
        const QImage during = LayerRenderer::flattened(*s.document(), RenderPurpose::Display);
        int changed = 0;
        for (int y = 0; y < 160; y += 2) for (int x = 0; x < 200; x += 2)
            if (std::abs(qAlpha(before.pixel(x, y)) - qAlpha(during.pixel(x, y))) > 12) ++changed;
        QVERIFY2(changed > 100 * 80 / 20, qPrintable(QString::number(changed)));
    }

    // ---- Look: slim sliders, solid toolbar pills (2fc7936, 475acdd) -----------------------------------------------

    void slidersAreSlimWithARoundKnob()
    {
        QSlider slider(Qt::Horizontal); slider.setRange(0, 100); slider.setValue(50); slider.resize(200, 22);
        slider.setStyle(new SliderJumpStyle(QStringLiteral("Fusion")));
        QImage image(slider.size(), QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
        slider.render(&image, {}, {}, QWidget::DrawChildren);
        // Away from the knob, the track is exactly 2 pixels, crisp: no half-covered rows either side.
        int rows = 0, soft = 0;
        for (int y = 0; y < image.height(); ++y) { const int a = qAlpha(image.pixel(30, y)); if (a > 200) ++rows; else if (a > 0) ++soft; }
        QVERIFY2(rows == 2 && soft == 0, qPrintable(QStringLiteral("%1 solid, %2 soft").arg(rows).arg(soft)));
        // The fill reaches the knob's center, the knob sits at the middle and is round: its corner box is clear.
        QStyleOptionSlider option; option.initFrom(&slider); option.minimum = 0; option.maximum = 100; option.sliderPosition = 50; option.orientation = Qt::Horizontal;
        const QRect handle = slider.style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, &slider);
        QCOMPARE(handle.size(), QSize(SliderLook::knob, SliderLook::knob));
        QVERIFY(std::abs(handle.center().x() - 100) <= 1);
        QVERIFY(qAlpha(image.pixel(handle.center())) == 255);
        QVERIFY(qAlpha(image.pixel(handle.topLeft())) < 200);
        QCOMPARE(SliderLook::fillLength(0, 200), 0.0);
        QCOMPARE(SliderLook::fillLength(1, 200), 200.0);
        QCOMPARE(SliderLook::fillLength(.5, 200), 100.0);
        // A colored track is painted from its colors, a little thicker.
        SliderTrack::apply(&slider, SliderTrack::temperature());
        image.fill(Qt::transparent); slider.render(&image, {}, {}, QWidget::DrawChildren);
        const QColor left = image.pixelColor(6, handle.center().y()), right = image.pixelColor(193, handle.center().y());
        QVERIFY2(left.blue() > left.red() && right.red() > right.blue(), qPrintable(left.name() + right.name()));
    }

    // Dialogs open wide enough for their title between the title bar's buttons; frameless panels are left as they are.
    void dialogsOpenWideEnoughForTheirTitle()
    {
        applyEditorTheme(*qApp, ThemeMode::Dark);
        QDialog dialog; dialog.setWindowTitle(QStringLiteral("Canvas Size"));
        auto *layout = new QVBoxLayout(&dialog); layout->addWidget(new QLabel(QStringLiteral("x")));
        dialog.show(); QCoreApplication::processEvents();
        QVERIFY(dialog.width() >= DialogTitleFit::widthFor(dialog.windowTitle()));
        QVERIFY(DialogTitleFit::widthFor(QStringLiteral("A much longer dialog title than any field would need")) > DialogTitleFit::minimumWidth);
        QDialog panel(nullptr, Qt::FramelessWindowHint); panel.setWindowTitle(QStringLiteral("Panel")); panel.resize(120, 80);
        panel.show(); QCoreApplication::processEvents();
        QCOMPARE(panel.width(), 120);
        dialog.close(); panel.close();
    }

    void toolbarPillsInMacOrder()
    {
        MainWindow window; window.resize(1200, 700); window.show(); QCoreApplication::processEvents();
        QList<QWidget *> pills;
        for (auto *widget : window.findChildren<QWidget *>(QStringLiteral("toolbarPill"))) if (widget->isVisible()) pills << widget;
        std::sort(pills.begin(), pills.end(), [](QWidget *a, QWidget *b) { return a->mapToGlobal(QPoint()).x() < b->mapToGlobal(QPoint()).x(); });
        QCOMPARE(pills.size(), 4);
        QCOMPARE(static_cast<QPushButton *>(pills[0])->text(), QStringLiteral("Fit"));
        QCOMPARE(static_cast<QPushButton *>(pills[1])->text(), QStringLiteral("100%"));
        QCOMPARE(pills[2]->toolTip(), QStringLiteral("Zoom in"));
        QCOMPARE(pills[3]->toolTip(), QStringLiteral("Zoom out"));
        for (auto *pill : pills) QCOMPARE(pill->height(), 28);
        QCOMPARE(window.findChild<QToolButton *>(QStringLiteral("newTabButton"))->size(), QSize(28, 28));
    }
};

QTEST_MAIN(TestUpstream176)
#include "TestUpstream176.moc"
