// Guard rails for ViewLayout, and specifically for what it does with a document
// whose pages are not all the same size.
//
// Before v1.49.0 the fit scale was derived from ONE page (the current one) and,
// in two-page mode, from twice its width - a spread that need not exist. On
// an A3 title sheet followed by four A4 sheets gave
// two different answers for the same window depending on where the reader
// happened to be scrolled: one left 14 % of the width empty, the other overflowed
// it by 21 % and grew a horizontal scrollbar inside a mode called Fit Width. The
// invariant the fit now owes the caller is stated once, here:
//
//   in a fit mode the canvas is never wider than the viewport,
//   and never much narrower either.
//
// Everything else in this file exists to stop a future change from satisfying
// that cheaply - by shrinking everything, by fitting only the widest page, or by
// quietly reflowing the book. The mirror test pins the contract the header
// asserts but nothing enforced: heightForScale/widthForScale must reproduce
// relayout()'s arithmetic exactly, since the fit trusts them.
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "render/ViewLayout.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QSizeF>
#include <QTemporaryDir>
#include <QTest>

#include <cstdlib>
#include <memory>
#include <set>
#include <utility>
#include <vector>

using mervin::Document;
using mervin::RenderEngine;
using mervin::ViewLayout;
using Scroll = mervin::ViewLayout::Scroll;
using Mode = mervin::ViewLayout::Mode;

// The four layout modes. kSpread / kSingleSpread are the two-page ones; before
// v1.52.0 the spread could only be had with continuous scrolling, because the
// two axes shared one enum.
static constexpr Mode kContinuous{Scroll::Continuous, false};
static constexpr Mode kSingle{Scroll::Single, false};
static constexpr Mode kSpread{Scroll::Continuous, true};
static constexpr Mode kSingleSpread{Scroll::Single, true};

static QString modeName(Mode m)
{
    return QStringLiteral("%1%2")
        .arg(m.scroll == Scroll::Single ? QStringLiteral("single") : QStringLiteral("continuous"))
        .arg(m.spread ? QStringLiteral("+spread") : QString());
}

class TstViewLayout : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void mirrorsMatchLayout_data();
    void mirrorsMatchLayout();
    void fitNeverOverflows_data();
    void fitNeverOverflows();
    void fitFillsTheViewport();
    void fitIsPageIndependent();
    void axesAreIndependent();
    void singleSpreadLaysOutOneRow();
    void fitPageFitsTheRow();
    void spineIsShared();
    void trailingLonePageKeepsLeftColumn();
    void rowTopsAreShared();
    void pageAtYReportsEveryRowLeader_data();
    void pageAtYReportsEveryRowLeader();
    void oversizedSheetGetsItsOwnRow();
    void landscapePagesStillPair();
    void degenerateDocuments();
    void crossedDocumentIsCharacterised();

private:
    // Documents are built on demand and kept alive for the whole run: ViewLayout
    // holds a bare pointer to one.
    const Document *doc(const QString &key);
    const Document *build(const QString &key, const QList<QSizeF> &pages);
    const Document *openFile(const QString &path);

    std::unique_ptr<RenderEngine> engine_;
    std::unique_ptr<QTemporaryDir> dir_;
    std::vector<std::shared_ptr<Document>> keep_;
    QHash<QString, const Document *> cache_;
};

