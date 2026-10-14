#include "dialogs/MergeDialog.h"
#include "io/SaveCheck.h"
#include "io/JobContext.h"
#include "io/Background.h"
#include "Stalls.h"
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
#include <QFileDialog>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <memory>

#ifdef Q_OS_UNIX
#  include <sys/stat.h>
#endif

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
    void cleanup();
    void slowSaveAsCheckSaysSoAndMergeWaitsForIt();
    void pressedBeforeTheCheckKeepsAnOpenFile();

    void seededPlainDocumentIsReadyToMerge();
    void seededEncryptedDocumentIsBlockedUpFront();
    void seededEncryptedDocumentOpensWithTheTabPassword();
    void focusingARowEditorMakesThatRowCurrent();
    void outputFieldCanBeEmptied();
    void badRangeBlocksTheMerge();
    void outputOpenInATabIsRefused();
    void failedPlanCanBeReopened();
    void volumesProbeIndependently();
    void pendingRowsKeepTheirProbeIdentity();
    void batchWarningWaitsAndIgnoresRemovedRows_data();
    void batchWarningWaitsAndIgnoresRemovedRows();
    void ownerDeletionDuringQuestion_data();
    void ownerDeletionDuringQuestion();
#ifdef Q_OS_UNIX
    void fifoRowsReturnWithoutWaiting_data();
    void fifoRowsReturnWithoutWaiting();
#endif

private:
    QTemporaryDir dir_;
    QString plain_;
    QString encrypted_;

    static void prepare(MergeDialog &d)
    {
        d.setAttribute(Qt::WA_DontShowOnScreen, true);
        d.show(); // realises the layout without ever putting a window on screen
        QTest::qWait(1);
        settle(&d);
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
    // Lets the Save as check answer: it runs on a background thread, and the error
    // line and Merge wait for it.
    static void settle(QWidget *dialog)
    {
        const auto *check = dialog->findChild<mervin::SaveCheck *>();
        QVERIFY(check);
        const auto *merge = qobject_cast<MergeDialog *>(dialog);
        QVERIFY(QTest::qWaitFor([check, merge] {
            return !check->pending() && (!merge || !merge->reading());
        }, 10000));
    }
    static void replaceText(QLineEdit *field, const QString &text)
    {
        field->setFocus();
        field->selectAll();
        QTest::keyClicks(field, text);
        settle(field->window());
    }
    // Merge checks Save as again on a background thread, then accepts the plan.
    static void press(MergeDialog &d)
    {
        mergeBtn(d)->click();
        settle(&d);
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
    settle(&d);
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
    settle(&d);
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
    // As the tabs hold them.
    d.setOpenFiles({QFileInfo(plain_).canonicalFilePath(), QFileInfo(opened).canonicalFilePath()});
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

// Save as is checked on a background thread, as its folder can be on a share that
// stopped answering. A slow check says so, and Merge pressed meanwhile writes once
// it answers.
void TstMergeDialog::slowSaveAsCheckSaysSoAndMergeWaitsForIt()
{
    const QString target = dir_.filePath(QStringLiteral("merged-slowly.pdf"));
    MergeDialog d(plain_, 6);
    prepare(d);
    settle(&d);
    auto release = qScopeGuard([] { mervin::SaveCheck::holdForTesting(false); });
    mervin::SaveCheck::holdForTesting(true);
    output(d)->setFocus();
    output(d)->selectAll();
    QTest::keyClicks(output(d), QDir::toNativeSeparators(target));
    QVERIFY(mergeBtn(d)->isEnabled());
    QTRY_COMPARE(error(d), QStringLiteral("Checking the folder…"));
    QVERIFY(!mergeBtn(d)->isEnabled());
    mervin::SaveCheck::holdForTesting(false);
    settle(&d);
    QVERIFY(error(d).isEmpty());
    QVERIFY(mergeBtn(d)->isEnabled());

    mervin::SaveCheck::holdForTesting(true);
    mergeBtn(d)->click();
    QTRY_COMPARE(error(d), QStringLiteral("Checking the folder…"));
    QVERIFY(d.result() != QDialog::Accepted);
    QVERIFY(!QFileInfo::exists(target));
    mervin::SaveCheck::holdForTesting(false);
    settle(&d);
    QCOMPARE(d.result(), int(QDialog::Accepted));
    QVERIFY(!QFileInfo::exists(target));
    QCOMPARE(d.outputPath(), target);
    QCOMPARE(d.inputs().front().pages.size(), 6);
}

// Merge pressed before the check of a new name answers writes nothing when the
// answer is a reason not to, such as the file being open in a tab.
void TstMergeDialog::pressedBeforeTheCheckKeepsAnOpenFile()
{
    const QString opened = dir_.filePath(QStringLiteral("open-when-pressed.pdf"));
    makePdf(opened, 1);
    const QByteArray before = [&opened] {
        QFile f(opened);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }();
    MergeDialog d(plain_, 6);
    prepare(d);
    d.setOpenFiles({QFileInfo(plain_).canonicalFilePath(), QFileInfo(opened).canonicalFilePath()});
    auto release = qScopeGuard([] { mervin::SaveCheck::holdForTesting(false); });
    mervin::SaveCheck::holdForTesting(true);
    output(d)->setFocus();
    output(d)->selectAll();
    QTest::keyClicks(output(d), QDir::toNativeSeparators(opened));
    QVERIFY(mergeBtn(d)->isEnabled()); // no answer yet
    mergeBtn(d)->click();
    mervin::SaveCheck::holdForTesting(false);
    settle(&d);
    QVERIFY(d.result() != QDialog::Accepted);
    QCOMPARE(error(d), QStringLiteral("That file is open in a tab. Choose another name."));
    QFile f(opened);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), before);
}

