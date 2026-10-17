#include "TestWaits.h"
#include "TextFit.h"

#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "io/AppFiles.h"
#include "dialogs/CalibrationDialog.h"
#include "dialogs/ExportMeasureDialog.h"
#include "dialogs/ExtractDialog.h"
#include "dialogs/FirstRunDialog.h"
#include "dialogs/ManageLanguagesDialog.h"
#include "dialogs/MergeDialog.h"
#include "dialogs/OcrPopup.h"
#include "dialogs/PrintDialog.h"
#include "dialogs/SecurityDialog.h"
#include "dialogs/SettingsDialog.h"
#include "i18n/UiLanguage.h"
#include "io/JobContext.h"
#include "io/SaveCheck.h"
#include "ocr/TessdataManager.h"
#include "print/PrintPreviewWidget.h"
#include "render/AnnotModel.h"
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "recent/PathKey.h"
#include "security/QpdfService.h"
#include "ui/AnnotPanel.h"
#include "ui/AnnotPopup.h"
#include "ui/LanguageCombo.h"
#include "ui/MainWindow.h"
#include "ui/MeasurePanel.h"
#include "ui/TabPage.h"
#include "ui/TextLayout.h"
#include "ui/Theme.h"
#include "ui/ThemeTokens.h"
#include "ui/ViewerWidget.h"
#include "ui/WaitBar.h"
#include "update/Updater.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QScopeGuard>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMetaEnum>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPrinter>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStyleHints>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QTimeZone>
#include <QTimer>
#include <QToolButton>
#include <QXmlStreamReader>

#include <atomic>
#include <cstring>
#include <functional>
#include <utility>

namespace {

namespace i18n = mervin::i18n;

// CTest runs one process per language to keep each check within its timeout.
// Running the executable directly still exercises every shipped language.
QStringList layoutLanguages()
{
    const QString requested = qEnvironmentVariable("MERVIN_TEST_LANGUAGE");
    return requested.isEmpty() ? i18n::availableLanguages() : QStringList{requested};
}

// Runs the event loop until it is idle, so queued layout work has reached every
// widget. A single pass can stop halfway, as when a list gains its scroll bar and
// widens its header before the dialog's minimum follows.
void settle()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, 1000);
}

// Every new compiled catalog automatically enters the same layout matrix.
void addLanguageMatrix()
{
    QTest::addColumn<QString>("language");
    QTest::addColumn<double>("fontScale");
    QTest::addColumn<bool>("minimumSize");
    for (const QString &language : layoutLanguages()) {
        for (const double scale : {1.0, 1.5}) {
            for (const bool minimum : {false, true}) {
                const QByteArray name = QStringLiteral("%1-%2pt-%3")
                                            .arg(language).arg(10 * scale)
                                            .arg(minimum ? "minimum" : "default").toUtf8();
                QTest::newRow(name.constData()) << language << scale << minimum;
            }
        }
    }
}

// A real asynchronous QNetworkReply without sockets or credentials. The dialog
// still parses and displays its normal catalog and error responses.
class CatalogReply final : public QNetworkReply
{
public:
    CatalogReply(const QNetworkRequest &request, QByteArray payload, NetworkError error,
                 QObject *parent)
        : QNetworkReply(parent), payload_(std::move(payload))
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        if (error != NoError)
            setError(error, QStringLiteral("The language server could not be reached. Try again later."));
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
        if (!count)
            return -1;
        std::memcpy(data, payload_.constData() + offset_, size_t(count));
        offset_ += count;
        return count;
    }

private:
    QByteArray payload_;
    qint64 offset_ = 0;
};

class CatalogNetwork final : public QNetworkAccessManager
{
public:
    explicit CatalogNetwork(QByteArray payload,
                            QNetworkReply::NetworkError error = QNetworkReply::NoError)
        : payload_(std::move(payload)), error_(error)
    {}

    int requests = 0;
    int completed = 0;

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request,
                                 QIODevice *outgoing) override
    {
        Q_UNUSED(operation);
        Q_UNUSED(outgoing);
        ++requests;
        auto *reply = new CatalogReply(request, payload_, error_, this);
        connect(reply, &QNetworkReply::finished, this, [this] { ++completed; });
        return reply;
    }

private:
    QByteArray payload_;
    QNetworkReply::NetworkError error_;
};

QByteArray catalogPayload()
{
    QJsonArray models;
    for (const char *code : {"eng", "swe", "chi_tra", "aze", "osd", "srp"}) {
        const QString file = QLatin1String(code) + QStringLiteral(".traineddata");
        models.append(QJsonObject{
            {QStringLiteral("name"), file},
            {QStringLiteral("size"), 48765432},
            {QStringLiteral("download_url"),
             QStringLiteral("https://raw.githubusercontent.com/tesseract-ocr/tessdata_best/main/")
                 + file}});
    }
    return QJsonDocument(models).toJson(QJsonDocument::Compact);
}

} // namespace

// Opens each application-owned dialog using the shipped theme and real widgets.
// OS-owned file/print/colour pickers are outside this portable Qt geometry test.
// Add a slot and its addLanguageMatrix() data function for each new dialog.
class TstTranslationLayout : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();

    void catalogGlyphs_data();
    void catalogGlyphs();

    void calibration_data() { addLanguageMatrix(); }
    void calibration();
    void exportMeasurements_data() { addLanguageMatrix(); }
    void exportMeasurements();
    void extract_data() { addLanguageMatrix(); }
    void extract();
    void deletePages_data() { addLanguageMatrix(); }
    void deletePages();
    void firstRun_data() { addLanguageMatrix(); }
    void firstRun();
    void languagePicker_data() { addLanguageMatrix(); }
    void languagePicker();
    void loadPlaceholder_data() { addLanguageMatrix(); }
    void loadPlaceholder();
    void manageLanguages_data() { addLanguageMatrix(); }
    void manageLanguages();
    void merge_data() { addLanguageMatrix(); }
    void merge();
    void ocr_data() { addLanguageMatrix(); }
    void ocr();
    void print_data() { addLanguageMatrix(); }
    void print();
    void security_data() { addLanguageMatrix(); }
    void security();
    // Selects language, font and minimum-width rows for saved-file recovery warnings.
    void saveRecoveryWarning_data() { addLanguageMatrix(); }
    void saveRecoveryWarning();
    void settings_data() { addLanguageMatrix(); }
    void settings();
    void splitMessages_data() { addLanguageMatrix(); }
    void splitMessages();
    void toolPanels_data() { addLanguageMatrix(); }
    void toolPanels();
    void updateCheckWarning_data() { addLanguageMatrix(); }
    void updateCheckWarning();
    void waitBar_data() { addLanguageMatrix(); }
    void waitBar();

private:
    // Shows `widget`, at its minimum size in the minimum-size rows, and checks the text
    // in `part` of it, or in all of it.
    void inspect(QWidget &widget, const QString &state, QWidget *part = nullptr);
    void inspectMessage(QWidget &parent, const QString &state, const std::function<void()> &action);
    QString modelPath(const char *code) const;
    QTemporaryDir profile_;
    QString originalProfile_;
    QFont originalFont_;
    QString originalStyleSheet_;
    QPalette originalPalette_;
    Qt::ColorScheme originalScheme_ = Qt::ColorScheme::Unknown;
    QLocale originalLocale_;
    QString originalLanguage_;
    QString fontFamily_;
    QString plain_;
    QString encrypted_;
    bool minimumSize_ = false;
};

