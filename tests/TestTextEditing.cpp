// Per-letter text colors and faces in the editor and renderer (mirrors macOS TypeToolTests).
#include "core/Document.h"
#include "core/EditorSession.h"
#include "io/ProjectReader.h"
#include "io/ProjectWriter.h"
#include "rendering/TextLayout.h"
#include "ui/InlineTextEditor.h"
#include "ui/MainWindow.h"

#include <QDir>
#include <QMenu>
#include <QMenuBar>
#include <QTemporaryDir>
#include <QtTest>

using namespace compositor;

namespace {
TextStyle plainStyle(const QString &content)
{
    TextStyle style;
    style.content = content;
    style.fontName = QStringLiteral("DejaVu Sans");
    style.fontSize = 40.0;
    return style;
}

// Counts opaque-ish pixels dominated by red / blue within a column range.
struct Tally { int red = 0; int blue = 0; };
Tally tally(const QImage &image, int fromX, int toX)
{
    Tally out;
    const QImage rgba = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < rgba.height(); ++y)
        for (int x = std::max(0, fromX); x < std::min(rgba.width(), toX); ++x) {
            const QColor c = rgba.pixelColor(x, y);
            if (c.alpha() < 200) continue;
            if (c.red() > 200 && c.blue() < 60) ++out.red;
            if (c.blue() > 200 && c.red() < 60) ++out.blue;
        }
    return out;
}
}

class TestTextEditing : public QObject
{
    Q_OBJECT
    static QString fixture(const QString &sub) { return QDir(QStringLiteral(FIXTURES_DIR)).filePath(sub); }

private slots:
    void setColorSemantics();
    void setFontSemantics();
    void runsSurviveEdits();
    void renderingColorsLettersIndividually();
    void editorReadsBackUntouchedRuns();
    void editorColorsSelectionOnly();
    void editorTypedLettersInherit();
    void editorFaceAtSelectionReportsMixed();
    void editorPreviewIsTakenBack();
    void editorUndoIsOwnHistory();
    void sessionPromotesVersionAndUndoRestoresIt();
    void sessionEditKeepsRuns();
    void flatUpdateKeepsRunsUnlessBaseChanged();
    void packageRoundTripAfterEditing();
    void filtersWaitForTextEditing();
};

void TestTextEditing::setColorSemantics()
{
    TextStyle style = plainStyle(QStringLiteral("Hello World"));
    style.setColor({1, 0, 0}, 0, 5);
    QVERIFY(style.colorRuns);
    QCOMPARE(style.colorAt(0).r, 1.0);
    QCOMPARE(style.colorAt(5).r, 0.0);
    // Empty selection recolors everything and drops the runs.
    style.setColor({0, 0, 1}, 3, 0);
    QVERIFY(!style.colorRuns);
    QCOMPARE(style.colorAt(9).b, 1.0);
    // A range covering the whole text does too.
    style.setColor({0, 1, 0}, 2, 3);
    QVERIFY(style.colorRuns);
    style.setColor({0, 1, 0}, 0, 11);
    QVERIFY(!style.colorRuns);
    QCOMPARE(style.green, 1.0);
}

void TestTextEditing::setFontSemantics()
{
    TextStyle style = plainStyle(QStringLiteral("Hello World"));
    style.setFont(QStringLiteral("Courier"), 6, 5);
    QVERIFY(style.fontRuns);
    QCOMPARE(style.requiredFormatVersion(), 11);
    QCOMPARE(style.fontNameAt(0), QStringLiteral("DejaVu Sans"));
    QCOMPARE(style.uniformFontName(6, 5), QStringLiteral("Courier"));
    QVERIFY(style.uniformFontName(0, 11).isEmpty());
    style.setFont(QStringLiteral("Courier"), 0, 0);
    QVERIFY(!style.fontRuns);
    QCOMPARE(style.fontName, QStringLiteral("Courier"));
    QCOMPARE(style.requiredFormatVersion(), 1);
}

void TestTextEditing::runsSurviveEdits()
{
    TextStyle style = plainStyle(QStringLiteral("Hello World"));
    style.setColor({1, 0, 0}, 0, 5);
    style.replaceCharacters(5, 0, 3);   // typing after "Hello" inherits red
    style.content.insert(5, QStringLiteral("!!!"));
    QCOMPARE(style.colorAt(7).r, 1.0);
    QCOMPARE(style.colorAt(9).r, 0.0);
    style.replaceCharacters(0, 8, 0);   // delete "Hello!!!"
    style.content.remove(0, 8);
    QVERIFY(!style.colorRuns);
    QVERIFY(style.colorRunsAreValid());
}

