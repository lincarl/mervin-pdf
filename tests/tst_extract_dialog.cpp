#include "dialogs/ExtractDialog.h"
#include "dialogs/ExtractStrip.h"
#include "i18n/UiLanguage.h"
#include "io/SaveCheck.h"
#include "security/PageOps.h"
#include "security/QpdfService.h"
#include "render/Document.h"
#include "render/RenderEngine.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QPointer>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include <memory>

using mervin::ExtractDialog;
using mervin::SaveCheck;
using mervin::ExtractPlan;
using mervin::ExtractStrip;
using mervin::PageOps;
using mervin::QpdfService;

// Widget-level guard rails for the extract dialog. ExtractPlan (tst_extract_plan)
// covers the arithmetic; what is left here is the wiring that only exists in the
// widget layer: that typing and pasting separators makes rows, that moves and
// strip edits reach the plan and the job, that the strip keeps the current row
// in view without undoing the user's scrolling, that a disabled Extract always
// shows its reason, and that an encrypted file uses the probe's verified password
// or captures an inline password for worker verification. The Delete mode cases check the same
// window as Document > Delete Pages: its seed row, the strip's clicks, the refusal
// to delete every page, and the copy PageOps::deletePages writes.
//
// As in tst_merge_dialog, the dialog is flagged WA_DontShowOnScreen and never
// exec()d, so it never appears and nothing blocks; only the "Replace it?" box
// does, and answerReplaceWithYes() closes it. Most cases leave Source::doc and
// ::engine null. The thumbnail case uses a real rendered document.
namespace {

void makePdf(const QString &path, int n)
{
    QPDF q;
    q.emptyPDF();
    QPDFPageDocumentHelper dh(q);
    for (int i = 0; i < n; ++i) {
        QPDFObjectHandle box = QPDFObjectHandle::newArray();
        box.appendItem(QPDFObjectHandle::newInteger(0));
        box.appendItem(QPDFObjectHandle::newInteger(0));
        box.appendItem(QPDFObjectHandle::newInteger(612));
        box.appendItem(QPDFObjectHandle::newInteger(792));
        QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
        page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
        page.replaceKey("/MediaBox", box);
        // Which page this is, for checking which ones a written copy kept.
        page.replaceKey("/MervinTestIndex", QPDFObjectHandle::newInteger(i));
        dh.addPage(QPDFPageObjectHelper(q.makeIndirectObject(page)), false);
    }
    const QByteArray outputName = path.toUtf8();
    QPDFWriter w(q, outputName.constData());
    w.write();
}

// The /MervinTestIndex of every page of `path`, in order: the source pages it holds.
QList<int> sourcePages(const QString &path, const QString &password = QString())
{
    QPDF q;
    const QByteArray name = path.toUtf8();
    const QByteArray pw = password.toUtf8();
    q.processFile(name.constData(), password.isEmpty() ? nullptr : pw.constData());
    QList<int> out;
    for (QPDFPageObjectHelper &page : QPDFPageDocumentHelper(q).getAllPages())
        out << page.getObjectHandle().getKey("/MervinTestIndex").getIntValueAsInt();
    return out;
}

} // namespace

/// Verifies editor controls against supplied probes and value-owned accepted jobs.
class TstExtractDialog : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void opensWithOneRowOnTheSeedPage();
    void opensWithEveryPageWithoutASeed();
    void typedCommaStartsANewRow();
    void keysQueuedBehindACommaSplitOnce();
    void pastedListBecomesRows();
    void moveReordersAndTheJobFollows();
    void badRowBlocksExtractWithItsRow();
    void addedRangeBlocksUntilFilledOrRemoved();
    void captionsStayOverTheirColumns();
    void stripRemovalSplitsTheRow();
    void stripShowsTheCurrentRowAndKeepsItsScroll();
    void foldOpensAloneUntilResized();
    void saveAsFollowsThePlanUntilEdited();
    void browseKeepsWindowModalityAndOwnerLifetime_data();
    void browseKeepsWindowModalityAndOwnerLifetime();
    void destinationCannotBeTheSource();
    void outputOpenInATabIsRefused();
    void failedAttemptRestoresTheChoices();
    void slowSaveAsCheckSaysSoAndExtractWaitsForIt();
    void editAfterExtractNeedsAnotherPress();
    void encryptedSourceAsksOnceInline();
    void encryptedSourceUsesTheTabPassword();
    void staleTabPasswordFallsBackToTheRow();

    void deleteOpensOnTheCurrentPage();
    void deleteStripClickTogglesThePage();
    void deleteStripKeysToggleThePage();
    void deletingEveryPageIsRefused();
    void deleteWritesTheRemainingPages();
    void changedPagesCancelPendingAcceptance_data();
    void changedPagesCancelPendingAcceptance();
    void realThumbnailsFadeDeletedPagesAndRecover();