void TstTranslationLayout::initTestCase()
{
    QVERIFY(profile_.isValid());
    originalProfile_ = mervin::ConfigPaths::overrideDir();
    originalFont_ = QApplication::font();
    originalStyleSheet_ = qApp->styleSheet();
    originalPalette_ = qApp->palette();
    originalScheme_ = QGuiApplication::styleHints()->colorScheme();
    originalLanguage_ = i18n::current();
    mervin::ConfigPaths::setOverrideDir(profile_.path());

    for (const QString &language : layoutLanguages())
        QVERIFY2(i18n::availableLanguages().contains(language), qPrintable(language));

    // Load the same pinned fonts on Linux and Windows. Checking only Latin
    // and Chinese allowed missing glyphs in the other languages to go unseen.
    const QString fontPath = qEnvironmentVariable("MERVIN_TEST_FONT");
    const QStringList filenames{
        QStringLiteral("NotoSans-Regular.ttf"),
        QStringLiteral("NotoSansArabic-Regular.ttf"),
        QStringLiteral("NotoSansArmenian-Regular.ttf"),
        QStringLiteral("NotoSansGeorgian-Regular.ttf"),
        QStringLiteral("NotoSansDevanagari-Regular.ttf"),
        QStringLiteral("NotoSansThai-Regular.ttf"),
        QStringLiteral("NotoSansCJKsc-Regular.otf"),
        QStringLiteral("NotoSansCJKtc-Regular.otf"),
        QStringLiteral("NotoSansCJKjp-Regular.otf"),
        QStringLiteral("NotoSansCJKkr-Regular.otf")};
    if (!fontPath.isEmpty()) {
        const QDir directory = QFileInfo(fontPath).absoluteDir();
        for (const QString &name : filenames) {
            const QString path = directory.filePath(name);
            QVERIFY2(QFontDatabase::addApplicationFont(path) >= 0,
                     qPrintable(QStringLiteral("Could not load %1. Run scripts/fetch-test-font.py.")
                                    .arg(path)));
        }
    }
    const QStringList families{
        QStringLiteral("Noto Sans"), QStringLiteral("Noto Sans Arabic"),
        QStringLiteral("Noto Sans Armenian"), QStringLiteral("Noto Sans Georgian"),
        QStringLiteral("Noto Sans Devanagari"), QStringLiteral("Noto Sans Thai"),
        QStringLiteral("Noto Sans CJK SC"), QStringLiteral("Noto Sans CJK TC"),
        QStringLiteral("Noto Sans CJK JP"), QStringLiteral("Noto Sans CJK KR")};
    for (const QString &family : families)
        QVERIFY2(QFontDatabase::families().contains(family), qPrintable(family));
    fontFamily_ = QStringLiteral("Noto Sans");
    QFontDatabase::addApplicationFallbackFontFamily(QChar::Script_Arabic,
                                                    QStringLiteral("Noto Sans Arabic"));
    QFontDatabase::addApplicationFallbackFontFamily(QChar::Script_Armenian,
                                                    QStringLiteral("Noto Sans Armenian"));
    QFontDatabase::addApplicationFallbackFontFamily(QChar::Script_Georgian,
                                                    QStringLiteral("Noto Sans Georgian"));
    QFontDatabase::addApplicationFallbackFontFamily(QChar::Script_Devanagari,
                                                    QStringLiteral("Noto Sans Devanagari"));
    QFontDatabase::addApplicationFallbackFontFamily(QChar::Script_Thai,
                                                    QStringLiteral("Noto Sans Thai"));
    QFontDatabase::addApplicationFallbackFontFamily(QChar::Script_Han,
                                                    QStringLiteral("Noto Sans CJK SC"));
    QFontDatabase::addApplicationFallbackFontFamily(QChar::Script_Hangul,
                                                    QStringLiteral("Noto Sans CJK KR"));
    QFontDatabase::addApplicationFallbackFontFamily(QChar::Script_Hiragana,
                                                    QStringLiteral("Noto Sans CJK JP"));
    QFontDatabase::addApplicationFallbackFontFamily(QChar::Script_Katakana,
                                                    QStringLiteral("Noto Sans CJK JP"));

    const QString fixtures = QStringLiteral(MERVIN_FIXTURES_DIR);
    plain_ = profile_.filePath(QStringLiteral("structural-engineering-report-revised-for-construction.pdf"));
    QVERIFY(QFile::copy(QDir(fixtures).filePath(QStringLiteral("images.pdf")), plain_));
    encrypted_ = profile_.filePath(QStringLiteral("protected-engineering-report.pdf"));
    mervin::QpdfService::Permissions permissions;
    permissions.canPrint = permissions.canCopy = permissions.canModify = permissions.canAnnotate = false;
    mervin::QpdfService service;
    QString error;
    const auto result = service.encrypt(plain_, encrypted_, {}, QStringLiteral("secret"),
                                        QStringLiteral("owner-secret"),
                                        mervin::QpdfService::Algorithm::AES256, permissions, &error);
    QVERIFY2(result == mervin::QpdfService::Status::Ok, qPrintable(error));
    QVERIFY(QDir().mkpath(profile_.filePath(QStringLiteral("tessdata"))));
}

QString TstTranslationLayout::modelPath(const char *code) const
{
    return profile_.filePath(QStringLiteral("tessdata/%1.traineddata").arg(QLatin1String(code)));
}

void TstTranslationLayout::init()
{
    QFETCH(QString, language);
    QFETCH(double, fontScale);
    QFETCH(bool, minimumSize);
    minimumSize_ = minimumSize;
    // The UI language does not change regional formats in Mervin.
    QLocale::setDefault(QLocale::c());
    QFont font(fontFamily_);
    font.setPointSizeF(10 * fontScale);
    QApplication::setFont(font);
    i18n::apply(language);
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    qApp->setPalette(mervin::theme::darkPalette(QColor()));
    mervin::Theme::applyApp();
    // Only presence and size are read. These are never sent to the OCR engine.
    for (const char *code : {"eng", "swe", "chi_tra"}) {
        QFile file(modelPath(code));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray(1024, 'x')), qint64(1024));
    }
}

void TstTranslationLayout::cleanup()
{
    // Finish deferred widget/reply deletion before switching language and font.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void TstTranslationLayout::cleanupTestCase()
{
    QVERIFY(mervin::appfiles::flush(10000));
    i18n::apply(originalLanguage_);
    QLocale::setDefault(originalLocale_);
    QApplication::setFont(originalFont_);
    QGuiApplication::styleHints()->setColorScheme(originalScheme_);
    qApp->setPalette(originalPalette_);
    qApp->setStyleSheet(originalStyleSheet_);
    mervin::ConfigPaths::setOverrideDir(originalProfile_);
}

void TstTranslationLayout::catalogGlyphs_data()
{
    QTest::addColumn<QString>("language");
    QTest::addColumn<double>("fontScale");
    QTest::addColumn<bool>("minimumSize");
    for (const QString &language : layoutLanguages())
        QTest::newRow(qPrintable(language)) << language << 1.0 << false;
}

// Missing glyphs can occupy less space than the intended characters and make a
// fitting test pass with unreadable boxes. Check every catalog, including all
// plural forms and the English source, against the font and its Qt fallbacks.
void TstTranslationLayout::catalogGlyphs()
{
    QFETCH(QString, language);
    const QDir directory(QStringLiteral(MERVIN_TRANSLATIONS_DIR));
    QStringList paths{directory.filePath(QStringLiteral("mervin_%1.ts").arg(language))};
    const QString supplement = directory.filePath(QStringLiteral("qt/qtbase_%1.ts").arg(language));
    if (QFileInfo::exists(supplement))
        paths.append(supplement);
    for (const QString &path : paths) {
        QFile file(path);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
        QXmlStreamReader xml(&file);
        const QFontMetrics metrics(QApplication::font());
        QString context;
        QString source;
        QStringList missing;
        int messages = 0;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement())
                continue;
            if (xml.name() == QLatin1String("name")) {
                context = xml.readElementText();
            } else if (xml.name() == QLatin1String("source")
                       || xml.name() == QLatin1String("translation")) {
                const bool isSource = xml.name() == QLatin1String("source");
                // IncludeChildElements includes every <numerusform>, so each
                // plural form contributes its code points to this check.
                QString text = xml.readElementText(QXmlStreamReader::IncludeChildElements);
                if (isSource) {
                    source = text;
                    ++messages;
                }
                if (Qt::mightBeRichText(text)) {
                    QTextDocument document;
                    document.setHtml(text);
                    text = document.toPlainText();
                }
                for (const char32_t codePoint : text.toUcs4()) {
                    const auto category = QChar::category(codePoint);
                    if (QChar::isSpace(codePoint) || category == QChar::Other_Control
                        || category == QChar::Other_Format || metrics.inFontUcs4(codePoint))
                        continue;
                    missing.append(QStringLiteral("%1, source '%2', %3 has no glyph for U+%4")
                                       .arg(context, source,
                                            isSource ? QStringLiteral("English") : language,
                                            QString::number(codePoint, 16).toUpper().rightJustified(4, '0')));
                }
            }
        }
        QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
        QVERIFY(messages > 0);
        missing.removeDuplicates();
        QVERIFY2(missing.isEmpty(), qPrintable(path + QLatin1Char('\n') + missing.join(QLatin1Char('\n'))));
    }
}

