// Canvas-level selection behaviour (mac SelectionTests modifier/marquee/nudge cases, drawn through real mouse and key events).
#include "core/EditorSession.h"
#include "core/SelectionOps.h"
#include "ui/CanvasWidget.h"

#include <QtTest>

using namespace compositor;

class TestSelectionCanvas : public QObject
{
    Q_OBJECT
    struct Fixture {
        EditorSession session; CanvasWidget canvas;
        Fixture()
        {
            session.createDocument(100, 100, true);
            canvas.resize(400, 400); canvas.setEditorSession(&session); canvas.setDocument(session.document());
            canvas.show(); QVERIFY(QTest::qWaitForWindowExposed(&canvas));
            canvas.actualPixels();
            QObject::connect(&canvas, &CanvasWidget::rectangularSelectionRequested, [this](const QRect &r, int m) { session.setRectangularSelection(r, SelectionMode(m)); });
            QObject::connect(&canvas, &CanvasWidget::selectionMoveRequested, [this](const QPoint &o) { session.moveSelection(o); });
        }
        QPoint at(double x, double y) const { return (canvas.canvasRect().topLeft() + QPointF(x, y) * canvas.zoom()).toPoint(); }
        void drag(QPointF a, QPointF b, Qt::KeyboardModifiers m = {})
        {
            QTest::mousePress(&canvas, Qt::LeftButton, m, at(a.x(), a.y()));
            QTest::mouseEvent(QTest::MouseMove, &canvas, Qt::NoButton, m, at((a.x() + b.x()) / 2, (a.y() + b.y()) / 2));
            QTest::mouseEvent(QTest::MouseMove, &canvas, Qt::NoButton, m, at(b.x(), b.y()));
            QTest::mouseRelease(&canvas, Qt::LeftButton, m, at(b.x(), b.y()));
        }
        int cov(int x, int y) const { return session.document()->selection ? session.document()->selection->convertToFormat(QImage::Format_Grayscale8).constScanLine(y)[x] : 0; }
    };
private slots:
    void marqueeDragIsExactAndClickDeselects()
    {
        Fixture f; f.canvas.setTool(CanvasWidget::Tool::Marquee);
        f.drag({20, 30}, {60, 70});
        QCOMPARE(SelectionOps::coverageBounds(*f.session.document()->selection), QRect(20, 30, 40, 40));
        f.drag({50, 50}, {50, 50});   // a click deselects
        QVERIFY(!f.session.document()->selection);
    }
    void shiftAtPressAddsWithoutSquaring()
    {
        Fixture f; f.canvas.setTool(CanvasWidget::Tool::Marquee);
        f.drag({5, 5}, {15, 15});
        f.drag({40, 40}, {70, 50}, Qt::ShiftModifier);
        QVERIFY(f.cov(10, 10) == 255);                 // the earlier selection is kept
        QVERIFY(f.cov(65, 45) == 255 && f.cov(65, 60) == 0);   // not squared
    }
    void optionSubtracts()
    {
        Fixture f; f.canvas.setTool(CanvasWidget::Tool::Marquee);
        f.session.selectAll();
        f.drag({40, 40}, {60, 60}, Qt::AltModifier);
        QCOMPARE(f.cov(50, 50), 0); QCOMPARE(f.cov(30, 30), 255);
    }
    void arrowNudgesOutlineInAnySelectionMode()
    {
        Fixture f; f.canvas.setTool(CanvasWidget::Tool::Marquee);
        f.canvas.setSelectionMode(1);           // Add mode chosen in the options bar
        f.session.setRectangularSelection(QRect(10, 10, 20, 20));
        f.canvas.setFocus();
        QTest::keyClick(&f.canvas, Qt::Key_Right);
        QCOMPARE(SelectionOps::coverageBounds(*f.session.document()->selection), QRect(11, 10, 20, 20));
    }
    void marchingAntsAreDrawnAtTheOutline()
    {
        Fixture f; f.canvas.setTool(CanvasWidget::Tool::Marquee);
        f.session.setRectangularSelection(QRect(20, 20, 40, 40));
        f.canvas.update();
        const QImage with = f.canvas.grab().toImage();
        f.session.deselect(); f.canvas.update();
        const QImage without = f.canvas.grab().toImage();
        const QPoint edge = f.at(20, 40);
        int differing = 0;
        for (int dx = -2; dx <= 2; ++dx) if (with.pixel(edge + QPoint(dx, 0)) != without.pixel(edge + QPoint(dx, 0))) ++differing;
        QVERIFY2(differing > 0, "no outline drawn at the selection edge");
    }
};

QTEST_MAIN(TestSelectionCanvas)
#include "TestSelectionCanvas.moc"
