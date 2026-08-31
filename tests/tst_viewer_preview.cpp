// Guard rail for the zoom preview layer: proves that the frame drawn right after
// a zoom carries the previous image stretched, instead of the blank paper the
// viewer used to show for the length of a full page render (tens of ms at normal
// zoom, hundreds at deep zoom - see tst_perf_render).
//
// It is deterministic, not timing-dependent: RenderEngine emits resultReady from
// its worker threads, so the connection is queued and no fresh render can be
// delivered between a zoom call and a synchronous viewport grab. Whatever the
// grab shows is what the user would have seen in that gap.
//
// Two things keep the proof honest. Every claim is checked twice - once through
// ViewerWidget::paintStats() (how each page was drawn) and once by counting ink
// pixels in the grabbed frame - and the rotation case acts as a control: it drops
// the preview by design, so it reproduces the old blank-paper frame and shows the
// probe is able to fail.
//
// The last case pins the other half of a smooth zoom: where the view is anchored.
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "ui/ViewerWidget.h"
#include "ui/ThemeTokens.h"

#include <QImage>
#include <QLabel>
#include <QPalette>
#include <QScrollBar>
#include <QtTest>

#include <cstdio>
#include <memory>
#include <vector>

using mervin::PageTheme;
using mervin::ViewerWidget;

class TstViewerPreview : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void zoomInKeepsTheImageOnScreen();
    void zoomOutKeepsTheImageOnScreen();
    void pageModeChangeKeepsTheImage();
    void rotationShowsBlankPaper();   // control: the pre-change behaviour
    void pageThemeShowsBlankPaper();  // control
    void newDocumentDropsTheOldTiles();
    void stepZoomHoldsTheViewportCentre();
    void fitModeHoldsTheVerticalCentre();
    void zoomLadderStepsToRoundLevels();
    void easedZoomProducesIntermediateFrames();
    void easeSurvivesInterruption();
    void viewportChildrenInheritReadableInk();
    void previewBlitCost();

private:
    // A synchronous repaint of the viewport, with the paint accounting reset
    // first. No event loop runs, so no queued render result can slip in.
    QImage frame();
    // Spin until every visible page is drawn from its own sharp render and no zoom
    // ease is in flight (so "settled" also proves every ease terminated).
    bool settle(int maxMs = 15000);
    // Ink (non-paper) pixels in the centre of the frame, which sits inside the
    // page at every zoom level used here. Blank paper scores exactly 0.
    static int centreInk(const QImage &img);
    // Width of the drawn page on the frame's centre row: the run between the
    // outermost pixels that are not the canvas backdrop. With the whole page inside
    // the viewport this IS the magnification the frame was drawn at, which is what
    // separates real intermediate frames from the final frame drawn five times.
    static int pageSpan(const QImage &img);

    std::unique_ptr<mervin::RenderEngine> engine_;
    std::unique_ptr<mervin::Document> doc_;
    std::unique_ptr<ViewerWidget> viewer_;
    int settledInk_ = 0;
};

int TstViewerPreview::centreInk(const QImage &img)
{
    const int side = qMin(320, qMin(img.width(), img.height()) / 2);
    const QRect box(img.width() / 2 - side / 2, img.height() / 2 - side / 2, side, side);
    int ink = 0;
    for (int y = box.top(); y <= box.bottom(); ++y) {
        for (int x = box.left(); x <= box.right(); ++x) {
            const QRgb p = img.pixel(x, y);
            // Near-white scanner noise counts as paper, so only real content
            // registers; the flat fill the viewer used to leave is pure white.
            if (qRed(p) < 235 || qGreen(p) < 235 || qBlue(p) < 235)
                ++ink;
        }
    }
    return ink;
}