void TstTranslationLayout::languagePicker()
{
    mervin::LanguageCombo combo;
    inspect(combo, QStringLiteral("Closed language picker"));
    combo.showPopup();
    QTest::qWait(1);
    QListView *list = combo.listView();
    for (int row = 0; row < list->model()->rowCount(); ++row) {
        const QModelIndex index = list->model()->index(row, 0);
        list->scrollTo(index);
        QCoreApplication::processEvents();
        const QString text = index.data().toString();
        const QSize needed = QFontMetrics(list->font()).size(Qt::TextSingleLine, text);
        const QRect available = list->visualRect(index);
        QVERIFY2(available.height() >= needed.height(), qPrintable(text));
        QVERIFY2(available.width() >= needed.width(), qPrintable(text));
    }
    combo.hidePopup();
}

// The notice a tab shows in place of its pages when the drive stops answering while
// the document loads, with a long file name in a long folder. The main window sets the
// tab's size, down to the narrowest the window gets. Its own minimum height leaves
// almost no room for pages, so it keeps the height it opens with. Only the tab is
// checked, as the rest of the window is not part of this scenario.
void TstTranslationLayout::loadPlaceholder()
{
    const QString folder = profile_.filePath(QStringLiteral(
        "structural-engineering-department/project-archive-2026/revised-drawings-for-construction"));
    const QString path = QDir(folder).filePath(QFileInfo(plain_).fileName());
    // A name with nowhere to wrap, which the notice shortens in the middle.
    const QString unbroken = QDir(folder).filePath(QStringLiteral("Revision").repeated(30)
                                                   + QStringLiteral(".pdf"));
    // The loads wait at their first step, as on a drive that stopped answering.
    mervin::job::testing::holdWhen([folder](mervin::job::Step step, const QString &file) {
        return step == mervin::job::Step::Reading && QFileInfo(file).path() == folder;
    });
    mervin::TabPage::setSlowDelayForTesting(0);
    const auto restore = qScopeGuard([] {
        mervin::job::testing::release();
        mervin::TabPage::setSlowDelayForTesting(mervin::TabPage::kSlowMs);
    });
    mervin::RenderEngine engine;
    MainWindow window(&engine, nullptr);
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    settle();
    window.setMinimumHeight(window.height());
    QVERIFY(window.openFile(path, false, -1, true, {}, true));
    auto *tab = window.findChild<mervin::TabPage *>();
    QVERIFY(tab);
    auto *stop = tab->findChild<QPushButton *>(QStringLiteral("stopDocumentLoad"));
    QVERIFY(stop);
    QTRY_VERIFY(stop->isVisible());
    inspect(window, QStringLiteral("Slow document load"), tab);

    QVERIFY(window.openFile(unbroken, false, -1, true, {}, true));
    auto *first = tab;
    tab = qobject_cast<mervin::TabPage *>(window.findChild<QTabWidget *>()->currentWidget());
    QVERIFY(tab);
    // The new tab, so the first tab's button cannot pass the checks below.
    QVERIFY(tab != first);
    QCOMPARE(tab->tabTitle(), QFileInfo(unbroken).fileName());
    stop = tab->findChild<QPushButton *>(QStringLiteral("stopDocumentLoad"));
    QVERIFY(stop);
    QTRY_VERIFY(stop->isVisible());
    inspect(window, QStringLiteral("Slow document load with an unbroken file name"), tab);

    // Keep the real failed-load label and Retry layout while checking reader reasons.
    tab->stopWaiting();
    auto *status = tab->findChild<QLabel *>(QStringLiteral("documentLoadStatus"));
    QVERIFY(status && status->isVisible());
    const struct {
        const char *state;
        QString text;
    } failures[] = {
        {"Missing document", mervin::RenderEngine::tr("The file was not found.")},
        {"Damaged document", mervin::RenderEngine::tr("The file is damaged.")},
        {"Rejected password", mervin::RenderEngine::tr("The password was not accepted.")},
        {"Password required", mervin::RenderEngine::tr("This document is password-protected.")},
    };
    for (const auto &failure : failures) {
        status->setText(failure.text);
        inspect(window, QString::fromLatin1(failure.state), tab);
    }
    const QString output = profile_.filePath(
        QStringLiteral("structural-plan-reserved-for-an-active-page-tool.pdf"));
    mervin::job::State writing;
    const mervin::job::Scope scope(writing);
    QVERIFY(mervin::job::reserveDestinations({mervin::normalizePathKey(output)}));
    QVERIFY(window.openFile(output));
    auto *reserved = qobject_cast<mervin::TabPage *>(window.findChild<QTabWidget *>()->currentWidget());
    QVERIFY(reserved);
    QTRY_VERIFY(!reserved->isLoading());
    QCOMPARE(reserved->findChild<QLabel *>(QStringLiteral("documentLoadStatus"))->text(),
             mervin::TabPage::pathBeingWrittenError());
    inspect(window, QStringLiteral("Opening an output that is being written"), reserved);
}

/// Checks the warning a manual update check shows when the server's answer is not a release.
void TstTranslationLayout::updateCheckWarning()
{
    QMessageBox warning(QMessageBox::Warning, mervin::Updater::tr("Update"),
        mervin::Updater::tr("Couldn't check for updates right now.\n\n%1")
            .arg(mervin::Updater::tr("The update server's answer couldn't be read.")),
        QMessageBox::Ok);
    inspect(warning, QStringLiteral("Update check answer not readable"));
}

