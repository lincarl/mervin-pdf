#include "dialogs/ManageLanguagesDialog.h"

#include "net/UrlOpen.h"
#include "ocr/TessdataFile.h"
#include "ocr/TessdataManager.h"

#include <QDialogButtonBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <utility>

namespace mervin {
namespace {

const QUrl kCatalogUrl(QStringLiteral(
    "https://api.github.com/repos/tesseract-ocr/tessdata_best/contents"));

QWidget *makeRow(const QString &name, const QString &code, const QString &size,
                 const QString &buttonText, QWidget *parent,
                 const std::function<void()> &clicked, bool enabled)
{
    auto *row = new QWidget(parent);
    row->setObjectName(QStringLiteral("ocrLanguageRow"));
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(10, 4, 8, 4);
    layout->setSpacing(7);
    auto *nameLabel = new QLabel(name, row);
    nameLabel->setObjectName(QStringLiteral("ocrLanguageName"));
    layout->addWidget(nameLabel);
    auto *codeLabel = new QLabel(code, row);
    codeLabel->setObjectName(QStringLiteral("ocrLanguageCode"));
    layout->addWidget(codeLabel);
    layout->addStretch();
    if (!size.isEmpty()) {
        auto *sizeLabel = new QLabel(size, row);
        sizeLabel->setObjectName(QStringLiteral("ocrLanguageSize"));
        layout->addWidget(sizeLabel);
    }
    auto *button = new QPushButton(buttonText, row);
    button->setObjectName(QStringLiteral("ocrLanguageAction"));
    button->setEnabled(enabled);
    QObject::connect(button, &QPushButton::clicked, row, clicked);
    layout->addWidget(button);
    return row;
}

void addRow(QListWidget *list, QWidget *row)
{
    auto *item = new QListWidgetItem(list);
    item->setSizeHint(QSize(100, 39));
    list->setItemWidget(item, row);
}

} // namespace

ManageLanguagesDialog::ManageLanguagesDialog(const QString &defaultLanguage, QWidget *parent)
    : QDialog(parent), network_(new QNetworkAccessManager(this)),
      initialDefaultLanguage_(defaultLanguage)
{
    setWindowTitle(tr("Manage OCR Languages"));
    setMinimumSize(650, 430);
    resize(720, 470);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);

    auto *defaultRow = new QHBoxLayout;
    auto *defaultLabel = new QLabel(tr("Default language"), this);
    defaultLanguage_ = new QComboBox(this);
    defaultLanguage_->setObjectName(QStringLiteral("ocrDefaultLanguage"));
    defaultLanguage_->setAccessibleName(tr("Default OCR language"));
    defaultLabel->setBuddy(defaultLanguage_);
    defaultRow->addWidget(defaultLabel);
    defaultRow->addWidget(defaultLanguage_, 1);
    layout->addLayout(defaultRow);

    auto *columns = new QGridLayout;
    columns->setHorizontalSpacing(14);
    columns->setColumnMinimumWidth(0, 1);
    columns->setColumnMinimumWidth(1, 1);
    columns->setColumnStretch(0, 1);
    columns->setColumnStretch(1, 1);

    auto *installedColumn = new QVBoxLayout;
    installedColumn->setSpacing(7);
    installedHeading_ = new QLabel(this);
    installedHeading_->setObjectName(QStringLiteral("ocrLanguageHeading"));
    installedColumn->addWidget(installedHeading_);
    installedList_ = new QListWidget(this);
    installedList_->setObjectName(QStringLiteral("ocrInstalledLanguages"));
    installedList_->setSelectionMode(QAbstractItemView::NoSelection);
    installedColumn->addWidget(installedList_, 1);
    installedTotal_ = new QLabel(this);
    installedTotal_->setObjectName(QStringLiteral("ocrLanguageMeta"));
    installedColumn->addWidget(installedTotal_);
    columns->addLayout(installedColumn, 0, 0);

