#pragma once

#include "recent/RecentEntry.h"
#include "ui/RecentSearchField.h"

#include <QHash>
#include <QList>
#include <QStringList>
#include <QWidget>

class QLabel;
class QListWidget;
class QToolButton;
class QListWidgetItem;
class QTimer;

namespace mervin {

// Recent history, newest first. Starred files sit in a "Favourites" section at the
// top, followed by the rest under "Recent" (no captions while nothing is starred).
// The search field above the list searches file names, inside the documents, or
// both (names first, then contents); content-search results stream in through
// addContentHit(). Missing files offer Locate/Remove/Cancel.
class RecentFilesPanel : public QWidget
{
    Q_OBJECT

public:
    using Scope = RecentSearchField::Scope;

    explicit RecentFilesPanel(QWidget *parent = nullptr);

    // Replace the displayed history (entries most-recent first).
    void setEntries(const QList<RecentEntry> &entries);

    // How many non-starred entries to show when the search is empty (spec default
    // 100). Starred files are always shown.
    void setVisibleCount(int count);

    // The search scope from Settings: applied now, and kept until the user picks
    // another one or the setting changes.
    void setDefaultScope(Scope scope);

    // Focus the search field with its text selected (opening Recent, Ctrl+F).
    void focusSearch();

    // Move keyboard focus to the list, keeping the search (Escape or Down in the
    // field). The first file becomes current if none is, so arrows and Enter work.
    void focusList();

    // One-line summary of the current listing (e.g. "Your last 12 opened
    // documents"). Shown by the window in the status bar while Recent is active.
    QString statusSummary() const { return statusSummary_; }

public slots:
    // Content-search results streamed back by the window's ContentSearch.
    // `snippet` is a short preview of the matching text (may be empty).
    void addContentHit(const QString &path, int page, const QString &snippet); // page is 1-based
    void setContentProgress(int scanned, int total);
    void endContentSearch(bool canceled, int matched);

signals:
    void openRequested(const QString &path);
    void openInNewWindowRequested(const QString &path);
    void removeRequested(const QString &path);
    // Search inside the files at `paths` for `query`; hits come back through
    // addContentHit().
    void contentSearchRequested(const QString &query, const QStringList &paths);
    void contentSearchCanceled();
    // Emitted after every rebuild with the number of files listed.
    void countChanged(int count);
    // Emitted after every rebuild with the one-line listing summary, so the
    // window can display it in the status bar (where file paths appear).
    void statusSummaryChanged(const QString &text);
    // User toggled the favourite star on a file.
    void favoriteToggled(const QString &path, bool isFavorite);
    // User requested stale entries be removed from the current visible filter scope.
    void clearMissingRequested(const QStringList &paths);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    struct ContentHit
    {
        int page = 0; // 1-based
        QString snippet;
    };

    // Draws the whole list from entries_ and the content hits so far, so a history
    // change (remove, clear missing, a star from another window) reaches every
    // scope. Hits that arrive while a scan runs are added to the list directly.
    void rebuild();
    void updateSummary(int listed);
    void onSearchChanged();
    void startContentSearch();
    // Stop button: ends the scan and keeps what it found.
    void stopSearch();
    // While a scan is pending or running, the status is in full ink and Stop shows.
    // Both return to rest when it ends.
    void setRunning(bool running);
    void showScanProgress();
    bool contentMode() const; // Contents or All with a query: a scan runs or ran
    QString needle() const;
    bool nameMatches(const RecentEntry &entry) const;
    bool listedAsFavourite(const RecentEntry &entry) const;
    const RecentEntry *entryFor(const QString &path) const;
    void onItemActivated(QListWidgetItem *item);
    void handleMissingFile(const QString &path);
    void toggleItemFavorite(QListWidgetItem *item);
    QStringList missingFilesInCurrentFilter() const;

    // Sections. rebuild() clears them; insertResult() adds a file row to its
    // section, creating the caption on first use and dropping any empty-list note.
    void clearList();
    void insertResult(QListWidgetItem *item, bool favourite);
    void showEmptyNote(const QString &text);
    QListWidgetItem *makeFileItem(const RecentEntry &entry);
    QListWidgetItem *makeHitItem(const RecentEntry &entry, const ContentHit &hit);

    RecentSearchField *search_ = nullptr;
    QLabel *status_ = nullptr;
    QToolButton *stopBtn_ = nullptr;
    QListWidget *list_ = nullptr;
    QTimer *debounce_ = nullptr;
    QList<RecentEntry> entries_;
    int visibleCount_ = 100;
    bool searching_ = false;
    QString statusSummary_;

    QListWidgetItem *favCaption_ = nullptr;
    QListWidgetItem *recentCaption_ = nullptr;
    QListWidgetItem *emptyNote_ = nullptr;
    int favRows_ = 0;
    int recentRows_ = 0;
    int nameRows_ = 0; // name matches listed before a scan of the All scope
    // Content hits for the current search, in the order they arrived.
    QStringList hitOrder_;
    QHash<QString, ContentHit> hits_;
    bool scanDone_ = false;     // the scan for the current search finished
    bool rescanOnShow_ = false; // a scan was stopped or held back while Recent was hidden
    int scanned_ = 0;   // files finished in the running scan
    int scanTotal_ = 0; // files it reads
    // A row whose star was just toggled stays in the section it was shown in
    // until the search changes or Recent is left, so it never jumps away from
    // the pointer. Maps path -> listed as a favourite.
    QHash<QString, bool> stickySection_;
};

} // namespace mervin