// The bar over a tab whose operation waits for a drive that stopped answering, saving
// to a long folder and reading a file name that cannot wrap from it. The main window
// sets the tab's size, down to the narrowest the window gets, as for the slow load.
void TstTranslationLayout::waitBar()
{
    const QString folder = profile_.filePath(QStringLiteral(
        "structural-engineering-department/project-archive-2026/revised-drawings-for-construction"));
    const QString unbroken = QDir(folder).filePath(QStringLiteral("Revision").repeated(30)
                                                   + QStringLiteral(".pdf"));
    // The operations wait at their only step, as on a drive that stopped answering.
    mervin::job::testing::holdWhen([folder](mervin::job::Step step, const QString &path) {
        return (step == mervin::job::Step::Writing && path == folder)
            || (step == mervin::job::Step::Reading && QFileInfo(path).path() == folder);
    });
    mervin::TabPage::setSlowDelayForTesting(0);
    mervin::RenderEngine engine;
    MainWindow window(&engine, nullptr);
    // Release a held document before window/engine teardown, including a failed check.
    const auto restore = qScopeGuard([] {
        mervin::job::testing::release();
        mervin::TabPage::setSlowDelayForTesting(mervin::TabPage::kSlowMs);
    });
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    settle();
    window.setMinimumHeight(window.height());
    QVERIFY(mervin::testing::openAndWait(window, plain_));
    auto *tab = window.findChild<mervin::TabPage *>();
    QVERIFY(tab);
    auto *stop = tab->findChild<QPushButton *>(QStringLiteral("waitBarStop"));
    QVERIFY(stop);

    mervin::TabPage::Operation saving;
    saving.destination = folder;
    saving.work = [folder] { mervin::job::writing(folder); };
    tab->startOperation(saving);
    QTRY_VERIFY(stop->isVisible());
    inspect(window, QStringLiteral("Wait bar saving to a long folder"), tab);
    // The scroll bar's column stays free, on the left from right to left.
    const QRect bar = QStyle::visualRect(tab->layoutDirection(), tab->rect(),
                                         stop->parentWidget()->geometry());
    QCOMPARE(bar.left(), 0);
    QCOMPARE(bar.right(), tab->width() - tab->viewer()->verticalScrollBar()->width() - 1);

    tab->stopWaiting();
    mervin::TabPage::Operation reading;
    reading.destination = profile_.path();
    reading.work = [unbroken] { mervin::job::reading(unbroken); };
    tab->startOperation(reading);
    QTRY_VERIFY(stop->isVisible());
    QVERIFY(stop->parentWidget()->findChild<QLabel *>()->text().contains(QStringLiteral("Revision")));
    inspect(window, QStringLiteral("Wait bar reading an unbroken file name"), tab);
    tab->stopWaiting();
    mervin::job::testing::release();
    QTRY_COMPARE(mervin::job::testing::held(), 0);

    const QString printer = QStringLiteral("OfficeLaserArchitecturalDrawingsDepartmentPrinter")
                                .repeated(8);
    mervin::job::testing::holdWhen([printer](mervin::job::Step step, const QString &path) {
        return step == mervin::job::Step::Printing && path == printer;
    });
    mervin::TabPage::Operation printing;
    printing.work = [printer] { mervin::job::printing(printer); };
    tab->startOperation(std::move(printing));
    QTRY_VERIFY(stop->isVisible());
    QVERIFY(stop->parentWidget()->findChild<QLabel *>()->text().contains(QStringLiteral("Office")));
    inspect(window, QStringLiteral("Wait bar for a long printer name"), tab);
    tab->stopWaiting();
    mervin::job::testing::release();
    QTRY_COMPARE(mervin::job::testing::held(), 0);

    // Stop hides its button while an actual document Hold waits for the drive.
    // The longer notice gets the full bar width at every language/font/window size.
    mervin::job::testing::holdWhen([folder](mervin::job::Step step, const QString &path) {
        return step == mervin::job::Step::Writing && path == folder;
    });
    const auto lifetime = tab->viewer()->document()->lifetime();
    mervin::TabPage::Operation trapped;
    trapped.destination = folder;
    trapped.work = [lifetime, folder] {
        if (!mervin::job::beginHolding())
            return;
        const auto release = qScopeGuard([] { mervin::job::endHolding(); });
        const mervin::Document::Hold hold(lifetime, mervin::job::stopped);
        if (hold.document())
            mervin::job::writing(folder);
    };
    tab->startOperation(std::move(trapped));
    QTRY_VERIFY(stop->isVisible());
    QTRY_COMPARE(mervin::job::testing::held(), 1);
    tab->stopWaiting();
    QVERIFY(tab->isBusy());
    QVERIFY(tab->isOperationStopped());
    QVERIFY(stop->isHidden());
    QVERIFY(stop->parentWidget()->isVisible());
    auto *notice = stop->parentWidget()->findChild<QLabel *>(QStringLiteral("waitBarText"));
    QVERIFY(notice);
    QCOMPARE(notice->text(), mervin::TabPage::tr(
        "Stopped waiting. The document stays locked until the drive answers."));
    inspect(window, QStringLiteral("Stopped wait bar while the document remains locked"), tab);
    mervin::job::testing::release();
    QTRY_VERIFY(tab->isUsable());
    QVERIFY(stop->parentWidget()->isHidden());

    // A subsequent ordinary operation must restore the Stop button and normal text.
    mervin::job::testing::holdWhen([folder](mervin::job::Step step, const QString &path) {
        return step == mervin::job::Step::Writing && path == folder;
    });
    tab->startOperation(saving);
    QTRY_VERIFY(stop->isVisible());
    QVERIFY(notice->text() != mervin::TabPage::tr(
        "Stopped waiting. The document stays locked until the drive answers."));
    tab->stopWaiting();
}

void TstTranslationLayout::inspect(QWidget &widget, const QString &state, QWidget *part)
{
    widget.setAttribute(Qt::WA_DontShowOnScreen);
    widget.ensurePolished();
    widget.show();
    settle();
    if (minimumSize_) {
        // The smallest size the user can reach. A window can be dragged down to
        // its minimum size, even below what its layout needs, so a dialog's
        // minimum has to follow its layout. A layout gives an embedded panel at
        // least its minimum size hint. A fixed-size dialog has the same dimensions
        // in both matrix entries. The smaller size can raise the minimum again,
        // as when a list gains its scroll bar, so resize until the size holds.
        const bool window = qobject_cast<QDialog *>(&widget);
        const auto minimum = [&widget, window] {
            return window ? widget.minimumSize()
                          : widget.minimumSize().expandedTo(widget.minimumSizeHint());
        };
        for (int turn = 0; turn < 10 && widget.size() != minimum(); ++turn) {
            widget.resize(minimum());
            settle();
        }
        // Otherwise the check below would pass at a larger size.
        QVERIFY2(widget.size() == minimum(),
                 qPrintable(state + QStringLiteral(" did not settle at its minimum size")));
    }
    const QStringList problems = textfit::check(part ? *part : widget);
    const QString details = state + QLatin1Char('\n') + problems.join(QLatin1Char('\n'));
    const QPixmap capture = widget.grab();
    QVERIFY2(!capture.isNull(), qPrintable(state));
    const QString screenshotDirectory = qEnvironmentVariable("MERVIN_LAYOUT_SCREENSHOTS");
    if (!screenshotDirectory.isEmpty()) {
        QVERIFY(QDir().mkpath(screenshotDirectory));
        QString name = QStringLiteral("%1-%2-%3.png")
                           .arg(QLatin1String(QTest::currentTestFunction()),
                                QLatin1String(QTest::currentDataTag()), state);
        name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("-"));
        QVERIFY(capture.save(QDir(screenshotDirectory).filePath(name)));
    }
    QVERIFY2(problems.isEmpty(), qPrintable(details));
}

// Inspect the real message shown by an action, then dismiss it. The watchdog
// also closes an unexpected modal dialog so CI reports a failure instead of
// waiting indefinitely for a person to click a button. Some messages come at the
// end of a background job, such as a save that reopens the file three times, so
// the wait allows for a slow runner. It ends as soon as the message shows.
void TstTranslationLayout::inspectMessage(QWidget &parent, const QString &state,
                                         const std::function<void()> &action)
{
    constexpr qint64 kMessageWaitMs = 10000;
    bool sawMessage = false;
    bool timedOut = false;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer poll;
    connect(&poll, &QTimer::timeout, &parent, [&] {
        auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            sawMessage = true;
            poll.stop();
            inspect(*box, state);
            box->accept();
        } else if (elapsed.elapsed() > kMessageWaitMs) {
            timedOut = true;
            if (modal)
                modal->reject();
        }
    });
    poll.start(10);
    action();
    while (!sawMessage && !timedOut)
        QTest::qWait(10);
    poll.stop();
    QVERIFY2(!timedOut && sawMessage, qPrintable(state + QStringLiteral(" did not show the expected message")));
}

