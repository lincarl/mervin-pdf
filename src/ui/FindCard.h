#pragma once

#include <QPoint>
#include <QWidget>

class QAction;
class QCheckBox;
class QLabel;
class QTimer;
class QToolButton;

namespace mervin {

class PanelStack;
class SearchLineEdit;

// Find in document, as a card floating over the top right of one tab's viewer. It
// holds the search field (with the match count at its right end), Previous and
// Next, Match case and Whole word, and Close. The card is a child of the viewer's
// viewport and is stacked with the Measure and Comment panels by PanelStack, so it
// keeps the same 12px inset and drags with them as a group.
//
// The card only emits intent; the owning TabPage connects it to the viewer. Each
// tab has its own card, so the query, options and open state are per tab.
class FindCard : public QWidget
{
    Q_OBJECT

public:
    explicit FindCard(QWidget *viewport);

    // The stack that owns this card's position (non-owning).
    void setStack(PanelStack *stack) { stack_ = stack; }

    // Show the card and focus the field with its text selected. A non-empty
    // `preset` (such as the selected text) replaces the query and searches. A
    // query kept from before the card was closed is searched again, because
    // closing cleared its highlights.
    void open(const QString &preset = QString());

    // Hide the card, as Escape and the close button do. Emits openChanged(false).
    void dismiss();

    // True while the card is open in its tab, even when the tab itself is hidden.
    bool isOpen() const { return !isHidden(); }

    QString query() const;
    bool caseSensitive() const;
    bool wholeWord() const;

public slots:
    // current is 1-based (0 = no current match); total is the match count.
    void setResultCount(int current, int total);

    // Step to the next or previous match, as Enter, Shift+Enter, the chevrons and
    // F3 do. Text typed since the last search is searched first instead.
    void next();
    void prev();

signals:
    // The query or an option changed (debounced while typing).
    void searchChanged(const QString &query, bool caseSensitive, bool wholeWord);
    void findNext(); // step the viewer's current match; see next()
    void findPrev();
    // The card was opened or closed by open() or dismiss(). Not emitted when the
    // whole tab is shown or hidden.
    void openChanged(bool open);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void emitSearch();
    void updateSearchIcon();
    void placeCount();

    SearchLineEdit *edit_ = nullptr;
    QAction *searchAction_ = nullptr; // leading magnifier inside edit_
    QLabel *count_ = nullptr;         // "3 of 12" inside edit_'s right end
    QToolButton *prevBtn_ = nullptr;
    QToolButton *nextBtn_ = nullptr;
    QCheckBox *caseCheck_ = nullptr;
    QCheckBox *wordCheck_ = nullptr;
    QToolButton *closeBtn_ = nullptr;
    QTimer *debounce_ = nullptr;
    bool dirty_ = false; // the field changed since the last search
    PanelStack *stack_ = nullptr;
    bool dragging_ = false;
    QPoint dragLastGlobal_;
};

} // namespace mervin
