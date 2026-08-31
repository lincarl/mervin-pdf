#pragma once

#include <QWidget>

class QAbstractButton;
class QAction;
class QButtonGroup;
class QCheckBox;
class QLabel;
class QLineEdit;
class QTimer;
class QToolButton;

namespace mervin {

// Always-visible search bar that adapts to the active view.
//
// FindDocument mode (a PDF is open): query field + Previous/Next navigation +
//   match count + Match case / Whole word toggles.
//
// RecentSearch mode (the Recent/home panel is shown): query field +
//   Name / Inside-documents segmented toggle.
//   The document-specific controls are hidden in this mode.
class FindBar : public QWidget
{
    Q_OBJECT

public:
    enum class Mode { FindDocument, RecentSearch };

    explicit FindBar(QWidget *parent = nullptr);

    // Switch between document-find and recent-search layouts.
    void setMode(Mode mode);

    // Focus the field. In FindDocument mode, seeds the query from `preset`
    // (e.g. the current selection) if non-empty. In RecentSearch mode, preset
    // is ignored and the existing query is kept so the filter is not cleared.
    void activate(const QString &preset = QString());

    QString query() const;
    bool caseSensitive() const;
    bool wholeWord() const;

    // Restore the bar to a tab's saved search state without re-running a search
    // (the viewer already holds its matches). Used when switching document tabs
    // so each tab keeps its own query, options and result count.
    void restoreFindState(const QString &query, bool caseSensitive, bool wholeWord,
                          int current, int total);

public slots:
    // current is 1-based (0 = no current match); total is the match count.
    void setResultCount(int current, int total);

signals:
    // Emitted in FindDocument mode when the query or options change.
    void searchChanged(const QString &query, bool caseSensitive, bool wholeWord);
    void findNext();
    void findPrev();
    void escapePressed();

    // Emitted in RecentSearch mode when the query or scope toggle changes.
    void recentFilterChanged(const QString &query, bool contentSearch);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override; // re-tint the search glyph on theme change

private slots:
    void emitSearch();
    void onTextChanged();

private:
    // Sets the search-field placeholder for the active recent-search scope.
    void updateRecentPlaceholder();
    // (Re)render the leading magnifier glyph in the current (muted) text colour.
    void updateSearchIcon();

    // Shared
    QLineEdit *edit_ = nullptr;
    QAction *searchAction_ = nullptr; // leading magnifier glyph inside edit_
    QTimer *debounce_ = nullptr;
    bool dirty_ = false;
    Mode mode_ = Mode::FindDocument;

    // FindDocument controls
    QToolButton *prevBtn_ = nullptr;
    QToolButton *nextBtn_ = nullptr;
    QLabel *countLabel_ = nullptr;
    QCheckBox *caseCheck_ = nullptr;
    QCheckBox *wordCheck_ = nullptr;

    // RecentSearch controls
    // The bar is shared, but the two modes are separate search contexts: a
    // document tab gets its query back through restoreFindState(), so Recent
    // needs somewhere of its own to park one while a document is on screen.
    QString recentQuery_;
    QWidget *recentControls_ = nullptr;
    QToolButton *nameBtn_ = nullptr;
    QToolButton *contentsBtn_ = nullptr;
    QButtonGroup *scopeGroup_ = nullptr;
};

} // namespace mervin
