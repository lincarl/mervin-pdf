#include "i18n/UiLanguage.h"
#include "io/FileProbe.h"
#include "ui/RecentFilesPanel.h"

#include <QDir>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>

using mervin::RecentEntry;
using mervin::RecentFilesPanel;
using mervin::RecentSearchField;

// The Recent page lists starred files in a Favourites section above the rest, moves
// a file between the sections as soon as its star is clicked, and searches file names,
// contents, or names then contents. Content hits are fed in by hand, as the
// window's ContentSearch would deliver them. Counts use the English catalog's
// plural forms ("1 file found", "2 files found").
class TstRecentPanel : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void favouritesSitAboveTheRest();
    void nothingStarredMeansNoCaptions();
    void aStarMovesTheFileToItsSectionAtOnce();
    void aStarKeepsTheCurrentFileAndTheScrollOffset();
    void anUnstarredOldFileStaysForAnUndo();
    void namesFiltersAtOnceWithoutAScan();
    void contentsScansEveryFileIntoSections();
    void allListsNamesFirstThenScansTheRest();
    void escapeMovesToTheFirstFileAndKeepsTheSearch();
    void scopeClicksKeepTheCaretInTheField();
    void clearButtonShowsOnlyWithText();
    void homeAndPageKeysStepOverCaptions();
    void historyChangesReachAnAllSearch();
    void aHiddenPageScansOnlyWhenShownAgain();
    void statusAndStopFollowTheScan();
    void stopKeepsWhatWasFound();
    void summaryCountsWhatIsListed();
    void checksMarkMissingFilesAndSizes();
    void duplicateAndRemovedContentHitsDoNotAddRows();
    void zeroHitCompletionShowsAnEmptyResult();
    void rowActivationAndStarDoubleClick();
    void obsoleteMountFactsDoNotChangeTheRow();
    void missingFileActions_data();
    void missingFileActions();

private:
    QString file(const QString &name) const { return dir_.filePath(name); }
    RecentEntry entry(const QString &name, qint64 opened, bool favourite = false) const
    {
        RecentEntry e;
        e.path = file(name);
        e.lastOpened = opened;
        e.favorite = favourite;
        return e;
    }
    // Rows as "# caption" or the file name, top to bottom (empty-list notes skipped).
    QStringList rows() const
    {
        QStringList out;
        for (int i = 0; i < list_->count(); ++i) {
            const QListWidgetItem *item = list_->item(i);
            const QString path = item->data(Qt::UserRole).toString();
            if (!path.isEmpty())
                out << QFileInfo(path).fileName();
            else if (item->data(Qt::UserRole + 7).toBool())
                out << QStringLiteral("# ") + item->text();
        }
        return out;
    }
    QToolButton *scopeButton(const QString &text) const
    {
        for (QToolButton *b : panel_->findChildren<QToolButton *>(QStringLiteral("recentScope")))
            if (b->text() == text)
                return b;
        return nullptr;
    }

    QTemporaryDir dir_;
    RecentFilesPanel *panel_ = nullptr;
    RecentSearchField *field_ = nullptr;
    QListWidget *list_ = nullptr;
};

void TstRecentPanel::initTestCase()
{
    mervin::i18n::apply(QStringLiteral("en"));
}