// Checks the actual Save recovery warning across committed replacement failure states.
void TstTranslationLayout::saveRecoveryWarning()
{
    QTemporaryDir files(profile_.filePath(QStringLiteral("save-warning-XXXXXX")));
    QVERIFY(files.isValid());
    const QString path = files.filePath(QFileInfo(plain_).fileName());
    QVERIFY(QFile::copy(plain_, path));
    mervin::RenderEngine engine;
    MainWindow window(&engine, nullptr);
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    QVERIFY(mervin::testing::openAndWait(window, path));
    auto *tab = window.findChild<mervin::TabPage *>();
    QVERIFY(tab);
    QVERIFY(tab->viewer()->annotModel()->addTextNote(
                0, {40, 40}, Qt::yellow, {}, QStringLiteral("Retained edit")) >= 0);
    std::atomic<bool> reopenFailed{false};
    const auto restore = qScopeGuard([] {
        mervin::job::testing::release();
        mervin::job::testing::failWhen({});
    });
    mervin::job::testing::failWhen([path, &reopenFailed, reads = 0]
                                (mervin::job::Step step, const QString &file) mutable {
        if (step == mervin::job::Step::Reading && file == path && ++reads >= 3) {
            reopenFailed = true;
            return true;
        }
        return false;
    });
    // Keep recovery loading while the actual Save warning gives its retained path.
    mervin::job::testing::holdWhen([&reopenFailed]
                                (mervin::job::Step step, const QString &file) {
        return reopenFailed && step == mervin::job::Step::Reading
            && file.contains(QStringLiteral("/recovery/"));
    });
    inspectMessage(window, QStringLiteral("Saved file awaiting recovery after reopen failure"), [&] {
        QVERIFY(QMetaObject::invokeMethod(&window, "saveMeasurements"));
    });
    QVERIFY(tab->isLoading());
    QVERIFY(tab->hasRecoverySnapshot());
    mervin::job::testing::release();
    QTRY_VERIFY(tab->isUsable());
    QVERIFY(tab->hasUnsavedEdits());
}

void TstTranslationLayout::calibration()
{
    for (auto mode : {mervin::CalibrationDialog::Mode::Calibrate,
                      mervin::CalibrationDialog::Mode::SetScale}) {
        mervin::CalibrationDialog dialog(mode, 125.0, mervin::MeasureUnit::Millimeter);
        inspect(dialog, mode == mervin::CalibrationDialog::Mode::Calibrate
                            ? QStringLiteral("Calibrate") : QStringLiteral("Set scale"));
    }
}

void TstTranslationLayout::exportMeasurements()
{
    mervin::ExportMeasureDialog dialog;
    inspect(dialog, QStringLiteral("Export with measurements"));
}

/// Checks Extract controls with retained password, count-change and read failures.
void TstTranslationLayout::extract()
{
    mervin::ExtractDialog::Source source;
    source.path = plain_;
    source.viewerPageCount = 12;
    source.seedPage = 6;
    source.hasUnsavedEdits = true;
    source.probe = mervin::ExtractDialog::probeSource(source.path, source.password);
    mervin::ExtractDialog dialog(source);
    inspect(dialog, QStringLiteral("Extract with unsaved changes"));
    auto *range = dialog.findChild<QLineEdit *>(QStringLiteral("extractRowSpec"));
    auto *error = dialog.findChild<QLabel *>(QStringLiteral("extractError"));
    auto *output = dialog.findChild<QLineEdit *>(QStringLiteral("extractOutput"));
    QVERIFY(range && error && output);
    {
        // A Save as check that takes long, as on a share that stopped answering.
        auto release = qScopeGuard([] { mervin::SaveCheck::holdForTesting(false); });
        mervin::SaveCheck::holdForTesting(true);
        output->setText(profile_.filePath(QStringLiteral("held.pdf")));
        QTest::keyClick(output, Qt::Key_End); // an edit, which checks the new text
        QTest::keyClicks(output, QStringLiteral("x"));
        QTRY_COMPARE(error->text(), mervin::SaveCheck::checkingText());
        inspect(dialog, QStringLiteral("Extract checking the folder"));
    }
    QTRY_VERIFY(!dialog.findChild<mervin::SaveCheck *>()->pending());
    range->selectAll();
    QTest::keyClicks(range, QStringLiteral("999999"));
    QVERIFY(!error->text().isEmpty());
    inspect(dialog, QStringLiteral("Extract invalid range"));
    range->selectAll();
    QTest::keyClick(range, Qt::Key_Delete);
    QVERIFY(range->text().isEmpty());
    QVERIFY(!error->text().isEmpty());
    inspect(dialog, QStringLiteral("Extract empty range"));
    const auto *rows = dialog.findChild<QListWidget *>(QStringLiteral("extractList"));
    QVERIFY(rows && rows->count() > 0);
    const QWidget *row = rows->itemWidget(rows->item(0));
    QVERIFY(row);
    QVERIFY2(row->width() >= row->minimumSizeHint().width(),
             "Extract row allocation must include all column controls");

    source.path = encrypted_;
    source.probe = mervin::ExtractDialog::probeSource(source.path, source.password);
    mervin::ExtractDialog locked(source);
    auto *password = locked.findChild<QLineEdit *>(QStringLiteral("extractPassword"));
    auto *accept = locked.findChild<QPushButton *>(QStringLiteral("extractAccept"));
    QVERIFY(password && accept);
    inspect(locked, QStringLiteral("Extract encrypted document"));
    auto choices = locked.choices();
    choices.password = QStringLiteral("incorrect-password");
    choices.failure = mervin::ExtractDialog::Choices::Failure::Password;
    mervin::ExtractDialog rejected(source, nullptr, choices);
    inspect(rejected, QStringLiteral("Extract rejected password"));
    choices.failure = mervin::ExtractDialog::Choices::Failure::ChangedCount;
    source.probe = {mervin::PageOps::Status::Ok, 12, {}, {}};
    mervin::ExtractDialog changed(source, nullptr, choices);
    inspect(changed, QStringLiteral("Extract changed page count"));

    source.path = profile_.filePath(QStringLiteral("missing-source.pdf"));
    source.probe = mervin::ExtractDialog::probeSource(source.path, source.password);
    mervin::ExtractDialog missing(source);
    inspect(missing, QStringLiteral("Extract unreadable document"));
}

