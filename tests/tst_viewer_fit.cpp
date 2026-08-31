// Widget-level guard rails for the fit modes on documents whose pages differ in
// size. tst_view_layout proves ViewLayout's arithmetic; this proves the viewer
// asks it the right question and that the answer survives the scrollbar dance.
//
// The main fixture is generated here: an A3 landscape title sheet
// (1190.5 x 841.9 pt) followed by four A4 landscape sheets (841.9 x 595.3), i.e.
// one sheet of exactly twice the area of the rest. Before v1.49.0 the viewer
// fitted twice the CURRENT page's width, so Fit Width on this file returned two
// different scales 1.41x apart depending on where the reader had scrolled - one
// leaving a seventh of the window empty, the other overflowing it and raising a
// horizontal scrollbar inside a mode named Fit Width.
//
// Two more fixtures earn their keep:
//   - a one-page document, because two-page mode used to halve it;
//   - a synthetic document whose two halves of a spread differ in HEIGHT.
//     the mixed-size fixture cannot show that: once the oversized sheet has a row of its
//     own, every remaining row holds two identical sheets, so nothing inside a row
//     is misaligned and the navigation bugs become unreachable on it.
//
// Two cases are controls rather than assertions about the bug: singleMode... pins
// that the fit stays page-local where only one page is laid out (an over-broad fix
// would break it), and refitIsIdempotent is the v1.28.1 oscillation guard, run at
// rotation 90 as well as 0 because at rotation 0 the width term binds and the
// height half of the fit would never be exercised.
//
// One viewer and one engine serve every case, and every document stays alive until
// cleanup: creating and destroying viewers while the engine still has renders in
// flight crashes at teardown.
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "ui/ViewerWidget.h"

#include <QFile>
#include <QFileInfo>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using mervin::ViewLayout;
using mervin::ViewerWidget;

// The four layout modes. Scrolling and the spread are independent axes, so
// kSpread is "spread, scrolled continuously" and kSingleSpread is "one spread at
// a time" - the combination that did not exist before v1.52.0.
static constexpr ViewLayout::Mode kContinuous{ViewLayout::Scroll::Continuous, false};
static constexpr ViewLayout::Mode kSingle{ViewLayout::Scroll::Single, false};
static constexpr ViewLayout::Mode kSpread{ViewLayout::Scroll::Continuous, true};
static constexpr ViewLayout::Mode kSingleSpread{ViewLayout::Scroll::Single, true};

static QString modeName(ViewLayout::Mode m)
{
    return QStringLiteral("%1%2")
        .arg(m.scroll == ViewLayout::Scroll::Single ? QStringLiteral("single")
                                                    : QStringLiteral("continuous"))
        .arg(m.spread ? QStringLiteral("+spread") : QString());
}

class TstViewerFit : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void fitWidthIsPageIndependent();
    void fitNeverRaisesHorizontalScrollbar();
    void fitWidthFillsTheWindow();
    void fitPageFitsTheWholeRow();
    void refitIsIdempotent();
    void singleModeStaysPageLocal();
    void spreadTogglesOffAndKeepsTheScrollMode();
    void singleSpreadFitsTheWholeSpread();
    void singlePageStillLandsAtTheLeftEdge();
    void lonePageIsNotHalfSize();
    void goToPageLandsOnTheRowTop();
    void goToPageEmitsOnePageChange();
    void clickBesideAShortPageStaysOnThatPage();

private:
    // Point the one viewer at a document and settle the deferred layout pass.
    void use(mervin::Document *d, int w = 1000, int h = 800);
    // A ViewLayout laid out the same way the viewer's private one is, so geometry
    // can be asserted (the viewer keeps no accessor for it).
    ViewLayout mirror(mervin::Document *d, ViewLayout::Mode mode) const;

    std::unique_ptr<mervin::RenderEngine> engine_;
    std::unique_ptr<QTemporaryDir> dir_;
    std::shared_ptr<mervin::Document> schematic_;
    std::shared_ptr<mervin::Document> lone_;
    std::shared_ptr<mervin::Document> unequal_;
    std::unique_ptr<ViewerWidget> viewer_;
};