    auto *availableColumn = new QVBoxLayout;
    availableColumn->setSpacing(7);
    auto *heading = new QLabel(tr("Available - best models"), this);
    heading->setObjectName(QStringLiteral("ocrLanguageHeading"));
    availableColumn->addWidget(heading);
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("ocrLanguageSearch"));
    search_->setClearButtonEnabled(true);
    search_->setPlaceholderText(tr("Search languages or codes"));
    search_->setAccessibleName(tr("Search available OCR languages"));
    availableColumn->addWidget(search_);
    availableList_ = new QListWidget(this);
    availableList_->setObjectName(QStringLiteral("ocrAvailableLanguages"));
    availableList_->setSelectionMode(QAbstractItemView::NoSelection);
    availableColumn->addWidget(availableList_, 1);
    availableStatus_ = new QLabel(tr("Loading best-quality language models..."), this);
    availableStatus_->setObjectName(QStringLiteral("ocrLanguageMeta"));
    availableStatus_->setWordWrap(true);
    availableColumn->addWidget(availableStatus_);
    columns->addLayout(availableColumn, 0, 1);
    layout->addLayout(columns, 1);

    auto *buttons = new QDialogButtonBox(this);
    auto *openFolder = buttons->addButton(tr("Open Folder"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    connect(openFolder, &QPushButton::clicked, this, [] { TessdataManager::openFolder(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    connect(search_, &QLineEdit::textChanged, this, &ManageLanguagesDialog::applyFilter);
    refreshInstalled();
    fetchCatalog();
}

QString ManageLanguagesDialog::defaultLanguage() const
{
    return defaultLanguage_->currentData().toString();
}

void ManageLanguagesDialog::fetchCatalog()
{
    QNetworkRequest request = urlopen::makeRequest(kCatalogUrl);
    request.setRawHeader("Accept", QByteArrayLiteral("application/vnd.github+json"));
    request.setRawHeader("X-GitHub-Api-Version", QByteArrayLiteral("2022-11-28"));
    QNetworkReply *reply = network_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QByteArray payload = reply->readAll();
        const auto error = reply->error();
        const QString detail = reply->errorString();
        reply->deleteLater();
        if (error != QNetworkReply::NoError) {
            availableList_->addItem(tr("Language catalog unavailable"));
            availableStatus_->setText(tr("Check your connection, then reopen this dialog. %1")
                                          .arg(detail));
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(payload);
        if (!doc.isArray()) {
            availableStatus_->setText(tr("The language catalog returned an invalid response."));
            return;
        }
        for (const QJsonValue &value : doc.array()) {
            const QJsonObject object = value.toObject();
            const QString fileName = object.value(QStringLiteral("name")).toString();
            if (!fileName.endsWith(QStringLiteral(".traineddata")))
                continue;
            Language language;
            language.code = QFileInfo(fileName).completeBaseName();
            language.name = TessdataManager::languageName(language.code);
            language.size = qint64(object.value(QStringLiteral("size")).toDouble());
            language.url = object.value(QStringLiteral("download_url")).toString();
            const QUrl url(language.url);
            if (url.scheme() == QStringLiteral("https")
                && url.host() == QStringLiteral("raw.githubusercontent.com")
                && url.path().contains(QStringLiteral("/tessdata_best/")))
                catalog_.append(language);
        }
        std::sort(catalog_.begin(), catalog_.end(), [](const Language &a, const Language &b) {
            return a.name.localeAwareCompare(b.name) < 0;
        });
        rebuildLists();
    });
}

void ManageLanguagesDialog::refreshInstalled()
{
    installed_ = TessdataManager::installedLanguages();
    rebuildLists();
    rebuildDefaultLanguages();
}

void ManageLanguagesDialog::rebuildDefaultLanguages()
{
    QString selected = defaultLanguage_->currentData().toString();
    if (selected.isEmpty())
        selected = initialDefaultLanguage_;

    const QSignalBlocker blocker(defaultLanguage_);
    defaultLanguage_->clear();
    for (const QString &code : std::as_const(installed_))
        defaultLanguage_->addItem(TessdataManager::languageName(code), code);

    const int index = defaultLanguage_->findData(selected);
    defaultLanguage_->setCurrentIndex(index >= 0 ? index
                                                 : (defaultLanguage_->count() ? 0 : -1));
    defaultLanguage_->setEnabled(defaultLanguage_->count() > 0);
}

void ManageLanguagesDialog::rebuildLists()
{
    installedList_->clear();
    qint64 total = 0;
    for (const QString &code : std::as_const(installed_)) {
        const qint64 size = QFileInfo(QDir(TessdataManager::directory())
                                         .filePath(code + QStringLiteral(".traineddata"))).size();
        total += size;
        addRow(installedList_, makeRow(TessdataManager::languageName(code), code, formattedSize(size),
            code == busyCode_ ? tr("Working...") : tr("Remove"), installedList_,
            [this, code] { remove(code); }, busyCode_.isEmpty()));
    }
    if (installed_.isEmpty()) {
        auto *empty = new QListWidgetItem(tr("No OCR languages installed"), installedList_);
        empty->setFlags(Qt::NoItemFlags);
    }
    installedHeading_->setText(tr("Installed (%1)").arg(installed_.size()));
    installedTotal_->setText(tr("%1 total").arg(formattedSize(total)));
    applyFilter();
}

void ManageLanguagesDialog::applyFilter()
{
    availableList_->clear();
    const QString needle = search_->text().trimmed();
    int shown = 0;
    for (const Language &language : std::as_const(catalog_)) {
        if (installed_.contains(language.code)
            || (!needle.isEmpty() && !language.name.contains(needle, Qt::CaseInsensitive)
                && !language.code.contains(needle, Qt::CaseInsensitive)))
            continue;
        addRow(availableList_, makeRow(language.name, language.code,
            formattedSize(language.size), language.code == busyCode_ ? tr("Working...") : tr("Add"),
            availableList_, [this, language] { install(language); }, busyCode_.isEmpty()));
        ++shown;
    }
    if (catalog_.isEmpty())
        return;
    if (!shown) {
        auto *empty = new QListWidgetItem(tr("No matching languages"), availableList_);
        empty->setFlags(Qt::NoItemFlags);
    }
    availableStatus_->setText(tr("%1 best-quality model(s) available").arg(shown));
}

void ManageLanguagesDialog::install(const Language &language)
{
    setBusy(language.code, true);
    QNetworkReply *reply = network_->get(urlopen::makeRequest(QUrl(language.url)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, language] {
        const QByteArray data = reply->readAll();
        const auto error = reply->error();
        const QString detail = reply->errorString();
        reply->deleteLater();
        if (error != QNetworkReply::NoError) {
            setBusy(language.code, false);
            QMessageBox::warning(this, tr("OCR Language Download"),
                tr("Could not download %1.\n\n%2").arg(language.name, detail));
            return;
        }
        const QString path = QDir(TessdataManager::directory())
            .filePath(language.code + QStringLiteral(".traineddata"));
        QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size()
            || !output.commit()) {
            setBusy(language.code, false);
            QMessageBox::warning(this, tr("OCR Language Download"),
                tr("Could not save %1 in the OCR language folder.").arg(language.name));
            return;
        }
        QString validationError;
        if (!TessdataFile::validateLanguages(TessdataManager::directory(), {language.code},
                                             &validationError)) {
            QFile::remove(path);
            setBusy(language.code, false);
            QMessageBox::warning(this, tr("OCR Language Download"),
                tr("The downloaded model is not valid and was removed.\n\n%1")
                    .arg(validationError));
            return;
        }
        setBusy(language.code, false);
        refreshInstalled();
    });
}

void ManageLanguagesDialog::remove(const QString &code)
{
    const QString path = QDir(TessdataManager::directory())
        .filePath(code + QStringLiteral(".traineddata"));
    if (!QFile::remove(path)) {
        QMessageBox::warning(this, tr("Remove OCR Language"),
            tr("Could not remove %1 from the OCR language folder.")
                .arg(TessdataManager::languageName(code)));
        return;
    }
    refreshInstalled();
}

void ManageLanguagesDialog::setBusy(const QString &code, bool busy)
{
    busyCode_ = busy ? code : QString();
    search_->setEnabled(!busy);
    rebuildLists();
}

QString ManageLanguagesDialog::formattedSize(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

} // namespace mervin