private:
    QTemporaryDir dir_;
    QString plain_;
    QString encrypted_;

    /// Supplies fixture source facts before constructing the file-free editor.
    ExtractDialog::Source source(const QString &path) const
    {
        ExtractDialog::Source s;
        s.path = path;
        s.viewerPageCount = 31;
        s.seedPage = 6;
        s.probe = ExtractDialog::probeSource(s.path, s.password);
        return s;
    }
    // Document > Delete Pages with page 7 in view.
    ExtractDialog::Source deleting(const QString &path) const
    {
        ExtractDialog::Source s = source(path);
        s.mode = ExtractPlan::Mode::Delete;
        return s;
    }
    static QString summary(ExtractDialog &d) { return child<QLabel>(d, "extractSummary")->text(); }
    static QString tip(ExtractDialog &d, int cell)
    {
        return strip(d)->model()->index(cell, 0).data(Qt::ToolTipRole).toString();
    }
    // A click on a strip cell. The dialog applies it on the next event-loop turn.
    static void clickCell(ExtractDialog &d, int cell)
    {
        ExtractStrip *s = strip(d);
        QTest::mouseClick(s->viewport(), Qt::LeftButton, {},
                          s->visualRect(s->model()->index(cell, 0)).center());
    }
    // The strip action with this text, when it is the one showing.
    static QAction *stripAction(ExtractDialog &d, const QString &text)
    {
        for (QAction *a : strip(d)->actions())
            if (a->text() == text && a->isVisible() && a->isEnabled())
                return a;
        return nullptr;
    }
    static void prepare(ExtractDialog &d)
    {
        d.setAttribute(Qt::WA_DontShowOnScreen, true);
        d.show(); // realises the layout without ever putting a window on screen
        QTest::qWait(1);
        settle(&d);
    }
    template<typename T>
    static T *child(ExtractDialog &d, const char *name)
    {
        return d.findChild<T *>(QLatin1String(name));
    }
    static QListWidget *list(ExtractDialog &d) { return child<QListWidget>(d, "extractList"); }
    static QLineEdit *output(ExtractDialog &d) { return child<QLineEdit>(d, "extractOutput"); }
    static QPushButton *extract(ExtractDialog &d) { return child<QPushButton>(d, "extractAccept"); }
    static QString error(ExtractDialog &d) { return child<QLabel>(d, "extractError")->text(); }
    static ExtractStrip *strip(ExtractDialog &d) { return child<ExtractStrip>(d, "extractStrip"); }
    static QPushButton *button(ExtractDialog &d, const QString &text)
    {
        for (QPushButton *b : d.findChildren<QPushButton *>())
            if (b->text() == text)
                return b;
        return nullptr;
    }
    // The row fields, in row order.
    static QList<QLineEdit *> rows(ExtractDialog &d)
    {
        QList<QLineEdit *> out;
        QListWidget *l = list(d);
        for (int i = 0; i < l->count(); ++i)
            if (QWidget *row = l->itemWidget(l->item(i)))
                if (auto *e = row->findChild<QLineEdit *>(QStringLiteral("extractRowSpec")))
                    out << e;
        return out;
    }
    static QStringList texts(ExtractDialog &d)
    {
        QStringList out;
        for (QLineEdit *e : rows(d))
            out << e->text();
        return out;
    }
    // Lets the Save as check answer: it runs on a background thread, and the error
    // line and Extract wait for it.
    static void settle(QWidget *dialog)
    {
        const auto *check = dialog->findChild<mervin::SaveCheck *>();
        QVERIFY(check);
        QVERIFY(QTest::qWaitFor([check] { return !check->pending(); }, 5000));
    }
    // Replace a field's text the way a user does: select all, type.
    static void type(QLineEdit *field, const QString &text)
    {
        field->selectAll();
        if (text.isEmpty())
            QTest::keyClick(field, Qt::Key_Delete);
        else
            QTest::keyClicks(field, text);
        settle(field->window());
    }
    // Answer the next "Replace it?" box with Yes once it is up. The box runs its
    // own event loop, so a polling timer started beforehand is what reaches it.
    static void answerReplaceWithYes()
    {
        auto *timer = new QTimer;
        auto tries = std::make_shared<int>(0);
        QObject::connect(timer, &QTimer::timeout, timer, [timer, tries] {
            for (QWidget *w : QApplication::topLevelWidgets())
                if (auto *box = qobject_cast<QMessageBox *>(w); box && box->isVisible()) {
                    box->button(QMessageBox::Yes)->click();
                    timer->deleteLater();
                    return;
                }
            if (++*tries > 200) // 2 s: no box came, so the test fails on its own checks
                timer->deleteLater();
        });
        timer->start(10);
    }
    // What the dialog would write: Extract, then its job's pages.
    // Extract checks Save as again, then returns a value-owned job.
    static void press(ExtractDialog &d)
    {
        extract(d)->click();
        settle(&d);
    }
    /// Returns the accepted source-page order after asynchronous destination checks.
    static QList<int> accepted(ExtractDialog &d)
    {
        press(d);
        return d.result() == QDialog::Accepted ? d.job().pages : QList<int>{-1};
    }
};

void TstExtractDialog::initTestCase()
{
    // The English plural forms ("7 pages") come from the English catalog;
    // without it Qt would show "7 page(s)".
    mervin::i18n::apply(QStringLiteral("en"));
    QVERIFY(dir_.isValid());
    plain_ = dir_.filePath(QStringLiteral("annual-report.pdf"));
    encrypted_ = dir_.filePath(QStringLiteral("annual-report-locked.pdf"));
    makePdf(plain_, 31);

    QpdfService svc;
    QCOMPARE(svc.encrypt(plain_, encrypted_, QString(), QStringLiteral("secret"), QString(),
                         QpdfService::Algorithm::AES256, {}, nullptr),
             QpdfService::Status::Ok);
}

void TstExtractDialog::opensWithOneRowOnTheSeedPage()
{
    ExtractDialog d(source(plain_));
    prepare(d);
    QCOMPARE(texts(d), QStringList({QStringLiteral("7")}));
    QCOMPARE(rows(d).at(0)->selectedText(), QStringLiteral("7")); // typing replaces it
    QCOMPARE(d.focusWidget(), rows(d).at(0));
    QVERIFY(extract(d)->isEnabled());
    QVERIFY(extract(d)->isDefault()); // Enter extracts
    QVERIFY(error(d).isEmpty());
    QVERIFY(output(d)->text().endsWith(QStringLiteral("-p7.pdf")));
    QCOMPARE(child<QLabel>(d, "extractSummary")->text(), QStringLiteral("Result: 1 page."));
    QCOMPARE(child<QLabel>(d, "extractOf")->text(), QStringLiteral("of 31"));
    QVERIFY(d.openWhenDone());
    // A plain file has no password row.
    QVERIFY(!child<QLineEdit>(d, "extractPassword"));

    ExtractDialog::Source closed = source(plain_);
    closed.openWhenDone = false; // the last choice, from Settings
    ExtractDialog e(closed);
    QVERIFY(!e.openWhenDone());

    // qpdf writes from the file on disk, so the summary warns about edits in the tab.
    ExtractDialog::Source edited = source(plain_);
    edited.hasUnsavedEdits = true;
    ExtractDialog u(edited);
    prepare(u);
    QCOMPARE(child<QLabel>(u, "extractSummary")->text(),
             QStringLiteral("Result: 1 page. Unsaved changes are not included."));
}

// Document > Extract Pages has no seed: its one row holds the whole document.
void TstExtractDialog::opensWithEveryPageWithoutASeed()
{
    ExtractDialog::Source whole = source(plain_);
    whole.seedPage.reset();
    ExtractDialog d(whole);
    prepare(d);
    QCOMPARE(texts(d), QStringList({QStringLiteral("1-31")}));
    QCOMPARE(rows(d).at(0)->selectedText(), QStringLiteral("1-31")); // typing replaces it
    QVERIFY(extract(d)->isEnabled());
    QVERIFY(error(d).isEmpty());
    QVERIFY(output(d)->text().endsWith(QStringLiteral("-p1-31.pdf")));
    QCOMPARE(child<QLabel>(d, "extractSummary")->text(),
             QStringLiteral("Result: 31 pages, in the order shown."));
}