int TstViewerPreview::pageSpan(const QImage &img)
{
    const int y = img.height() / 2;
    int first = -1;
    int last = -1;
    for (int x = 0; x < img.width(); ++x) {
        const QRgb p = img.pixel(x, y);
        // The canvas backdrop is a dark neutral in both themes (0x3a3a3c / 0x3c3f44);
        // page paper is white here. Interior ink cannot widen the run, only the
        // outermost non-backdrop pixel counts.
        if (qRed(p) >= 0x50 || qGreen(p) >= 0x50 || qBlue(p) >= 0x50) {
            if (first < 0)
                first = x;
            last = x;
        }
    }
    return first < 0 ? 0 : last - first + 1;
}

QImage TstViewerPreview::frame()
{
    viewer_->resetPaintStats();
    return viewer_->viewport()->grab().toImage();
}

bool TstViewerPreview::settle(int maxMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < maxMs) {
        QTest::qWait(25); // delivers the queued resultReady signals
        frame();
        const ViewerWidget::PaintStats s = viewer_->paintStats();
        if (s.fresh > 0 && s.preview == 0 && s.blank == 0 && !viewer_->zoomEaseActive())
            return true;
    }
    return false;
}

void TstViewerPreview::initTestCase()
{
    const QString pdf = QStringLiteral(MERVIN_HOUSE_PDF);
    if (!QFileInfo::exists(pdf))
        QSKIP("examples/house-drawing.pdf not present");

    engine_ = std::make_unique<mervin::RenderEngine>();
    QString err;
    doc_ = engine_->openDocument(pdf, QString(), &err);
    QVERIFY2(doc_ != nullptr, qPrintable(err));

    viewer_ = std::make_unique<ViewerWidget>(engine_.get());
    // Laid out and painted, but never mapped: show() then runs the real
    // showEvent / fit-scale path without a window appearing, on whatever platform
    // plugin is at hand.
    viewer_->setAttribute(Qt::WA_DontShowOnScreen, true);
    viewer_->resize(1000, 800);
    viewer_->setDocument(doc_.get());
    viewer_->show();
    QVERIFY2(settle(), "the first render never completed");

    // Precondition for every ink assertion below: the sampled box has real
    // content in it when the page is properly drawn.
    settledInk_ = centreInk(frame());
    QVERIFY2(settledInk_ > 2000,
             qPrintable(QStringLiteral("sampled box is nearly empty (%1 ink px) - the ink probe "
                                       "would not discriminate")
                            .arg(settledInk_)));
}

void TstViewerPreview::zoomInKeepsTheImageOnScreen()
{
    QVERIFY(settle());
    viewer_->zoomIn(); // one 1.25x step, as Ctrl+= does
    const QImage after = frame();
    const ViewerWidget::PaintStats s = viewer_->paintStats();

    QCOMPARE(s.blank, 0);
    QVERIFY(s.preview >= 1);
    QVERIFY2(centreInk(after) > settledInk_ / 4,
             "the frame right after zooming in is blank paper");
}

void TstViewerPreview::zoomOutKeepsTheImageOnScreen()
{
    QVERIFY(settle());
    viewer_->setScale(viewer_->scale() / 2.0); // pulls in more of the document
    const QImage after = frame();
    const ViewerWidget::PaintStats s = viewer_->paintStats();

    QCOMPARE(s.blank, 0);
    QVERIFY(s.preview >= 1);
    QVERIFY2(centreInk(after) > settledInk_ / 4,
             "the frame right after zooming out is blank paper");

    // Back to a fit mode: same guarantee through the applyFitScale path.
    QVERIFY(settle());
    viewer_->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
    frame();
    QCOMPARE(viewer_->paintStats().blank, 0);
    QVERIFY(viewer_->paintStats().preview >= 1);
}

void TstViewerPreview::pageModeChangeKeepsTheImage()
{
    QVERIFY(settle());
    viewer_->setSpread(true);
    frame();
    QCOMPARE(viewer_->paintStats().blank, 0);

    QVERIFY(settle());
    viewer_->setSpread(false);
    frame();
    QCOMPARE(viewer_->paintStats().blank, 0);
}