void TstMergeDialog::failedPlanCanBeReopened()
{
    const QString target = dir_.filePath(QStringLiteral("retry.pdf"));
    MergeDialog d(plain_, 6);
    prepare(d);
    replaceText(specs(d).at(0), QStringLiteral("2-3"));
    replaceText(output(d), QDir::toNativeSeparators(target));
    press(d);
    QCOMPARE(d.result(), int(QDialog::Accepted));
    QVERIFY(!QFileInfo::exists(target));
    MergeDialog retry(d.plan(), target, QStringLiteral("Injected input failure"), 0);
    prepare(retry);
    QCOMPARE(specs(retry).at(0)->text(), QStringLiteral("2-3"));
    QCOMPARE(output(retry)->text(), QDir::toNativeSeparators(target));
    QVERIFY(error(retry).contains(QFileInfo(plain_).fileName()));
    QCOMPARE(retry.findChild<QLabel *>(QStringLiteral("mergeError"))->toolTip(),
             QStringLiteral("Injected input failure"));
    QVERIFY(mergeBtn(retry)->isEnabled());
    press(retry);
    QCOMPARE(retry.result(), int(QDialog::Accepted));
    QCOMPARE(retry.inputs().front().pages, (QList<int>{1, 2}));
    QVERIFY(!QFileInfo::exists(target));
}

void TstMergeDialog::cleanup()
{
    mervin::job::testing::release();
    mervin::SaveCheck::holdForTesting(false);
    QVERIFY(mervin::background::settle(10000));
}

void TstMergeDialog::volumesProbeIndependently()
{
    QTemporaryDir files;
    QVERIFY(QDir(files.path()).mkdir("held"));
    QVERIFY(QDir(files.path()).mkdir("healthy"));
    const QString held = files.filePath("held/a.pdf");
    const QString same = files.filePath("held/b.pdf");
    const QString healthy = files.filePath("healthy/c.pdf");
    for (const QString &path : {held, same, healthy})
        QVERIFY(QFile::copy(plain_, path));
    mervin::job::testing::holdWhen([held](mervin::job::Step step, const QString &path) {
        return step == mervin::job::Step::Reading && path == held;
    });
    const auto release = qScopeGuard([] { mervin::job::testing::release(); });
    MergeDialog d(QString(), 0);
    d.show();
    const int guiSteps = mervin::job::testing::stepsOnMainThread();
    QElapsedTimer elapsed;
    elapsed.start();
    d.addPaths({held, same, healthy});
    QVERIFY(elapsed.elapsed() < 1000);
    QTRY_COMPARE(mervin::job::testing::held(), 1);
    const auto counts = d.findChildren<QLabel *>("rowListCount");
    QCOMPARE(counts.size(), 3);
    QTRY_COMPARE(counts.at(2)->text(), QStringLiteral("6"));
    QCOMPARE(counts.at(0)->text(), QStringLiteral("Reading…"));
    QCOMPARE(counts.at(1)->text(), QStringLiteral("Reading…"));
    QVERIFY(!mergeBtn(d)->isEnabled());
    QVERIFY(error(d).isEmpty());
    QCOMPARE(mervin::job::testing::stepsOnMainThread(), guiSteps);
    mervin::job::testing::release();
    settle(&d);
    QVERIFY(mergeBtn(d)->isEnabled());
}