void TstRecentPanel::init()
{
    for (const char *name : {"report-2024.pdf", "notes.pdf", "plan.pdf", "report-fav.pdf"}) {
        QFile f(dir_.filePath(QString::fromLatin1(name)));
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    panel_ = new RecentFilesPanel;
    panel_->resize(900, 600);
    field_ = panel_->findChild<RecentSearchField *>();
    list_ = panel_->findChild<QListWidget *>();
    QVERIFY(field_ && list_);
    panel_->show();
    QVERIFY(QTest::qWaitForWindowExposed(panel_));
}

void TstRecentPanel::cleanup()
{
    delete panel_;
    panel_ = nullptr;
}

// The panel learns from a FileProbe which files are missing and how large the
// others are. Until a check answers, a row shows no size and counts as there. A
// file deleted later shows as missing once the row is checked again.
void TstRecentPanel::checksMarkMissingFilesAndSizes()
{
    {
        QFile sized(file(QStringLiteral("notes.pdf")));
        QVERIFY(sized.open(QIODevice::WriteOnly));
        QCOMPARE(sized.write(QByteArray(2048, 'x')), 2048);
    }
    const QString gone = file(QStringLiteral("gone.pdf"));
    const auto row = [this](const QString &path) -> const QListWidgetItem * {
        for (int i = 0; i < list_->count(); ++i)
            if (list_->item(i)->data(Qt::UserRole).toString() == path)
                return list_->item(i);
        return nullptr;
    };
    const auto missing = [&row](const QString &path) {
        return row(path)->data(Qt::UserRole + 1).toBool();
    };
    const auto size = [&row](const QString &path) {
        return row(path)->data(Qt::UserRole + 8).toLongLong();
    };

    RecentEntry deleted;
    deleted.path = gone;
    deleted.lastOpened = 10;
    panel_->setEntries({entry("notes.pdf", 30), deleted});
    QVERIFY(!missing(gone));
    QCOMPARE(size(file(QStringLiteral("notes.pdf"))), 0);

    mervin::FileProbe probe;
    panel_->setFileProbe(&probe);
    QTRY_VERIFY(missing(gone));
    QVERIFY(row(gone)->toolTip().contains(QStringLiteral("no longer on disk")));
    QTRY_COMPARE(size(file(QStringLiteral("notes.pdf"))), 2048);
    QVERIFY(!missing(file(QStringLiteral("notes.pdf"))));

    // Shown again after the answers have aged, the rows are checked again.
    QVERIFY(QFile::remove(file(QStringLiteral("notes.pdf"))));
    QTest::qWait(2100);
    panel_->setEntries({entry("notes.pdf", 30), deleted});
    QTRY_VERIFY(missing(file(QStringLiteral("notes.pdf"))));
    panel_->setFileProbe(nullptr);
}

void TstRecentPanel::favouritesSitAboveTheRest()
{
    // Newest first, as the store supplies them.
    panel_->setEntries({entry("report-2024.pdf", 40), entry("report-fav.pdf", 30, true),
                        entry("notes.pdf", 20), entry("plan.pdf", 10, true)});

    QCOMPARE(rows(), QStringList({"# Favourites", "report-fav.pdf", "plan.pdf",
                                  "# Recent", "report-2024.pdf", "notes.pdf"}));
}

void TstRecentPanel::nothingStarredMeansNoCaptions()
{
    panel_->setEntries({entry("report-2024.pdf", 40), entry("notes.pdf", 20)});

    QCOMPARE(rows(), QStringList({"report-2024.pdf", "notes.pdf"}));
}

void TstRecentPanel::aStarMovesTheFileToItsSectionAtOnce()
{
    QList<RecentEntry> entries{entry("report-2024.pdf", 40), entry("notes.pdf", 20)};
    panel_->setEntries(entries);
    QSignalSpy starred(panel_, &RecentFilesPanel::favoriteToggled);
    // The star sits at the right end of a row.
    const auto clickStar = [this](int row) {
        const QRect rect = list_->visualItemRect(list_->item(row));
        QTest::mouseClick(list_->viewport(), Qt::LeftButton, {},
                          QPoint(rect.right() - 16 - 10, rect.center().y()));
    };

    // Starring notes.pdf moves it into Favourites without waiting for the store.
    clickStar(1);
    QCOMPARE(starred.count(), 1);
    QCOMPARE(starred.at(0).at(0).toString(), file("notes.pdf"));
    QCOMPARE(starred.at(0).at(1).toBool(), true);
    QCOMPARE(rows(), QStringList({"# Favourites", "notes.pdf", "# Recent", "report-2024.pdf"}));

    // The store's echo of the change leaves the list as it is.
    entries[1].favorite = true;
    panel_->setEntries(entries);
    QCOMPARE(rows(), QStringList({"# Favourites", "notes.pdf", "# Recent", "report-2024.pdf"}));

    // Unstarring moves it straight back, and the captions go with the last star.
    clickStar(1);
    QCOMPARE(starred.count(), 2);
    QCOMPARE(starred.at(1).at(1).toBool(), false);
    QCOMPARE(rows(), QStringList({"report-2024.pdf", "notes.pdf"}));
}

void TstRecentPanel::aStarKeepsTheCurrentFileAndTheScrollOffset()
{
    // More rows than fit, so the list scrolls. The files need not exist.
    QList<RecentEntry> entries;
    for (int i = 0; i < 30; ++i)
        entries << entry(QStringLiteral("file-%1.pdf").arg(i, 2, 10, QLatin1Char('0')), 100 - i);
    panel_->setEntries(entries);
    list_->setCurrentRow(0); // file-00.pdf
    QScrollBar *bar = list_->verticalScrollBar();
    bar->setValue(bar->maximum());
    const int scroll = bar->value();
    QVERIFY(scroll > 0);

    // Star the row in the middle of the view: it moves up into Favourites, out of view.
    QListWidgetItem *middle = list_->itemAt(list_->viewport()->rect().center());
    QVERIFY(middle);
    const QString starredName = middle->text();
    const QRect rect = list_->visualItemRect(middle);
    QTest::mouseClick(list_->viewport(), Qt::LeftButton, {},
                      QPoint(rect.right() - 16 - 10, rect.center().y()));
    QCOMPARE(rows().mid(0, 2), QStringList({"# Favourites", starredName}));
    QCOMPARE(bar->value(), scroll);
    QVERIFY(list_->currentItem());
    QCOMPARE(list_->currentItem()->text(), QStringLiteral("file-00.pdf"));
    QVERIFY(list_->currentItem()->isSelected());

    // A star from another window keeps them too.
    entries[1].favorite = true;
    for (RecentEntry &e : entries)
        if (QFileInfo(e.path).fileName() == starredName)
            e.favorite = true;
    panel_->setEntries(entries);
    QCOMPARE(rows().mid(0, 3), QStringList({"# Favourites", "file-01.pdf", starredName}));
    QCOMPARE(bar->value(), scroll);
    QCOMPARE(list_->currentItem()->text(), QStringLiteral("file-00.pdf"));
}

// Unstarring an old favourite moves it to Recent, past the visible count. It stays
// listed until the search changes, so a second click on its star undoes the first.
void TstRecentPanel::anUnstarredOldFileStaysForAnUndo()
{
    panel_->setVisibleCount(1);
    panel_->setEntries({entry("report-2024.pdf", 40), entry("notes.pdf", 20),
                        entry("plan.pdf", 10, true)});
    QCOMPARE(rows(), QStringList({"# Favourites", "plan.pdf", "# Recent", "report-2024.pdf"}));
    const auto clickStar = [this](const QString &name) {
        for (int i = 0; i < list_->count(); ++i) {
            if (QFileInfo(list_->item(i)->data(Qt::UserRole).toString()).fileName() == name) {
                const QRect rect = list_->visualItemRect(list_->item(i));
                QTest::mouseClick(list_->viewport(), Qt::LeftButton, {},
                                  QPoint(rect.right() - 16 - 10, rect.center().y()));
                return;
            }
        }
        QFAIL(qPrintable(name + " is not listed"));
    };

    clickStar(QStringLiteral("plan.pdf"));
    QCOMPARE(rows(), QStringList({"report-2024.pdf", "plan.pdf"})); // notes.pdf is past the count
    clickStar(QStringLiteral("plan.pdf"));
    QCOMPARE(rows(), QStringList({"# Favourites", "plan.pdf", "# Recent", "report-2024.pdf"}));

    // Once the search changes, the visible count applies to it again.
    clickStar(QStringLiteral("plan.pdf"));
    field_->setText(QStringLiteral("x"));
    field_->clear();
    QCOMPARE(rows(), QStringList({"report-2024.pdf"}));
}

void TstRecentPanel::namesFiltersAtOnceWithoutAScan()
{
    panel_->setEntries({entry("report-2024.pdf", 40), entry("report-fav.pdf", 30, true),
                        entry("notes.pdf", 20)});
    QSignalSpy scans(panel_, &RecentFilesPanel::contentSearchRequested);

    field_->setText(QStringLiteral("report"));

    QCOMPARE(rows(), QStringList({"# Favourites", "report-fav.pdf", "# Recent", "report-2024.pdf"}));
    QTest::qWait(600); // longer than the content debounce
    QVERIFY(scans.isEmpty());
}

void TstRecentPanel::contentsScansEveryFileIntoSections()
{
    panel_->setEntries({entry("report-2024.pdf", 40), entry("report-fav.pdf", 30, true),
                        entry("notes.pdf", 20)});
    panel_->setDefaultScope(RecentSearchField::Scope::Contents);
    QSignalSpy scans(panel_, &RecentFilesPanel::contentSearchRequested);

    field_->setText(QStringLiteral("pressure"));
    QTRY_COMPARE(scans.count(), 1);
    QCOMPARE(scans.at(0).at(0).toString(), QStringLiteral("pressure"));
    QCOMPARE(scans.at(0).at(1).toStringList(),
             QStringList({file("report-2024.pdf"), file("report-fav.pdf"), file("notes.pdf")}));

    // Hits arrive in scan order; each lands in its own section.
    panel_->addContentHit(file("notes.pdf"), 3, QStringLiteral("… pressure …"));
    panel_->addContentHit(file("report-fav.pdf"), 1, QStringLiteral("… pressure …"));
    QCOMPARE(rows(), QStringList({"# Favourites", "report-fav.pdf", "# Recent", "notes.pdf"}));

    panel_->endContentSearch(false, 2);
    QCOMPARE(panel_->findChild<QLabel *>(QStringLiteral("recentSearchStatus"))->text(),
             QStringLiteral("2 files found"));
}

void TstRecentPanel::allListsNamesFirstThenScansTheRest()
{
    panel_->setEntries({entry("report-2024.pdf", 40), entry("report-fav.pdf", 30, true),
                        entry("notes.pdf", 20), entry("plan.pdf", 10)});
    panel_->setDefaultScope(RecentSearchField::Scope::All);
    QSignalSpy scans(panel_, &RecentFilesPanel::contentSearchRequested);

    // Names are listed at once...
    field_->setText(QStringLiteral("report"));
    QCOMPARE(rows(), QStringList({"# Favourites", "report-fav.pdf", "# Recent", "report-2024.pdf"}));

    // ...then only the files not already listed are scanned.
    QTRY_COMPARE(scans.count(), 1);
    QCOMPARE(scans.at(0).at(1).toStringList(), QStringList({file("notes.pdf"), file("plan.pdf")}));

    // Content hits follow the name matches in their section.
    panel_->addContentHit(file("plan.pdf"), 2, QStringLiteral("… report …"));
    QCOMPARE(rows(), QStringList({"# Favourites", "report-fav.pdf",
                                  "# Recent", "report-2024.pdf", "plan.pdf"}));

    panel_->endContentSearch(false, 1);
    QCOMPARE(panel_->findChild<QLabel *>(QStringLiteral("recentSearchStatus"))->text(),
             QStringLiteral("3 files found"));
}

void TstRecentPanel::escapeMovesToTheFirstFileAndKeepsTheSearch()
{
    panel_->setEntries({entry("report-2024.pdf", 40), entry("report-fav.pdf", 30, true)});
    panel_->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(panel_));
    panel_->focusSearch();
    QTest::keyClicks(field_, QStringLiteral("rep"));

    QTest::keyClick(field_, Qt::Key_Escape);

    QVERIFY(list_->hasFocus());
    // The first file, not the Favourites caption above it.
    QCOMPARE(QFileInfo(list_->currentItem()->data(Qt::UserRole).toString()).fileName(),
             QStringLiteral("report-fav.pdf"));
    QCOMPARE(field_->text(), QStringLiteral("rep"));
}