namespace {

// The layout's own pixel constants. Duplicated here deliberately: a test that
// read them from the class could not notice one of them changing.
constexpr int kGap = 12;
constexpr int kInnerGap = 8;
constexpr int kMargin = 16;
constexpr int kSlack = 8;

// ---- fixtures ---------------------------------------------------------------
//
// spine    pages that differ but still pair (area ratio 1.21, under the 1.5
//          threshold), odd count: exercises the two columns and a lone trailing
//          page.
// tallOdd  the TALLER page at an ODD index - the case a bottom-sorted page scan
//          resolves to the right-hand sheet mid-spread.
// step     a 4x-area first sheet, so the spread plan gives it a row of its own.
// crossed  widest page on the left in one row and on the right in the other, so
//          the canvas is wider than any single row.
QList<QSizeF> fixture(const QString &key)
{
    if (key == QLatin1String("spine"))
        return {{400, 300}, {440, 330}, {400, 300}, {440, 330}, {400, 300}};
    if (key == QLatin1String("tallOdd"))
        return {{400, 300}, {400, 420}, {400, 300}, {400, 420}};
    if (key == QLatin1String("step"))
        return {{800, 600}, {400, 300}, {400, 300}, {400, 300}, {400, 300}};
    if (key == QLatin1String("uniform"))
        return {{400, 300}, {400, 300}, {400, 300}, {400, 300}, {400, 300}};
    if (key == QLatin1String("crossed"))
        return {{400, 300}, {360, 300}, {360, 300}, {400, 300}};
    if (key == QLatin1String("one"))
        return {{400, 300}};
    if (key == QLatin1String("two"))
        return {{400, 300}, {400, 300}};
    if (key == QLatin1String("three"))
        return {{400, 300}, {400, 300}, {400, 300}};
    if (key == QLatin1String("landscapeMix")) {
        // A 2:1 sheet, so a turned page is TWICE as wide as its neighbours while
        // holding exactly the same area. A4's 1.41 would not discriminate: it
        // sits under the 1.5 oversized threshold whichever quantity is measured.
        QList<QSizeF> pages;
        for (int i = 0; i < 10; ++i)
            pages << ((i == 4 || i == 8) ? QSizeF(800, 400) : QSizeF(400, 800));
        return pages;
    }
    return {};
}

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
    pdf += "xref\n0 " + QByteArray::number(n) + "\n";
    pdf += "0000000000 65535 f \n";
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

// Pages carrying nothing but a /MediaBox, so every size is an exact integer and
// the arithmetic below can be checked by hand.
QByteArray mediaBoxPdf(const QList<QSizeF> &pages)
{
    QList<QByteArray> bodies;
    QByteArray kids;
    for (int i = 0; i < pages.size(); ++i)
        kids += QByteArray::number(i + 3) + " 0 R ";
    bodies << "<< /Type /Catalog /Pages 2 0 R >>";
    bodies << "<< /Type /Pages /Count " + QByteArray::number(pages.size()) + " /Kids [" + kids
                    + "] >>";
    for (const QSizeF &s : pages) {
        bodies << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 "
                        + QByteArray::number(s.width()) + " " + QByteArray::number(s.height())
                        + "] >>";
    }
    return assemblePdf(bodies);
}

// The scales swept by the mirror test: below the minimum zoom, the fits that
// matter on real windows, unity, and deep zoom.
const QList<double> kScales = {0.08, 0.463983, 0.757227, 1.0, 3.0};
const QList<int> kRotations = {0, 90, 180, 270};

void addFixtureRows()
{
    QTest::addColumn<QString>("key");
    for (const char *k : {"spine", "tallOdd", "step", "uniform"})
        QTest::newRow(k) << QString::fromLatin1(k);
}

} // namespace

void TstViewLayout::initTestCase()
{
    engine_ = std::make_unique<RenderEngine>();
    dir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(dir_->isValid());
}

const Document *TstViewLayout::doc(const QString &key)
{
    if (cache_.contains(key))
        return cache_[key];
    return build(key, fixture(key));
}

