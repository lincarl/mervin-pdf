#include "ui/OpenPdfDialog.h"

#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

using namespace mervin;

class TstOpenPdfDialog : public QObject
{
    Q_OBJECT

private slots:
    void acceptsTypedUrlImmediately();
};

void TstOpenPdfDialog::acceptsTypedUrlImmediately()
{
    OpenPdfDialog dialog;
#ifdef Q_OS_WIN
    // The Windows implementation is the real COM picker. Its URL-entry path is
    // exercised by the UI integration test; keep this target as a compile/link
    // guard that also prevents an accidental return to Qt's custom picker.
    QVERIFY(dialog.usesNativeDialog());
#else
    QVERIFY(dialog.testOption(QFileDialog::DontUseNativeDialog));
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    auto *nameEdit = dialog.findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
    QVERIFY(nameEdit);
    const QString url = QStringLiteral(
        "http://www.analog.com/static/imported-files/data_sheets/AD5628_5648_5668.pdf");
    nameEdit->setFocus();
    QTest::keyClicks(nameEdit, url);

    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    QVERIFY(buttons);
    auto *open = buttons->button(QDialogButtonBox::Open);
    QVERIFY(open);
    QVERIFY(open->isEnabled());

    QSignalSpy accepted(&dialog, &QDialog::accepted);
    QElapsedTimer elapsed;
    elapsed.start();
    QTest::mouseClick(open, Qt::LeftButton);
    QCOMPARE(accepted.count(), 1);
    QVERIFY(elapsed.elapsed() < 1000);
    QCOMPARE(dialog.internetUrl(), url);
#endif
}

QTEST_MAIN(TstOpenPdfDialog)
#include "tst_open_pdf_dialog.moc"