// The control that proves the probes above can fail: a quarter turn flips the
// page's aspect, so the frozen images are dropped on purpose and this frame is
// exactly the blank paper every zoom used to produce.
void TstViewerPreview::rotationShowsBlankPaper()
{
    QVERIFY(settle());
    viewer_->rotateRight();
    const QImage blink = frame();

    QVERIFY(viewer_->paintStats().blank >= 1);
    QCOMPARE(viewer_->paintStats().preview, 0);
    QCOMPARE(centreInk(blink), 0);

    viewer_->rotateLeft();
    QVERIFY(settle());
}

void TstViewerPreview::pageThemeShowsBlankPaper()
{
    QVERIFY(settle());
    viewer_->setPageTheme(PageTheme::Comfort); // re-tones the pixels: previews are void
    frame();
    QVERIFY(viewer_->paintStats().blank >= 1);
    QCOMPARE(viewer_->paintStats().preview, 0);

    viewer_->setPageTheme(PageTheme::Light);
    QVERIFY(settle());
}

void TstViewerPreview::newDocumentDropsTheOldTiles()
{
    QVERIFY(settle());
    viewer_->setDocument(nullptr);
    frame();
    QCOMPARE(viewer_->paintStats().preview, 0);

    viewer_->setDocument(doc_.get());
    QVERIFY(settle());
}

// zoomIn / zoomOut / the zoom box have no cursor behind them, so they hold the
// middle of the view still. (Ctrl+wheel and pinch anchor on the cursor instead,
// through the same code path with a different point.) The old behaviour scrolled
// to the top of the page, which this fails against.
void TstViewerPreview::stepZoomHoldsTheViewportCentre()
{
    QCOMPARE(viewer_->rotation(), 0); // the fraction maths below assumes it
    viewer_->setScale(3.0);           // room to scroll on both axes
    QVERIFY(settle());
    // Park in the middle of the document, away from the scrollbar limits where a
    // clamp would hide any anchoring error.
    viewer_->verticalScrollBar()->setValue(viewer_->verticalScrollBar()->maximum() / 2);
    viewer_->horizontalScrollBar()->setValue(viewer_->horizontalScrollBar()->maximum() / 2);
    QVERIFY(settle());

    // Where the middle of the viewport sits within its page, as a fraction of the
    // page - scale-independent, so it must not move when the scale does.
    const QSizeF pt = doc_->pageSize(0);
    const auto centreFraction = [&] {
        const ViewerWidget::ScrollAnchor a = viewer_->scrollAnchor();
        const QSize vp = viewer_->viewport()->size();
        return QPointF(a.fracX + (vp.width() / 2.0) / (pt.width() * viewer_->scale()),
                       a.fracY + (vp.height() / 2.0) / (pt.height() * viewer_->scale()));
    };

    // Guard against a vacuous pass: parked near the top of the page, holding the
    // centre and jumping to the page top would look the same.
    QVERIFY2(centreFraction().y() > 0.3, "not parked far enough down the page to discriminate");

    for (const char *what : {"zoom in", "zoom out"}) {
        const QPointF before = centreFraction();
        const int page = viewer_->scrollAnchor().page;
        if (qstrcmp(what, "zoom in") == 0)
            viewer_->zoomIn();
        else
            viewer_->zoomOut();
        const QPointF after = centreFraction();
        QCOMPARE(viewer_->scrollAnchor().page, page);
        // Two device pixels of slack: the scroll bars are integers and the page
        // rect is rounded to whole pixels at each scale.
        const double tolX = 2.0 / (pt.width() * viewer_->scale());
        const double tolY = 2.0 / (pt.height() * viewer_->scale());
        QVERIFY2(qAbs(after.x() - before.x()) < tolX, what);
        QVERIFY2(qAbs(after.y() - before.y()) < tolY, what);
        QVERIFY(settle());
    }
}

