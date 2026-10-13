#include "dialogs/ManageLanguagesDialog.h"

#include "dialogs/LayoutMinimum.h"
#include "net/UrlOpen.h"
#include "ocr/TessdataManager.h"
#include "ui/TextLayout.h"

#include <QDialogButtonBox>
#include <QComboBox>
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
    // The styled controls determine the row height and full text width. A
    // narrower list can scroll horizontally instead of squeezing its labels.
    row->ensurePolished();
    QSize rowSize = row->sizeHint();
    rowSize.setHeight(qMax(39, rowSize.height()));
    auto *item = new QListWidgetItem(list);
    item->setSizeHint(rowSize);
    list->setItemWidget(item, row);
}

} // namespace

ManageLanguagesDialog::ManageLanguagesDialog(const QString &defaultLanguage, QWidget *parent,
                                           QNetworkAccessManager *network)
    : QDialog(parent), network_(network ? network : new QNetworkAccessManager(this)),
      initialDefaultLanguage_(defaultLanguage)
{
    setWindowTitle(tr("Manage OCR Languages"));
    resize(720, 470);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);

    auto *defaultRow = new QHBoxLayout;
    //: Label for the OCR language that new OCR selections start with.
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
    //: Column heading. The list holds Tesseract's best-quality OCR models.
    auto *heading = new QLabel(tr("Available - best models"), this);
    heading->setObjectName(QStringLiteral("ocrLanguageHeading"));
    availableColumn->addWidget(heading);
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("ocrLanguageSearch"));
    search_->setClearButtonEnabled(true);
    search_->setPlaceholderText(tr("Search languages or codes"));
    fitPlaceholder(*search_);
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
    buttons->addButton(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    connect(search_, &QLineEdit::textChanged, this, &ManageLanguagesDialog::applyFilter);
    refreshInstalled();
    fetchCatalog();
    followLayoutMinimum(*this, QSize(650, 430));
}

QString ManageLanguagesDialog::defaultLanguage() const
{
    if (!listed_)
        return initialDefaultLanguage_;
    const QString selected = defaultLanguage_->currentData().toString();
    if (selected.isEmpty() || selected != removing_)
        return selected;
    for (const TessdataManager::Model &model : installed_)
        if (model.code != removing_)
            return model.code;
    return {};
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
            //: %1 is the network error message.
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
    TessdataManager::listModels(this, [this](const QList<TessdataManager::Model> &models) {
        showInstalled(models);
    });
}

void ManageLanguagesDialog::showInstalled(const QList<TessdataManager::Model> &models)
{
    installed_ = models;
    listed_ = true;
    rebuildLists();
    rebuildDefaultLanguages();
}

bool ManageLanguagesDialog::isInstalled(const QString &code) const
{
    return std::any_of(installed_.begin(), installed_.end(),
                       [&code](const TessdataManager::Model &model) { return model.code == code; });
}

void ManageLanguagesDialog::rebuildDefaultLanguages()
{
    QString selected = defaultLanguage_->currentData().toString();
    if (selected.isEmpty())
        selected = initialDefaultLanguage_;

    const QSignalBlocker blocker(defaultLanguage_);
    defaultLanguage_->clear();
    for (const TessdataManager::Model &model : std::as_const(installed_))
        defaultLanguage_->addItem(TessdataManager::languageName(model.code), model.code);

    const int index = defaultLanguage_->findData(selected);
    defaultLanguage_->setCurrentIndex(index >= 0 ? index
                                                 : (defaultLanguage_->count() ? 0 : -1));
    defaultLanguage_->setEnabled(defaultLanguage_->count() > 0);
    // The languages arrive after the dialog is shown (see refreshInstalled), so the
    // combo's room for the longest one is set here.
    fitComboText(*defaultLanguage_);
}

