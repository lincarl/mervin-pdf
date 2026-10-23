#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "io/AppFiles.h"
#include "dialogs/ManageLanguagesDialog.h"
#include "ocr/TessdataManager.h"
#include "dialogs/OcrPopup.h"
#include "i18n/UiLanguage.h"

#include <QComboBox>
#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <cstring>
#include <memory>

using mervin::OcrPopup;
using mervin::ManageLanguagesDialog;

namespace {

/// Delivers a small catalog through the real asynchronous network-reply path.
class CatalogReply : public QNetworkReply
{
public:
    CatalogReply(const QNetworkRequest &request, QObject *parent) : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this] {
            setFinished(true);
            emit readyRead();
            emit finished();
        });
    }
    void abort() override {}
    qint64 bytesAvailable() const override
    {
        return payload_.size() - offset_ + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maximum) override
    {
        const qint64 count = qMin(maximum, payload_.size() - offset_);
        if (count == 0)
            return -1;
        std::memcpy(data, payload_.constData() + offset_, size_t(count));
        offset_ += count;
        return count;
    }

private:
    const QByteArray payload_ = QByteArrayLiteral(
        "[{\"name\":\"swe.traineddata\",\"size\":200,\"download_url\":"
        "\"https://raw.githubusercontent.com/tesseract-ocr/tessdata_best/main/swe.traineddata\"},"
        "{\"name\":\"deu.traineddata\",\"size\":300,\"download_url\":"
        "\"https://raw.githubusercontent.com/tesseract-ocr/tessdata_best/main/deu.traineddata\"},"
        "{\"name\":\"README.md\",\"size\":10}]");
    qint64 offset_ = 0;
};

/// Records every request without opening a network connection.
class CatalogNetwork : public QNetworkAccessManager
{
public:
    QList<QUrl> requested;

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request,
                                 QIODevice *) override
    {
        Q_ASSERT(operation == GetOperation);
        requested.append(request.url());
        return new CatalogReply(request, this);
    }
};

class ConfigOverrideGuard
{
public:
    ConfigOverrideGuard()
        : original_(mervin::ConfigPaths::overrideDir())
    {
    }

    ~ConfigOverrideGuard()
    {
        mervin::ConfigPaths::setOverrideDir(original_);
    }

private:
    QString original_;
};

QComboBox *languageCombo(OcrPopup &popup)
{
    return popup.findChild<QComboBox *>(QStringLiteral("ocrLanguageCombo"));
}

} // namespace

class TstOcrPopup : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void startsWithConfiguredDefault();
    void changingLanguageRequestsRecognition();
    void refreshAddsLanguagesWithoutLosingSelection();
    void refreshUsesChangedDefaultAndRequestsRecognition();
    void recognizeButtonIsAbsent();
    void noInstalledLanguagesDisablesPicker();
    void defaultLanguagePersistsInSettings();
    void managerShowsDefaultAndEqualWidthLists();
    void textOptionsAndEditedCopy_data();
    void textOptionsAndEditedCopy();
    void refreshHandlesMissingAndRemovedLanguages();
};

// Some checks read English UI text.
void TstOcrPopup::initTestCase()
{
    mervin::i18n::apply(QStringLiteral("en"));
}

void TstOcrPopup::startsWithConfiguredDefault()
{
    OcrPopup popup({QStringLiteral("deu"), QStringLiteral("eng"), QStringLiteral("swe")},
                   QStringLiteral("swe"));
    QCOMPARE(popup.selectedLanguages(), QStringList{QStringLiteral("swe")});
    QCOMPARE(languageCombo(popup)->currentData().toString(), QStringLiteral("swe"));
}

void TstOcrPopup::changingLanguageRequestsRecognition()
{
    OcrPopup popup({QStringLiteral("eng"), QStringLiteral("swe")}, QStringLiteral("eng"));
    QSignalSpy spy(&popup, &OcrPopup::recognizeRequested);

    languageCombo(popup)->setCurrentIndex(languageCombo(popup)->findData(QStringLiteral("swe")));

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toStringList(), QStringList{QStringLiteral("swe")});
}

void TstOcrPopup::refreshAddsLanguagesWithoutLosingSelection()
{
    OcrPopup popup({QStringLiteral("eng")}, QStringLiteral("eng"));
    QSignalSpy spy(&popup, &OcrPopup::recognizeRequested);

    popup.refreshLanguages({QStringLiteral("eng"), QStringLiteral("swe")},
                           QStringLiteral("eng"));

    QCOMPARE(languageCombo(popup)->count(), 2);
    QCOMPARE(popup.selectedLanguages(), QStringList{QStringLiteral("eng")});
    QCOMPARE(spy.count(), 0);
}

