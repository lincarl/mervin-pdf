#include "dialogs/ExtractDialog.h"
#include "dialogs/ExtractStrip.h"
#include "security/PageOps.h"
#include "security/QpdfService.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTest>

using mervin::ExtractDialog;
using mervin::ExtractPlan;
using mervin::ExtractStrip;
using mervin::PageOps;
using mervin::QpdfService;

// Widget-level guard rails for the extract dialog. ExtractPlan (tst_extract_plan)
// covers the arithmetic; what is left here is the wiring that only exists in the
// widget layer: that typing and pasting separators makes rows, that moves and
// strip edits reach the plan and the job, that the strip keeps the current row
// in view without undoing the user's scrolling, that a disabled Extract always
// shows its reason, and that an encrypted file is unlocked by the tab's password
// or, failing that, inline before close.
//
// As in tst_merge_dialog, the dialog is flagged WA_DontShowOnScreen and never
// exec()d, so nothing appears and nothing blocks. Source::doc and ::engine stay
// null: thumbnails are out of scope, and the strip must cope without them.
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
        dh.addPage(QPDFPageObjectHelper(q.makeIndirectObject(page)), false);
    }
    QPDFWriter w(q, path.toUtf8().constData());
    w.write();
}

} // namespace

class TstExtractDialog : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void opensWithOneRowOnTheCurrentPage();
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
    void destinationCannotBeTheSource();
    void encryptedSourceAsksOnceInline();
    void encryptedSourceUsesTheTabPassword();
    void staleTabPasswordFallsBackToTheRow();

private:
    QTemporaryDir dir_;
    QString plain_;
    QString encrypted_;

    ExtractDialog::Source source(const QString &path) const
    {
        ExtractDialog::Source s;
        s.path = path;
        s.viewerPageCount = 31;
        s.currentPage = 6;
        return s;
    }
    static void prepare(ExtractDialog &d)
    {
        d.setAttribute(Qt::WA_DontShowOnScreen, true);
        d.show(); // realises the layout without ever putting a window on screen
        QTest::qWait(1);
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
    // Replace a field's text the way a user does: select all, type.
    static void type(QLineEdit *field, const QString &text)
    {
        field->selectAll();
        if (text.isEmpty())
            QTest::keyClick(field, Qt::Key_Delete);
        else
            QTest::keyClicks(field, text);
    }
    // What the dialog would write: Extract, then its job's pages.
    static QList<int> accepted(ExtractDialog &d)
    {
        extract(d)->click();
        return d.result() == QDialog::Accepted ? d.job().pages : QList<int>{-1};
    }
};

void TstExtractDialog::initTestCase()
{
    QVERIFY(dir_.isValid());
    plain_ = dir_.filePath(QStringLiteral("annual-report.pdf"));
    encrypted_ = dir_.filePath(QStringLiteral("annual-report-locked.pdf"));
    makePdf(plain_, 31);

    QpdfService svc;
    QCOMPARE(svc.encrypt(plain_, encrypted_, QString(), QStringLiteral("secret"), QString(),
                         QpdfService::Algorithm::AES256, {}, nullptr),
             QpdfService::Status::Ok);
}

void TstExtractDialog::opensWithOneRowOnTheCurrentPage()
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
    QVERIFY(extract(d)->isEnabled());

    // Added below the current row, not at the end.
    list(d)->setCurrentRow(0);
    add->click();
    QCOMPARE(texts(d), QStringList({QStringLiteral("7"), QString(), QStringLiteral("9")}));
    QVERIFY(!extract(d)->isEnabled());
    button(d, QStringLiteral("&Remove"))->click();
    QCOMPARE(texts(d), QStringList({QStringLiteral("7"), QStringLiteral("9")}));
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

void TstExtractDialog::encryptedSourceAsksOnceInline()
{
    ExtractDialog d(source(encrypted_));
    prepare(d);
    QLineEdit *password = child<QLineEdit>(d, "extractPassword");
    QVERIFY(password);
    QCOMPARE(error(d), QStringLiteral("Enter the password for this PDF."));
    QVERIFY(!extract(d)->isEnabled());

    QTest::keyClicks(password, QStringLiteral("nope"));
    QVERIFY(extract(d)->isEnabled());
    extract(d)->click();
    QVERIFY(d.result() != QDialog::Accepted); // stays open
    QCOMPARE(error(d), QStringLiteral("That password is not correct."));
    QCOMPARE(password->selectedText(), QStringLiteral("nope")); // ready to retype

    QTest::keyClicks(password, QStringLiteral("secret"));
    QVERIFY(error(d).isEmpty()); // editing clears the verdict
    extract(d)->click();
    QCOMPARE(d.result(), int(QDialog::Accepted));
    QCOMPARE(d.password(), QStringLiteral("secret"));
    QCOMPARE(d.job().pages, QList<int>({6}));
}

void TstExtractDialog::encryptedSourceUsesTheTabPassword()
{
    // The password that opened the tab unlocks the file: the dialog is the plain one.
    ExtractDialog::Source s = source(encrypted_);
    s.password = QStringLiteral("secret");
    ExtractDialog d(s);
    prepare(d);
    QVERIFY(!child<QLineEdit>(d, "extractPassword"));
    QVERIFY(error(d).isEmpty());
    QVERIFY(extract(d)->isEnabled());

    extract(d)->click();
    QCOMPARE(d.result(), int(QDialog::Accepted));
    QCOMPARE(d.password(), QStringLiteral("secret"));

    // What MainWindow does with the result: the job really writes with it.
    const ExtractPlan::Job job = d.job();
    QString err;
    QCOMPARE(PageOps::merge({PageOps::MergeInput{encrypted_, job.pages, d.password()}}, job.path,
                            &err),
             PageOps::Status::Ok);
    int count = 0;
    QCOMPARE(PageOps::probe(job.path, &count, QString(), &err), PageOps::Status::Ok);
    QCOMPARE(count, 1);
}

void TstExtractDialog::staleTabPasswordFallsBackToTheRow()
{
    // A remembered password that no longer opens the file (it changed on disk)
    // must not pass for a verified one: the row appears as if none were known.
    ExtractDialog::Source s = source(encrypted_);
    s.password = QStringLiteral("stale");
    ExtractDialog d(s);
    prepare(d);
    QVERIFY(child<QLineEdit>(d, "extractPassword"));
    QCOMPARE(error(d), QStringLiteral("Enter the password for this PDF."));
    QVERIFY(!extract(d)->isEnabled());
    QVERIFY(d.password().isEmpty());
}

QTEST_MAIN(TstExtractDialog)
#include "tst_extract_dialog.moc"
