#include "ui/LanguageCombo.h"

#include "i18n/UiLanguage.h"
#include "ui/Icons.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QScreen>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>

namespace mervin {

namespace {

constexpr int kCodeRole = Qt::UserRole;
constexpr int kSearchRole = Qt::UserRole + 1;
constexpr int kMaxVisibleRows = 8;
constexpr int kPopupGap = 4; // between the combo and the popup

// Search key: compatibility-decomposed, combining marks dropped, case-folded,
// so "francais" finds "Français" and "SVENSKA" finds "Svenska".
QString folded(const QString &text)
{
    const QString decomposed = text.normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar c : decomposed)
        if (c.category() != QChar::Mark_NonSpacing)
            out.append(c);
    return out.toCaseFolded();
}

} // namespace

LanguageCombo::LanguageCombo(QWidget *parent)
    : QComboBox(parent)
    , model_(new QStandardItemModel(this))
    , proxy_(new QSortFilterProxyModel(this))
{
    for (const QString &code : i18n::availableLanguages()) {
        auto *item = new QStandardItem(i18n::displayName(code));
        item->setData(code, kCodeRole);
        model_->appendRow(item);
    }
    setModel(model_);
    proxy_->setSourceModel(model_);
    proxy_->setFilterRole(kSearchRole);

    // A translucent top-level holding a styled card (QFrame#languagePopup in
    // Theme), so the card's corners can be rounded.
    popup_ = new QFrame(this, Qt::Popup);
    popup_->setAttribute(Qt::WA_TranslucentBackground);
    auto *popupLayout = new QVBoxLayout(popup_);
    popupLayout->setContentsMargins(0, 0, 0, 0);
    auto *card = new QFrame(popup_);
    card->setObjectName(QStringLiteral("languagePopup"));
    popupLayout->addWidget(card);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(6, 6, 6, 6);
    cardLayout->setSpacing(6);

    search_ = new QLineEdit(card);
    search_->setObjectName(QStringLiteral("uiLanguageSearch"));
    searchIcon_ = search_->addAction(QIcon(), QLineEdit::LeadingPosition);
    cardLayout->addWidget(search_);

    list_ = new QListView(card);
    list_->setObjectName(QStringLiteral("uiLanguageList"));
    list_->setModel(proxy_);
    list_->setFrameShape(QFrame::NoFrame);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // Fallback fonts for different writing systems can have different heights.
    list_->setUniformItemSizes(false);
    list_->viewport()->setAutoFillBackground(false);
    cardLayout->addWidget(list_);

    connect(search_, &QLineEdit::textChanged, this, [this](const QString &text) {
        proxy_->setFilterFixedString(folded(text.trimmed()));
        if (proxy_->rowCount() > 0)
            list_->setCurrentIndex(proxy_->index(0, 0));
    });
    connect(list_, &QListView::clicked, this, &LanguageCombo::pick);
    connect(this, &QComboBox::activated, this,
            [this](int row) { emit languagePicked(itemData(row, kCodeRole).toString()); });
    search_->installEventFilter(this);
    list_->installEventFilter(this);

    retranslate();
    tintSearchIcon();
}

QString LanguageCombo::language() const
{
    return currentData(kCodeRole).toString();
}

void LanguageCombo::setLanguage(const QString &code)
{
    const int row = findData(i18n::normalized(code, i18n::availableLanguages()), kCodeRole);
    setCurrentIndex(row >= 0 ? row : 0);
}

QSize LanguageCombo::fitLabelHeight(QSize hint) const
{
    const QFontMetrics metrics(font());
    int textHeight = metrics.height();
    for (int row = 0; row < count(); ++row)
        textHeight = std::max(textHeight, metrics.size(Qt::TextSingleLine, itemText(row)).height());
    // QComboBox sizes the closed field from the primary font. Preserve its
    // style padding while allowing taller fallback fonts in every choice.
    hint.rheight() += textHeight - metrics.height();
    return hint;
}

QSize LanguageCombo::sizeHint() const
{
    return fitLabelHeight(QComboBox::sizeHint());
}

QSize LanguageCombo::minimumSizeHint() const
{
    return fitLabelHeight(QComboBox::minimumSizeHint());
}

void LanguageCombo::showPopup()
{
    if (!isEnabled() || count() == 0)
        return;
    search_->clear();
    list_->setCurrentIndex(proxy_->mapFromSource(model_->index(currentIndex(), 0)));
    popup_->show(); // first, so the list's row height includes the stylesheet padding
    fitPopup();
    placePopup();
    popup_->raise();
    popup_->activateWindow();
    search_->setFocus(Qt::PopupFocusReason);
}

