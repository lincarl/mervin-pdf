#include "security/AtomicPdfWriter.h"
#include "security/PageOps.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <qpdf/QPDF.hh>
#include <stdexcept>

using namespace mervin;

class TstAtomicPdfWriter : public QObject
{
    Q_OBJECT
private slots:
    void interruptedWritePreservesDestination_data()
    {
        QTest::addColumn<bool>("existing");
        QTest::newRow("replace") << true;
        QTest::newRow("new-file") << false;
    }

    void interruptedWritePreservesDestination()
    {
        QFETCH(bool, existing);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("result.pdf"));
        const QByteArray original("existing file must survive");
        if (existing) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(original), original.size());
        }
        bool beganWriting = false;
        try {
            QPDF pdf;
            pdf.processFile(MERVIN_FIXTURE_PDF);
            AtomicPdfWriter output(pdf, path);
            output.options().registerProgressReporter(
                std::make_shared<QPDFWriter::FunctionProgressReporter>([&](int progress) {
                    if (progress > 0) {
                        beganWriting = true;
                        throw std::runtime_error("Simulated interruption");
                    }
                }));
            output.write();
            QFAIL("Write must be interrupted");
        } catch (const std::runtime_error &error) {
            QCOMPARE(QByteArray(error.what()), QByteArray("Simulated interruption"));
        }
        QVERIFY(beganWriting);
        QCOMPARE(QFile::exists(path), existing);
        if (existing) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), original);
        }
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).size(), existing ? 1 : 0);
    }

    void successfulWriteReplacesDestination()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("résultat.pdf"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("previous contents");
        }
        QString error;
        QCOMPARE(PageOps::rotatePages(QStringLiteral(MERVIN_FIXTURE_PDF), path, {0}, 90,
                                       true, {}, &error), PageOps::Status::Ok);
        QCOMPARE(PageOps::pageCount(path), PageOps::pageCount(QStringLiteral(MERVIN_FIXTURE_PDF)));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
    }
};

QTEST_GUILESS_MAIN(TstAtomicPdfWriter)
#include "tst_atomic_pdf_writer.moc"
