#include "ui/OpenPdfDialog.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>
#include <QTemporaryDir>
#include <QTranslator>

using namespace mervin;

class TstOpenPdfDialog : public QObject
{
    Q_OBJECT

private slots:
    void acceptsTypedUrlImmediately_data();
    void acceptsTypedUrlImmediately();
    void localFilesAndCancel_data();
    void localFilesAndCancel();
    void unsupportedSchemeIsNotAcceptedAsAnInternetUrl();
    void textComesFromItsOwnContext();
};

void TstOpenPdfDialog::acceptsTypedUrlImmediately_data()
{
    QTest::addColumn<QString>("url");
    QTest::newRow("http") << QStringLiteral("http://example.invalid/document.pdf");
    QTest::newRow("https") << QStringLiteral("https://example.invalid/document.pdf?revision=2");
}

void TstOpenPdfDialog::acceptsTypedUrlImmediately()
{
    QFETCH(QString, url);
    OpenPdfDialog dialog;
#ifdef Q_OS_WIN
    // This branch checks native-picker selection only. Native COM interaction
    // still needs a Windows client and is not established by this assertion.
    QVERIFY(dialog.usesNativeDialog());
#else
    QVERIFY(dialog.testOption(QFileDialog::DontUseNativeDialog));
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    auto *nameEdit = dialog.findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
    QVERIFY(nameEdit);
    nameEdit->setFocus();
    QTest::keyClicks(nameEdit, url);

    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    QVERIFY(buttons);
    auto *open = buttons->button(QDialogButtonBox::Open);
    QVERIFY(open);
    QVERIFY(open->isEnabled());

    QSignalSpy accepted(&dialog, &QDialog::accepted);
    QTest::mouseClick(open, Qt::LeftButton);
    QCOMPARE(accepted.count(), 1);
    QVERIFY(!dialog.isVisible());
    QCOMPARE(dialog.internetUrl(), url);
#endif
}

void TstOpenPdfDialog::localFilesAndCancel_data()
{
    QTest::addColumn<bool>("multiple");
    QTest::addColumn<bool>("cancel");
    QTest::newRow("one file") << false << false;
    QTest::newRow("multiple files") << true << false;
    QTest::newRow("cancel URL") << false << true;
}

void TstOpenPdfDialog::localFilesAndCancel()
{
#ifndef Q_OS_WIN
    QFETCH(bool, multiple);
    QFETCH(bool, cancel);
    QTemporaryDir files;
    QVERIFY(files.isValid());
    for (const QString &name : {QStringLiteral("first.pdf"), QStringLiteral("second.pdf")}) {
        QFile file(files.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("%PDF-1.7\n");
    }
    OpenPdfDialog dialog;
    dialog.setDirectory(files.path());
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *field = dialog.findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    QVERIFY(field && buttons);
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    QSignalSpy rejected(&dialog, &QDialog::rejected);
    QTest::keyClicks(field, cancel ? QStringLiteral("https://example.invalid/canceled.pdf")
                                  : multiple ? QStringLiteral("\"first.pdf\" \"second.pdf\"")
                                             : QStringLiteral("first.pdf"));
    QTest::mouseClick(buttons->button(cancel ? QDialogButtonBox::Cancel : QDialogButtonBox::Open),
                      Qt::LeftButton);
    QCOMPARE(accepted.count(), cancel ? 0 : 1);
    QCOMPARE(rejected.count(), cancel ? 1 : 0);
    QVERIFY(!dialog.isVisible());
    QVERIFY(dialog.internetUrl().isEmpty());
    if (!cancel) {
        QStringList expected{files.filePath(QStringLiteral("first.pdf"))};
        if (multiple)
            expected.append(files.filePath(QStringLiteral("second.pdf")));
        QCOMPARE(dialog.selectedFiles(), expected);
    }
#endif
}

void TstOpenPdfDialog::unsupportedSchemeIsNotAcceptedAsAnInternetUrl()
{
#ifndef Q_OS_WIN
    QTemporaryDir files;
    QVERIFY(files.isValid());
    OpenPdfDialog dialog;
    dialog.setDirectory(files.path());
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *field = dialog.findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
    QVERIFY(field);
    QTest::keyClicks(field, QStringLiteral("ftp://example.invalid/document.pdf"));
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    QVERIFY(buttons);
    QVERIFY(!buttons->button(QDialogButtonBox::Open)->isEnabled());
    QTest::mouseClick(buttons->button(QDialogButtonBox::Open), Qt::LeftButton);
    QVERIFY(accepted.isEmpty());
    QVERIFY(dialog.internetUrl().isEmpty());
    QVERIFY(dialog.isVisible());
    dialog.reject();
#endif
}

// The picker looks its text up in "mervin::OpenPdfDialog", the context lupdate
// files it under, rather than in QFileDialog's, where no translation is found.
void TstOpenPdfDialog::textComesFromItsOwnContext()
{
    class Catalog : public QTranslator
    {
    public:
        QString translate(const char *context, const char *source, const char *, int) const override
        {
            return qstrcmp(context, "mervin::OpenPdfDialog") == 0 && qstrcmp(source, "Open PDF") == 0
                ? QStringLiteral("Öppna PDF")
                : QString();
        }
        bool isEmpty() const override { return false; }
    } catalog;
    QCoreApplication::installTranslator(&catalog);
    const auto removeCatalog = qScopeGuard([&] { QCoreApplication::removeTranslator(&catalog); });
    QCOMPARE(OpenPdfDialog::tr("Open PDF"), QStringLiteral("Öppna PDF"));
#ifndef Q_OS_WIN
    // The native Windows picker shows its title outside Qt's widgets.
    OpenPdfDialog dialog;
    QCOMPARE(dialog.windowTitle(), QStringLiteral("Öppna PDF"));
#endif
}

QTEST_MAIN(TstOpenPdfDialog)
#include "tst_open_pdf_dialog.moc"