void TstExtractDialog::typedCommaStartsANewRow()
{
    ExtractDialog d(source(plain_));
    prepare(d);
    QTest::keyClicks(rows(d).at(0), QStringLiteral("2,")); // replaces the selected "7"
    QTRY_COMPARE(texts(d), QStringList({QStringLiteral("2"), QString()}));
    // The caret moved on to the new row, so the next digit lands there.
    QCOMPARE(d.focusWidget(), rows(d).at(1));
    QTest::keyClicks(d.focusWidget(), QStringLiteral("5"));
    QCOMPARE(texts(d), QStringList({QStringLiteral("2"), QStringLiteral("5")}));
    QCOMPARE(list(d)->currentRow(), 1);
    QCOMPARE(accepted(d), QList<int>({1, 4}));
}

void TstExtractDialog::keysQueuedBehindACommaSplitOnce()
{
    // keyClicks gives the event loop no turn between keys, like a busy UI thread
    // or an input tool: the keys after the comma reach the field before its split
    // has rebuilt the rows, and must be split with it, once.
    ExtractDialog d(source(plain_));
    prepare(d);
    QTest::keyClicks(rows(d).at(0), QStringLiteral("2,5"));
    QTRY_COMPARE(texts(d), QStringList({QStringLiteral("2"), QStringLiteral("5")}));
    QCOMPARE(d.focusWidget(), rows(d).at(1));
    QCOMPARE(accepted(d), QList<int>({1, 4}));

    ExtractDialog e(source(plain_));
    prepare(e);
    QTest::keyClicks(rows(e).at(0), QStringLiteral("1,2,"));
    QTRY_COMPARE(texts(e), QStringList({QStringLiteral("1"), QStringLiteral("2"), QString()}));
    QCOMPARE(e.focusWidget(), rows(e).at(2));
}

void TstExtractDialog::pastedListBecomesRows()
{
    ExtractDialog d(source(plain_));
    prepare(d);
    rows(d).at(0)->selectAll();
    rows(d).at(0)->insert(QStringLiteral("1-3, 5, 8-10"));
    QTRY_COMPARE(texts(d),
                 QStringList({QStringLiteral("1-3"), QStringLiteral("5"), QStringLiteral("8-10")}));
    QCOMPARE(d.focusWidget(), rows(d).at(2));
    QCOMPARE(rows(d).at(2)->cursorPosition(), 4); // at the end, nothing selected
    QVERIFY(!rows(d).at(2)->hasSelectedText());
    QCOMPARE(child<QLabel>(d, "extractSummary")->text(),
             QStringLiteral("Result: 7 pages, in the order shown."));
    QCOMPARE(strip(d)->model()->rowCount(), 7);
}

void TstExtractDialog::moveReordersAndTheJobFollows()
{
    ExtractDialog d(source(plain_));
    prepare(d);
    rows(d).at(0)->selectAll();
    rows(d).at(0)->insert(QStringLiteral("1-3, 5, 8"));
    QTRY_COMPARE(rows(d).size(), 3);

    QPushButton *up = button(d, QStringLiteral("Move &Up"));
    QPushButton *down = button(d, QStringLiteral("Move &Down"));
    QVERIFY(up && down);
    QCOMPARE(list(d)->currentRow(), 2);
    QVERIFY(!down->isEnabled()); // already last
    up->click();
    QCOMPARE(texts(d), QStringList({QStringLiteral("1-3"), QStringLiteral("8"), QStringLiteral("5")}));
    QCOMPARE(list(d)->currentRow(), 1); // the moved row stays current...
    QCOMPARE(d.focusWidget(), rows(d).at(1)); // ...with the caret in it
    up->click();
    QCOMPARE(texts(d), QStringList({QStringLiteral("8"), QStringLiteral("1-3"), QStringLiteral("5")}));
    QVERIFY(!up->isEnabled());
    // The strip and the Output column follow at once.
    const QModelIndex first = strip(d)->model()->index(0, 0);
    QCOMPARE(first.data(Qt::DisplayRole).toString(), QStringLiteral("8"));
    QCOMPARE(list(d)->itemWidget(list(d)->item(1))
                 ->findChild<QLabel *>(QStringLiteral("rowListOutput"))
                 ->text(),
             QStringLiteral("2-4"));
    down->click();
    QCOMPARE(texts(d), QStringList({QStringLiteral("1-3"), QStringLiteral("8"), QStringLiteral("5")}));
    QCOMPARE(list(d)->currentRow(), 1);
    QCOMPARE(d.focusWidget(), rows(d).at(1));
    QCOMPARE(accepted(d), QList<int>({0, 1, 2, 7, 4}));
}

void TstExtractDialog::badRowBlocksExtractWithItsRow()
{
    ExtractDialog d(source(plain_));
    prepare(d);
    rows(d).at(0)->selectAll();
    rows(d).at(0)->insert(QStringLiteral("5-7, 40"));
    QTRY_COMPARE(rows(d).size(), 2);
    QVERIFY(!extract(d)->isEnabled());
    QCOMPARE(error(d), QStringLiteral("Row 2: Page 40 is out of range (1-31)."));
    QVERIFY(rows(d).at(1)->property("invalid").toBool());
    QVERIFY(!rows(d).at(0)->property("invalid").toBool());
    // The bad row is a cell where it would land, not dropped.
    const QAbstractItemModel *m = strip(d)->model();
    QCOMPARE(m->rowCount(), 4);
    QCOMPARE(m->index(3, 0).data(ExtractStrip::KindRole).toInt(), int(ExtractPlan::Cell::Kind::Bad));
    // Painting with no Document or RenderEngine: pending paper, no crash.
    QVERIFY(!strip(d)->grab().isNull());

    type(rows(d).at(1), QStringLiteral("30"));
    QVERIFY(extract(d)->isEnabled());
    QVERIFY(error(d).isEmpty());
    QVERIFY(!rows(d).at(1)->property("invalid").toBool());
}

void TstExtractDialog::addedRangeBlocksUntilFilledOrRemoved()
{
    ExtractDialog d(source(plain_));
    prepare(d);
    QPushButton *add = button(d, QStringLiteral("&Add Range"));
    QVERIFY(add);
    add->click();
    QCOMPARE(texts(d), QStringList({QStringLiteral("7"), QString()}));
    QCOMPARE(d.focusWidget(), rows(d).at(1));
    QVERIFY(!extract(d)->isEnabled());
    QCOMPARE(error(d), QStringLiteral("Row 2: Enter a page or range, like 5-7."));
    QVERIFY(!rows(d).at(1)->property("invalid").toBool()); // unfinished, not wrong

    QTest::keyClicks(rows(d).at(1), QStringLiteral("9"));
    settle(&d);
    QVERIFY(extract(d)->isEnabled());

    // Added below the current row, not at the end.
    list(d)->setCurrentRow(0);
    add->click();
    QCOMPARE(texts(d), QStringList({QStringLiteral("7"), QString(), QStringLiteral("9")}));
    QVERIFY(!extract(d)->isEnabled());
    button(d, QStringLiteral("&Remove"))->click();
    QCOMPARE(texts(d), QStringList({QStringLiteral("7"), QStringLiteral("9")}));
    settle(&d);
    QVERIFY(extract(d)->isEnabled());

    // No rows at all has its own reason.
    button(d, QStringLiteral("&Remove"))->click();
    button(d, QStringLiteral("&Remove"))->click();
    QCOMPARE(list(d)->count(), 0);
    QCOMPARE(error(d), QStringLiteral("Add a range of pages to extract."));
    QVERIFY(!extract(d)->isEnabled());
}