// Fit Width / Fit Page are menu zooms too, so they hold the centre as well. This
// needs a multi-page document to mean anything: the old code kept the raw scroll
// value through a layout of a different height, which after a deep zoom left the
// reader on a different page.
void TstViewerPreview::fitModeHoldsTheVerticalCentre()
{
    const QString multi = QStringLiteral(MERVIN_IMAGES_PDF);
    if (!QFileInfo::exists(multi))
        QSKIP("examples/images.pdf not present");
    QString err;
    std::unique_ptr<mervin::Document> pages = engine_->openDocument(multi, QString(), &err);
    QVERIFY2(pages != nullptr, qPrintable(err));
    QVERIFY(pages->pageCount() > 2);

    viewer_->setDocument(pages.get());
    viewer_->setScale(3.0);
    QVERIFY(settle());
    viewer_->verticalScrollBar()->setValue(viewer_->verticalScrollBar()->maximum() / 2);
    QVERIFY(settle());

    // Vertical position of the middle of the viewport within its own page.
    const auto centreFraction = [this, &pages] {
        const ViewerWidget::ScrollAnchor a = viewer_->scrollAnchor();
        const double pageH = pages->pageSize(a.page).height() * viewer_->scale();
        return a.fracY + (viewer_->viewport()->height() / 2.0) / pageH;
    };

    const int page = viewer_->scrollAnchor().page;
    QVERIFY2(page > 0, "not parked past the first page - the case would not discriminate");
    const double before = centreFraction();

    viewer_->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
    QCOMPARE(viewer_->scrollAnchor().page, page);
    const double tol = 2.0 / (pages->pageSize(page).height() * viewer_->scale());
    QVERIFY(qAbs(centreFraction() - before) < tol);

    viewer_->setDocument(doc_.get()); // hand the viewer back its own document
    QVERIFY(settle());
}

// The +/- buttons, Ctrl+= / Ctrl+- and the wheel step a ladder of round levels that
// doubles every two rungs - about twice the old flat 1.25x multiplier, which landed
// on values like 137% and moved too little to read as a zoom. A scale sitting just
// short of a rung skips it, so no gesture is ever smaller than that old step.
void TstViewerPreview::zoomLadderStepsToRoundLevels()
{
    viewer_->setScale(1.0);
    viewer_->zoomIn();
    QCOMPARE(viewer_->scale(), 1.5);
    viewer_->zoomIn();
    QCOMPARE(viewer_->scale(), 2.0);
    viewer_->zoomOut();
    QCOMPARE(viewer_->scale(), 1.5);
    viewer_->zoomOut();
    QCOMPARE(viewer_->scale(), 1.0);

    // Off the ladder - a fit scale, a restored session, or a trackpad pinch - the
    // near rung is skipped rather than producing a zoom too small to see.
    viewer_->setScale(0.95);
    viewer_->zoomIn();
    QCOMPARE(viewer_->scale(), 1.5); // not 100%, which would be a 5% zoom
    viewer_->setScale(1.05);
    viewer_->zoomOut();
    QCOMPARE(viewer_->scale(), 0.75); // not 100%

    // The ladder ends on the scale clamps, so stepping past either is a no-op.
    viewer_->setScale(100.0);
    viewer_->zoomIn();
    QCOMPARE(viewer_->scale(), 100.0);
    viewer_->setScale(0.08);
    viewer_->zoomOut();
    QCOMPARE(viewer_->scale(), 0.08);

    viewer_->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
    QVERIFY(settle());
}

