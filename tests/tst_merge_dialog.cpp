#include "dialogs/MergeDialog.h"
#include "security/QpdfService.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <QApplication>
#include <QFocusEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>

using mervin::MergeDialog;
using mervin::QpdfService;

// Widget-level guard rails for the merge dialog. MergePlan (tst_merge_plan)
// covers the arithmetic; what is left here is the wiring that only exists in the
// widget layer and that a hand test is the usual - and unreliable - way to check:
// whether the seeded row is really probed, whether the current row follows the
// editor the user is typing in, and whether the output field can be emptied.
//
// The dialog is flagged WA_DontShowOnScreen and never exec()d, so nothing appears
// and nothing blocks - the same arrangement tst_viewer_preview uses. exec() here
// would hang the suite forever.
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

class TstMergeDialog : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void seededPlainDocumentIsReadyToMerge();
    void seededEncryptedDocumentIsBlockedUpFront();
    void focusingARowEditorMakesThatRowCurrent();
    void outputFieldCanBeEmptied();
    void badRangeBlocksTheMerge();

private:
    QTemporaryDir dir_;
    QString plain_;
    QString encrypted_;

    static void prepare(MergeDialog &d)
    {
        d.setAttribute(Qt::WA_DontShowOnScreen, true);
        d.show(); // realises the layout without ever putting a window on screen
        QTest::qWait(1);
    }
    static QListWidget *list(MergeDialog &d)
    {
        return d.findChild<QListWidget *>(QStringLiteral("mergeList"));
    }
    static QLineEdit *output(MergeDialog &d)
    {
        return d.findChild<QLineEdit *>(QStringLiteral("mergeOutput"));
    }
    static QPushButton *mergeBtn(MergeDialog &d)
    {
        return d.findChild<QPushButton *>(QStringLiteral("mergeAccept"));
    }
    // The page-range editors, in row order.
    static QList<QLineEdit *> specs(MergeDialog &d)
    {
        QList<QLineEdit *> out;
        QListWidget *l = list(d);
        for (int i = 0; i < l->count(); ++i)
            if (QWidget *row = l->itemWidget(l->item(i)))
                if (auto *e = row->findChild<QLineEdit *>(QStringLiteral("mergeRowSpec")))
                    out << e;
        return out;
    }
};

void TstMergeDialog::initTestCase()
{
    QVERIFY(dir_.isValid());
    plain_ = dir_.filePath(QStringLiteral("plain.pdf"));
    encrypted_ = dir_.filePath(QStringLiteral("locked.pdf"));
    makePdf(plain_, 6);

    QpdfService svc;
    QCOMPARE(svc.encrypt(plain_, encrypted_, QString(), QStringLiteral("secret"), QString(),
                         QpdfService::Algorithm::AES256, {}, nullptr),
             QpdfService::Status::Ok);
}

void TstMergeDialog::seededPlainDocumentIsReadyToMerge()
{
    MergeDialog d(plain_, 6);
    prepare(d);
    QCOMPARE(list(d)->count(), 1);
    QCOMPARE(d.inputs().size(), 1);
    QCOMPARE(d.inputs().at(0).pages.size(), 6);
    QVERIFY(!output(d)->text().isEmpty()); // a destination is proposed
    QVERIFY(mergeBtn(d)->isEnabled());
}

void TstMergeDialog::seededEncryptedDocumentIsBlockedUpFront()
{
    // The regression this exists for: the seeded row used to be trusted from the
    // viewer's page count instead of probed, so an encrypted open document showed
    // a healthy row and the merge only died after the user had built the whole
    // plan and chosen an output path. The viewer really can have such a document
    // open - it prompts for the password at open time and then discards it, so
    // qpdf meets the file locked.
    MergeDialog d(encrypted_, 6); // 6 = what the viewer would report
    prepare(d);
    QCOMPARE(list(d)->count(), 1);
    QVERIFY2(!mergeBtn(d)->isEnabled(), "an encrypted seeded row must block the merge");
    // The row says why, and says it before anything is written.
    QVERIFY(specs(d).size() == 1 && !specs(d).at(0)->isEnabled());
}