void TstOcrPopup::refreshUsesChangedDefaultAndRequestsRecognition()
{
    OcrPopup popup({QStringLiteral("eng")}, QStringLiteral("eng"));
    QSignalSpy spy(&popup, &OcrPopup::recognizeRequested);

    popup.refreshLanguages({QStringLiteral("eng"), QStringLiteral("swe")},
                           QStringLiteral("swe"));

    QCOMPARE(popup.selectedLanguages(), QStringList{QStringLiteral("swe")});
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toStringList(), QStringList{QStringLiteral("swe")});
}

void TstOcrPopup::recognizeButtonIsAbsent()
{
    OcrPopup popup({QStringLiteral("eng")}, QStringLiteral("eng"));

    QVERIFY(!popup.findChild<QPushButton *>(QStringLiteral("ocrRecognizeButton")));
    const QList<QPushButton *> buttons = popup.findChildren<QPushButton *>();
    for (const QPushButton *button : buttons)
        QVERIFY2(button->text() != QStringLiteral("Recognise"),
                 "OCR Selection must not show a Recognise button");
}

void TstOcrPopup::noInstalledLanguagesDisablesPicker()
{
    OcrPopup popup({}, QStringLiteral("eng"));

    QVERIFY(!languageCombo(popup)->isEnabled());
    QVERIFY(popup.selectedLanguages().isEmpty());
}

void TstOcrPopup::defaultLanguagePersistsInSettings()
{
    ConfigOverrideGuard guard;
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    mervin::ConfigPaths::setOverrideDir(profile.path());

    QCOMPARE(mervin::Settings::load().ocrDefaultLanguage, QStringLiteral("eng"));
    mervin::Settings settings;
    settings.ocrDefaultLanguage = QStringLiteral("swe");
    settings.save();
    QCOMPARE(mervin::Settings::load().ocrDefaultLanguage, QStringLiteral("swe"));
    QVERIFY(mervin::appfiles::flush(10000)); // before the profile goes
}

void TstOcrPopup::managerShowsDefaultAndEqualWidthLists()
{
    ConfigOverrideGuard guard;
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    mervin::ConfigPaths::setOverrideDir(profile.path());

    const QString tessdata = QDir(profile.path()).filePath(QStringLiteral("tessdata"));
    QVERIFY(QDir().mkpath(tessdata));
    for (const QString &code : {QStringLiteral("eng"), QStringLiteral("swe")}) {
        QFile model(QDir(tessdata).filePath(code + QStringLiteral(".traineddata")));
        QVERIFY(model.open(QIODevice::WriteOnly));
    }

    CatalogNetwork network;
    ManageLanguagesDialog manager(QStringLiteral("swe"), nullptr, &network);
    QCOMPARE(network.requested, QList<QUrl>{QUrl(QStringLiteral(
        "https://api.github.com/repos/tesseract-ocr/tessdata_best/contents"))});
    // The installed languages are read off the UI thread.
    QTRY_VERIFY(!mervin::TessdataManager::busy());
    auto *defaultCombo =
        manager.findChild<QComboBox *>(QStringLiteral("ocrDefaultLanguage"));
    QVERIFY(defaultCombo);
    QCOMPARE(defaultCombo->count(), 2);
    QCOMPARE(manager.defaultLanguage(), QStringLiteral("swe"));

    const QList<QLabel *> headings =
        manager.findChildren<QLabel *>(QStringLiteral("ocrLanguageHeading"));
    QStringList headingTexts;
    for (const QLabel *heading : headings)
        headingTexts.append(heading->text());
    QVERIFY(headingTexts.contains(QStringLiteral("Installed (2)")));
    QVERIFY(headingTexts.contains(QStringLiteral("Available - best models")));

    manager.resize(720, 470);
    QVERIFY(manager.layout()->activate());
    auto *installed =
        manager.findChild<QListWidget *>(QStringLiteral("ocrInstalledLanguages"));
    auto *available =
        manager.findChild<QListWidget *>(QStringLiteral("ocrAvailableLanguages"));
    QVERIFY(installed);
    QVERIFY(available);
    QTRY_COMPARE(available->count(), 1);
    QStringList codes;
    for (int i = 0; i < available->count(); ++i) {
        QWidget *row = available->itemWidget(available->item(i));
        QVERIFY(row);
        codes.append(row->findChild<QLabel *>(QStringLiteral("ocrLanguageCode"))->text());
    }
    QCOMPARE(codes, QStringList({QStringLiteral("deu")}));
    QCOMPARE(installed->count(), 2);
    QCOMPARE(installed->width(), available->width());
}