void LanguageCombo::hidePopup()
{
    // Hiding the popup while its search field has focus moves focus to the next
    // widget inside the popup, which then keeps it. Hand it back to the combo.
    const bool hadFocus = popup_->isAncestorOf(QApplication::focusWidget());
    popup_->hide();
    QComboBox::hidePopup();
    if (hadFocus) {
        window()->activateWindow();
        setFocus(Qt::PopupFocusReason);
    }
}

bool LanguageCombo::isPopupVisible() const
{
    return popup_->isVisible();
}

void LanguageCombo::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange)
        retranslate();
    QComboBox::changeEvent(event);
}

// The search field keeps the keyboard: list keys typed there move the list,
// and text typed while the list has focus goes back to the search.
bool LanguageCombo::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != search_ && watched != list_)
        return QComboBox::eventFilter(watched, event);
    if (watched == search_
        && (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange))
        tintSearchIcon();
    if (event->type() != QEvent::KeyPress)
        return QComboBox::eventFilter(watched, event);

    auto *key = static_cast<QKeyEvent *>(event);
    switch (key->key()) {
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        if (watched == search_) {
            QCoreApplication::sendEvent(list_, event);
            return true;
        }
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (list_->currentIndex().isValid())
            pick(list_->currentIndex());
        return true;
    case Qt::Key_Escape:
        hidePopup();
        return true;
    default:
        if (watched == list_ && !key->text().isEmpty() && key->text().at(0).isPrint()) {
            search_->setFocus(Qt::OtherFocusReason);
            QCoreApplication::sendEvent(search_, event);
            return true;
        }
        break;
    }
    return QComboBox::eventFilter(watched, event);
}

void LanguageCombo::retranslate()
{
    search_->setPlaceholderText(tr("Search languages"));
    search_->setAccessibleName(tr("Search languages"));
    //: Accessible name of the list of UI languages below the search field.
    list_->setAccessibleName(tr("Languages"));
    // The search also matches the name in the UI language, which just changed.
    for (int row = 0; row < model_->rowCount(); ++row) {
        QStandardItem *item = model_->item(row);
        item->setData(folded(i18n::searchText(item->data(kCodeRole).toString())), kSearchRole);
    }
}

void LanguageCombo::tintSearchIcon()
{
    searchIcon_->setIcon(icons::glyph(icons::Glyph::Search,
                                      search_->palette().color(QPalette::PlaceholderText)));
}

// The popup keeps one height while typing (room for every language, up to
// kMaxVisibleRows) and is at least as wide as the combo and its widest entry.
void LanguageCombo::fitPopup()
{
    list_->ensurePolished();
    // Lay the rows out again with the stylesheet's item padding. Reparenting an
    // ancestor (Settings moves each page into a scroll area) makes the list lay
    // out at once, before it is polished, and those rows stay without padding.
    list_->doItemsLayout();
    // Size for the tallest writing system's fallback font.
    int rowHeight = 0;
    for (int row = 0; row < proxy_->rowCount(); ++row)
        rowHeight = std::max(rowHeight, list_->sizeHintForRow(row));
    if (rowHeight <= 0)
        rowHeight = list_->fontMetrics().height() + 12;
    const int rows = std::clamp(model_->rowCount(), 1, kMaxVisibleRows);
    list_->setFixedHeight(rows * rowHeight + 2 * list_->frameWidth());

    int textWidth = 0;
    for (int row = 0; row < model_->rowCount(); ++row)
        textWidth = std::max(textWidth, list_->fontMetrics().horizontalAdvance(
                                            model_->item(row)->text()));
    const int chrome = 6 * 2 + 8 * 2 + 4; // card margins, item padding, slack
    const int scrollBar = model_->rowCount() > kMaxVisibleRows
        ? style()->pixelMetric(QStyle::PM_ScrollBarExtent) : 0;
    popup_->setFixedWidth(std::max(width(), textWidth + chrome + scrollBar));
    popup_->adjustSize();
}

// Below the combo, or above it when the screen has no room below.
void LanguageCombo::placePopup()
{
    // The screen under the combo, which can differ from its window's screen.
    QScreen *under = QGuiApplication::screenAt(mapToGlobal(rect().center()));
    if (!under)
        under = screen();
    const QRect area = under ? under->availableGeometry() : QRect();
    const QPoint below = mapToGlobal(QPoint(0, height() + kPopupGap));
    QPoint pos = below;
    if (area.isValid()) {
        if (below.y() + popup_->height() > area.bottom())
            pos.setY(mapToGlobal(QPoint(0, 0)).y() - kPopupGap - popup_->height());
        pos.setX(std::clamp(pos.x(), area.left(), std::max(area.left(), area.right() - popup_->width())));
    }
    popup_->move(pos);
}

void LanguageCombo::pick(const QModelIndex &proxyIndex)
{
    const int row = proxy_->mapToSource(proxyIndex).row();
    hidePopup();
    if (row < 0)
        return;
    setCurrentIndex(row);
    emit activated(row);
    emit pickedFromList();
}

} // namespace mervin