void TstMergeDialog::focusingARowEditorMakesThatRowCurrent()
{
    // Remove / Duplicate / Move Up / Move Down all act on the list's current row.
    // Clicking into a row's page-range editor does not move the list's selection
    // by itself, so without the focus filter those buttons acted on whatever row
    // happened to be selected - typically row 1, the open document.
    MergeDialog d(plain_, 6);
    prepare(d);
    d.addPaths({plain_, plain_});
    QCOMPARE(list(d)->count(), 3);

    list(d)->setCurrentRow(0);
    QCOMPARE(list(d)->currentRow(), 0);

    // The focus event is posted directly rather than via setFocus(): this dialog
    // is WA_DontShowOnScreen, so its window is never active, and QWidget::setFocus
    // on an inactive window only records the focus widget - Qt withholds FocusIn
    // until the window is activated, which here never happens. A real click or Tab
    // in a live dialog delivers exactly the event sent below.
    auto focus = [](QWidget *w, Qt::FocusReason r) {
        QFocusEvent ev(QEvent::FocusIn, r);
        QApplication::sendEvent(w, &ev);
    };

    focus(specs(d).at(2), Qt::MouseFocusReason);
    QCOMPARE(list(d)->currentRow(), 2);

    focus(specs(d).at(1), Qt::TabFocusReason);
    QCOMPARE(list(d)->currentRow(), 1);
}

void TstMergeDialog::outputFieldCanBeEmptied()
{
    // Clearing the field used to refill it instantly from the derived default, so
    // select-all-delete-then-type appended onto the old path.
    MergeDialog d(plain_, 6);
    prepare(d);
    QLineEdit *out = output(d);
    QVERIFY(!out->text().isEmpty());

    out->setFocus();
    out->selectAll();
    QTest::keyClick(out, Qt::Key_Delete);
    QVERIFY2(out->text().isEmpty(), qPrintable(QStringLiteral("field snapped back to \"%1\"")
                                                   .arg(out->text())));
    QVERIFY2(!mergeBtn(d)->isEnabled(), "no destination means no merge");

    QTest::keyClicks(out, QStringLiteral("combined.pdf"));
    QCOMPARE(out->text(), QStringLiteral("combined.pdf"));
    QVERIFY(mergeBtn(d)->isEnabled());
}

void TstMergeDialog::badRangeBlocksTheMerge()
{
    MergeDialog d(plain_, 6);
    prepare(d);
    QVERIFY(mergeBtn(d)->isEnabled());

    QLineEdit *spec = specs(d).at(0);
    spec->setText(QStringLiteral("40")); // the file has 6 pages
    QVERIFY(!mergeBtn(d)->isEnabled());

    spec->setText(QStringLiteral("2-3"));
    QVERIFY(mergeBtn(d)->isEnabled());
    QCOMPARE(d.inputs().at(0).pages, QList<int>({1, 2}));
}

// NOTE for anyone tempted to add a drag-and-drop case here: synthesised drag
// events cannot be delivered. QApplication routes DragEnter/DragMove/Drop
// through the drag manager, not through normal event dispatch, so
// QApplication::sendEvent(list->viewport(), &dragMoveEvent) is silently dropped -
// measured on Qt 6.8.3, with an event-filter probe confirming 92 other events
// arrive at that same viewport and not one drag event does. That is why the
// gap-to-index arithmetic lives in MergePlan::moveToGap and is covered by
// tst_merge_plan::dropGapsMapToTheRightRow instead; what remains here in the
// widget (mime payload, hit-testing a gap, painting the marker) is verified by
// hand in the running app.

QTEST_MAIN(TstMergeDialog)
#include "tst_merge_dialog.moc"