// The zoom ease: the zoom lands on its target in one go, and the DRAWING is what
// steps there over ~130 ms. Both halves are asserted - the scale is final the
// instant zoomIn() returns, and the frames in between are drawn at genuinely
// intermediate magnifications, ending exactly on the final layout. Driven through
// the progress hook rather than by sleeping, so it is deterministic.
void TstViewerPreview::easedZoomProducesIntermediateFrames()
{
    // Park the whole page comfortably inside the viewport so pageSpan measures the
    // drawn magnification instead of saturating at the viewport edge.
    viewer_->setZoomMode(ViewerWidget::ZoomMode::FitPage);
    QVERIFY(settle());
    viewer_->setScale(viewer_->scale() * 0.6);
    QVERIFY(settle());

    const QImage before = frame();
    const int startSpan = pageSpan(before);
    QVERIFY2(startSpan > 100 && startSpan < before.width() - 20,
             "the page is not comfortably inside the viewport - pageSpan would saturate");

    // One ladder step's worth of magnification, applied directly: 0.6 -> 0.9 of Fit
    // Page keeps the whole page inside the viewport, so pageSpan stays meaningful.
    // (zoomIn() itself is covered by zoomLadderStepsToRoundLevels.)
    const double startScale = viewer_->scale();
    const double k = 1.5;
    viewer_->setScale(startScale * k);
    QCOMPARE(viewer_->scale(), startScale * k); // the state is final immediately
    QVERIFY2(viewer_->zoomEaseActive(), "a full zoom step did not arm the ease");

    std::vector<int> spans;
    for (double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        viewer_->setZoomEaseProgressForTest(t);
        const QImage f = frame();
        QCOMPARE(viewer_->paintStats().blank, 0);
        QVERIFY2(viewer_->paintStats().eased >= 1, "the frame was not drawn by the ease");
        spans.push_back(pageSpan(f));
    }
    for (std::size_t i = 1; i < spans.size(); ++i) {
        QVERIFY2(spans[i] > spans[i - 1],
                 qPrintable(QStringLiteral("the ease is not stepping the drawn geometry: %1")
                                .arg(QString::number(spans[i - 1]) + " -> "
                                     + QString::number(spans[i]))));
    }
    QVERIFY2(qAbs(spans.front() - startSpan) <= 2, "u == 0 is not the pre-zoom geometry");
    QVERIFY2(qAbs(spans.back() - qRound(startSpan * k)) <= 3, "u == 1 missed the target");

    // Control, in the spirit of rotationShowsBlankPaper: with the ease switched off
    // the same sweep must produce identical frames, so the probe above is able to
    // fail. No waiting involved, so no render can slip in and fake a difference.
    viewer_->setZoomEaseProgressForTest(-1.0); // hand progress back to the clock
    QVERIFY(settle());
    viewer_->setZoomEaseMs(0);
    viewer_->zoomIn();
    QVERIFY(!viewer_->zoomEaseActive());
    const QImage flat = frame();
    for (double t : {0.0, 0.5, 1.0}) {
        viewer_->setZoomEaseProgressForTest(t);
        QVERIFY2(frame() == flat, "the progress hook moved a frame with the ease disabled");
    }
    QCOMPARE(viewer_->paintStats().eased, 0);

    viewer_->setZoomEaseMs(130);
    viewer_->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
    QVERIFY(settle());
}

// The ease is cosmetic, but a stale one would draw pages at rects that no longer
// mean anything - and a stale one that never terminates would strand the view
// mid-flight. Every cancel site is checked here; settle() now requires the ease to
// be finished, so this is what would otherwise ship as "a page sometimes looks
// wrong after zooming" and be unreproducible.
void TstViewerPreview::easeSurvivesInterruption()
{
    const auto interrupted = [this](const char *what, auto &&interrupt) {
        // 100% leaves room to scroll on both axes (so the scroll case is not
        // vacuous) and keeps every render cheap; a ladder step from here is 150%.
        viewer_->setScale(1.0);
        QVERIFY2(settle(), what);
        viewer_->zoomIn();
        interrupt();
        QVERIFY2(settle(), what);
        QVERIFY2(!viewer_->zoomEaseActive(), what);
    };
    interrupted("second zoom", [&] { viewer_->zoomOut(); });
    interrupted("rotate", [&] { viewer_->rotateRight(); viewer_->rotateLeft(); });
    interrupted("page theme", [&] {
        viewer_->setPageTheme(PageTheme::Comfort);
        viewer_->setPageTheme(PageTheme::Light);
    });
    interrupted("page mode", [&] {
        viewer_->setSpread(true);
        viewer_->setSpread(false);
    });
    interrupted("scroll", [&] {
        QVERIFY(viewer_->verticalScrollBar()->maximum() > 40);
        viewer_->verticalScrollBar()->setValue(viewer_->verticalScrollBar()->value() + 40);
    });
    interrupted("resize", [&] {
        viewer_->resize(900, 700);
        viewer_->resize(1000, 800);
    });
    interrupted("hide/show", [&] { viewer_->hide(); viewer_->show(); });
    interrupted("fit mode", [&] { viewer_->setZoomMode(ViewerWidget::ZoomMode::FitWidth); });
}