// The Output caption's right edge is the Output column's, however the rows came
// about: the first build (while the dialog was still being laid out, when the
// list's scrollbar hides without a Resize) and a rebuild at the final size.
void TstExtractDialog::captionsStayOverTheirColumns()
{
    ExtractDialog d(source(plain_));
    prepare(d);
    const auto captionRight = [&d] {
        QWidget *header = child<QWidget>(d, "rowListHeader");
        for (QLabel *l : header->findChildren<QLabel *>())
            if (l->text() == QStringLiteral("Output"))
                return l->mapTo(&d, l->rect().topRight()).x();
        return -1;
    };
    const auto columnRight = [&d](int row) {
        QWidget *w = list(d)->itemWidget(list(d)->item(row));
        auto *l = w->findChild<QLabel *>(QStringLiteral("rowListOutput"));
        return l->mapTo(&d, l->rect().topRight()).x();
    };
    QTRY_COMPARE(columnRight(0), captionRight());
    button(d, QStringLiteral("&Add Range"))->click();
    QTRY_COMPARE(columnRight(0), captionRight());
    QTRY_COMPARE(columnRight(1), captionRight());
}

void TstExtractDialog::stripRemovalSplitsTheRow()
{
    ExtractDialog d(source(plain_));
    prepare(d);
    rows(d).at(0)->selectAll();
    rows(d).at(0)->insert(QStringLiteral("5-7, 12"));
    QTRY_COMPARE(rows(d).size(), 2);

    // Clicking a strip cell makes its row current in the list.
    ExtractStrip *s = strip(d);
    list(d)->setCurrentRow(1);
    QTest::mouseClick(s->viewport(), Qt::LeftButton, {},
                      s->visualRect(s->model()->index(1, 0)).center());
    QCOMPARE(list(d)->currentRow(), 0);

    // The strip's Remove, as its context menu or Delete would trigger it.
    s->setCurrentIndex(s->model()->index(1, 0)); // page 6
    for (QAction *a : s->actions())
        if (a->text() == QStringLiteral("Remove"))
            a->trigger();
    QCOMPARE(texts(d), QStringList({QStringLiteral("5"), QStringLiteral("7"), QStringLiteral("12")}));
    QCOMPARE(s->currentIndex().row(), 1); // page 7 slid into its place...
    QCOMPARE(list(d)->currentRow(), 1);   // ...and its row is current
    QCOMPARE(accepted(d), QList<int>({4, 6, 11}));
}

void TstExtractDialog::stripShowsTheCurrentRowAndKeepsItsScroll()
{
    ExtractDialog d(source(plain_));
    prepare(d);
    ExtractStrip *s = strip(d);
    const auto inView = [s](int cell) {
        return s->visualRect(s->model()->index(cell, 0)).intersects(s->viewport()->rect());
    };
    // Ten single pages cannot fold, so the strip scrolls; the split makes the
    // last row current, and the strip shows it.
    rows(d).at(0)->selectAll();
    rows(d).at(0)->insert(QStringLiteral("1,3,5,7,9,11,13,15,17,19"));
    QTRY_COMPARE(rows(d).size(), 10);
    QCOMPARE(s->model()->rowCount(), 10);
    QVERIFY(!inView(0));
    QVERIFY(inView(9));
    // A moved row stays in view.
    button(d, QStringLiteral("Move &Up"))->click();
    QCOMPARE(list(d)->currentRow(), 8);
    QVERIFY(inView(8));
    // A refill for anything else leaves the user's scrolling alone.
    QScrollBar *bar = s->horizontalScrollBar();
    bar->setValue(bar->maximum());
    const int scrolled = bar->value();
    QTest::keyClicks(output(d), QStringLiteral("x"));
    QCOMPARE(bar->value(), scrolled);
}

void TstExtractDialog::foldOpensAloneUntilResized()
{
    ExtractDialog d(source(plain_));
    prepare(d);
    ExtractStrip *s = strip(d);
    const QAbstractItemModel *m = s->model();
    // The strip's own menu item, as the context menu or Space would trigger it.
    const auto expandCurrent = [s] {
        for (QAction *a : s->actions())
            if (a->isVisible() && a->text().startsWith(QStringLiteral("Show Pages")))
                a->trigger();
    };
    // Rows of the strip that are folds, left to right.
    const auto folds = [m] {
        QList<int> rows;
        for (int r = 0; r < m->rowCount(); ++r)
            if (m->index(r, 0).data(ExtractStrip::KindRole).toInt()
                == int(ExtractPlan::Cell::Kind::Fold))
                rows << r;
        return rows;
    };

    rows(d).at(0)->selectAll();
    rows(d).at(0)->insert(QStringLiteral("1-31, 5-25"));
    QTRY_COMPARE(rows(d).size(), 2);
    const int folded = m->rowCount(); // 1 [2-30] 31 5 6 7 [8-24] 25
    QCOMPARE(folds().size(), 2);
    s->setCurrentIndex(m->index(folds().at(0), 0));
    expandCurrent();
    QCOMPARE(folds().size(), 1); // that run opens; the other stays folded
    QVERIFY(m->rowCount() > folded);
    // Editing the run away closes it, so typing it again starts folded.
    type(rows(d).at(0), QStringLiteral("1-31"));
    QCOMPARE(m->rowCount(), folded);

    s->setCurrentIndex(m->index(folds().at(1), 0));
    expandCurrent();
    QCOMPARE(folds().size(), 1);
    d.resize(d.width() + 40, d.height()); // resizing refolds from scratch
    QTRY_COMPARE(folds().size(), 2);
}

void TstExtractDialog::saveAsFollowsThePlanUntilEdited()
{
    ExtractDialog d(source(plain_));
    prepare(d);
    type(rows(d).at(0), QStringLiteral("12-18"));
    QVERIFY(output(d)->text().endsWith(QStringLiteral("-p12-18.pdf")));

    QTest::keyClicks(output(d), QStringLiteral("x")); // any edit latches the field
    const QString latched = output(d)->text();
    type(rows(d).at(0), QStringLiteral("3"));
    QCOMPARE(output(d)->text(), latched);

    type(output(d), QString());
    QVERIFY(output(d)->text().isEmpty()); // it does not snap back
    QCOMPARE(error(d), QStringLiteral("Choose where to save the extracted pages."));
    QVERIFY(!extract(d)->isEnabled());

    // A folder is not a file name.
    type(output(d), QDir::toNativeSeparators(dir_.path() + QLatin1Char('/')));
    QCOMPARE(error(d), QStringLiteral("That is a folder. Add a file name."));
    QVERIFY(!extract(d)->isEnabled());
}