void TstRecentPanel::scopeClicksKeepTheCaretInTheField()
{
    panel_->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(panel_));
    panel_->focusSearch();
    QToolButton *contents = scopeButton(QStringLiteral("Contents"));
    QVERIFY(contents);

    QTest::mouseClick(contents, Qt::LeftButton);

    QCOMPARE(field_->scope(), RecentSearchField::Scope::Contents);
    QVERIFY(field_->hasFocus());
    QCOMPARE(field_->placeholderText(), QStringLiteral("Search inside documents"));
}

void TstRecentPanel::clearButtonShowsOnlyWithText()
{
    auto *clear = panel_->findChild<QToolButton *>(QStringLiteral("recentSearchClear"));
    QVERIFY(clear);
    QVERIFY(!clear->isVisible());

    field_->setText(QStringLiteral("notes"));
    QVERIFY(clear->isVisible());

    QTest::mouseClick(clear, Qt::LeftButton);
    QVERIFY(field_->text().isEmpty());
    QVERIFY(!clear->isVisible());
}

void TstRecentPanel::homeAndPageKeysStepOverCaptions()
{
    panel_->setEntries({entry("report-2024.pdf", 40), entry("report-fav.pdf", 30, true),
                        entry("notes.pdf", 20), entry("plan.pdf", 10, true)});
    panel_->focusList();
    const auto current = [this] {
        return QFileInfo(list_->currentItem()->data(Qt::UserRole).toString()).fileName();
    };

    QTest::keyClick(list_, Qt::Key_End);
    QCOMPARE(current(), QStringLiteral("notes.pdf"));
    // Row 0 is the Favourites caption: Home must land on the first file under it.
    QTest::keyClick(list_, Qt::Key_Home);
    QCOMPARE(current(), QStringLiteral("report-fav.pdf"));
    QTest::keyClick(list_, Qt::Key_PageDown);
    QCOMPARE(current(), QStringLiteral("notes.pdf"));
    QTest::keyClick(list_, Qt::Key_PageUp);
    QCOMPARE(current(), QStringLiteral("report-fav.pdf"));
}