// Document > Delete Pages: the Extract window in its Delete mode, with its own
// title, button, result line and errors.
/// Checks Delete controls with retained password, count-change and read failures.
void TstTranslationLayout::deletePages()
{
    mervin::ExtractDialog::Source source;
    source.mode = mervin::ExtractPlan::Mode::Delete;
    source.path = plain_;
    source.viewerPageCount = 12;
    source.seedPage = 6;
    source.hasUnsavedEdits = true;
    source.probe = mervin::ExtractDialog::probeSource(source.path, source.password);
    mervin::ExtractDialog dialog(source);
    inspect(dialog, QStringLiteral("Delete pages with unsaved changes"));
    auto *rows = dialog.findChild<QListWidget *>(QStringLiteral("extractList"));
    auto *error = dialog.findChild<QLabel *>(QStringLiteral("extractError"));
    auto *summary = dialog.findChild<QLabel *>(QStringLiteral("extractSummary"));
    QVERIFY(rows && error && summary);
    // The rows are rebuilt after a split, so each step looks the field up again.
    const auto field = [rows](int row) {
        QWidget *w = rows->itemWidget(rows->item(row));
        return w ? w->findChild<QLineEdit *>(QStringLiteral("extractRowSpec")) : nullptr;
    };
    QVERIFY(field(0));

    // Every other page: the longest result line, with the unsaved-changes note.
    field(0)->selectAll();
    field(0)->insert(QStringLiteral("2, 4, 6, 8, 10"));
    QTRY_COMPARE(rows->count(), 5);
    QVERIFY(!summary->text().isEmpty());
    inspect(dialog, QStringLiteral("Delete pages long result"));

    field(0)->selectAll();
    QTest::keyClicks(field(0), QStringLiteral("all"));
    QVERIFY(!error->text().isEmpty());
    QVERIFY(summary->text().isEmpty());
    inspect(dialog, QStringLiteral("Delete pages every page"));
    field(0)->selectAll();
    QTest::keyClicks(field(0), QStringLiteral("999999"));
    QVERIFY(!error->text().isEmpty());
    inspect(dialog, QStringLiteral("Delete pages invalid range"));
    // Each row's ✕ removes it on the next event-loop turn, and the rows are rebuilt.
    for (int left = rows->count(); left > 0; --left) {
        auto *x = rows->itemWidget(rows->item(0))->findChild<QToolButton *>(
            QStringLiteral("rowListX"));
        QVERIFY(x);
        x->click();
        QTRY_COMPARE(rows->count(), left - 1);
    }
    QVERIFY(!error->text().isEmpty());
    inspect(dialog, QStringLiteral("Delete pages no rows"));

    source.path = encrypted_;
    source.probe = mervin::ExtractDialog::probeSource(source.path, source.password);
    mervin::ExtractDialog locked(source);
    auto *password = locked.findChild<QLineEdit *>(QStringLiteral("extractPassword"));
    auto *accept = locked.findChild<QPushButton *>(QStringLiteral("extractAccept"));
    QVERIFY(password && accept);
    inspect(locked, QStringLiteral("Delete pages encrypted document"));
    auto choices = locked.choices();
    choices.password = QStringLiteral("incorrect-password");
    choices.failure = mervin::ExtractDialog::Choices::Failure::Password;
    mervin::ExtractDialog rejected(source, nullptr, choices);
    inspect(rejected, QStringLiteral("Delete pages rejected password"));
    choices.failure = mervin::ExtractDialog::Choices::Failure::ChangedCount;
    source.probe = {mervin::PageOps::Status::Ok, 12, {}, {}};
    mervin::ExtractDialog changed(source, nullptr, choices);
    inspect(changed, QStringLiteral("Delete pages changed page count"));

    source.path = profile_.filePath(QStringLiteral("missing-source.pdf"));
    source.probe = mervin::ExtractDialog::probeSource(source.path, source.password);
    mervin::ExtractDialog missing(source);
    inspect(missing, QStringLiteral("Delete pages unreadable document"));
}

void TstTranslationLayout::firstRun()
{
    for (const bool offerDefaultApp : {false, true}) {
        mervin::FirstRunDialog dialog(offerDefaultApp);
        inspect(dialog, offerDefaultApp ? QStringLiteral("Welcome with default-app option")
                                       : QStringLiteral("Welcome"));
        auto *download = dialog.findChild<QCheckBox *>(QStringLiteral("downloadOcr"));
        QVERIFY(download);
        QVERIFY(download->isChecked());
        download->setChecked(false);
        inspect(dialog, QStringLiteral("Welcome without OCR download"));
    }
}

void TstTranslationLayout::manageLanguages()
{
    {
        CatalogNetwork network(catalogPayload());
        mervin::ManageLanguagesDialog dialog(QStringLiteral("chi_tra"), nullptr, &network);
        QTRY_COMPARE(network.completed, 1);
        QTRY_VERIFY(!mervin::TessdataManager::busy()); // the installed languages
        QCOMPARE(network.requests, 1);
        inspect(dialog, QStringLiteral("OCR languages installed and available"));
        for (const char *name : {"ocrInstalledLanguages", "ocrAvailableLanguages"}) {
            auto *list = dialog.findChild<QListWidget *>(QLatin1String(name));
            QVERIFY(list);
            for (int index = 0; index < list->count(); ++index) {
                QListWidgetItem *item = list->item(index);
                QWidget *row = list->itemWidget(item);
                QVERIFY(row);
                list->scrollToItem(item, QAbstractItemView::PositionAtCenter);
                list->horizontalScrollBar()->setValue(list->horizontalScrollBar()->maximum());
                QCoreApplication::processEvents();
                const QRect allocation = list->visualItemRect(item);
                QVERIFY2(allocation.width() >= row->minimumSizeHint().width(),
                         "A language row must reserve its full width in the scrollable list.");
                QVERIFY2(allocation.height() >= row->minimumSizeHint().height(),
                         "A language row must reserve its full height without overlapping the next row.");
                auto *action = row->findChild<QPushButton *>(QStringLiteral("ocrLanguageAction"));
                QVERIFY(action);
                const QRect actionRect(action->mapTo(list->viewport(), QPoint()), action->size());
                QVERIFY2(list->viewport()->rect().contains(actionRect),
                         "Scrolling to a language row must reveal its entire action button.");
            }
            inspect(dialog, QStringLiteral("OCR language actions %1").arg(QLatin1String(name)));
            list->horizontalScrollBar()->setValue(0);
            list->verticalScrollBar()->setValue(0);
        }
        auto *search = dialog.findChild<QLineEdit *>(QStringLiteral("ocrLanguageSearch"));
        QVERIFY(search);
        search->setText(QStringLiteral("no-such-language"));
        inspect(dialog, QStringLiteral("OCR languages no search matches"));
    }
    for (const auto error : {QNetworkReply::NoError, QNetworkReply::HostNotFoundError}) {
        CatalogNetwork network(QByteArrayLiteral("invalid-json"), error);
        mervin::ManageLanguagesDialog dialog(QStringLiteral("eng"), nullptr, &network);
        QTRY_COMPARE(network.completed, 1);
        QTRY_VERIFY(!mervin::TessdataManager::busy()); // the installed languages
        QCOMPARE(network.requests, 1);
        inspect(dialog, error == QNetworkReply::NoError ? QStringLiteral("OCR catalog invalid")
                                                       : QStringLiteral("OCR catalog unavailable"));
    }
    for (const char *code : {"eng", "swe", "chi_tra"})
        QVERIFY(QFile::remove(modelPath(code)));
    CatalogNetwork network(catalogPayload());
    mervin::ManageLanguagesDialog empty(QString(), nullptr, &network);
    QTRY_COMPARE(network.completed, 1);
    QTRY_VERIFY(!mervin::TessdataManager::busy()); // the installed languages
    inspect(empty, QStringLiteral("OCR languages none installed"));
}

