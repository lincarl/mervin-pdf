#pragma once

#include <QDialog>
#include <QList>
#include <QStringList>

class QLabel;
class QLineEdit;
class QListWidget;
class QComboBox;
class QNetworkAccessManager;

namespace mervin {

// Manages the profile-scoped OCR models. The catalog and downloads come only
// from Tesseract's best-quality repository; fast models are never offered.
class ManageLanguagesDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ManageLanguagesDialog(const QString &defaultLanguage,
                                   QWidget *parent = nullptr);

    // The language selected for the next OCR selection. Empty means that no
    // language remains installed.
    QString defaultLanguage() const;

private:
    struct Language { QString code, name, url; qint64 size = 0; };
    void fetchCatalog();
    void refreshInstalled();
    void rebuildDefaultLanguages();
    void rebuildLists();
    void applyFilter();
    void install(const Language &language);
    void remove(const QString &code);
    void setBusy(const QString &code, bool busy);
    static QString formattedSize(qint64 bytes);

    QNetworkAccessManager *network_ = nullptr;
    QComboBox *defaultLanguage_ = nullptr;
    QLineEdit *search_ = nullptr;
    QListWidget *installedList_ = nullptr;
    QListWidget *availableList_ = nullptr;
    QLabel *installedHeading_ = nullptr;
    QLabel *installedTotal_ = nullptr;
    QLabel *availableStatus_ = nullptr;
    QList<Language> catalog_;
    QStringList installed_;
    QString initialDefaultLanguage_;
    QString busyCode_;
};

} // namespace mervin