void TstMergeDialog::pendingRowsKeepTheirProbeIdentity()
{
    mervin::job::testing::holdWhen([this](mervin::job::Step step, const QString &path) {
        return step == mervin::job::Step::Reading && path == plain_;
    });
    const auto release = qScopeGuard([] { mervin::job::testing::release(); });
    MergeDialog d(plain_, 6);
    d.show();
    QTRY_COMPARE(mervin::job::testing::held(), 1);
    list(d)->setCurrentRow(0);
    auto *duplicate = [&] {
        for (auto *button : d.findChildren<QPushButton *>())
            if (button->text() == QStringLiteral("Duplicate"))
                return button;
        return static_cast<QPushButton *>(nullptr);
    }();
    QVERIFY(duplicate);
    duplicate->click();
    QCOMPARE(list(d)->count(), 2);
    const QPointer<QLineEdit> pending(specs(d).front());
    mervin::job::testing::release();
    settle(&d);
    QVERIFY(pending); // completion updated rows without rebuilding their widgets
    QCOMPARE(d.inputs().size(), 2);
    QCOMPARE(d.inputs().at(0).pages.size(), 6);
    QCOMPARE(d.inputs().at(1).pages.size(), 6);
}

void TstMergeDialog::batchWarningWaitsAndIgnoresRemovedRows_data()
{
    QTest::addColumn<bool>("remove");
    QTest::addColumn<bool>("seeded");
    QTest::newRow("one batch warning") << false << false;
    QTest::newRow("removed failure") << true << false;
    QTest::newRow("removed failure after reopening") << true << true;
}

void TstMergeDialog::batchWarningWaitsAndIgnoresRemovedRows()
{
    QFETCH(bool, remove);
    QFETCH(bool, seeded);
    QTemporaryDir files;
    const QString held = files.filePath("held.pdf");
    QVERIFY(QFile::copy(plain_, held));
    const QString missing = dir_.filePath("missing-batch.pdf");
    MergeDialog initial(plain_, 6);
    prepare(initial);
    const auto reopened = seeded ? std::make_unique<MergeDialog>(initial.plan(),
        dir_.filePath("reopened.pdf"), QStringLiteral("Prior failure"), -1) : nullptr;
    MergeDialog &d = reopened ? *reopened : initial;
    if (reopened)
        prepare(d);
    mervin::job::testing::holdWhen([held](mervin::job::Step step, const QString &path) {
        return step == mervin::job::Step::Reading && path == held;
    });
    const auto release = qScopeGuard([] { mervin::job::testing::release(); });
    QStringList warnings;
    QTimer messages;
    connect(&messages, &QTimer::timeout, &d, [&] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            warnings.append(box->text());
            box->accept();
        }
    });
    messages.start(10);
    d.addPaths({missing, held});
    QTRY_COMPARE(mervin::job::testing::held(), 1);
    QTRY_COMPARE(d.plan().at(1).load, mervin::MergePlan::Load::Unreadable);
    QVERIFY(warnings.isEmpty());
    QVERIFY(error(d).isEmpty());
    if (remove) {
        list(d)->setCurrentRow(1);
        for (auto *button : d.findChildren<QPushButton *>())
            if (button->text() == QStringLiteral("Remove"))
                button->click();
        QCOMPARE(list(d)->count(), 2);
    }
    mervin::job::testing::release();
    settle(&d);
    QVERIFY(mervin::background::settle(10000));
    QCoreApplication::processEvents();
    if (remove) {
        QVERIFY(warnings.isEmpty());
        QVERIFY(mergeBtn(d)->isEnabled());
    } else {
        QCOMPARE(warnings.size(), 1);
        QVERIFY(warnings.front().contains(QStringLiteral("missing-batch.pdf")));
        QVERIFY(!mergeBtn(d)->isEnabled());
    }
}