/// Checks translated Merge layouts while rows read, output checks wait and validation fails.
void TstTranslationLayout::merge()
{
    QTemporaryDir inputs;
    QVERIFY(inputs.isValid());
    for (const QString &name : {
             QStringLiteral("Building coordination report with structural revisions and updated floor plans.pdf"),
             QStringLiteral("BuildingCoordinationReportWithStructuralRevisionsAndUpdatedFloorPlans"
                            "WithStructuralRevisionsAndUpdatedFloorPlans.pdf")}) {
        const QString pending = inputs.filePath(name);
        QVERIFY(QFile::copy(plain_, pending));
        mervin::job::testing::holdWhen([pending](mervin::job::Step step, const QString &path) {
            return step == mervin::job::Step::Reading && path == pending;
        });
        const auto release = qScopeGuard([] { mervin::job::testing::release(); });
        mervin::MergeDialog reading(pending, 12);
        QTRY_VERIFY(mervin::job::testing::held() > 0);
        QVERIFY(reading.reading());
        inspect(reading, QStringLiteral("Merge reading a long input file name"));
        const auto *reason = reading.findChild<QLabel *>(QStringLiteral("mergeError"));
        QVERIFY(reason);
        // Elision may shorten the name, but its identity and direction stay visible.
        QVERIFY(reason->text().contains(QChar(0x2068)));
        QVERIFY(reason->text().contains(QChar(0x2069)));
        QVERIFY(reason->text().contains(QStringLiteral("Building")));
        QVERIFY(reason->text().contains(QStringLiteral(".pdf")));
        mervin::job::testing::release();
        QTRY_VERIFY(!reading.reading());
    }
    mervin::MergeDialog dialog(plain_, 12);
    dialog.addPaths({plain_});
    QTRY_VERIFY(!dialog.reading());
    inspect(dialog, QStringLiteral("Merge populated documents"));
    {
        // An encrypted document merged with another file shows the encryption note.
        mervin::MergeDialog mixed(encrypted_, 12, QStringLiteral("secret"));
        mixed.addPaths({plain_});
        QTRY_VERIFY(!mixed.reading());
        inspect(mixed, QStringLiteral("Merge encrypted and plain documents"));
        const auto *note = mixed.findChild<QLabel *>(QStringLiteral("mergeEncryptionNote"));
        QVERIFY(note && note->isVisible());
    }
    for (const auto conflict : {mervin::MergeDialog::Conflict::Changed,
                                mervin::MergeDialog::Conflict::CheckFailed}) {
        const QString output = profile_.filePath(QStringLiteral("merge-consent-output.pdf"));
        mervin::MergeDialog failed(dialog.plan(), output, QStringLiteral("Destination check failed"),
                                   -1, conflict);
        auto *reason = failed.findChild<QLabel *>(QStringLiteral("mergeError"));
        auto *retry = failed.findChild<QPushButton *>(QStringLiteral("mergeAccept"));
        QVERIFY(reason && retry);
        QTRY_COMPARE(reason->text(),
                     mervin::MergeDialog::failureSummary(dialog.plan(), output, -1, conflict));
        QTRY_VERIFY(retry->isEnabled());
        inspect(failed, conflict == mervin::MergeDialog::Conflict::Changed
                            ? QStringLiteral("Merge destination changed before publication")
                            : QStringLiteral("Merge destination could not be checked"));
    }
    {
        // A Save as check that takes long, as on a share that stopped answering.
        auto *output = dialog.findChild<QLineEdit *>(QStringLiteral("mergeOutput"));
        auto *error = dialog.findChild<QLabel *>(QStringLiteral("mergeError"));
        QVERIFY(output && error);
        auto release = qScopeGuard([] { mervin::SaveCheck::holdForTesting(false); });
        mervin::SaveCheck::holdForTesting(true);
        output->setText(profile_.filePath(QStringLiteral("held.pdf")));
        QTest::keyClick(output, Qt::Key_End); // an edit, which checks the new text
        QTest::keyClicks(output, QStringLiteral("x"));
        QTRY_COMPARE(error->text(), mervin::SaveCheck::checkingText());
        inspect(dialog, QStringLiteral("Merge checking the folder"));
    }
    QTRY_VERIFY(!dialog.findChild<mervin::SaveCheck *>()->pending());
    auto *range = dialog.findChild<QLineEdit *>(QStringLiteral("mergeRowSpec"));
    QVERIFY(range);
    range->selectAll();
    QTest::keyClicks(range, QStringLiteral("999999"));
    inspect(dialog, QStringLiteral("Merge invalid range"));
    inspectMessage(dialog, QStringLiteral("Merge unreadable-file warning"), [&] {
        dialog.addPaths({encrypted_, profile_.filePath(QStringLiteral("missing-source.pdf"))});
        QVERIFY(QTest::qWaitFor([&dialog] { return !dialog.reading(); }, 5000));
    });
    inspect(dialog, QStringLiteral("Merge encrypted and missing documents"));
    mervin::MergeDialog empty(QString(), 0);
    inspect(empty, QStringLiteral("Merge no documents"));
}

void TstTranslationLayout::ocr()
{
    mervin::OcrPopup populated({QStringLiteral("eng"), QStringLiteral("swe"),
                               QStringLiteral("chi_tra")}, QStringLiteral("chi_tra"));
    inspect(populated, QStringLiteral("OCR awaiting text"));
    populated.setRawText(QStringLiteral("The engineering report contains a long recognised sentence.\n"
                                       "施工图纸包含测量数据。\nMätningen följer ritningens skala."));
    inspect(populated, QStringLiteral("OCR recognised text"));
    mervin::OcrPopup empty({});
    inspect(empty, QStringLiteral("OCR no installed languages"));
}

void TstTranslationLayout::print()
{
    mervin::RenderEngine engine;
    auto document = engine.openDocument(QDir(QStringLiteral(MERVIN_FIXTURES_DIR))
                                           .filePath(QStringLiteral("long.pdf")));
    QVERIFY(document);
    QPrinter printer;
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setPageSize(QPageSize(QPageSize::A4));
    PrintDialog dialog(&printer, &engine, document.get(), 0, QPageLayout::Portrait, 10000,
                       QStringLiteral("structural-engineering-report"));
    auto *preview = dialog.findChild<mervin::PrintPreviewWidget *>();
    auto *custom = dialog.findChild<QRadioButton *>(QStringLiteral("printCustomPages"));
    auto *pages = dialog.findChild<QLineEdit *>(QStringLiteral("printPages"));
    auto *confirm = dialog.findChild<QPushButton *>(QStringLiteral("printConfirm"));
    QVERIFY(preview && custom && pages && confirm);
    dialog.setAttribute(Qt::WA_DontShowOnScreen);
    dialog.show();
    QTRY_VERIFY(confirm->isEnabled());
    QTRY_VERIFY(preview->isReady());
    inspect(dialog, QStringLiteral("Print 10000-page document"));
    custom->setChecked(true);
    pages->setText(QStringLiteral("10000,1,10000"));
    QTRY_VERIFY(confirm->isEnabled());
    QTRY_VERIFY(preview->isReady());
    inspect(dialog, QStringLiteral("Print custom page order"));
    const auto *pageLabel = dialog.findChild<QLabel *>(QStringLiteral("printPageLabel"));
    QVERIFY(pageLabel);
    QVERIFY2(preview->geometry().bottom() < pageLabel->geometry().top(),
             "Print navigation overlaps the preview at the selected dialog size");
    pages->setText(QStringLiteral("10001"));
    QTRY_VERIFY(!confirm->isEnabled());
    QVERIFY(!preview->isReady());
    inspect(dialog, QStringLiteral("Print invalid page selection"));
}

/// Checks Security information, retained choices and destination-consent messages in every language.
void TstTranslationLayout::security()
{
    mervin::SecurityDialog clear(plain_, mervin::QpdfService::Info{});
    inspect(clear, QStringLiteral("Security unencrypted document"));
    mervin::QpdfService::Info info;
    QCOMPARE(mervin::QpdfService().readInfo(encrypted_, QStringLiteral("secret"), info),
             mervin::QpdfService::Status::Ok);
    mervin::SecurityDialog protectedDocument(encrypted_, info);
    inspect(protectedDocument, QStringLiteral("Security restricted encrypted document"));
    mervin::SecurityDialog::Request choices;
    choices.output = QStringLiteral("/srv/nas/docs/chosen-encrypted-copy.pdf");
    choices.userPassword = QStringLiteral("chosen open password");
    choices.ownerPassword = QStringLiteral("chosen owner password");
    choices.algorithm = mervin::QpdfService::Algorithm::AES128;
    choices.permissions = {false, false, false, false};
    mervin::SecurityDialog failedRead(encrypted_, {},
        mervin::SecurityDialog::tr("A password is required."), choices);
    inspect(failedRead, QStringLiteral("Security failed information and retained choices"));

    const QStringList failures{
        mervin::SecurityDialog::tr("The destination changed. Try again to review it."),
        mervin::SecurityDialog::tr("Could not check the destination. Try again or choose another name.")};
    for (int index = 0; index < failures.size(); ++index) {
        QMessageBox warning(QMessageBox::Warning,
            mervin::SecurityDialog::tr("Operation failed"), failures.at(index), QMessageBox::Ok);
        inspect(warning, QStringLiteral("Security destination failure %1").arg(index));
    }
    QMessageBox replacement(QMessageBox::Question,
        mervin::SecurityDialog::tr("Document Security"), {},
        QMessageBox::Yes | QMessageBox::No);
    replacement.setTextFormat(Qt::PlainText);
    replacement.setDefaultButton(QMessageBox::No);
    replacement.ensurePolished();
    const QString name = QChar(0x2068) + mervin::elidedLabelText(replacement.fontMetrics(),
        QStringLiteral("Revision").repeated(30) + QStringLiteral(".pdf"), Qt::ElideMiddle,
        qMax(1, replacement.screen()->availableGeometry().width() / 2)) + QChar(0x2069);
    replacement.setText(mervin::SecurityDialog::tr("\"%1\" already exists. Replace it?").arg(name));
    inspect(replacement, QStringLiteral("Security replacement with long filename"));
}