namespace {

QByteArray assemblePdf(const QList<QByteArray> &bodies)
{
    QByteArray pdf = "%PDF-1.7\n";
    QList<int> offsets;
    for (int i = 0; i < bodies.size(); ++i) {
        offsets << pdf.size();
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + bodies[i] + "\nendobj\n";
    }
    const int xrefOff = pdf.size();
    const int n = bodies.size() + 1;
    pdf += "xref\n0 " + QByteArray::number(n) + "\n0000000000 65535 f \n";
    for (int off : offsets) {
        QByteArray rec = QByteArray::number(off);
        while (rec.size() < 10)
            rec.prepend('0');
        pdf += rec + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + QByteArray::number(n) + " /Root 1 0 R >>\nstartxref\n"
           + QByteArray::number(xrefOff) + "\n%%EOF\n";
    return pdf;
}

// Pages carrying nothing but a /MediaBox, so the geometry is exact.
QByteArray mediaBoxPdf(const QList<QSizeF> &sizes)
{
    QList<QByteArray> bodies;
    QByteArray kids;
    for (int i = 0; i < sizes.size(); ++i)
        kids += QByteArray::number(i + 3) + " 0 R ";
    bodies << "<< /Type /Catalog /Pages 2 0 R >>";
    bodies << "<< /Type /Pages /Count " + QByteArray::number(sizes.size()) + " /Kids [" + kids
                    + "] >>";
    for (const QSizeF &s : sizes)
        bodies << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 "
                        + QByteArray::number(s.width()) + " " + QByteArray::number(s.height())
                        + "] >>";
    return assemblePdf(bodies);
}

} // namespace

void TstViewerFit::initTestCase()
{
    engine_ = std::make_unique<mervin::RenderEngine>();
    dir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(dir_->isValid());

    const QString pdf = dir_->filePath(QStringLiteral("mixed-sizes.pdf"));
    QFile schematicFile(pdf);
    QVERIFY(schematicFile.open(QIODevice::WriteOnly));
    schematicFile.write(mediaBoxPdf({{1190.5, 841.9}, {841.9, 595.3}, {841.9, 595.3},
                                     {841.9, 595.3}, {841.9, 595.3}}));
    schematicFile.close();

    const QString house = dir_->filePath(QStringLiteral("one-page.pdf"));
    QFile houseFile(house);
    QVERIFY(houseFile.open(QIODevice::WriteOnly));
    houseFile.write(mediaBoxPdf({{612, 792}}));
    houseFile.close();

    QString err;
    schematic_ = engine_->openDocument(pdf, QString(), &err);
    QVERIFY2(schematic_ != nullptr, qPrintable(err));
    QCOMPARE(schematic_->pageCount(), 5);
    // The premise of the cases below: this document really is mixed-size.
    QVERIFY(schematic_->pageSize(0).width() > schematic_->pageSize(1).width() * 1.4);

    lone_ = engine_->openDocument(house, QString(), &err);
    QVERIFY2(lone_ != nullptr, qPrintable(err));
    QCOMPARE(lone_->pageCount(), 1);

    // 400x300 beside 400x420: an area ratio of 1.4, under the oversized threshold,
    // so they pair - and the shorter sheet leaves a band beside its partner.
    const QString path = dir_->filePath(QStringLiteral("unequal.pdf"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(mediaBoxPdf({{400, 300}, {400, 420}, {400, 300}, {400, 420}}));
    f.close();
    unequal_ = engine_->openDocument(path, QString(), &err);
    QVERIFY2(unequal_ != nullptr, qPrintable(err));

    viewer_ = std::make_unique<ViewerWidget>(engine_.get());
    // Laid out and painted, but never mapped: show() runs the real showEvent /
    // fit-scale path without a window appearing (as tst_viewer_preview does).
    viewer_->setAttribute(Qt::WA_DontShowOnScreen, true);
    viewer_->resize(1000, 800);
    viewer_->setDocument(schematic_.get());
    viewer_->show();
}

void TstViewerFit::cleanupTestCase()
{
    // This test walks three documents through every page mode, rotation and fit
    // mode without ever waiting for a render, so it leaves the worker threads a
    // long queue. Join them explicitly before anything is destroyed: without this
    // the process passes all its cases and then dies in MuPDF at teardown
    // ("uninitialized font structure"), because a worker is still rasterising a
    // document whose context is being torn down under it. tst_viewer_preview gets
    // away without it only because it settles after each step.
    engine_->shutdown();
    viewer_.reset();
    QCoreApplication::processEvents();
}

void TstViewerFit::use(mervin::Document *d, int w, int h)
{
    viewer_->resize(w, h);
    viewer_->setDocument(d);
    // Qt defers scrollbar visibility (and the resizeEvent that follows) to the
    // next layout pass, so a range read before it is an intermediate state.
    QCoreApplication::processEvents();
}

ViewLayout TstViewerFit::mirror(mervin::Document *d, ViewLayout::Mode mode) const
{
    ViewLayout l;
    l.setDocument(d);
    l.setMode(mode);
    l.setRotation(viewer_->rotation());
    l.setScale(viewer_->scale());
    return l;
}

// The bug the user reported, at the level they met it.
void TstViewerFit::fitWidthIsPageIndependent()
{
    use(schematic_.get());
    for (ViewLayout::Mode mode :
         {kSpread, kContinuous}) {
        viewer_->setLayoutMode(mode);
        viewer_->setRotation(0);
        double first = 0.0;
        for (int p = 0; p < 5; ++p) {
            viewer_->goToPage(p);
            viewer_->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
            if (p == 0)
                first = viewer_->scale();
            QVERIFY2(qFuzzyCompare(viewer_->scale(), first),
                     qPrintable(QStringLiteral("mode=%1: Fit Width from page %2 gives %3, from "
                                               "page 1 it gives %4")
                                    .arg(modeName(mode)).arg(p + 1).arg(viewer_->scale()).arg(first)));
        }
    }
}

// A fit mode that scrolls sideways is a contradiction in terms.
void TstViewerFit::fitNeverRaisesHorizontalScrollbar()
{
    use(schematic_.get());
    for (ViewLayout::Mode mode : {kSpread,
                                      kContinuous,
                                      kSingle}) {
        viewer_->setLayoutMode(mode);
        for (int rot : {0, 90, 180, 270}) {
            viewer_->setRotation(rot);
            for (ViewerWidget::ZoomMode zm :
                 {ViewerWidget::ZoomMode::FitWidth, ViewerWidget::ZoomMode::FitPage}) {
                for (int p = 0; p < 5; ++p) {
                    viewer_->goToPage(p);
                    viewer_->setZoomMode(zm);
                    QCoreApplication::processEvents(); // settle the deferred layout pass
                    QVERIFY2(viewer_->horizontalScrollBar()->maximum() == 0,
                             qPrintable(QStringLiteral("mode=%1 rot=%2 zoom=%3 page=%4: %5 px of "
                                                       "horizontal scroll in a fit mode")
                                            .arg(modeName(mode)).arg(rot).arg(int(zm)).arg(p + 1)
                                            .arg(viewer_->horizontalScrollBar()->maximum())));
                }
            }
        }
    }
    viewer_->setRotation(0);
}

// ...and one that fits by shrinking everything is no better. This is what stops a
// future "just use twice the widest page" from passing the case above: that basis
// would leave the A3 spread about a seventh under-zoomed.
void TstViewerFit::fitWidthFillsTheWindow()
{
    use(schematic_.get());
    viewer_->setRotation(0);
    for (ViewLayout::Mode mode :
         {kSpread, kContinuous}) {
        viewer_->setLayoutMode(mode);
        viewer_->goToPage(0);
        viewer_->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
        const int unused = viewer_->viewport()->width() - mirror(schematic_.get(), mode).totalSize().width();
        QVERIFY2(unused >= 0 && unused <= 28,
                 qPrintable(QStringLiteral("mode=%1: %2 px of the viewport went unused")
                                .arg(modeName(mode)).arg(unused)));
    }
}

// Fit Page has to fit the taller half of the spread too.
void TstViewerFit::fitPageFitsTheWholeRow()
{
    use(unequal_.get(), 1000, 400); // short enough that the height axis binds
    viewer_->setLayoutMode(kSpread);
    for (int p = 0; p < 4; ++p) {
        viewer_->goToPage(p);
        viewer_->setZoomMode(ViewerWidget::ZoomMode::FitPage);
        const ViewLayout m = mirror(unequal_.get(), kSpread);
        int rowH = 0;
        for (int k = m.rowStart(p); k < m.rowEnd(p); ++k)
            rowH = std::max(rowH, m.pageRect(k).height());
        QVERIFY2(rowH <= viewer_->viewport()->height() - 32,
                 qPrintable(QStringLiteral("page %1: its row is %2 px in a %3 px viewport")
                                .arg(p + 1).arg(rowH).arg(viewer_->viewport()->height())));
    }
}

// The v1.28.1 guard: a re-fit must be a fixed point, or a scrollbar toggling
// visibility resizes the viewport back into applyFitScale for ever.
void TstViewerFit::refitIsIdempotent()
{
    use(schematic_.get());
    for (ViewLayout::Mode mode : {kSpread,
                                      kContinuous,
                                      kSingle}) {
        viewer_->setLayoutMode(mode);
        for (int rot : {0, 90}) {
            viewer_->setRotation(rot);
            for (ViewerWidget::ZoomMode zm :
                 {ViewerWidget::ZoomMode::FitWidth, ViewerWidget::ZoomMode::FitPage}) {
                viewer_->setZoomMode(zm);
                const double base = viewer_->scale();
                viewer_->resize(1000, 801);
                viewer_->resize(1000, 800);
                QVERIFY2(qFuzzyCompare(viewer_->scale(), base),
                         qPrintable(QStringLiteral("mode=%1 rot=%2 zoom=%3: %4 after a resize round "
                                                   "trip, was %5")
                                        .arg(modeName(mode)).arg(rot).arg(int(zm))
                                        .arg(viewer_->scale()).arg(base)));
                viewer_->setZoomMode(zm); // re-selecting the same mode is a no-op
                QVERIFY(qFuzzyCompare(viewer_->scale(), base));
                viewer_->hide();
                viewer_->show();
                QVERIFY2(qFuzzyCompare(viewer_->scale(), base), "hide/show moved the fit scale");
            }
        }
    }
    viewer_->setRotation(0);
}

// Control against over-fixing: Single mode lays out only the current page, so its
// fit must stay page-local. Fitting the widest page there would shrink an A4 sheet
// to 71 % for no reason at all.
void TstViewerFit::singleModeStaysPageLocal()
{
    use(schematic_.get());
    viewer_->setLayoutMode(kSingle);
    viewer_->goToPage(0);
    viewer_->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
    const double a3 = viewer_->scale();
    viewer_->goToPage(2);
    viewer_->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
    const double a4 = viewer_->scale();
    QVERIFY2(a4 > a3 * 1.3, "Single mode must fit the page it is showing, not the widest one");
    QCoreApplication::processEvents();
    QCOMPARE(viewer_->horizontalScrollBar()->maximum(), 0);
}

// The bug this file's newest cases exist for: Two-Page Spread was one of three
// values of a single enum, so it could only be turned ON - clicking it again
// re-sent the same value and nothing happened - and choosing it threw away the
// reader's Continuous/Single choice. Both axes must now move alone.
void TstViewerFit::spreadTogglesOffAndKeepsTheScrollMode()
{
    use(unequal_.get()); // 400x300 / 400x420 x2 - the halves of a spread differ
    for (ViewLayout::Scroll scroll : {ViewLayout::Scroll::Continuous, ViewLayout::Scroll::Single}) {
        viewer_->setLayoutMode({scroll, false});
        viewer_->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
        QCoreApplication::processEvents();
        const double flat = viewer_->scale();

        viewer_->setSpread(true);
        QCoreApplication::processEvents();
        QCOMPARE(viewer_->layoutMode().spread, true);
        QCOMPARE(viewer_->layoutMode().scroll, scroll); // the scroll choice survived
        QVERIFY2(viewer_->scale() < flat * 0.75, "a spread must fit two sheets, so it is smaller");

        // Clicking Two-Page Spread again turns it off and lands back exactly where
        // it started. Before the split there was no second state to return to.
        viewer_->setSpread(false);
        QCoreApplication::processEvents();
        QCOMPARE(viewer_->layoutMode().spread, false);
        QCOMPARE(viewer_->layoutMode().scroll, scroll);
        QVERIFY2(qFuzzyCompare(viewer_->scale(), flat),
                 qPrintable(QStringLiteral("spread off returned %1, started at %2")
                                .arg(viewer_->scale()).arg(flat)));
    }

    // And the mirror: changing the scroll mode must not disturb the spread.
    viewer_->setLayoutMode({ViewLayout::Scroll::Continuous, true});
    viewer_->setScrollMode(ViewLayout::Scroll::Single);
    QCOMPARE(viewer_->layoutMode().spread, true);
    viewer_->setScrollMode(ViewLayout::Scroll::Continuous);
    QCOMPARE(viewer_->layoutMode().spread, true);

    // layoutModeChanged has to fire on EITHER axis, because MainWindow hangs the
    // menu's Scroll rows and Two-Page Spread tick off it. Without that connection
    // the tick only refreshed on a tab switch, so a two-page default opened in a
    // spread with the menu insisting it was off - and the toggle's first click
    // then re-sent the state the viewer was already in and appeared to do nothing.
    QSignalSpy spy(viewer_.get(), &ViewerWidget::layoutModeChanged);
    viewer_->setSpread(false);
    QCOMPARE(spy.count(), 1);
    viewer_->setScrollMode(ViewLayout::Scroll::Single);
    QCOMPARE(spy.count(), 2);
    viewer_->setSpread(false); // already off - no change, no signal
    viewer_->setScrollMode(ViewLayout::Scroll::Single);
    QCOMPARE(spy.count(), 2);
    viewer_->setLayoutMode({ViewLayout::Scroll::Continuous, true}); // both at once: one signal
    QCOMPARE(spy.count(), 3);
}

// One spread at a time - the combination the old enum could not express. The fit
// owes it the same invariant as every other mode: the canvas fits the viewport in
// both axes, and no horizontal scrollbar appears inside a fit mode. The unequal
// fixture is the one that can break it, because heightForScale has to take the
// TALLER half of the row or the vertical-bar prediction misses.
void TstViewerFit::singleSpreadFitsTheWholeSpread()
{
    use(unequal_.get());
    viewer_->setLayoutMode(kSingleSpread);
    for (int page : {0, 1, 2, 3}) {
        viewer_->goToPage(page);
        for (ViewerWidget::ZoomMode zm :
             {ViewerWidget::ZoomMode::FitWidth, ViewerWidget::ZoomMode::FitPage}) {
            viewer_->setZoomMode(zm);
            QCoreApplication::processEvents();
            const QString at = QStringLiteral("page %1 zm=%2").arg(page + 1).arg(int(zm));
            QVERIFY2(viewer_->horizontalScrollBar()->maximum() == 0,
                     qPrintable(QStringLiteral("%1: a fit mode raised a horizontal scrollbar (max %2)")
                                    .arg(at).arg(viewer_->horizontalScrollBar()->maximum())));
            if (zm == ViewerWidget::ZoomMode::FitPage)
                QVERIFY2(viewer_->verticalScrollBar()->maximum() == 0,
                         qPrintable(QStringLiteral("%1: Fit Page left the spread scrolling")
                                        .arg(at)));
        }
    }

    // Both sheets of the row are laid out, and the fit is the same whichever half
    // is current - so turning to the facing page does not rescale the spread.
    viewer_->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
    viewer_->goToPage(0);
    QCoreApplication::processEvents();
    const double onLeft = viewer_->scale();
    viewer_->goToPage(1);
    QCoreApplication::processEvents();
    QVERIFY2(qFuzzyCompare(viewer_->scale(), onLeft),
             "the facing sheet is the same row and must fit identically");

    const ViewLayout m = mirror(unequal_.get(), kSingleSpread);
    QVERIFY2(m.pageRect(0).isValid() && m.pageRect(1).isValid(),
             "both halves of the current spread must be laid out");
    QVERIFY2(!m.pageRect(2).isValid() && !m.pageRect(3).isValid(),
             "no page outside the current row may be laid out");
    QCOMPARE(m.pageRect(0).top(), m.pageRect(1).top()); // top-aligned, as a bound book

    // Next / Previous move by the unit on SCREEN. In Single+spread that is the
    // spread, so one press must reach the next one - stepping a single page would
    // land on the facing sheet of the row already laid out and the toolbar arrow
    // would look dead every other click.
    viewer_->goToPage(0);
    viewer_->nextPage();
    QCOMPARE(viewer_->currentPage(), 2); // row {0,1} -> row {2,3}
    viewer_->nextPage();
    QCOMPARE(viewer_->currentPage(), 2); // last row: stay put rather than half-step
    viewer_->prevPage();
    QCOMPARE(viewer_->currentPage(), 0);
    viewer_->prevPage();
    QCOMPARE(viewer_->currentPage(), 0); // first row

    // And the control: without the spread, paging is page-granular as it always was.
    viewer_->setSpread(false);
    viewer_->goToPage(0);
    viewer_->nextPage();
    QCOMPARE(viewer_->currentPage(), 1);
    viewer_->prevPage();
    QCOMPARE(viewer_->currentPage(), 0);
}

// goToPage grew a shared "scroll the least that brings the target page into view"
// step so that navigating to the RIGHT-hand sheet of a zoomed spread does not
// leave it off screen. That step must stay invisible where it cannot apply: a
// single page zoomed past the viewport still lands at the left edge, as it did
// before the axes were split.
void TstViewerFit::singlePageStillLandsAtTheLeftEdge()
{
    use(schematic_.get());
    viewer_->setLayoutMode(kSingle);
    viewer_->setScale(4.0); // well past Fit Width, so the page overflows sideways
    QCoreApplication::processEvents();
    for (int page : {1, 2, 0}) {
        viewer_->goToPage(page);
        QVERIFY2(viewer_->horizontalScrollBar()->maximum() > 0,
                 "the fixture must actually be scrolling sideways at this zoom");
        QCOMPARE(viewer_->horizontalScrollBar()->value(), 0);
        QCOMPARE(viewer_->verticalScrollBar()->value(), 0);
    }
    viewer_->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
}

// A single-page document in two-page mode is a page, not half a spread. It used to
// be fitted against twice its own width, so the same window drew it at half the
// size every other mode gave it - with a horizontal scrollbar showing while half
// the viewport sat empty.
void TstViewerFit::lonePageIsNotHalfSize()
{
    use(lone_.get());
    viewer_->setLayoutMode(kContinuous);
    viewer_->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
    const double continuous = viewer_->scale();
    viewer_->setLayoutMode(kSpread);
    viewer_->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
    const double spread = viewer_->scale();

    QVERIFY2(qFuzzyCompare(spread, continuous),
             qPrintable(QStringLiteral("one page fits at %1 in two-page mode but %2 in continuous "
                                       "(ratio %3)")
                            .arg(spread).arg(continuous).arg(continuous / spread)));
    QCoreApplication::processEvents();
    QCOMPARE(viewer_->horizontalScrollBar()->maximum(), 0);
}

// Navigating to the shorter half of a spread must show the whole row. Aiming at
// the page's own top cut the head off its taller partner.
void TstViewerFit::goToPageLandsOnTheRowTop()
{
    use(unequal_.get(), 1000, 400); // short, so every row needs scrolling to reach
    viewer_->setLayoutMode(kSpread);
    viewer_->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
    const ViewLayout m = mirror(unequal_.get(), kSpread);
    // Precondition: the halves really do differ, so the test can discriminate.
    QVERIFY(m.pageRect(0).height() < m.pageRect(1).height());
    QVERIFY(viewer_->verticalScrollBar()->maximum() > 0);

    for (int p = 0; p < 4; ++p) {
        viewer_->goToPage(p);
        // The HIGHEST top edge in the row, not the target page's own: the property
        // that matters is that no sheet of the spread has its head cut off, and
        // taking the target's top would move with the layout and assert nothing.
        int rowTop = INT_MAX;
        for (int k = m.rowStart(p); k < m.rowEnd(p); ++k)
            rowTop = std::min(rowTop, m.pageRect(k).top());
        QVERIFY2(viewer_->verticalScrollBar()->value() <= rowTop,
                 qPrintable(QStringLiteral("page %1: scrolled to %2, past the top of its row at %3 "
                                           "- the taller sheet is clipped")
                                .arg(p + 1).arg(viewer_->verticalScrollBar()->value()).arg(rowTop)));
        QCOMPARE(viewer_->currentPage(), p);
    }
}

// goToPage's own scrollbar write reaches updateCurrentPage synchronously, which
// used to overwrite currentPage_ and then emit a second, disagreeing pageChanged.
// The page box showed a page the viewer was not actually on.
void TstViewerFit::goToPageEmitsOnePageChange()
{
    use(unequal_.get(), 1000, 400);
    viewer_->setLayoutMode(kSpread);
    viewer_->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
    viewer_->goToPage(0);

    for (int p = 1; p < 4; ++p) {
        QSignalSpy spy(viewer_.get(), &ViewerWidget::pageChanged);
        viewer_->goToPage(p);
        QVERIFY2(viewer_->currentPage() == p,
                 qPrintable(QStringLiteral("asked for page %1, landed on %2")
                                .arg(p + 1).arg(viewer_->currentPage() + 1)));
        QVERIFY2(spy.count() == 1,
                 qPrintable(QStringLiteral("page %1: %2 pageChanged emissions")
                                .arg(p + 1).arg(spy.count())));
        QCOMPARE(spy.at(0).at(0).toInt(), p + 1);
    }
}

// The severe one: nothing on screen tells the reader that a click in the blank
// strip beside the shorter half of a spread was attributed to its taller
// neighbour, but a measurement vertex or a comment then lands on the wrong sheet.
void TstViewerFit::clickBesideAShortPageStaysOnThatPage()
{
    use(unequal_.get());
    viewer_->setLayoutMode(kSpread);
    viewer_->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
    const ViewLayout m = mirror(unequal_.get(), kSpread);

    // Page 0 is the short half of row 0; the band under it belongs to no page but
    // is nearer to page 0 than to its taller partner.
    const QRect shortPage = m.pageRect(0);
    const QRect tallPage = m.pageRect(1);
    QVERIFY(shortPage.isValid() && tallPage.isValid());
    QVERIFY2(shortPage.bottom() + 8 < tallPage.bottom(), "the fixture has no band to probe");
    const QPoint band(shortPage.center().x(), (shortPage.bottom() + tallPage.bottom()) / 2);
    for (int i = 0; i < 4; ++i)
        QVERIFY(!m.pageRect(i).contains(band)); // genuinely off-page
    QVERIFY2(viewer_->pageAtCanvasForTest(band) == 0,
             qPrintable(QStringLiteral("a click in the band beside page 1 resolved to page %1")
                            .arg(viewer_->pageAtCanvasForTest(band) + 1)));

    // Below the last row is nearest to the last page, not back to the first.
    const QRect last = m.pageRect(3);
    QCOMPARE(viewer_->pageAtCanvasForTest(QPoint(last.center().x(), last.bottom() + 40)), 3);
}

QTEST_MAIN(TstViewerFit)
#include "tst_viewer_fit.moc"