void TstRecentPanel::historyChangesReachAnAllSearch()
{
    QList<RecentEntry> entries{entry("report-2024.pdf", 40), entry("notes.pdf", 20),
                               entry("plan.pdf", 10)};
    panel_->setEntries(entries);
    panel_->setDefaultScope(RecentSearchField::Scope::All);
    QSignalSpy scans(panel_, &RecentFilesPanel::contentSearchRequested);
    field_->setText(QStringLiteral("report"));
    QTRY_COMPARE(scans.count(), 1);
    panel_->addContentHit(file("plan.pdf"), 2, QStringLiteral("… report …"));
    QCOMPARE(rows(), QStringList({"report-2024.pdf", "plan.pdf"}));

    // Remove from history: the store sends the shorter list while All is active.
    entries.removeFirst();
    panel_->setEntries(entries);
    QCOMPARE(rows(), QStringList({"plan.pdf"}));
}

void TstRecentPanel::aHiddenPageScansOnlyWhenShownAgain()
{
    panel_->setEntries({entry("report-2024.pdf", 40), entry("notes.pdf", 20)});
    field_->setText(QStringLiteral("report"));
    QSignalSpy scans(panel_, &RecentFilesPanel::contentSearchRequested);

    // Settings switches the default to Contents while a document is on screen.
    panel_->hide();
    panel_->setDefaultScope(RecentSearchField::Scope::Contents);
    QTest::qWait(600); // longer than the content pause
    QVERIFY(scans.isEmpty());

    panel_->show();
    QTRY_COMPARE(scans.count(), 1);
    QCOMPARE(scans.at(0).at(0).toString(), QStringLiteral("report"));
}