/// Checks Split's replacement question and kept-file warning with a long folder and name.
void TstTranslationLayout::splitMessages()
{
    // MainWindow fits the folder and the name to half the screen like this.
    const QString folder = QStringLiteral("/srv/nas/projects/")
        + QStringLiteral("Construction").repeated(12) + QStringLiteral("/drawings/issued/split");
    QMessageBox replacement(QMessageBox::Question, MainWindow::tr("Split Pages"), {},
                            QMessageBox::Yes | QMessageBox::No);
    replacement.setTextFormat(Qt::PlainText);
    replacement.setDefaultButton(QMessageBox::No);
    replacement.ensurePolished();
    const int width = qMax(1, replacement.screen()->availableGeometry().width() / 2);
    replacement.setText(mervin::fitFileSentence([](const QString &, const QString &shown) {
        return MainWindow::tr("%n file(s) in %1 already exist. Replace them?", nullptr, 12)
            .arg(shown);
    }, {}, folder, replacement.fontMetrics(), width));
    inspect(replacement, QStringLiteral("Split replacement with long folder"));
    QMessageBox kept(QMessageBox::Warning, MainWindow::tr("Split Pages"), {}, QMessageBox::Ok);
    kept.setTextFormat(Qt::PlainText);
    kept.ensurePolished();
    kept.setText(mervin::fitFileSentence([](const QString &shown, const QString &) {
        return MainWindow::tr("\"%1\" changed after Split checked it, so Split stopped "
                              "without replacing it.").arg(shown);
    }, QStringLiteral("Revision").repeated(30) + QStringLiteral("-003.pdf"), {},
        kept.fontMetrics(), width));
    inspect(kept, QStringLiteral("Split kept a changed file with a long name"));
}

void TstTranslationLayout::settings()
{
    mervin::Settings values;
    values.uiLanguage = i18n::current();
    values.annotationAuthor = QStringLiteral("Construction drawing review and approval team");
    values.ocrDefaultLanguage = QStringLiteral("chi_tra");
    SettingsDialog::UpdateInfo updates;
    updates.checkAvailable = true;
    updates.canSelfUpdate = true;
    updates.lastCheck = QDateTime(QDate(2026, 9, 28), QTime(12, 30), QTimeZone::utc());
    SettingsDialog dialog(values, updates, SettingsDialog::Page::General);
    QTRY_VERIFY(!mervin::TessdataManager::busy()); // the installed OCR languages
    const auto pages = QMetaEnum::fromType<SettingsDialog::Page>();
    for (int i = 0; i < pages.keyCount(); ++i) {
        const auto page = static_cast<SettingsDialog::Page>(pages.value(i));
        dialog.showPage(page);
        QCOMPARE(dialog.currentPage(), page);
        inspect(dialog, QStringLiteral("Settings %1").arg(QLatin1String(pages.key(i))));
        for (auto *scroll : dialog.findChildren<QScrollArea *>()) {
            if (scroll->isVisibleTo(&dialog)) {
                scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
                inspect(dialog, QStringLiteral("Settings %1 scrolled to bottom")
                                    .arg(QLatin1String(pages.key(i))));
                scroll->verticalScrollBar()->setValue(0);
            }
        }
        if (page == SettingsDialog::Page::General) {
            auto *search = dialog.findChild<QComboBox *>(QStringLiteral("recentSearchScope"));
            QVERIFY(search);
            const int previous = search->currentIndex();
            const int all = search->findData(QStringLiteral("all"));
            QVERIFY(all >= 0);
            search->setCurrentIndex(all);
            inspect(dialog, QStringLiteral("Settings recent search names and contents"));
            search->setCurrentIndex(previous);
        }
    }
    auto portableUpdates = updates;
    portableUpdates.canSelfUpdate = false;
    SettingsDialog portable(values, portableUpdates, SettingsDialog::Page::Updates);
    QTRY_VERIFY(!mervin::TessdataManager::busy());
    inspect(portable, QStringLiteral("Settings updates without automatic installation"));
    dialog.showPage(SettingsDialog::Page::General);
    auto *duration = dialog.findChild<QSpinBox *>(QStringLiteral("unloadInactiveMinutes"));
    auto *never = dialog.findChild<QCheckBox *>(QStringLiteral("neverUnloadDocuments"));
    QVERIFY(duration && never);
    never->setChecked(true);
    inspect(dialog, QStringLiteral("Settings never unload"));
    never->setChecked(false);
    duration->findChild<QLineEdit *>()->clear();
    auto *hint = dialog.findChild<QLabel *>(QStringLiteral("unloadHint"));
    QVERIFY(hint && !hint->text().isEmpty());
    inspect(dialog, QStringLiteral("Settings invalid unload interval"));
    never->setChecked(true);

    // Exercise the actual translated save-failure message without touching a
    // profile on disk or starting the application restart path.
    connect(&dialog, &SettingsDialog::applyRequested, &dialog, [&dialog] {
        dialog.reportSaveFailure(QStringLiteral("The configuration folder could not be written. "
                                               "Check the folder permissions and try again."));
    });
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    QVERIFY(buttons && buttons->button(QDialogButtonBox::Apply));
    QVERIFY(buttons->button(QDialogButtonBox::Apply)->isEnabled());
    inspectMessage(dialog, QStringLiteral("Settings save failure"), [&] {
        buttons->button(QDialogButtonBox::Apply)->click();
    });
}

void TstTranslationLayout::toolPanels()
{
    mervin::MeasurePanel measure;
    measure.setScaleText(QStringLiteral("1 : 1000000"));
    measure.setReadout(QStringLiteral("123456.789 m²"));
    measure.setResetVisible(true);
    measure.setMeasurements({QStringLiteral("123456.789 m²"), QStringLiteral("98765.432 mm")});
    inspect(measure, QStringLiteral("Measurement tools with reset and results"));
    mervin::AnnotPanel annotations;
    inspect(annotations, QStringLiteral("Annotation tools"));
    // The comment card of a highlight without an author: the translated
    // "Annotation" and the date head it, above the empty comment and the colours.
    mervin::AnnotPopup card;
    mervin::Annotation highlight;
    highlight.type = mervin::AnnotType::Highlight;
    highlight.color = mervin::annot::palette().front();
    highlight.modifiedMs = QDateTime(QDate(2026, 10, 12), QTime(12, 0)).toMSecsSinceEpoch();
    card.showFor(highlight);
    inspect(card, QStringLiteral("Annotation card"));
}

QTEST_MAIN(TstTranslationLayout)
#include "tst_translation_layout.moc"