/// Covers picker acceptance, cancellation and owner destruction.
void TstExtractDialog::browseKeepsWindowModalityAndOwnerLifetime_data()
{
    QTest::addColumn<int>("answer");
    QTest::newRow("cancel") << 0;
    QTest::newRow("accept") << 1;
    QTest::newRow("owner closes") << 2;
}

/// Keeps Browse window-modal and ignores answers after its owner disappears.
void TstExtractDialog::browseKeepsWindowModalityAndOwnerLifetime()
{
    QFETCH(int, answer);
    QPointer<QWidget> owner = new QWidget;
    const auto cleanup = qScopeGuard([&] { delete owner.data(); });
    QPointer<ExtractDialog> dialog = new ExtractDialog(source(plain_), owner);
    dialog->setWindowModality(Qt::WindowModal);
    prepare(*dialog);
    const QString before = child<QLineEdit>(*dialog, "extractOutput")->text();
    const QString chosen = dir_.filePath(QStringLiteral("browsed-copy.pdf"));
    int shown = 0;
    Qt::WindowModality observedModality = Qt::NonModal;
    QTimer respond;
    respond.setInterval(5);
    connect(&respond, &QTimer::timeout, this, [&] {
        auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        if (!picker || picker->property("testSubmitted").toBool())
            return;
        picker->setProperty("testSubmitted", true);
        ++shown;
        observedModality = picker->windowModality();
        QCOMPARE(picker->acceptMode(), QFileDialog::AcceptSave);
        if (answer == 2) {
            delete owner.data();
        } else if (answer == 1) {
            picker->selectFile(chosen);
            if (auto *name = picker->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
                name->setText(chosen);
            QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
        } else {
            picker->reject();
        }
    });
    respond.start();
    child<QPushButton>(*dialog, "extractBrowse")->click();
    QCOMPARE(shown, 1);
    QCOMPARE(observedModality, Qt::WindowModal);
    if (answer == 2) {
        QVERIFY(!dialog && !owner);
    } else {
        QVERIFY(dialog);
        QCOMPARE(child<QLineEdit>(*dialog, "extractOutput")->text(),
                 answer == 1 ? QDir::toNativeSeparators(chosen) : before);
    }
}

void TstExtractDialog::destinationCannotBeTheSource()
{
    ExtractDialog d(source(plain_));
    prepare(d);
    type(output(d), QDir::toNativeSeparators(plain_));
    QCOMPARE(error(d), QStringLiteral("The extract cannot replace the document it comes from."));
    QVERIFY(!extract(d)->isEnabled());

    type(output(d), QStringLiteral("other.pdf")); // beside the source
    QVERIFY(error(d).isEmpty());
    QVERIFY(extract(d)->isEnabled());
}

/// Keeps an open destination unavailable to an accepted page plan.
void TstExtractDialog::outputOpenInATabIsRefused()
{
    // The viewer holds an open tab's file open, so writing over it would fail. The
    // error line says so before Extract is pressed, and Extract stays disabled.
    const QString opened = dir_.filePath(QStringLiteral("opened-in-tab.pdf"));
    makePdf(opened, 2);
    ExtractDialog::Source s = source(plain_);
    s.openPaths = {plain_, opened};
    ExtractDialog d(s);
    prepare(d);
    type(output(d), QDir::toNativeSeparators(opened));
    QCOMPARE(error(d), QStringLiteral("That file is open in a tab. Choose another name."));
    QVERIFY(!extract(d)->isEnabled());
    QTest::keyClick(output(d), Qt::Key_Return); // Enter does nothing either
    QVERIFY(d.result() != QDialog::Accepted);
    QVERIFY(QFileInfo(opened).size() > 0);

    type(output(d), QStringLiteral("not-open.pdf"));
    QVERIFY(error(d).isEmpty());
    QVERIFY(extract(d)->isEnabled());
}

/// Restores ranges and editable fields from a failed worker attempt.
void TstExtractDialog::failedAttemptRestoresTheChoices()
{
    const QString target = dir_.filePath(QStringLiteral("failed-output.pdf"));
    ExtractDialog::Choices choices;
    choices.specs = {QStringLiteral("5-7"), QStringLiteral("12")};
    choices.output = QDir::toNativeSeparators(target);
    choices.outputEdited = true;
    choices.openWhenDone = false;
    choices.failure = ExtractDialog::Choices::Failure::Write;
    choices.error = QStringLiteral("The test writer could not finish.");
    ExtractDialog d(source(plain_), nullptr, choices);
    prepare(d);
    QCOMPARE(texts(d), choices.specs);
    QCOMPARE(output(d)->text(), choices.output);
    QVERIFY(!d.openWhenDone());
    QVERIFY2(error(d).startsWith(QStringLiteral("Could not write \"failed-output.pdf\".")),
             qPrintable(error(d)));
    QCOMPARE(child<QLabel>(d, "extractError")->toolTip(), choices.error);
    QVERIFY(extract(d)->isEnabled());
    press(d);
    QCOMPARE(d.result(), int(QDialog::Accepted));
    QCOMPARE(d.job().pages, QList<int>({4, 5, 6, 11}));
    QVERIFY(!QFileInfo::exists(target)); // acceptance leaves writing to the tab job
}

// Save as is checked on a background thread, as its folder can be on a share that
// stopped answering. A quick check changes nothing on screen; a slow one says so,
// and Extract pressed meanwhile returns its job once it answers.
/// Shows the slow-check reason and postpones acceptance until the destination answers.
void TstExtractDialog::slowSaveAsCheckSaysSoAndExtractWaitsForIt()
{
    const QString target = dir_.filePath(QStringLiteral("checked-slowly.pdf"));
    ExtractDialog d(source(plain_));
    prepare(d);
    settle(&d);
    auto release = qScopeGuard([] { SaveCheck::holdForTesting(false); });
    SaveCheck::holdForTesting(true);
    output(d)->selectAll();
    QTest::keyClicks(output(d), QDir::toNativeSeparators(target));
    QVERIFY(error(d).isEmpty());
    QVERIFY(extract(d)->isEnabled());
    QTRY_COMPARE(error(d), QStringLiteral("Checking the folder…"));
    QVERIFY(!extract(d)->isEnabled());
    // A row's own reason shows meanwhile, and the check's comes back after it.
    rows(d).at(0)->selectAll();
    QTest::keyClicks(rows(d).at(0), QStringLiteral("99"));
    QCOMPARE(error(d), QStringLiteral("Page 99 is out of range (1-31)."));
    rows(d).at(0)->selectAll();
    QTest::keyClicks(rows(d).at(0), QStringLiteral("7"));
    QCOMPARE(error(d), QStringLiteral("Checking the folder…"));
    QVERIFY(!extract(d)->isEnabled());
    SaveCheck::holdForTesting(false);
    settle(&d);
    QVERIFY(error(d).isEmpty());
    QVERIFY(extract(d)->isEnabled());

    // Pressed for a name already checked, while the folder stops answering.
    SaveCheck::holdForTesting(true);
    extract(d)->click();
    QTRY_COMPARE(error(d), QStringLiteral("Checking the folder…"));
    QVERIFY(!extract(d)->isEnabled());
    QVERIFY(d.result() != QDialog::Accepted);
    QVERIFY(!QFileInfo::exists(target));
    SaveCheck::holdForTesting(false);
    settle(&d);
    QCOMPARE(d.result(), int(QDialog::Accepted));
    QCOMPARE(d.job().path, target);
    QVERIFY(!QFileInfo::exists(target));
}

// Extract writes what it was pressed for. Typing another name while the check runs
// takes that press back.
/// Invalidates a pending acceptance when the user changes its plan.
void TstExtractDialog::editAfterExtractNeedsAnotherPress()
{
    const QString first = dir_.filePath(QStringLiteral("pressed-for.pdf"));
    const QString second = dir_.filePath(QStringLiteral("typed-after.pdf"));
    ExtractDialog d(source(plain_));
    prepare(d);
    type(output(d), QDir::toNativeSeparators(first));
    auto release = qScopeGuard([] { SaveCheck::holdForTesting(false); });
    SaveCheck::holdForTesting(true);
    extract(d)->click();
    output(d)->selectAll();
    QTest::keyClicks(output(d), QDir::toNativeSeparators(second));
    SaveCheck::holdForTesting(false);
    settle(&d);
    QVERIFY(d.result() != QDialog::Accepted);
    QVERIFY(!QFileInfo::exists(first));
    QVERIFY(!QFileInfo::exists(second));
    QVERIFY(extract(d)->isEnabled());
    press(d);
    QCOMPARE(d.result(), int(QDialog::Accepted));
    QCOMPARE(d.job().path, second);
    QVERIFY(!QFileInfo::exists(second));
}

/// Captures an unverified inline password for the worker without reading the source.
void TstExtractDialog::encryptedSourceAsksOnceInline()
{
    const auto src = source(encrypted_);
    ExtractDialog d(src);
    prepare(d);
    QLineEdit *password = child<QLineEdit>(d, "extractPassword");
    QVERIFY(password);
    QCOMPARE(error(d), QStringLiteral("Enter the password for this PDF."));
    QVERIFY(!extract(d)->isEnabled());
    QTest::keyClicks(password, QStringLiteral("nope"));
    press(d);
    QCOMPARE(d.result(), int(QDialog::Accepted));
    QCOMPARE(d.choices().password, QStringLiteral("nope"));
    auto choices = d.choices();
    choices.failure = ExtractDialog::Choices::Failure::Password;
    ExtractDialog retry(src, nullptr, choices);
    prepare(retry);
    password = child<QLineEdit>(retry, "extractPassword");
    QVERIFY(password);
    QCOMPARE(error(retry), QStringLiteral("That password is not correct."));
    password->selectAll();
    QTest::keyClicks(password, QStringLiteral("secret"));
    QVERIFY(error(retry).isEmpty());
    press(retry);
    QCOMPARE(retry.result(), int(QDialog::Accepted));
    QCOMPARE(retry.password(), QStringLiteral("secret"));
    QCOMPARE(retry.job().pages, QList<int>({6}));
}

/// Reuses the worker probe's verified password in the accepted job.
void TstExtractDialog::encryptedSourceUsesTheTabPassword()
{
    // The password that opened the tab unlocks the file: the dialog is the plain one.
    ExtractDialog::Source s = source(encrypted_);
    s.password = QStringLiteral("secret");
    s.probe = ExtractDialog::probeSource(s.path, s.password);
    ExtractDialog d(s);
    prepare(d);
    QVERIFY(!child<QLineEdit>(d, "extractPassword"));
    QVERIFY(error(d).isEmpty());
    QVERIFY(extract(d)->isEnabled());

    press(d);
    QCOMPARE(d.result(), int(QDialog::Accepted));
    QCOMPARE(d.password(), QStringLiteral("secret"));

    QCOMPARE(d.job().pages, QList<int>{6});
    QVERIFY(!QFileInfo::exists(d.job().path));
}

/// Allows a replacement password when the captured tab password fails probing.
void TstExtractDialog::staleTabPasswordFallsBackToTheRow()
{
    // A remembered password that no longer opens the file (it changed on disk)
    // must not pass for a verified one: the row appears as if none were known.
    ExtractDialog::Source s = source(encrypted_);
    s.password = QStringLiteral("stale");
    s.probe = ExtractDialog::probeSource(s.path, s.password);
    ExtractDialog d(s);
    prepare(d);
    QVERIFY(child<QLineEdit>(d, "extractPassword"));
    QCOMPARE(error(d), QStringLiteral("Enter the password for this PDF."));
    QVERIFY(!extract(d)->isEnabled());
    QVERIFY(d.password().isEmpty());
}

void TstExtractDialog::deleteOpensOnTheCurrentPage()
{
    ExtractDialog d(deleting(plain_));
    prepare(d);
    QCOMPARE(d.windowTitle(), QStringLiteral("Delete pages from annual-report.pdf"));
    // The page in view is the one row, selected, so typing replaces it.
    QCOMPARE(texts(d), QStringList({QStringLiteral("7")}));
    QCOMPARE(rows(d).at(0)->selectedText(), QStringLiteral("7"));
    QCOMPARE(d.focusWidget(), rows(d).at(0));
    QCOMPARE(extract(d)->text(), QStringLiteral("Delete pages"));
    QVERIFY(extract(d)->isEnabled());
    QVERIFY(extract(d)->isDefault()); // Enter deletes
    QVERIFY(error(d).isEmpty());
    QVERIFY(output(d)->text().endsWith(QStringLiteral("annual-report-edited.pdf")));
    QCOMPARE(summary(d), QStringLiteral("Result: 30 of 31 pages remain (1-6, 8-31)."));
    QVERIFY(d.openWhenDone());

    // Pages to delete have no order: no grips, no moves, no Output column.
    QVERIFY(!button(d, QStringLiteral("Move &Up")));
    QVERIFY(!button(d, QStringLiteral("Move &Down")));
    QVERIFY(button(d, QStringLiteral("&Add Range")));
    QVERIFY(!list(d)->findChild<QLabel *>(QStringLiteral("rowListGrip")));
    QVERIFY(!list(d)->findChild<QLabel *>(QStringLiteral("rowListOutput")));
    for (QLabel *l : child<QWidget>(d, "rowListHeader")->findChildren<QLabel *>())
        QVERIFY(l->text() != QStringLiteral("Output"));

    // The strip shows every page, the one to delete among them.
    QCOMPARE(strip(d)->model()->rowCount(), 31);
    QCOMPARE(tip(d, 6), QStringLiteral("Page 7 will be deleted. Click to keep it."));
    QCOMPARE(tip(d, 5), QStringLiteral("Click to delete page 6."));

    // Unsaved edits in the tab are left out, as in Extract.
    ExtractDialog::Source edited = deleting(plain_);
    edited.hasUnsavedEdits = true;
    ExtractDialog u(edited);
    prepare(u);
    QCOMPARE(summary(u), QStringLiteral("Result: 30 of 31 pages remain (1-6, 8-31). Unsaved "
                                        "changes are not included."));
}

void TstExtractDialog::deleteStripClickTogglesThePage()
{
    ExtractDialog d(deleting(plain_));
    prepare(d);
    type(rows(d).at(0), QStringLiteral("5-7"));
    ExtractStrip *s = strip(d);

    // Keeping the middle page of a range splits it.
    clickCell(d, 5); // page 6
    QTRY_COMPARE(texts(d), QStringList({QStringLiteral("5"), QStringLiteral("7")}));
    QCOMPARE(list(d)->currentRow(), 1);   // the row holding what followed
    QCOMPARE(s->currentIndex().row(), 5); // the page stays current
    QCOMPARE(tip(d, 5), QStringLiteral("Click to delete page 6."));
    QCOMPARE(summary(d), QStringLiteral("Result: 29 of 31 pages remain (1-4, 6, 8-31)."));

    // Deleting it again joins the two rows; a page with no neighbour gets a row.
    clickCell(d, 5);
    QTRY_COMPARE(texts(d), QStringList({QStringLiteral("5-7")}));
    clickCell(d, 1); // page 2
    QTRY_COMPARE(texts(d), QStringList({QStringLiteral("5-7"), QStringLiteral("2")}));
    QCOMPARE(list(d)->currentRow(), 1);
    QCOMPARE(summary(d), QStringLiteral("Result: 27 of 31 pages remain (1, 3-4, 8-31)."));

    // The context menu, and its keys, offer what a click would do.
    s->setCurrentIndex(s->model()->index(1, 0));
    QVERIFY(!stripAction(d, QStringLiteral("Delete Page")));
    QAction *keep = stripAction(d, QStringLiteral("Keep Page"));
    QVERIFY(keep);
    keep->trigger();
    QCOMPARE(texts(d), QStringList({QStringLiteral("5-7")}));
    QVERIFY(!stripAction(d, QStringLiteral("Keep Page")));
    QAction *del = stripAction(d, QStringLiteral("Delete Page"));
    QVERIFY(del);
    QVERIFY(del->shortcuts().contains(QKeySequence(Qt::Key_Delete)));
    del->trigger();
    QCOMPARE(texts(d), QStringList({QStringLiteral("5-7"), QStringLiteral("2")}));
    // Extract's own strip actions are not there.
    QVERIFY(!stripAction(d, QStringLiteral("Remove")));
}

// The keys the strip's context menu shows, pressed for real: two actions share
// Space, and only the enabled one may take it. Delete and Backspace only delete.
void TstExtractDialog::deleteStripKeysToggleThePage()
{
    ExtractDialog d(deleting(plain_)); // opens with page 7 to delete
    d.show();
    d.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&d));
    ExtractStrip *s = strip(d);
    s->setFocus();
    s->setCurrentIndex(s->model()->index(1, 0)); // page 2, kept
    const QStringList seeded{QStringLiteral("7")};
    const QStringList both{QStringLiteral("7"), QStringLiteral("2")};
    QCOMPARE(texts(d), seeded);

    QTest::keyClick(s, Qt::Key_Space);
    QTRY_COMPARE(texts(d), both);
    QTest::keyClick(s, Qt::Key_Delete); // already deleted: no change
    QTest::keyClick(s, Qt::Key_Backspace);
    QTest::qWait(1);
    QCOMPARE(texts(d), both);
    QTest::keyClick(s, Qt::Key_Space); // keeps it again
    QTRY_COMPARE(texts(d), seeded);
    QTest::keyClick(s, Qt::Key_Backspace);
    QTRY_COMPARE(texts(d), both);
    QTest::keyClick(s, Qt::Key_Space);
    QTRY_COMPARE(texts(d), seeded);
    QTest::keyClick(s, Qt::Key_Delete);
    QTRY_COMPARE(texts(d), both);
}

