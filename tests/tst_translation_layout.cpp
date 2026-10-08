#include "TextFit.h"

#include "config/ConfigPaths.h"
#include "config/Settings.h"
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
#include "print/PrintPreviewWidget.h"
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "security/QpdfService.h"
#include "ui/AnnotPanel.h"
#include "ui/LanguageCombo.h"
#include "ui/MeasurePanel.h"
#include "ui/Theme.h"
#include "ui/ThemeTokens.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
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
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QTimeZone>
#include <QTimer>
#include <QXmlStreamReader>

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
    void firstRun_data() { addLanguageMatrix(); }
    void firstRun();
    void languagePicker_data() { addLanguageMatrix(); }
    void languagePicker();
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
    void settings_data() { addLanguageMatrix(); }
    void settings();
    void toolPanels_data() { addLanguageMatrix(); }
    void toolPanels();

private:
    void inspect(QWidget &widget, const QString &state);
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

void TstTranslationLayout::inspect(QWidget &widget, const QString &state)
{
    widget.setAttribute(Qt::WA_DontShowOnScreen);
    widget.ensurePolished();
    widget.show();
    QTest::qWait(1);
    if (minimumSize_) {
        // Respect the actual layout minimum, including translated labels. A
        // fixed-size dialog has the same dimensions in both matrix entries.
        widget.resize(widget.minimumSize().expandedTo(widget.minimumSizeHint()));
        QTest::qWait(1);
    }
    const QStringList problems = textfit::check(widget);
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
// waiting indefinitely for a person to click a button.
void TstTranslationLayout::inspectMessage(QWidget &parent, const QString &state,
                                         const std::function<void()> &action)
{
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
        } else if (elapsed.elapsed() > 2000) {
            timedOut = true;
            if (modal)
                modal->reject();
        }
    });
    poll.start(10);
    action();
    poll.stop();
    QVERIFY2(!timedOut && sawMessage, qPrintable(state + QStringLiteral(" did not show the expected message")));
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

void TstTranslationLayout::extract()
{
    mervin::ExtractDialog::Source source;
    source.path = plain_;
    source.viewerPageCount = 12;
    source.currentPage = 6;
    source.hasUnsavedEdits = true;
    mervin::ExtractDialog dialog(source);
    inspect(dialog, QStringLiteral("Extract with unsaved changes"));
    auto *range = dialog.findChild<QLineEdit *>(QStringLiteral("extractRowSpec"));
    auto *error = dialog.findChild<QLabel *>(QStringLiteral("extractError"));
    QVERIFY(range && error);
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
    mervin::ExtractDialog locked(source);
    auto *password = locked.findChild<QLineEdit *>(QStringLiteral("extractPassword"));
    auto *accept = locked.findChild<QPushButton *>(QStringLiteral("extractAccept"));
    QVERIFY(password && accept);
    inspect(locked, QStringLiteral("Extract encrypted document"));
    QTest::keyClicks(password, QStringLiteral("incorrect-password"));
    accept->click();
    QCOMPARE(locked.result(), int(QDialog::Rejected));
    inspect(locked, QStringLiteral("Extract rejected password"));

    source.path = profile_.filePath(QStringLiteral("missing-source.pdf"));
    mervin::ExtractDialog missing(source);
    inspect(missing, QStringLiteral("Extract unreadable document"));
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
        QCOMPARE(network.requests, 1);
        inspect(dialog, error == QNetworkReply::NoError ? QStringLiteral("OCR catalog invalid")
                                                       : QStringLiteral("OCR catalog unavailable"));
    }
    for (const char *code : {"eng", "swe", "chi_tra"})
        QVERIFY(QFile::remove(modelPath(code)));
    CatalogNetwork network(catalogPayload());
    mervin::ManageLanguagesDialog empty(QString(), nullptr, &network);
    QTRY_COMPARE(network.completed, 1);
    inspect(empty, QStringLiteral("OCR languages none installed"));
}

void TstTranslationLayout::merge()
{
    mervin::MergeDialog dialog(plain_, 12);
    dialog.addPaths({plain_});
    inspect(dialog, QStringLiteral("Merge populated documents"));
    auto *range = dialog.findChild<QLineEdit *>(QStringLiteral("mergeRowSpec"));
    QVERIFY(range);
    range->selectAll();
    QTest::keyClicks(range, QStringLiteral("999999"));
    inspect(dialog, QStringLiteral("Merge invalid range"));
    inspectMessage(dialog, QStringLiteral("Merge unreadable-file warning"), [&] {
        dialog.addPaths({encrypted_, profile_.filePath(QStringLiteral("missing-source.pdf"))});
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

void TstTranslationLayout::security()
{
    mervin::SecurityDialog clear(plain_);
    inspect(clear, QStringLiteral("Security unencrypted document"));
    mervin::SecurityDialog protectedDocument(encrypted_, QStringLiteral("secret"));
    inspect(protectedDocument, QStringLiteral("Security restricted encrypted document"));
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
}

QTEST_MAIN(TstTranslationLayout)
#include "tst_translation_layout.moc"