void ManageLanguagesDialog::rebuildLists()
{
    installedList_->clear();
    qint64 total = 0;
    for (const TessdataManager::Model &model : std::as_const(installed_)) {
        const QString code = model.code;
        const qint64 size = model.bytes;
        total += size;
        //: Button on an installed OCR language. It deletes the language data.
        const QString removeText = tr("Remove");
        //: Button text while a language downloads or is removed.
        addRow(installedList_, makeRow(TessdataManager::languageName(code), code, formattedSize(size),
            code == busyCode_ ? tr("Working...") : removeText, installedList_,
            [this, code] { remove(code); }, busyCode_.isEmpty()));
    }
    if (listed_ && installed_.isEmpty()) {
        auto *empty = new QListWidgetItem(tr("No OCR languages installed"), installedList_);
        empty->setFlags(Qt::NoItemFlags);
    }
    // Nothing about the installed languages until they have been read.
    //: Column heading. %1 is the number of installed OCR languages.
    installedHeading_->setText(listed_ ? tr("Installed (%1)").arg(installed_.size()) : QString());
    //: %1 is the combined size of the installed OCR languages, such as "48.2 MB".
    installedTotal_->setText(listed_ ? tr("%1 total").arg(formattedSize(total)) : QString());
    applyFilter();
}

void ManageLanguagesDialog::applyFilter()
{
    availableList_->clear();
    const QString needle = search_->text().trimmed();
    int shown = 0;
    for (const Language &language : std::as_const(catalog_)) {
        // The name in the UI language, its English name or the code.
        if (isInstalled(language.code)
            || (!needle.isEmpty() && !language.name.contains(needle, Qt::CaseInsensitive)
                && !TessdataManager::englishLanguageName(language.code)
                        .contains(needle, Qt::CaseInsensitive)
                && !language.code.contains(needle, Qt::CaseInsensitive)))
            continue;
        //: Button on an available OCR language. It downloads and installs it.
        const QString addText = tr("Add");
        // Add waits for the installed languages: one listed here may be installed.
        addRow(availableList_, makeRow(language.name, language.code,
            formattedSize(language.size), language.code == busyCode_ ? tr("Working...") : addText,
            availableList_, [this, language] { install(language); },
            busyCode_.isEmpty() && listed_));
        ++shown;
    }
    if (catalog_.isEmpty())
        return;
    if (!shown) {
        auto *empty = new QListWidgetItem(tr("No matching languages"), availableList_);
        empty->setFlags(Qt::NoItemFlags);
    }
    //: %n is the number of OCR language models listed.
    availableStatus_->setText(tr("%n best-quality model(s) available", nullptr, shown));
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
                //: %1 is an OCR language name, %2 the network error message.
                tr("Could not download %1.\n\n%2").arg(language.name, detail));
            return;
        }
        // Saved and checked off the UI thread, as the profile can be on a share.
        TessdataManager::saveModel(
            language.code, data, this,
            [this, language](TessdataManager::Saved saved, const QString &detail,
                             const QList<TessdataManager::Model> &models) {
                busyCode_.clear(); // showInstalled() rebuilds the rows without it
                search_->setEnabled(true);
                showInstalled(models);
                if (saved == TessdataManager::Saved::Failed) {
                    QMessageBox::warning(this, tr("OCR Language Download"),
                        //: %1 is an OCR language name.
                        tr("Could not save %1 in the OCR language folder.").arg(language.name));
                } else if (saved == TessdataManager::Saved::Invalid) {
                    QMessageBox::warning(this, tr("OCR Language Download"),
                        //: %1 explains what is wrong with the file.
                        tr("The downloaded model is not valid and was removed.\n\n%1")
                            .arg(detail));
                }
            });
    });
}

void ManageLanguagesDialog::remove(const QString &code)
{
    removing_ = code;
    setBusy(code, true);
    TessdataManager::removeModel(
        code, this, [this, code](bool removed, const QList<TessdataManager::Model> &models) {
            removing_.clear();
            busyCode_.clear(); // showInstalled() rebuilds the rows without it
            search_->setEnabled(true);
            showInstalled(models);
            if (!removed) {
                QMessageBox::warning(this, tr("Remove OCR Language"),
                    //: %1 is an OCR language name.
                    tr("Could not remove %1 from the OCR language folder.")
                        .arg(TessdataManager::languageName(code)));
            }
        });
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