void TstMergeDialog::ownerDeletionDuringQuestion_data()
{
    QTest::addColumn<QString>("action");
    QTest::newRow("add picker") << QStringLiteral("Add Files…");
    QTest::newRow("output picker") << QStringLiteral("Browse…");
    QTest::newRow("overwrite question") << QStringLiteral("overwrite");
    QTest::newRow("input conflict") << QStringLiteral("input");
}

void TstMergeDialog::ownerDeletionDuringQuestion()
{
    QFETCH(QString, action);
    const QPointer<QWidget> owner(new QWidget);
    const auto cleanup = qScopeGuard([owner] { delete owner.data(); });
    const QPointer<MergeDialog> dialog(new MergeDialog(plain_, 6, {}, owner));
    prepare(*dialog);
    QString target;
    if (action == QStringLiteral("overwrite")) {
        target = dir_.filePath("owner-deletion.pdf");
        QVERIFY(QFile::copy(plain_, target));
        replaceText(output(*dialog), target);
    } else if (action == QStringLiteral("input")) {
        replaceText(output(*dialog), plain_);
    }
    QElapsedTimer elapsed;
    elapsed.start();
    bool reached = false;
    bool windowModal = false;
    QTimer deletion;
    connect(&deletion, &QTimer::timeout, this, [&] {
        auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!modal)
            return;
        if (qobject_cast<QFileDialog *>(modal) || qobject_cast<QMessageBox *>(modal)) {
            reached = true;
            windowModal = modal->windowModality() == Qt::WindowModal;
            delete owner.data();
        } else if (elapsed.elapsed() > 3000) {
            modal->reject();
        }
    });
    deletion.start(10);
    if (action == QStringLiteral("overwrite") || action == QStringLiteral("input")) {
        mergeBtn(*dialog)->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog, 5000);
    } else {
        auto *button = [&] {
            for (auto *candidate : dialog->findChildren<QPushButton *>())
                if (candidate->text() == action)
                    return candidate;
            return static_cast<QPushButton *>(nullptr);
        }();
        QVERIFY(button);
        button->click();
    }
    QVERIFY(reached);
    QVERIFY(windowModal);
    QVERIFY(!dialog);
    QVERIFY(!owner);
    if (!target.isEmpty())
        QCOMPARE(QFileInfo(target).size(), QFileInfo(plain_).size());
}

#ifdef Q_OS_UNIX
void TstMergeDialog::fifoRowsReturnWithoutWaiting_data()
{
    QTest::addColumn<bool>("seeded");
    QTest::newRow("constructor") << true;
    QTest::newRow("add files") << false;
}

void TstMergeDialog::fifoRowsReturnWithoutWaiting()
{
    QFETCH(bool, seeded);
    QTemporaryDir files;
    const QString path = files.filePath("slow.pdf");
    QVERIFY(::mkfifo(QFile::encodeName(path).constData(), 0600) == 0);
    mervin::testing::PipeWriter writer(path, 3000);
    auto *warningTimer = new QTimer(this);
    connect(warningTimer, &QTimer::timeout, this, [] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            box->accept();
    });
    warningTimer->start(10);
    const auto stop = qScopeGuard([warningTimer] { delete warningTimer; });
    QElapsedTimer elapsed;
    elapsed.start();
    MergeDialog d(seeded ? path : QString(), 0);
    if (!seeded)
        d.addPaths({path});
    QVERIFY2(elapsed.elapsed() < 1000, qPrintable(QString("Probe took %1 ms").arg(elapsed.elapsed())));
    QVERIFY(d.reading());
    QVERIFY(!mergeBtn(d)->isEnabled());
    QCOMPARE(d.findChild<QLabel *>("rowListCount")->text(), QStringLiteral("Reading…"));
    d.reject();
    writer.release();
    QVERIFY(mervin::background::settle(10000));
}
#endif

QTEST_MAIN(TstMergeDialog)
#include "tst_merge_dialog.moc"