void TstRecentPanel::statusAndStopFollowTheScan()
{
    panel_->setEntries({entry("report-2024.pdf", 40), entry("notes.pdf", 30), entry("plan.pdf", 20),
                        entry("report-fav.pdf", 10, true)});
    auto *status = panel_->findChild<QLabel *>(QStringLiteral("recentSearchStatus"));
    auto *stop = panel_->findChild<QToolButton *>(QStringLiteral("recentSearchStop"));
    QVERIFY(status && stop);
    panel_->setDefaultScope(RecentSearchField::Scope::Contents);
    QSignalSpy scans(panel_, &RecentFilesPanel::contentSearchRequested);

    // The status and Stop show at once, before the pause runs out.
    field_->setText(QStringLiteral("pressure"));
    QCOMPARE(status->text(), QStringLiteral("Searching…"));
    QVERIFY(stop->isVisible());
    QVERIFY(status->property("running").toBool());

    QCoreApplication::processEvents(); // lay out the row with Stop showing
    const int stopX = stop->mapTo(panel_, QPoint()).x();

    QTRY_COMPARE(scans.count(), 1);
    QCOMPARE(status->text(), QStringLiteral("Searching file 1 of 4"));
    // "Searching…" became longer text: Stop must stay where the pointer found it.
    QCoreApplication::processEvents();
    QCOMPARE(stop->mapTo(panel_, QPoint()).x(), stopX);
    panel_->setContentProgress(1, 4);
    QCOMPARE(status->text(), QStringLiteral("Searching file 2 of 4"));

    panel_->addContentHit(file("notes.pdf"), 3, QStringLiteral("… pressure …"));
    panel_->endContentSearch(false, 1);
    QVERIFY(!stop->isVisible());
    QVERIFY(!status->property("running").toBool());
    QCOMPARE(status->text(), QStringLiteral("1 file found"));

    // Back to Names: no scan, no status.
    panel_->setDefaultScope(RecentSearchField::Scope::Names);
    QVERIFY(!status->isVisible());
    QVERIFY(!stop->isVisible());
}