/// Options transform recognition text, while Copy uses the user's edited display.
void TstOcrPopup::textOptionsAndEditedCopy_data()
{
    QTest::addColumn<bool>("preserve");
    QTest::addColumn<bool>("trim");
    QTest::addColumn<QString>("expected");
    QTest::newRow("preserve-trim") << true << true << QStringLiteral("First  line\nSecond line");
    QTest::newRow("preserve-untrimmed") << true << false << QStringLiteral(" \n First  line\nSecond line \n ");
    QTest::newRow("join-trim") << false << true << QStringLiteral("First  line Second line");
    QTest::newRow("join-untrimmed") << false << false << QStringLiteral("   First  line Second line   ");
}

void TstOcrPopup::textOptionsAndEditedCopy()
{
    QFETCH(bool, preserve);
    QFETCH(bool, trim);
    QFETCH(QString, expected);
    auto saved = std::make_unique<QMimeData>();
    if (const QMimeData *original = QApplication::clipboard()->mimeData())
        for (const QString &format : original->formats())
            saved->setData(format, original->data(format));
    const auto restore = qScopeGuard([&] { QApplication::clipboard()->setMimeData(saved.release()); });

    OcrPopup popup({QStringLiteral("eng")}, QStringLiteral("eng"));
    popup.show();
    QVERIFY(QTest::qWaitForWindowExposed(&popup));
    popup.setRawText(QStringLiteral(" \n First  line\nSecond line \n "));
    for (auto *box : popup.findChildren<QCheckBox *>()) {
        const bool desired = box->text() == QStringLiteral("Preserve line breaks") ? preserve : trim;
        if (box->isChecked() != desired)
            QTest::mouseClick(box, Qt::LeftButton, {}, QPoint(4, box->height() / 2));
    }
    auto *editor = popup.findChild<QPlainTextEdit *>();
    QVERIFY(editor);
    QCOMPARE(editor->toPlainText(), expected);
    editor->selectAll();
    QTest::keyClicks(editor, QStringLiteral("Corrected OCR text"));
    QPushButton *copy = nullptr;
    for (auto *button : popup.findChildren<QPushButton *>())
        if (button->text() == QStringLiteral("Copy"))
            copy = button;
    QVERIFY(copy);
    QTest::mouseClick(copy, Qt::LeftButton);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("Corrected OCR text"));
    QVERIFY(popup.isVisible());
}

/// Missing preferences preserve an installed selection, then fall back when it disappears.
void TstOcrPopup::refreshHandlesMissingAndRemovedLanguages()
{
    OcrPopup popup({QStringLiteral("eng"), QStringLiteral("swe")}, QStringLiteral("swe"));
    QSignalSpy recognized(&popup, &OcrPopup::recognizeRequested);
    popup.refreshLanguages({QStringLiteral("eng"), QStringLiteral("swe")}, QStringLiteral("missing"));
    QCOMPARE(popup.selectedLanguages(), QStringList{QStringLiteral("swe")});
    QVERIFY(recognized.isEmpty());
    popup.refreshLanguages({QStringLiteral("eng")}, QStringLiteral("missing"));
    QCOMPARE(popup.selectedLanguages(), QStringList{QStringLiteral("eng")});
    QCOMPARE(recognized.takeFirst().at(0).toStringList(), QStringList{QStringLiteral("eng")});
    popup.refreshLanguages({}, QStringLiteral("eng"));
    QVERIFY(popup.selectedLanguages().isEmpty());
    QVERIFY(!languageCombo(popup)->isEnabled());
    QVERIFY(recognized.isEmpty());
    popup.refreshLanguages({QStringLiteral("swe")}, {});
    QVERIFY(languageCombo(popup)->isEnabled());
    QCOMPARE(popup.selectedLanguages(), QStringList{QStringLiteral("swe")});
    QCOMPARE(recognized.count(), 1);
    QCOMPARE(recognized.first().at(0).toStringList(), QStringList{QStringLiteral("swe")});
}

QTEST_MAIN(TstOcrPopup)
#include "tst_ocr_popup.moc"
