#pragma once

#include "recent/RecentEntry.h"

#include <QList>
#include <QStringList>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QTimer;
class QToolButton;

namespace mervin {

// The recent-files home panel. Lists the recent history (most-recent first).
// Files no longer on disk are greyed out; activating one offers
// Locate / Remove from history / Cancel.
//
// Filtering and scope selection are driven externally via setSearch() - the
// internal filter field and checkbox are functional but not shown in the UI;
// they live in the global adaptive FindBar instead.
//
// Content search is on-demand (M7): results stream in through addContentHit().
//
// Each row shows a star icon (right edge) that toggles the favourite flag;
// clicking it emits favoriteToggled(). A Recent / Favorites toggle below the
// heading filters the list to show only starred entries.
class RecentFilesPanel : public QWidget
{
    Q_OBJECT

public:
    explicit RecentFilesPanel(QWidget *parent = nullptr);

    // Replace the displayed history (entries most-recent first).
    void setEntries(const QList<RecentEntry> &entries);

    // How many entries to show when the filter is empty (spec default 100).
    void setVisibleCount(int count);

    // Drive filtering from the external FindBar. `contentSearch` true means
    // search inside file contents; false means filter by filename.
    void setSearch(const QString &text, bool contentSearch);

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
    // favoritesOnly true when the Favorites view-mode is active, so the window
    // scopes the content scan to starred files instead of the whole history.
    void contentSearchRequested(const QString &query, bool favoritesOnly);
    void contentSearchCanceled();
    // Emitted after every rebuild so FindBar can update the file-count label.
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

private:
    enum class Mode { Recent, Favorites };

    void rebuild();
    void onFilterOrModeChanged();
    void startContentSearch();
    bool contentMode() const;
    void onItemActivated(QListWidgetItem *item);
    void handleMissingFile(const QString &path);
    void toggleItemFavorite(QListWidgetItem *item);
    bool matchesCurrentFilter(const RecentEntry &entry) const;
    QStringList missingFilesInCurrentFilter() const;

    // Internal filter widgets - functional but not shown in the layout;
    // driven via setSearch() from the global FindBar.
    QLineEdit *filter_ = nullptr;
    QCheckBox *contentCheck_ = nullptr;

    QLabel *status_ = nullptr;
    QListWidget *list_ = nullptr;
    QTimer *debounce_ = nullptr;
    QList<RecentEntry> entries_;
    int visibleCount_ = 100;
    bool searching_ = false;
    Mode mode_ = Mode::Recent;
    QString statusSummary_;

    QToolButton *modeRecentBtn_ = nullptr;
    QToolButton *modeFavoritesBtn_ = nullptr;
};

} // namespace mervin