void TstRecentPanel::stopKeepsWhatWasFound()
{
    panel_->setEntries({entry("report-2024.pdf", 40), entry("notes.pdf", 30), entry("plan.pdf", 20)});
    panel_->setDefaultScope(RecentSearchField::Scope::Contents);
    QSignalSpy scans(panel_, &RecentFilesPanel::contentSearchRequested);
    QSignalSpy canceled(panel_, &RecentFilesPanel::contentSearchCanceled);
    field_->setText(QStringLiteral("pressure"));
    QTRY_COMPARE(scans.count(), 1);
    panel_->addContentHit(file("plan.pdf"), 2, QStringLiteral("… pressure …"));

    auto *stop = panel_->findChild<QToolButton *>(QStringLiteral("recentSearchStop"));
    QTest::mouseClick(stop, Qt::LeftButton);

    QCOMPARE(canceled.count(), 1);
    QCOMPARE(rows(), QStringList({"plan.pdf"}));
    QCOMPARE(panel_->findChild<QLabel *>(QStringLiteral("recentSearchStatus"))->text(),
             QStringLiteral("Stopped, 1 file found"));
    QVERIFY(!stop->isVisible());

    // These callbacks were already queued when Stop was pressed.
    panel_->addContentHit(file("notes.pdf"), 7, QStringLiteral("late hit"));
    panel_->setContentProgress(2, 3);
    QCOMPARE(rows(), QStringList({"plan.pdf"}));
    auto *status = panel_->findChild<QLabel *>(QStringLiteral("recentSearchStatus"));
    QCOMPARE(status->text(), QStringLiteral("Stopped, 1 file found"));
    panel_->endContentSearch(false, 2);
    QCOMPARE(status->text(), QStringLiteral("Stopped, 1 file found"));
    panel_->endContentSearch(true, 2);
    QVERIFY(status->isVisible());
    QVERIFY(!status->property("running").toBool());
    QVERIFY(!stop->isVisible());

    // Stopped on purpose: leaving and returning does not start it again.
    panel_->hide();
    panel_->show();
    QTest::qWait(600);
    QCOMPARE(scans.count(), 1);
}