// The blit was added to every paint of a not-yet-rendered page, so its cost is
// part of the frame budget. The printed numbers are the deliverable (compare
// them across changes on the same machine, like tst_perf_render); the QVERIFY
// only catches a runaway.
void TstViewerPreview::previewBlitCost()
{
    const auto timeFrames = [this](int n) {
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < n; ++i)
            frame();
        return t.nsecsElapsed() / 1e6 / n;
    };

    // timeFrames pumps no events, so a running ease would freeze at t == 0 and the
    // zoom cases below would time a 1x blit instead of the 6x stretch they exist to
    // measure. Switch it off for the duration.
    viewer_->setZoomEaseMs(0);
    QVERIFY(settle());
    const double freshMs = timeFrames(10);

    // Zoom in far enough that the page is rendered as clipped tiles, so the
    // stretch runs from a whole-page image into a target far larger than the
    // window - the deep-zoom case.
    viewer_->setScale(viewer_->scale() * 6.0);
    const double zoomInMs = timeFrames(10);
    QVERIFY(viewer_->paintStats().preview >= 1);

    // The other direction: a large settled tile scaled down into the window.
    QVERIFY(settle());
    const QImage big = frame();
    Q_UNUSED(big);
    viewer_->setScale(viewer_->scale() / 5.0);
    const double zoomOutMs = timeFrames(10);

    std::printf("paint 1000x800: fresh=%.2f ms  preview(zoom in)=%.2f ms  "
                "preview(zoom out)=%.2f ms\n",
                freshMs, zoomInMs, zoomOutMs);
    std::fflush(stdout);
    viewer_->setZoomEaseMs(130);
    QVERIFY2(zoomInMs < 100.0 && zoomOutMs < 100.0, "the preview blit grossly regressed");
}

// The floating tool panels and page popups are children of the viewport, and Qt
// derives a child's foregroundRole from the inherited backgroundRole: Dark and
// Shadow map to QPalette::Light, every other role to something readable. With
// QPalette::Dark on the viewport, every plain QLabel in the measuring panel drew
// its text in Light - #2a313a on the dark panel, pure white on the light one, i.e.
// invisible in both themes. This is the regression test for that whole class of
// bug, so it asserts on the role rather than on one panel.
void TstViewerPreview::viewportChildrenInheritReadableInk()
{
    QLabel child(QStringLiteral("x"), viewer_->viewport());
    QCOMPARE(child.foregroundRole(), QPalette::WindowText);
    QVERIFY(child.backgroundRole() != QPalette::Dark);
    QVERIFY(child.backgroundRole() != QPalette::Shadow);

    // And the ink that role resolves to has to be legible on the panel surface.
    const QPalette pal = mervin::theme::darkPalette(QColor(0x4f, 0x8c, 0xff));
    const mervin::theme::Chrome t = mervin::theme::chrome(pal);
    const auto luminance = [](const QColor &c) {
        const auto lin = [](int v) {
            const double s = v / 255.0;
            return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * lin(c.red()) + 0.7152 * lin(c.green()) + 0.0722 * lin(c.blue());
    };
    const double a = luminance(pal.color(child.foregroundRole()));
    const double b = luminance(t.popover);
    const double ratio = (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
    QVERIFY2(ratio > 4.5, "a panel label's inherited ink must clear AA on the panel surface");
}

QTEST_MAIN(TstViewerPreview)
#include "tst_viewer_preview.moc"