void TestTextEditing::renderingColorsLettersIndividually()
{
    TextStyle style = plainStyle(QStringLiteral("MMMM  WWWW"));
    style.setColor({1, 0, 0}, 0, 4);
    style.setColor({0, 0, 1}, 6, 4);
    QFont font(style.fontName); font.setPixelSize(40);
    const QImage image = renderText(style, QSize(), font, 0, false);
    QVERIFY(!image.isNull());
    const int half = image.width() / 2;
    const Tally left = tally(image, 0, half);
    const Tally right = tally(image, half, image.width());
    QVERIFY(left.red > 50);
    QCOMPARE(left.blue, 0);
    QVERIFY(right.blue > 50);
    QCOMPARE(right.red, 0);

    // Uniform text renders as it always did.
    TextStyle uniform = plainStyle(QStringLiteral("MMMM  WWWW"));
    uniform.red = 1.0;
    const QImage flat = renderText(uniform.content, QSize(), font, QColor::fromRgbF(1, 0, 0), 0, false);
    QCOMPARE(renderText(uniform, QSize(), font, 0, false), flat);
}

void TestTextEditing::editorReadsBackUntouchedRuns()
{
    const Document doc = ProjectReader::load(fixture(QStringLiteral("valid/v11_text_font_runs.comp")));
    const TextStyle original = *doc.layers.at(0).text;
    InlineTextEditor editor;
    editor.loadStyle(original);
    QCOMPARE(editor.toPlainText(), original.content);
    const TextStyle back = editor.collectStyle(original);
    QCOMPARE(back, original);
}