void TstRecentPanel::summaryCountsWhatIsListed()
{
    panel_->setEntries({entry("report-2024.pdf", 40), entry("report-fav.pdf", 30),
                        entry("notes.pdf", 20)});
    QCOMPARE(panel_->statusSummary(), QStringLiteral("Your last 3 opened documents"));

    field_->setText(QStringLiteral("report"));
    QCOMPARE(panel_->statusSummary(), QStringLiteral("2 results"));
    field_->setText(QStringLiteral("notes"));
    QCOMPARE(panel_->statusSummary(), QStringLiteral("1 result"));
    field_->setText(QStringLiteral("nothing"));
    QCOMPARE(panel_->statusSummary(), QStringLiteral("0 results"));
}

/// Duplicate pages and hits for files removed mid-search must not inflate the listing.
void TstRecentPanel::duplicateAndRemovedContentHitsDoNotAddRows()
{
    panel_->setEntries({entry("notes.pdf", 30), entry("plan.pdf", 20)});
    panel_->setDefaultScope(RecentSearchField::Scope::Contents);
    QSignalSpy scans(panel_, &RecentFilesPanel::contentSearchRequested);
    field_->setText(QStringLiteral("pressure"));
    QTRY_COMPARE(scans.count(), 1);
    panel_->addContentHit(file("notes.pdf"), 2, QStringLiteral("first match"));
    const QString firstTip = list_->item(0)->toolTip();
    panel_->addContentHit(file("notes.pdf"), 9, QStringLiteral("duplicate match"));
    QCOMPARE(rows(), QStringList({"notes.pdf"}));
    QCOMPARE(list_->item(0)->toolTip(), firstTip);
    panel_->setEntries({entry("notes.pdf", 30)});
    panel_->addContentHit(file("plan.pdf"), 1, QStringLiteral("removed from history"));
    panel_->endContentSearch(false, 3);
    QCOMPARE(rows(), QStringList({"notes.pdf"}));
    QCOMPARE(panel_->statusSummary(), QStringLiteral("1 result"));
    QCOMPARE(panel_->findChild<QLabel *>(QStringLiteral("recentSearchStatus"))->text(),
             QStringLiteral("1 file found"));
}

void TstRecentPanel::zeroHitCompletionShowsAnEmptyResult()
{
    panel_->setEntries({entry("notes.pdf", 30)});
    panel_->setDefaultScope(RecentSearchField::Scope::Contents);
    QSignalSpy scans(panel_, &RecentFilesPanel::contentSearchRequested);
    field_->setText(QStringLiteral("absent"));
    QTRY_COMPARE(scans.count(), 1);
    panel_->endContentSearch(false, 0);
    QVERIFY(rows().isEmpty());
    QCOMPARE(panel_->statusSummary(), QStringLiteral("0 results"));
    QCOMPARE(list_->count(), 1);
    QVERIFY(list_->item(0)->text().contains(QStringLiteral("No documents contain")));
    QVERIFY(!list_->item(0)->flags().testFlag(Qt::ItemIsSelectable));
    QVERIFY(!panel_->findChild<QToolButton *>(QStringLiteral("recentSearchStop"))->isVisible());
    QVERIFY(!panel_->findChild<QLabel *>(QStringLiteral("recentSearchStatus"))->isVisible());
}

/// Keyboard activation opens a file, while the second star click cannot open a moved row.
void TstRecentPanel::rowActivationAndStarDoubleClick()
{
    panel_->setEntries({entry("report-2024.pdf", 40), entry("notes.pdf", 30), entry("plan.pdf", 20)});
    QSignalSpy opened(panel_, &RecentFilesPanel::openRequested);
    QSignalSpy starred(panel_, &RecentFilesPanel::favoriteToggled);
    list_->setCurrentRow(1);
    QTest::keyClick(list_, Qt::Key_Return);
    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.takeFirst().at(0).toString(), file("notes.pdf"));

    const QRect rect = list_->visualItemRect(list_->item(1));
    const QPoint star(rect.right() - 26, rect.center().y());
    QTest::mouseClick(list_->viewport(), Qt::LeftButton, {}, star);
    QTest::mouseDClick(list_->viewport(), Qt::LeftButton, {}, star);
    QCOMPARE(starred.count(), 1);
    QVERIFY(opened.isEmpty());
    QCOMPARE(rows(), QStringList({"# Favourites", "notes.pdf", "# Recent", "report-2024.pdf", "plan.pdf"}));
}