void TstExtractDialog::deletingEveryPageIsRefused()
{
    ExtractDialog d(deleting(plain_));
    prepare(d);
    type(rows(d).at(0), QStringLiteral("all"));
    QVERIFY(!extract(d)->isEnabled());
    QCOMPARE(error(d), QStringLiteral("Cannot delete every page."));
    QVERIFY(summary(d).isEmpty());
    QTest::keyClick(rows(d).at(0), Qt::Key_Return); // Enter does nothing either
    QVERIFY(d.result() != QDialog::Accepted);

    type(rows(d).at(0), QStringLiteral("1-30"));
    QVERIFY(extract(d)->isEnabled());
    QVERIFY(error(d).isEmpty());
    QCOMPARE(summary(d), QStringLiteral("Result: 1 of 31 pages remains (31)."));

    // With no rows the line says how to start again. Delete pages has no Remove
    // button beside the rows. The row's own ✕ takes the row away.
    QVERIFY(!child<QPushButton>(d, "extractRemove"));
    QVERIFY(!button(d, QStringLiteral("&Remove")));
    list(d)->itemWidget(list(d)->item(0))
        ->findChild<QToolButton *>(QStringLiteral("rowListX"))->click();
    QTRY_COMPARE(list(d)->count(), 0);
    QCOMPARE(error(d), QStringLiteral("Click the pages to delete, or add a range."));
    QVERIFY(!extract(d)->isEnabled());
}

