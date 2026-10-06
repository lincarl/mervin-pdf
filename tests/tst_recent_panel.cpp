#include "ui/RecentFilesPanel.h"

#include <QDir>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

using mervin::RecentEntry;
using mervin::RecentFilesPanel;
using mervin::RecentSearchField;

// The Recent page lists starred files in a Favourites section above the rest, keeps
// a just-starred row where it is until the search changes, and searches file names,
// contents, or names then contents. Content hits are fed in by hand, as the
// window's ContentSearch would deliver them.
class TstRecentPanel : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void favouritesSitAboveTheRest();
    void nothingStarredMeansNoCaptions();
    void aStarredRowKeepsItsPlaceUntilTheSearchChanges();
    void namesFiltersAtOnceWithoutAScan();
    void contentsScansEveryFileIntoSections();
    void allListsNamesFirstThenScansTheRest();
    void escapeMovesToTheFirstFileAndKeepsTheSearch();
    void scopeClicksKeepTheCaretInTheField();
    void clearButtonShowsOnlyWithText();
    void homeAndPageKeysStepOverCaptions();
    void historyChangesReachAnAllSearch();
    void aHiddenPageScansOnlyWhenShownAgain();
    void theProgressLineFollowsTheScan();
    void stopKeepsWhatWasFound();

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

void TstRecentPanel::aStarredRowKeepsItsPlaceUntilTheSearchChanges()
{
    QList<RecentEntry> entries{entry("report-2024.pdf", 40), entry("notes.pdf", 20)};
    panel_->setEntries(entries);
    QSignalSpy starred(panel_, &RecentFilesPanel::favoriteToggled);

    // Click the star at the right end of the notes.pdf row.
    const QRect row = list_->visualItemRect(list_->item(1));
    QTest::mouseClick(list_->viewport(), Qt::LeftButton, {},
                      QPoint(row.right() - 16 - 10, row.center().y()));
    QCOMPARE(starred.count(), 1);
    QCOMPARE(starred.at(0).at(1).toBool(), true);

    // The store echoes the change back. The row must not jump away from the pointer.
    entries[1].favorite = true;
    panel_->setEntries(entries);
    QCOMPARE(rows(), QStringList({"report-2024.pdf", "notes.pdf"}));

    // A new search settles it into Favourites.
    field_->setText(QStringLiteral("x"));
    field_->clear();
    QCOMPARE(rows(), QStringList({"# Favourites", "notes.pdf", "# Recent", "report-2024.pdf"}));
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

void TstRecentPanel::theProgressLineFollowsTheScan()
{
    panel_->setEntries({entry("report-2024.pdf", 40), entry("notes.pdf", 30), entry("plan.pdf", 20),
                        entry("report-fav.pdf", 10, true)});
    auto *status = panel_->findChild<QLabel *>(QStringLiteral("recentSearchStatus"));
    auto *stop = panel_->findChild<QToolButton *>(QStringLiteral("recentSearchStop"));
    QVERIFY(status && stop);
    panel_->setDefaultScope(RecentSearchField::Scope::Contents);
    QSignalSpy scans(panel_, &RecentFilesPanel::contentSearchRequested);

    // The line and Stop show at once, before the pause runs out.
    field_->setText(QStringLiteral("pressure"));
    QCOMPARE(field_->progress(), 0.0);
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
    QCOMPARE(field_->progress(), 0.25);
    QCOMPARE(status->text(), QStringLiteral("Searching file 2 of 4"));
    // Halfway through the second file's pages, the line keeps moving.
    panel_->setContentPageProgress(50, 100);
    QCOMPARE(field_->progress(), 0.375);

    panel_->addContentHit(file("notes.pdf"), 3, QStringLiteral("… pressure …"));
    panel_->endContentSearch(false, 1);
    QVERIFY(field_->progress() < 0);
    QVERIFY(!stop->isVisible());
    QVERIFY(!status->property("running").toBool());
    QCOMPARE(status->text(), QStringLiteral("1 file found"));

    // Back to Names: no scan, no line.
    panel_->setDefaultScope(RecentSearchField::Scope::Names);
    QVERIFY(field_->progress() < 0);
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
    QVERIFY(field_->progress() < 0);
    QVERIFY(!stop->isVisible());

    // Stopped on purpose: leaving and returning does not start it again.
    panel_->hide();
    panel_->show();
    QTest::qWait(600);
    QCOMPARE(scans.count(), 1);
}

QTEST_MAIN(TstRecentPanel)
#include "tst_recent_panel.moc"
