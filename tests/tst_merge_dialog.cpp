#include "dialogs/MergeDialog.h"
#include "security/QpdfService.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFocusEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <memory>

using mervin::MergeDialog;
using mervin::QpdfService;

// Widget-level guard rails for the merge dialog. MergePlan (tst_merge_plan)
// covers the arithmetic; what is left here is the wiring that only exists in the
// widget layer and that a hand test is the usual - and unreliable - way to check:
// whether the seeded row is really probed, whether the current row follows the
// editor the user is typing in, and whether the output field can be emptied.
//
// The dialog is flagged WA_DontShowOnScreen and never exec()d, so it never
// appears and nothing blocks - the same arrangement tst_viewer_preview uses.
// exec() here would hang the suite forever. The one window that does appear is
// the "Replace it?" box, which answerReplaceWithYes() closes from its own loop.
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
    const QByteArray outputName = path.toUtf8();
    QPDFWriter w(q, outputName.constData());
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
    void seededEncryptedDocumentOpensWithTheTabPassword();
    void focusingARowEditorMakesThatRowCurrent();
    void outputFieldCanBeEmptied();
    void badRangeBlocksTheMerge();
    void outputOpenInATabIsRefused();
    void failedWriteKeepsThePlan();

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
    static QString error(MergeDialog &d)
    {
        return d.findChild<QLabel *>(QStringLiteral("mergeError"))->text();
    }
    static void replaceText(QLineEdit *field, const QString &text)
    {
        field->setFocus();
        field->selectAll();
        QTest::keyClicks(field, text);
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
    QCOMPARE(specs(d).at(0)->text(), QStringLiteral("All")); // the keyword, every page
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
    // open without a password that qpdf accepts - none passed, or a stale one
    // after the file changed on disk - so qpdf meets the file locked.
    for (const QString &password : {QString(), QStringLiteral("stale")}) {
        MergeDialog d(encrypted_, 6, password); // 6 = what the viewer would report
        prepare(d);
        QCOMPARE(list(d)->count(), 1);
        QVERIFY2(!mergeBtn(d)->isEnabled(), "an encrypted seeded row must block the merge");
        // The row says why, and says it before anything is written.
        QVERIFY(specs(d).size() == 1 && !specs(d).at(0)->isEnabled());
    }
}

void TstMergeDialog::seededEncryptedDocumentOpensWithTheTabPassword()
{
    // The tab remembers the password it was opened with; the seeded row uses it.
    MergeDialog d(encrypted_, 6, QStringLiteral("secret"));
    prepare(d);
    QCOMPARE(list(d)->count(), 1);
    QVERIFY(specs(d).size() == 1 && specs(d).at(0)->isEnabled());
    QVERIFY(mergeBtn(d)->isEnabled());
    QCOMPARE(d.inputs().size(), 1);
    QCOMPARE(d.inputs().at(0).pages.size(), 6);
    QCOMPARE(d.inputs().at(0).password, QStringLiteral("secret"));
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

void TstMergeDialog::outputOpenInATabIsRefused()
{
    // The viewer holds an open tab's file open, so writing over it would fail. The
    // error line says so before Merge is pressed, and Merge stays disabled.
    const QString opened = dir_.filePath(QStringLiteral("opened-in-tab.pdf"));
    makePdf(opened, 1);
    MergeDialog d(plain_, 6);
    prepare(d);
    d.setOpenFiles({plain_, opened});
    const QString inTab = QStringLiteral("That file is open in a tab. Choose another name.");
    replaceText(output(d), QDir::toNativeSeparators(opened));
    QCOMPARE(error(d), inTab);
    QVERIFY(!mergeBtn(d)->isEnabled());
    // Without ".pdf" it is the same file, once the merge appends it.
    replaceText(output(d), QDir::toNativeSeparators(dir_.filePath(QStringLiteral("opened-in-tab"))));
    QCOMPARE(error(d), inTab);

    replaceText(output(d), QDir::toNativeSeparators(dir_.filePath(QStringLiteral("fresh.pdf"))));
    QVERIFY(error(d).isEmpty());
    QVERIFY(mergeBtn(d)->isEnabled());
}

void TstMergeDialog::failedWriteKeepsThePlan()
{
    // A write that fails (a read-only file here, standing in for one that another
    // program holds open) is reported on the error line. The dialog stays open
    // with its plan, Merge stays enabled, and once the cause is gone a second
    // Merge succeeds without retyping anything.
    const QString target = dir_.filePath(QStringLiteral("read-only.pdf"));
    makePdf(target, 1);
    QVERIFY(QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::ReadUser));
    MergeDialog d(plain_, 6);
    prepare(d);
    replaceText(specs(d).at(0), QStringLiteral("2-3"));
    replaceText(output(d), QDir::toNativeSeparators(target));
    QVERIFY(mergeBtn(d)->isEnabled());

    answerReplaceWithYes(); // the file exists, so Merge asks first
    mergeBtn(d)->click();
    QVERIFY(d.result() != QDialog::Accepted);                 // still open...
    QCOMPARE(specs(d).at(0)->text(), QStringLiteral("2-3")); // ...with its plan
    QVERIFY2(error(d).startsWith(QStringLiteral("Could not write \"read-only.pdf\".")),
             qPrintable(error(d)));
    QVERIFY(!d.findChild<QLabel *>(QStringLiteral("mergeError"))->toolTip().isEmpty());
    QVERIFY(mergeBtn(d)->isEnabled());

    QVERIFY(QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                              | QFileDevice::ReadUser | QFileDevice::WriteUser));
    answerReplaceWithYes();
    mergeBtn(d)->click();
    QCOMPARE(d.result(), int(QDialog::Accepted));
    QCOMPARE(QFileInfo(d.outputPath()), QFileInfo(target));
    int count = 0;
    QString err;
    QCOMPARE(mervin::PageOps::probe(target, &count, QString(), &err), mervin::PageOps::Status::Ok);
    QCOMPARE(count, 2);
}

QTEST_MAIN(TstMergeDialog)
#include "tst_merge_dialog.moc"