/// Applies the accepted Delete request and checks retained source-page identities.
void TstExtractDialog::deleteWritesTheRemainingPages()
{
    const QString target = dir_.filePath(QStringLiteral("deleted-copy.pdf"));
    ExtractDialog d(deleting(plain_));
    prepare(d);
    rows(d).at(0)->selectAll();
    rows(d).at(0)->insert(QStringLiteral("2, 5-6"));
    QTRY_COMPARE(rows(d).size(), 2);
    type(output(d), QDir::toNativeSeparators(target));
    QVERIFY(extract(d)->isEnabled());
    press(d);
    QCOMPARE(d.result(), int(QDialog::Accepted));
    QCOMPARE(d.job().mode, ExtractPlan::Mode::Delete);
    QCOMPARE(d.job().pages, QList<int>({1, 4, 5}));
    QList<int> kept;
    for (int page = 0; page < 31; ++page)
        if (page != 1 && page != 4 && page != 5)
            kept << page;
    QVERIFY(!QFileInfo::exists(target));
    QCOMPARE(PageOps::deletePages(plain_, target, d.job().pages, d.password()), PageOps::Status::Ok);
    QCOMPARE(sourcePages(target), kept);
    QCOMPARE(sourcePages(plain_).size(), 31); // the original keeps every page

    // An encrypted source gives an encrypted copy, written with the tab's password.
    const QString locked = dir_.filePath(QStringLiteral("deleted-locked.pdf"));
    ExtractDialog::Source s = deleting(encrypted_);
    s.password = QStringLiteral("secret");
    s.probe = ExtractDialog::probeSource(s.path, s.password);
    ExtractDialog e(s);
    prepare(e);
    QVERIFY(!child<QLineEdit>(e, "extractPassword"));
    type(output(e), QDir::toNativeSeparators(locked));
    press(e);
    QCOMPARE(e.result(), int(QDialog::Accepted));
    QCOMPARE(e.password(), QStringLiteral("secret"));
    QVERIFY(!QFileInfo::exists(locked));
    QCOMPARE(PageOps::deletePages(encrypted_, locked, e.job().pages, e.password()), PageOps::Status::Ok);
    int count = 0;
    QString err;
    QCOMPARE(PageOps::probe(locked, &count, QString(), &err), PageOps::Status::NeedsPassword);
    QCOMPARE(PageOps::probe(locked, &count, QStringLiteral("secret"), &err), PageOps::Status::Ok);
    QCOMPARE(count, 30);
    QCOMPARE(sourcePages(locked, QStringLiteral("secret")).value(6), 7); // page 7 went
}