void TestTextEditing::editorColorsSelectionOnly()
{
    InlineTextEditor editor;
    editor.loadStyle(plainStyle(QStringLiteral("Hello World")));
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(0); cursor.setPosition(5, QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
    editor.applyColor(QColor(255, 0, 0));
    QCOMPARE(editor.colorAtSelection(), QColor(255, 0, 0));
    const TextStyle edited = editor.collectStyle(plainStyle(QStringLiteral("Hello World")));
    QVERIFY(edited.colorRuns);
    QCOMPARE(edited.colorAt(0).r, 1.0);
    QCOMPARE(edited.colorAt(6).r, 0.0);
    QCOMPARE(edited.requiredFormatVersion(), 10);

    // Nothing selected: the whole text changes and the runs go.
    cursor.setPosition(3);
    editor.setTextCursor(cursor);
    editor.applyColor(QColor(0, 0, 255));
    const TextStyle all = editor.collectStyle(edited);
    QVERIFY(!all.colorRuns);
    QCOMPARE(all.blue, 1.0);
}

void TestTextEditing::editorTypedLettersInherit()
{
    InlineTextEditor editor;
    TextStyle style = plainStyle(QStringLiteral("Hello World"));
    style.setColor({1, 0, 0}, 0, 5);
    editor.loadStyle(style);
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(5);
    editor.setTextCursor(cursor);
    editor.insertPlainText(QStringLiteral("!!"));
    const TextStyle edited = editor.collectStyle(style);
    QCOMPARE(edited.content, QStringLiteral("Hello!! World"));
    QCOMPARE(edited.colorAt(5).r, 1.0);
    QCOMPARE(edited.colorAt(6).r, 1.0);
    QCOMPARE(edited.colorAt(7).r, 0.0);
    QVERIFY(edited.colorRunsAreValid());
}

void TestTextEditing::editorFaceAtSelectionReportsMixed()
{
    InlineTextEditor editor;
    TextStyle style = plainStyle(QStringLiteral("Hello World"));
    style.setFont(QStringLiteral("Courier"), 6, 5);
    editor.loadStyle(style);
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(0); cursor.setPosition(11, QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
    QVERIFY(editor.faceAtSelection().isEmpty());   // "(Multiple)"
    cursor.setPosition(6); cursor.setPosition(11, QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
    QCOMPARE(editor.faceAtSelection(), QStringLiteral("Courier"));
    // Choosing a face keeps the selection.
    editor.applyFace(QStringLiteral("Times"));
    QCOMPARE(editor.textCursor().selectionStart(), 6);
    QCOMPARE(editor.textCursor().selectionEnd(), 11);
    const TextStyle edited = editor.collectStyle(style);
    QCOMPARE(edited.fontNameAt(8), QStringLiteral("Times"));
    QCOMPARE(edited.fontNameAt(0), QStringLiteral("DejaVu Sans"));
}

void TestTextEditing::editorPreviewIsTakenBack()
{
    InlineTextEditor editor;
    TextStyle style = plainStyle(QStringLiteral("Hello World"));
    editor.loadStyle(style);
    editor.document()->clearUndoRedoStacks();
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(0); cursor.setPosition(5, QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
    editor.previewLetterFormat([&] { editor.applyFace(QStringLiteral("Courier")); });
    editor.previewLetterFormat([&] { editor.applyColor(QColor(255, 0, 0)); });
    QVERIFY(editor.hasPreview());
    editor.endPreview(false);
    QCOMPARE(editor.collectStyle(style), style);
    QCOMPARE(editor.textCursor().selectionEnd(), 5);
    QVERIFY(!editor.document()->isUndoAvailable());
}

void TestTextEditing::editorUndoIsOwnHistory()
{
    InlineTextEditor editor;
    editor.show();
    editor.loadStyle(plainStyle(QString()));
    editor.document()->clearUndoRedoStacks();
    QTest::keyClicks(&editor, QStringLiteral("typed text"));
    QCOMPARE(editor.toPlainText(), QStringLiteral("typed text"));
    editor.undo();
    QVERIFY(editor.toPlainText().isEmpty());   // one step takes back the typing
    editor.redo();
    QCOMPARE(editor.toPlainText(), QStringLiteral("typed text"));
}

void TestTextEditing::sessionPromotesVersionAndUndoRestoresIt()
{
    EditorSession session;
    session.createDocument(400, 200);
    const int before = session.document()->formatVersion;
    TextStyle style = plainStyle(QStringLiteral("Hello World"));
    style.setColor({1, 0, 0}, 0, 5);
    QVERIFY(session.addText(style, QRectF(10, 10, 300, 100), false, false, false, false));
    QVERIFY(session.document()->formatVersion >= 10);
    QVERIFY(session.activeLayer()->text->colorRuns);
    const QUuid id = session.activeLayer()->id;

    // Fonts need version 11; undoing the edit brings the older version back.
    const int afterColor = session.document()->formatVersion;
    style.setFont(QStringLiteral("Courier"), 6, 5);
    QVERIFY(session.updateText(id, style, QRectF(10, 10, 300, 100), false, false, false, false));
    QVERIFY(session.document()->formatVersion >= 11);
    session.undo();
    QCOMPARE(session.document()->formatVersion, afterColor);
    QVERIFY(!session.activeLayer()->text->fontRuns);
    session.undo();
    QCOMPARE(session.document()->formatVersion, before);
}

void TestTextEditing::sessionEditKeepsRuns()
{
    EditorSession session;
    session.createDocument(400, 200);
    TextStyle style = plainStyle(QStringLiteral("MMMM WWWW"));
    style.setColor({1, 0, 0}, 0, 4);
    style.setColor({0, 0, 1}, 5, 4);
    QVERIFY(session.addText(style, QRectF(0, 0, 0, 0), false, false, false, false));
    const QUuid id = session.activeLayer()->id;
    const Layer &layer = *session.activeLayer();
    QFont font(style.fontName); font.setPixelSize(40);
    QVERIFY(tally(layer.image, 0, layer.image.width() / 2).red > 50);
    QVERIFY(tally(layer.image, layer.image.width() / 2, layer.image.width()).blue > 50);

    // Edit through an editor, as the Type tool does.
    InlineTextEditor editor;
    editor.loadStyle(style);
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(4); editor.setTextCursor(cursor);
    editor.insertPlainText(QStringLiteral("M"));
    const TextStyle edited = editor.collectStyle(style);
    QVERIFY(session.updateText(id, edited, QRectF(0, 0, 200, 60), false, false, false, false));
    const TextStyle &saved = *session.activeLayer()->text;
    QCOMPARE(saved.content, QStringLiteral("MMMMM WWWW"));
    QVERIFY(saved.colorRuns);
    QCOMPARE(saved.colorAt(4).r, 1.0);
    QCOMPARE(saved.colorAt(7).b, 1.0);
    QVERIFY(tally(session.activeLayer()->image, session.activeLayer()->image.width() / 2, session.activeLayer()->image.width()).blue > 50);
}

void TestTextEditing::flatUpdateKeepsRunsUnlessBaseChanged()
{
    EditorSession session;
    session.createDocument(400, 200);
    TextStyle style = plainStyle(QStringLiteral("Hello World"));
    style.setColor({1, 0, 0}, 0, 5);
    QVERIFY(session.addText(style, QRectF(0, 0, 300, 100), false, false, false, true));
    const QUuid id = session.activeLayer()->id;
    const QColor base = QColor::fromRgbF(style.red, style.green, style.blue);
    QVERIFY(session.updateText(id, QStringLiteral("Hello, World"), QRectF(0, 0, 300, 100), style.fontName, 40, false, false, false, 0, base, true));
    QVERIFY(session.activeLayer()->text->colorRuns);
    QCOMPARE(session.activeLayer()->text->colorAt(0).r, 1.0);
    QCOMPARE(session.activeLayer()->text->colorAt(8).r, 0.0);
    // A different flat color restyles the whole text.
    QVERIFY(session.updateText(id, QStringLiteral("Hello, World"), QRectF(0, 0, 300, 100), style.fontName, 40, false, false, false, 0, QColor(0, 255, 0), true));
    QVERIFY(!session.activeLayer()->text->colorRuns);
    QCOMPARE(session.activeLayer()->text->green, 1.0);
}

void TestTextEditing::packageRoundTripAfterEditing()
{
    EditorSession session;
    session.createDocument(400, 200);
    TextStyle style = plainStyle(QStringLiteral("Hello World"));
    style.setColor({1, 0, 0}, 0, 5);
    style.setFont(QStringLiteral("Courier"), 6, 5);
    QVERIFY(session.addText(style, QRectF(0, 0, 300, 100), true, false, false, true));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("edited.comp"));
    ProjectWriter::save(*session.document(), out);
    const Document reloaded = ProjectReader::load(out);
    QCOMPARE(reloaded.formatVersion, 11);
    const TextStyle &text = *reloaded.layers.at(0).text;
    QCOMPARE(text.colorAt(0).r, 1.0);
    QCOMPARE(text.fontNameAt(8), QStringLiteral("Courier"));
    QVERIFY(text.uniformFontName(0, 5) == QStringLiteral("DejaVu Sans"));
}

void TestTextEditing::filtersWaitForTextEditing()
{
    MainWindow window;
    window.session().createDocument(400, 300);
    window.session().insertImage(QImage(100, 100, QImage::Format_RGBA8888_Premultiplied), QStringLiteral("Pixels"));
    window.syncDocumentViews();
    window.show();
    QAction *invert = nullptr, *blur = nullptr;
    for (QAction *top : window.menuBar()->actions()) if (QMenu *menu = top->menu())
        for (QAction *item : menu->actions()) {
            if (item->text() == QStringLiteral("&Invert")) invert = item;
            if (item->text() == QStringLiteral("Gaussian Blur…")) blur = item;
        }
    QVERIFY(invert && blur);
    QVERIFY(invert->isEnabled() && blur->isEnabled());
    Q_EMIT window.canvas()->textBoxRequested(QRectF(20, 20, 200, 80), true);
    QVERIFY(window.inlineTextEditor() != nullptr);
    QVERIFY(!invert->isEnabled());
    QVERIFY(!blur->isEnabled());
    // Undo while typing answers from the text box, not the document.
    QTest::keyClicks(window.inlineTextEditor(), QStringLiteral("abc"));
    const bool documentCouldUndo = window.session().canUndo();
    QAction *undo = window.findChild<QAction *>(QStringLiteral("commandUndo"));
    QVERIFY(undo && undo->isEnabled());
    undo->trigger();
    QVERIFY(window.inlineTextEditor() != nullptr);
    QVERIFY(window.inlineTextEditor()->toPlainText().isEmpty());
    QCOMPARE(window.session().canUndo(), documentCouldUndo);
    window.inlineTextEditor()->finish(false);
    QVERIFY(window.inlineTextEditor() == nullptr);
    QVERIFY(invert->isEnabled() && blur->isEnabled());
}

QTEST_MAIN(TestTextEditing)
#include "TestTextEditing.moc"
