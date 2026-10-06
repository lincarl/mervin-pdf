#pragma once

#include <QComboBox>

class QAction;
class QFrame;
class QLineEdit;
class QListView;
class QModelIndex;
class QSortFilterProxyModel;
class QStandardItemModel;

namespace mervin {

// The UI language picker, shared by the first-run window and Settings. Closed,
// it is an ordinary drop-down of the languages this build ships, each shown as
// "Svenska (Swedish)". Opening it shows a search field above the list: typing
// filters on the native and English names, the catalog ID and the name in the
// current UI language, ignoring case and accents. Arrow keys move in the list,
// Enter picks and Esc closes only the list.
class LanguageCombo : public QComboBox
{
    Q_OBJECT

public:
    explicit LanguageCombo(QWidget *parent = nullptr);

    // Catalog ID of the selected language ("en", "sv", "zh_CN").
    QString language() const;
    // Selects `code`; an ID this build doesn't ship selects English.
    void setLanguage(const QString &code);

    void showPopup() override;
    void hidePopup() override;

    bool isPopupVisible() const;
    QLineEdit *searchField() const { return search_; }
    QListView *listView() const { return list_; }

signals:
    // The user chose a language, from the list, the keyboard or the mouse wheel.
    // setLanguage() doesn't emit it.
    void languagePicked(const QString &code);

protected:
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void retranslate();
    void tintSearchIcon();
    void fitPopup();
    void placePopup();
    void pick(const QModelIndex &proxyIndex);

    QStandardItemModel *model_ = nullptr;
    QSortFilterProxyModel *proxy_ = nullptr;
    QFrame *popup_ = nullptr;
    QLineEdit *search_ = nullptr;
    QAction *searchIcon_ = nullptr;
    QListView *list_ = nullptr;
};

} // namespace mervin
