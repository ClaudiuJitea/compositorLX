// Format 10/11: per-letter colors and faces in editable text (mirrors macOS TypeToolTests / ProjectStore rules).
#include "core/Document.h"
#include "io/ProjectReader.h"
#include "io/ProjectWriter.h"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

using namespace compositor;

class TestTextRuns : public QObject
{
    Q_OBJECT
    static QString fixture(const QString &sub) { return QDir(QStringLiteral(FIXTURES_DIR)).filePath(sub); }

private slots:
    void readsVersion10ColorRuns();
    void readsVersion11FontRuns();
    void roundTripPreservesRunsAndVersion();
    void rejectsInvalidRuns_data();
    void rejectsInvalidRuns();
    void writerRefusesRunsInOlderVersion();
    void runAlgebra();
    void replaceCharactersInheritsFromPreviousLetter();
};

void TestTextRuns::readsVersion10ColorRuns()
{
    const Document doc = ProjectReader::load(fixture(QStringLiteral("valid/v10_text_color_runs.comp")));
    QCOMPARE(doc.formatVersion, 10);
    const TextStyle &text = *doc.layers.at(0).text;
    QVERIFY(text.colorRuns && !text.fontRuns);
    QCOMPARE(text.colorRuns->size(), 2);
    QCOMPARE(text.colorAt(0).r, 1.0);
    QCOMPARE(text.colorAt(5).b, 0.3);   // gap between runs: the text's own color
    QCOMPARE(text.colorAt(6).b, 1.0);
}

void TestTextRuns::readsVersion11FontRuns()
{
    const Document doc = ProjectReader::load(fixture(QStringLiteral("valid/v11_text_font_runs.comp")));
    QCOMPARE(doc.formatVersion, 11);
    const TextStyle &text = *doc.layers.at(0).text;
    QVERIFY(text.colorRuns && text.fontRuns);
    QCOMPARE(text.fontNameAt(0), QStringLiteral("Helvetica"));
    QCOMPARE(text.fontNameAt(8), QStringLiteral("Courier"));
    QCOMPARE(text.uniformFontName(6, 5), QStringLiteral("Courier"));
    QVERIFY(text.uniformFontName(0, 11).isEmpty());   // mixed faces
}

void TestTextRuns::roundTripPreservesRunsAndVersion()
{
    for (const QString &name : {QStringLiteral("valid/v10_text_color_runs.comp"), QStringLiteral("valid/v11_text_font_runs.comp")}) {
        const Document original = ProjectReader::load(fixture(name));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString out = dir.filePath(QStringLiteral("out.comp"));
        ProjectWriter::save(original, out);
        const Document reloaded = ProjectReader::load(out);
        QCOMPARE(reloaded.formatVersion, original.formatVersion);
        QCOMPARE(*reloaded.layers.at(0).text, *original.layers.at(0).text);
    }
}

void TestTextRuns::rejectsInvalidRuns_data()
{
    QTest::addColumn<QString>("name");
    for (const char *n : {"malformed_v9_with_color_runs", "malformed_v10_with_font_runs", "malformed_text_runs_overlap",
                          "malformed_text_runs_past_end", "malformed_text_runs_empty", "malformed_text_run_color_range"})
        QTest::newRow(n) << QString::fromLatin1(n);
}

void TestTextRuns::rejectsInvalidRuns()
{
    QFETCH(QString, name);
    bool threw = false;
    try { (void)ProjectReader::load(fixture(QStringLiteral("malformed/") + name + QStringLiteral(".comp"))); }
    catch (const std::exception &) { threw = true; }
    QVERIFY2(threw, qPrintable(name + QStringLiteral(" should be rejected")));
}

void TestTextRuns::writerRefusesRunsInOlderVersion()
{
    Document doc = ProjectReader::load(fixture(QStringLiteral("valid/v11_text_font_runs.comp")));
    doc.formatVersion = 9;
    QTemporaryDir dir;
    bool threw = false;
    try { ProjectWriter::save(doc, dir.filePath(QStringLiteral("x.comp"))); } catch (const std::exception &) { threw = true; }
    QVERIFY(threw);
    QCOMPARE(ProjectWriter::computeTargetVersion(doc), 9);   // explicit version is honored, so the save is refused
}

void TestTextRuns::runAlgebra()
{
    TextStyle t;
    t.content = QStringLiteral("abcdef");
    t.red = t.green = t.blue = 0;
    t.setColor({1, 0, 0}, 1, 2);
    QVERIFY(t.colorRuns && t.colorRuns->size() == 1);
    QCOMPARE(t.colorRuns->first().location, 1);
    QCOMPARE(t.colorRuns->first().length, 2);
    t.setColor({1, 0, 0}, 3, 1);                       // adjacent, same color: merges
    QCOMPARE(t.colorRuns->size(), 1);
    QCOMPARE(t.colorRuns->first().length, 3);
    t.setColor({0, 0, 1}, 0, 6);                       // whole text: becomes the base color, runs vanish
    QVERIFY(!t.colorRuns);
    QCOMPARE(t.blue, 1.0);
    t.setColor({0, 1, 0}, 2, 0);                       // empty range = all
    QCOMPARE(t.green, 1.0);
    t.setFont(QStringLiteral("Courier"), 2, 2);
    QVERIFY(t.fontRuns && t.isValid());
    t.setFont(QStringLiteral("Courier"), 0, 6);
    QVERIFY(!t.fontRuns);
    QCOMPARE(t.fontName, QStringLiteral("Courier"));
    QCOMPARE(t.requiredFormatVersion(), 1);
}

void TestTextRuns::replaceCharactersInheritsFromPreviousLetter()
{
    TextStyle t;
    t.content = QStringLiteral("abcd");
    t.setColor({1, 0, 0}, 0, 2);                       // "ab" red
    t.replaceCharacters(2, 0, 2);                      // type two letters after "ab"
    t.content = QStringLiteral("abXYcd");
    QCOMPARE(t.colorAt(2).r, 1.0);                     // inherited from "b"
    QCOMPARE(t.colorAt(3).r, 1.0);
    QCOMPARE(t.colorAt(4).r, 0.0);
    QVERIFY(t.isValid());
    t.replaceCharacters(0, 6, 0);                      // delete everything
    t.content.clear();
    QVERIFY(!t.colorRuns);
}

QTEST_MAIN(TestTextRuns)
#include "TestTextRuns.moc"