const Document *TstViewLayout::build(const QString &key, const QList<QSizeF> &pages)
{
    if (pages.isEmpty())
        return nullptr;
    const QString path = dir_->filePath(key + QStringLiteral(".pdf"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return nullptr;
    f.write(mediaBoxPdf(pages));
    f.close();
    const Document *d = openFile(path);
    cache_[key] = d;
    return d;
}

const Document *TstViewLayout::openFile(const QString &path)
{
    QString err;
    auto d = engine_->openDocument(path, QString(), &err);
    if (!d)
        return nullptr;
    keep_.push_back(std::move(d));
    return keep_.back().get();
}

void TstViewLayout::mirrorsMatchLayout_data()
{
    addFixtureRows();
}

// heightForScale/widthForScale are what the fit reasons about; relayout() is what
// the reader sees. The header says to keep them in sync and until now nothing
// checked it - so a rounding rule or a margin could move in one and not the other.
void TstViewLayout::mirrorsMatchLayout()
{
    QFETCH(QString, key);
    const Document *d = doc(key);
    QVERIFY(d);

    ViewLayout l;
    l.setDocument(d);
    for (Mode mode : {kContinuous, kSingle, kSpread, kSingleSpread}) {
        l.setMode(mode);
        for (int rot : kRotations) {
            l.setRotation(rot);
            for (double s : kScales) {
                l.setScale(s);
                for (int cur = 0; cur < d->pageCount(); ++cur) {
                    l.setCurrentPage(cur);
                    const QString at = QStringLiteral("%1 mode=%2 rot=%3 s=%4 cur=%5")
                                           .arg(key).arg(modeName(mode)).arg(rot).arg(s).arg(cur);
                    QVERIFY2(l.widthForScale(s, mode, rot, cur) == l.totalSize().width(),
                             qPrintable(QStringLiteral("width mirror: %1 predicted %2, laid out %3")
                                            .arg(at)
                                            .arg(l.widthForScale(s, mode, rot, cur))
                                            .arg(l.totalSize().width())));
                    QVERIFY2(l.heightForScale(s, mode, rot, cur) == l.totalSize().height(),
                             qPrintable(QStringLiteral("height mirror: %1 predicted %2, laid out %3")
                                            .arg(at)
                                            .arg(l.heightForScale(s, mode, rot, cur))
                                            .arg(l.totalSize().height())));
                }
            }
        }
    }
}

void TstViewLayout::fitNeverOverflows_data()
{
    addFixtureRows();
}

// The invariant, swept over EVERY viewport width in a wide band - odd ones
// included, because a one-pixel overflow only shows on some of them.
void TstViewLayout::fitNeverOverflows()
{
    QFETCH(QString, key);
    const Document *d = doc(key);
    QVERIFY(d);

    ViewLayout l;
    l.setDocument(d);
    int checked = 0;
    for (Mode mode : {kContinuous, kSingle, kSpread, kSingleSpread}) {
        l.setMode(mode);
        for (int rot : kRotations) {
            l.setRotation(rot);
            for (int w = 400; w <= 2000; ++w) {
                for (int cur : {0, d->pageCount() / 2, d->pageCount() - 1}) {
                    l.setCurrentPage(cur);
                    l.setScale(l.fitBasis(w, 900, mode, rot, cur).widthScale);
                    ++checked;
                    QVERIFY2(l.totalSize().width() <= w,
                             qPrintable(QStringLiteral("%1 mode=%2 rot=%3 vp=%4 cur=%5: canvas %6 "
                                                       "overflows by %7 px")
                                            .arg(key).arg(modeName(mode)).arg(rot).arg(w).arg(cur)
                                            .arg(l.totalSize().width())
                                            .arg(l.totalSize().width() - w)));
                }
            }
        }
    }
    QVERIFY(checked > 10000); // the sweep really ran
}

// The other half of the invariant: a fit that fits by shrinking everything to
// nothing would satisfy the test above. The canvas has to reach the viewport bar
// the margins, the breathing space and the per-column rounding.
void TstViewLayout::fitFillsTheViewport()
{
    ViewLayout l;
    for (const char *key : {"spine", "step", "uniform"}) {
        const Document *d = doc(QString::fromLatin1(key));
        QVERIFY(d);
        l.setDocument(d);
        for (Mode mode : {kContinuous, kSingle, kSpread, kSingleSpread}) {
            l.setMode(mode);
            for (int w : {700, 1000, 1301, 1600}) {
                l.setScale(l.fitBasis(w, 900, mode, 0, 0).widthScale);
                // The content budget is w - 2*margin - slack, so the canvas lands
                // at about w - slack; allow the inner gap and two pixels of
                // rounding allowance on top.
                const int lo = w - kSlack - kInnerGap - 2;
                QVERIFY2(l.totalSize().width() >= lo,
                         qPrintable(QStringLiteral("%1 mode=%2 vp=%3: canvas only %4, wanted >= %5")
                                        .arg(key).arg(modeName(mode)).arg(w)
                                        .arg(l.totalSize().width()).arg(lo)));
            }
        }
    }
}

// Fit Width must not depend on which page is current. Nothing re-fits on scroll,
// so a page-dependent basis meant the same window produced whichever scale the
// reader's last scroll position implied - and a later resize silently changed it.
// Single mode is the control: there only one page is laid out, so the basis is
// page-local by definition and must stay that way.
void TstViewLayout::fitIsPageIndependent()
{
    const Document *d = doc(QStringLiteral("step"));
    QVERIFY(d);
    ViewLayout l;
    l.setDocument(d);

    for (Mode mode : {kContinuous, kSpread}) {
        l.setMode(mode);
        for (int rot : kRotations) {
            const double first = l.fitBasis(1600, 1000, mode, rot, 0).widthScale;
            for (int cur = 1; cur < d->pageCount(); ++cur) {
                const double here = l.fitBasis(1600, 1000, mode, rot, cur).widthScale;
                QVERIFY2(qFuzzyCompare(here, first),
                         qPrintable(QStringLiteral("mode=%1 rot=%2: page %3 fits at %4, page 0 at %5")
                                        .arg(modeName(mode)).arg(rot).arg(cur).arg(here).arg(first)));
            }
        }
    }

    l.setMode(kSingle);
    const double p0 = l.fitBasis(1600, 1000, kSingle, 0, 0).widthScale;
    const double p1 = l.fitBasis(1600, 1000, kSingle, 0, 1).widthScale;
    QVERIFY2(p1 > p0 * 1.9, "Single mode must still fit the page it lays out, and only that page");

    // Single+spread inherits that: the basis is ROW-local, so it must not move
    // between the two halves of one spread (turning to the facing sheet would
    // otherwise rescale the very spread already on screen) while still differing
    // between rows. "spine" is the fixture that can tell the two apart: it pairs
    // 0|1 and 2|3 and leaves 4 alone, so the lone trailing sheet is a row roughly
    // half as wide. ("step" cannot: its oversized page is exactly twice an A4, so
    // its standalone row and its spreads happen to be the same width.)
    const Document *paired = doc(QStringLiteral("spine"));
    QVERIFY(paired);
    ViewLayout p;
    p.setDocument(paired);
    p.setMode(kSingleSpread);
    const double r0 = p.fitBasis(1600, 1000, kSingleSpread, 0, 0).widthScale;
    const double r1 = p.fitBasis(1600, 1000, kSingleSpread, 0, 1).widthScale;
    const double lone = p.fitBasis(1600, 1000, kSingleSpread, 0, 4).widthScale;
    QVERIFY2(qFuzzyCompare(r0, r1), "the two halves of one spread must fit identically");
    QVERIFY2(lone > r0 * 1.9, "a narrower row must fit larger - the basis is row-local");
}

// The two axes must be independent in the layout, not just in the menu: setting
// one may not perturb the other. The regression this pins is the old three-valued
// enum, where asking for a spread silently reset the scrolling choice - and, worse
// for a toggle, there was no way back to where you started.
void TstViewLayout::axesAreIndependent()
{
    for (const char *key : {"spine", "step", "tallOdd", "uniform", "crossed"}) {
        const Document *d = doc(QString::fromLatin1(key));
        QVERIFY(d);
        ViewLayout l;
        l.setDocument(d);
        l.setScale(0.9);

        for (Mode base : {kContinuous, kSingle}) {
            for (int cur = 0; cur < d->pageCount(); ++cur) {
                l.setMode(base);
                l.setCurrentPage(cur);
                const QSize before = l.totalSize();
                std::vector<QRect> rects;
                for (int i = 0; i < d->pageCount(); ++i)
                    rects.push_back(l.pageRect(i));

                // Spread on, then off again: the geometry must come back exactly.
                l.setMode({base.scroll, true});
                l.setMode(base);
                const QString at = QStringLiteral("%1 %2 cur=%3").arg(key, modeName(base)).arg(cur);
                QVERIFY2(l.totalSize() == before,
                         qPrintable(QStringLiteral("%1: canvas %2x%3 after the round trip, was %4x%5")
                                        .arg(at)
                                        .arg(l.totalSize().width()).arg(l.totalSize().height())
                                        .arg(before.width()).arg(before.height())));
                for (int i = 0; i < d->pageCount(); ++i)
                    QVERIFY2(l.pageRect(i) == rects[i],
                             qPrintable(QStringLiteral("%1: page %2 moved on the round trip")
                                            .arg(at).arg(i)));

                // And the scroll axis survives a spread change: turning the spread
                // on must leave the number of laid-out rows governed by `scroll`
                // alone. Single lays out one row whatever the spread setting.
                l.setMode({base.scroll, true});
                int laidOut = 0;
                for (int i = 0; i < d->pageCount(); ++i)
                    if (l.pageRect(i).isValid())
                        ++laidOut;
                if (base.scroll == Scroll::Single)
                    QVERIFY2(laidOut >= 1 && laidOut <= 2,
                             qPrintable(QStringLiteral("%1: single+spread laid out %2 pages, "
                                                       "expected one row")
                                            .arg(at).arg(laidOut)));
                else
                    QCOMPARE(laidOut, d->pageCount());
            }
        }
    }
}

// One spread at a time: the combination that could not be expressed before the
// split. It must lay out exactly the current row - both facing sheets, nothing
// else - with the same geometry that row has in the scrolling spread, shifted to
// the top of the canvas.
void TstViewLayout::singleSpreadLaysOutOneRow()
{
    const Document *d = doc(QStringLiteral("spine")); // 400x300 / 440x330 x2, then a lone 400x300
    QVERIFY(d);
    QCOMPARE(d->pageCount(), 5);

    ViewLayout l;
    l.setDocument(d);
    l.setScale(1.0);
    l.setMode(kSingleSpread);

    // Pages 0|1 are a spread: both laid out, top-aligned, sharing the spine.
    l.setCurrentPage(0);
    QCOMPARE(l.pageRect(0), QRect(kMargin, kMargin, 400, 300));
    QCOMPARE(l.pageRect(1), QRect(kMargin + 400 + kInnerGap, kMargin, 440, 330));
    QCOMPARE(l.totalSize(), QSize(400 + kInnerGap + 440 + 2 * kMargin, 330 + 2 * kMargin));
    for (int i = 2; i < 5; ++i)
        QVERIFY2(!l.pageRect(i).isValid(), "only the current row may be laid out");

    // The facing sheet is the SAME row: selecting it must not move anything.
    const QRect left = l.pageRect(0), right = l.pageRect(1);
    l.setCurrentPage(1);
    QCOMPARE(l.pageRect(0), left);
    QCOMPARE(l.pageRect(1), right);

    // rowStart/rowEnd must agree that both sheets belong to the row on screen,
    // or the viewer drags the current page back to the left-hand sheet.
    QCOMPARE(l.rowStart(1), 0);
    QCOMPARE(l.rowEnd(1), 2);

    // pageAtY must report the row LEADER anywhere in the row, including the band
    // below the shorter left sheet but beside the taller right one. A rect scan
    // ordered by bottom() returns page 1 there; only a row walk gets this right.
    for (int y : {kMargin, kMargin + 200, kMargin + 310, kMargin + 329})
        QCOMPARE(l.pageAtY(y), 0);

    // The trailing lone page is a row of its own: one sheet, canvas sized to it.
    l.setCurrentPage(4);
    QCOMPARE(l.pageRect(4), QRect(kMargin, kMargin, 400, 300));
    QCOMPARE(l.totalSize(), QSize(400 + 2 * kMargin, 300 + 2 * kMargin));
    QVERIFY(!l.pageRect(3).isValid());
}

// Fit Page fits the ROW, not the current page: the taller half of a spread has to
// be on screen too. This is the case that used to clip a sheet by hundreds of
// pixels in the mode whose whole promise is that the page fits.
void TstViewLayout::fitPageFitsTheRow()
{
    const Document *d = doc(QStringLiteral("tallOdd"));
    QVERIFY(d);
    ViewLayout l;
    l.setDocument(d);
    l.setMode(kSpread);

    // Page 0 is 300 pt tall, its partner 420. Fitting page 0 alone overflows.
    const ViewLayout::FitBasis b = l.fitBasis(1600, 500, kSpread, 0, 0);
    l.setScale(std::min(b.widthScale, b.heightScale));
    const int rowBottom = std::max(l.pageRect(0).bottom(), l.pageRect(1).bottom());
    QVERIFY2(rowBottom + kMargin <= 500,
             qPrintable(QStringLiteral("row runs to %1 px in a 500 px viewport").arg(rowBottom)));
    // And the height axis is what bound it, so the fit is not merely conservative.
    QVERIFY(b.heightScale < b.widthScale);
}

// One document-wide gutter. Centring each row inside the widest row (what this
// used to do) slid the spine sideways as the reader scrolled a mixed document,
// which is most of what looked broken.
void TstViewLayout::spineIsShared()
{
    const Document *d = doc(QStringLiteral("spine"));
    QVERIFY(d);
    ViewLayout l;
    l.setDocument(d);
    l.setMode(kSpread);
    l.setScale(1.0);

    // Left column: pages 0, 2, 4 (400 wide). Right column: 1, 3 (440 wide).
    QCOMPARE(l.pageRect(0).right(), l.pageRect(2).right());
    QCOMPARE(l.pageRect(1).left(), l.pageRect(3).left());
    // Exact geometry, so a change of model has to be deliberate: the spine sits at
    // margin + widest left page, the right column one inner gap later.
    QCOMPARE(l.pageRect(0), QRect(kMargin, kMargin, 400, 300));
    QCOMPARE(l.pageRect(1), QRect(kMargin + 400 + kInnerGap, kMargin, 440, 330));
    QCOMPARE(l.totalSize().width(), 400 + kInnerGap + 440 + 2 * kMargin);
    // Rows are 330, 330 and 300 tall with two gaps between them.
    QCOMPARE(l.totalSize().height(), 330 + kGap + 330 + kGap + 300 + 2 * kMargin);
}

void TstViewLayout::trailingLonePageKeepsLeftColumn()
{
    const Document *d = doc(QStringLiteral("spine"));
    QVERIFY(d);
    ViewLayout l;
    l.setDocument(d);
    l.setMode(kSpread);
    l.setScale(1.0);
    // The last page of a book, not a page floating in the middle of the window.
    QCOMPARE(l.pageRect(4).right(), l.pageRect(0).right());
}

void TstViewLayout::rowTopsAreShared()
{
    for (const char *key : {"spine", "tallOdd"}) {
        const Document *d = doc(QString::fromLatin1(key));
        QVERIFY(d);
        ViewLayout l;
        l.setDocument(d);
        l.setMode(kSpread);
        l.setScale(1.0);
        for (int i = 0; i + 1 < d->pageCount(); i += 2) {
            QVERIFY2(l.pageRect(i).top() == l.pageRect(i + 1).top(),
                     qPrintable(QStringLiteral("%1: row %2 tops are %3 and %4")
                                    .arg(key).arg(i / 2)
                                    .arg(l.pageRect(i).top()).arg(l.pageRect(i + 1).top())));
        }
    }
}

void TstViewLayout::pageAtYReportsEveryRowLeader_data()
{
    QTest::addColumn<QString>("key");
    // Taller page on the LEFT: a bottom-sorted scan skips the right-hand sheet.
    QTest::newRow("tallLeft") << QStringLiteral("spine");
    // Taller page on the RIGHT: it hijacks a band and reports mid-spread instead.
    QTest::newRow("tallRight") << QStringLiteral("tallOdd");
    QTest::newRow("standalone") << QStringLiteral("step");
}

// Sweeping y down the canvas must walk the row leaders in order and name nothing
// else. The old scan compared against each rect's own bottom in index order, so
// whichever half of a spread ended higher was unreachable for ever - on
// schematic.pdf the page indicator counted 1, 3, 5.
void TstViewLayout::pageAtYReportsEveryRowLeader()
{
    QFETCH(QString, key);
    const Document *d = doc(key);
    QVERIFY(d);
    ViewLayout l;
    l.setDocument(d);
    l.setMode(kSpread);
    l.setScale(1.0);

    std::set<int> expected;
    for (int i = 0; i < d->pageCount(); ++i)
        if (l.rowStart(i) == i)
            expected.insert(i);

    std::set<int> seen;
    int previous = -1;
    for (int y = 0; y <= l.totalSize().height(); ++y) {
        const int p = l.pageAtY(y);
        QVERIFY2(expected.count(p) == 1,
                 qPrintable(QStringLiteral("%1: y=%2 reported page %3, which leads no row")
                                .arg(key).arg(y).arg(p)));
        QVERIFY2(p >= previous, "pageAtY must not walk backwards down the canvas");
        previous = p;
        seen.insert(p);
    }
    QCOMPARE(seen, expected);
}

void TstViewLayout::oversizedSheetGetsItsOwnRow()
{
    const Document *d = doc(QStringLiteral("step"));
    QVERIFY(d);
    ViewLayout l;
    l.setDocument(d);
    l.setMode(kSpread);
    l.setScale(1.0);

    QVERIFY(l.isStandalone(0));
    QCOMPARE(l.rowStart(0), 0);
    QCOMPARE(l.rowEnd(0), 1);
    QCOMPARE(l.rowStart(1), 1);
    QCOMPARE(l.rowEnd(1), 3); // pages 1|2 now share a row
    QCOMPARE(l.rowStart(4), 3);
    QCOMPARE(l.rowEnd(4), 5);
    // Centred, so its centreline lands on the gutter of the spreads below.
    const int leftGap = l.pageRect(0).left() - kMargin;
    const int rightGap = l.totalSize().width() - kMargin - (l.pageRect(0).right() + 1);
    QVERIFY2(std::abs(leftGap - rightGap) <= 1,
             qPrintable(QStringLiteral("standalone sheet is off-centre: %1 px left, %2 px right")
                            .arg(leftGap).arg(rightGap)));

}

// The discriminating case for AREA against WIDTH. A turned sheet in a portrait
// report is wider than its neighbours but the same piece of paper: pairing must be
// untouched, or one landscape page would give itself a row and re-pair the whole
// document after it. Measuring width instead of area fails this.
void TstViewLayout::landscapePagesStillPair()
{
    const Document *d = doc(QStringLiteral("landscapeMix"));
    QVERIFY(d);
    ViewLayout l;
    l.setDocument(d);
    l.setMode(kSpread);

    for (int i = 0; i < 10; ++i) {
        QVERIFY2(!l.isStandalone(i),
                 qPrintable(QStringLiteral("page %1 was treated as oversized").arg(i)));
        QCOMPARE(l.rowStart(i), (i / 2) * 2);
    }
}

void TstViewLayout::degenerateDocuments()
{
    // One page in two-page mode is a page, not half a spread. It used to be
    // fitted against twice its own width and so drawn at half the size the same
    // window gave it in every other mode.
    const Document *one = doc(QStringLiteral("one"));
    QVERIFY(one);
    ViewLayout l;
    l.setDocument(one);
    l.setMode(kSpread);
    l.setScale(1.0);
    QCOMPARE(l.pageRect(0), QRect(kMargin, kMargin, 400, 300));
    QCOMPARE(l.totalSize(), QSize(400 + 2 * kMargin, 300 + 2 * kMargin));
    const double fit = l.fitBasis(1000, 800, kSpread, 0, 0).widthScale;
    const double cont = l.fitBasis(1000, 800, kContinuous, 0, 0).widthScale;
    QVERIFY2(qFuzzyCompare(fit, cont), "a lone page fits the same in either mode");

    const Document *two = doc(QStringLiteral("two"));
    QVERIFY(two);
    l.setDocument(two);
    QCOMPARE(l.rowEnd(0), 2);
    QCOMPARE(l.totalSize(), QSize(400 + kInnerGap + 400 + 2 * kMargin, 300 + 2 * kMargin));

    const Document *three = doc(QStringLiteral("three"));
    QVERIFY(three);
    l.setDocument(three);
    QCOMPARE(l.rowStart(2), 2);
    QCOMPARE(l.rowEnd(2), 3);
    QCOMPARE(l.totalSize().height(), 300 + kGap + 300 + 2 * kMargin);
}

// A document whose widest page is on the left in one row and on the right in
// another: with a fixed spine the canvas is wider than any single row, so Fit
// Width scales everything down a little. That is the accepted cost of a straight
// gutter (Chromium is looser still and always reserves twice the widest page).
// Pinned so the trade-off stays a decision rather than becoming a surprise.
void TstViewLayout::crossedDocumentIsCharacterised()
{
    const Document *d = doc(QStringLiteral("crossed"));
    QVERIFY(d);
    ViewLayout l;
    l.setDocument(d);
    l.setMode(kSpread);
    l.setScale(1.0);
    for (int i = 0; i < 4; ++i)
        QVERIFY(!l.isStandalone(i)); // an area ratio of 1.11 pairs normally
    QCOMPARE(l.totalSize().width(), 400 + kInnerGap + 400 + 2 * kMargin);
    QVERIFY(l.totalSize().width() - 2 * kMargin > 400 + kInnerGap + 360); // widest single row
}

QTEST_GUILESS_MAIN(TstViewLayout)
#include "tst_view_layout.moc"