void TstRecentPanel::obsoleteMountFactsDoNotChangeTheRow()
{
    mervin::FileProbe probe;
    panel_->setFileProbe(&probe);
    RecentEntry current = entry("notes.pdf", 30);
    current.mount = QStringLiteral("current-mount");
    panel_->setEntries({current});
    mervin::FileFacts present;
    present.exists = true;
    present.size = 42;
    emit probe.checked({current.path, current.mount}, present);
    QCOMPARE(list_->item(0)->data(Qt::UserRole + 8).toLongLong(), 42);
    emit probe.checked({current.path, QStringLiteral("old-mount")}, {});
    QVERIFY(!list_->item(0)->data(Qt::UserRole + 1).toBool());
    QCOMPARE(list_->item(0)->data(Qt::UserRole + 8).toLongLong(), 42);
    emit probe.checked({current.path, current.mount}, {});
    QVERIFY(list_->item(0)->data(Qt::UserRole + 1).toBool());
    panel_->setFileProbe(nullptr);
}

void TstRecentPanel::missingFileActions_data()
{
    QTest::addColumn<int>("action");
    QTest::newRow("cancel") << 0;
    QTest::newRow("remove") << 1;
    QTest::newRow("locate") << 2;
}

void TstRecentPanel::missingFileActions()
{
    QFETCH(int, action);
    // Exercise the Qt picker flow deterministically; native shell interaction is separate.
    const bool nativeDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    const auto restore = qScopeGuard([nativeDisabled] {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDisabled);
    });
    mervin::FileProbe probe;
    panel_->setFileProbe(&probe);
    panel_->setEntries({entry("gone.pdf", 10)});
    QTRY_VERIFY(list_->item(0)->data(Qt::UserRole + 1).toBool());
    QSignalSpy removed(panel_, &RecentFilesPanel::removeRequested);
    QSignalSpy opened(panel_, &RecentFilesPanel::openRequested);
    bool answered = false;
    bool picked = false;
    QTimer answer;
    connect(&answer, &QTimer::timeout, this, [&] {
        if (auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
            auto *field = picker->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
            auto *buttons = picker->findChild<QDialogButtonBox *>();
            if (field && buttons && buttons->button(QDialogButtonBox::Open)) {
                field->setText(file("notes.pdf"));
                picked = true;
                buttons->button(QDialogButtonBox::Open)->click();
            } else {
                picker->reject();
            }
            return;
        }
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box)
            return;
        for (auto *button : box->buttons())
            if ((action == 0 && box->standardButton(button) == QMessageBox::Cancel)
                || (action == 1 && button->text() == QStringLiteral("Remove from history"))
                || (action == 2 && button->text() == QStringLiteral("Locate"))) {
                answered = true;
                button->click();
                return;
            }
        box->reject(); // an unexpected question must fail without leaving a modal loop open
    });
    QTimer watchdog;
    connect(&watchdog, &QTimer::timeout, this, [] {
        if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
            dialog->reject();
    });
    watchdog.start(3000);
    answer.start(10);
    list_->setCurrentRow(0);
    QTest::keyClick(list_, Qt::Key_Return);
    QVERIFY(answered);
    QCOMPARE(opened.count(), action == 2 ? 1 : 0);
    QCOMPARE(picked, action == 2);
    if (action == 2)
        QCOMPARE(opened.first().at(0).toString(), file("notes.pdf"));
    QCOMPARE(removed.count(), action == 0 ? 0 : 1);
    if (action != 0)
        QCOMPARE(removed.first().at(0).toString(), file("gone.pdf"));
    panel_->setFileProbe(nullptr);
}

QTEST_MAIN(TstRecentPanel)
#include "tst_recent_panel.moc"