void TstExtractDialog::changedPagesCancelPendingAcceptance_data()
{
    QTest::addColumn<bool>("reorder");
    QTest::newRow("range edit") << false;
    QTest::newRow("row reorder") << true;
}

/// A late destination answer must not accept the page order from an earlier press.
void TstExtractDialog::changedPagesCancelPendingAcceptance()
{
    QFETCH(bool, reorder);
    ExtractDialog d(source(plain_));
    prepare(d);
    type(rows(d).first(), QStringLiteral("1,3"));
    QTRY_COMPARE(rows(d).size(), 2);
    settle(&d);
    QSignalSpy acceptedSignal(&d, &QDialog::accepted);
    const auto release = qScopeGuard([] { SaveCheck::holdForTesting(false); });
    SaveCheck::holdForTesting(true);
    extract(d)->click();
    QVERIFY(d.findChild<SaveCheck *>()->pending());
    if (reorder) {
        list(d)->setCurrentRow(1);
        QPushButton *up = nullptr;
        for (auto *button : d.findChildren<QPushButton *>())
            if (button->text() == QStringLiteral("Move &Up"))
                up = button;
        QVERIFY(up);
        up->click();
    } else {
        rows(d).first()->selectAll();
        QTest::keyClicks(rows(d).first(), QStringLiteral("2"));
    }
    SaveCheck::holdForTesting(false);
    settle(&d);
    QVERIFY(acceptedSignal.isEmpty());
    QVERIFY(d.isVisible());
    press(d);
    QCOMPARE(acceptedSignal.count(), 1);
    QCOMPARE(d.job().pages, reorder ? QList<int>({2, 0}) : QList<int>({1, 2}));
}

/// Rendered page pixels remain in order, fade on deletion, and recover when kept again.
void TstExtractDialog::realThumbnailsFadeDeletedPagesAndRecover()
{
    const QString path = dir_.filePath(QStringLiteral("thumbnail-colors.pdf"));
    QPDF pdf;
    pdf.emptyPDF();
    QPDFPageDocumentHelper pages(pdf);
    for (const char *color : {"1 0 0", "0 0 1"}) {
        auto page = QPDFObjectHandle::newDictionary();
        page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
        page.replaceKey("/MediaBox", QPDFObjectHandle::parse("[0 0 200 300]"));
        page.replaceKey("/Resources", QPDFObjectHandle::newDictionary());
        page.replaceKey("/Contents", pdf.newStream(std::string(color) + " rg 0 0 200 300 re f"));
        pages.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);
    }
    const QByteArray filename = path.toUtf8();
    QPDFWriter writer(pdf, filename.constData());
    writer.write();
    mervin::RenderEngine engine;
    auto document = engine.openDocument(path);
    QVERIFY(document);
    ExtractDialog::Source input = source(path);
    input.mode = ExtractPlan::Mode::Delete;
    input.seedPage.reset();
    input.viewerPageCount = 2;
    input.doc = document.get();
    input.engine = &engine;
    ExtractDialog dialog(input);
    prepare(dialog);
    ExtractStrip *thumbnails = strip(dialog);
    QCOMPARE(thumbnails->model()->rowCount(), 2);
    const auto centerColor = [thumbnails](int row) {
        const QRect cell = thumbnails->visualRect(thumbnails->model()->index(row, 0));
        const QImage image = thumbnails->viewport()->grab().toImage();
        const qreal dpr = image.devicePixelRatio();
        return image.pixelColor(QPoint(qRound(cell.center().x() * dpr),
                                       qRound(cell.center().y() * dpr)));
    };
    QTRY_VERIFY(centerColor(0).red() > 240 && centerColor(0).blue() < 15);
    QTRY_VERIFY(centerColor(1).blue() > 240 && centerColor(1).red() < 15);
    const QColor original = centerColor(0);
    const QColor neighbor = centerColor(1);
    clickCell(dialog, 0);
    QTRY_COMPARE(dialog.choices().specs, QStringList{QStringLiteral("1")});
    const QColor faded = centerColor(0);
    QVERIFY(faded != original);
    QVERIFY(faded.red() > faded.blue());
    QCOMPARE(centerColor(1), neighbor);
    QCOMPARE(tip(dialog, 0), QStringLiteral("Page 1 will be deleted. Click to keep it."));
    clickCell(dialog, 0);
    QTRY_VERIFY(dialog.choices().specs.isEmpty());
    QCOMPARE(centerColor(0), original);
    QCOMPARE(centerColor(1), neighbor);
}

QTEST_MAIN(TstExtractDialog)
#include "tst_extract_dialog.moc"
