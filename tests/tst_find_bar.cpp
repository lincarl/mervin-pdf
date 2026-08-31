#include "ui/FindBar.h"

#include <QApplication>
#include <QClipboard>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTest>

using mervin::FindBar;

// Guard rails for the two search contexts the one find bar serves. The widget is
// shared between "find in document" and "search the recent list", but the two are
// separate searches and must not bleed into each other: a document tab gets its
// query back through restoreFindState(), and the Recent view parks its own while a
// document is on screen.
//
// The bug these pin: RecentFilesPanel is never told to drop its filter when the
// user leaves the Recent view, so the field emptying itself on the way out left an
// empty search box sitting over a still-filtered list, with nothing on screen to
// explain why those files were missing. The mirror case is just as confusing - a
// document's query showing in the recent box over an unfiltered list.
//
// No window is shown: the bar is never shown() and never exec()d, so the offscreen
// default is enough (the same arrangement tst_merge_dialog uses).
class TstFindBar : public QObject
{
    Q_OBJECT

private slots:
    void recentQuerySurvivesADocumentVisit();
    void enteringRecentDoesNotInheritTheDocumentQuery();
    void pasteTrimsOuterWhitespace();
    void typingPreservesOuterWhitespace();
};

namespace {
// The bar owns exactly one QLineEdit (the shared search field); it is private, so
// the test reaches it the way a user does - by typing into the only field there is.
QLineEdit *fieldOf(FindBar &bar)
{
    return bar.findChild<QLineEdit *>();
}
} // namespace

void TstFindBar::recentQuerySurvivesADocumentVisit()
{
    FindBar bar;
    bar.setMode(FindBar::Mode::RecentSearch);

    QLineEdit *field = fieldOf(bar);
    QVERIFY(field);

    QSignalSpy filterSpy(&bar, &FindBar::recentFilterChanged);
    field->setText(QStringLiteral("invoice"));

    // The field debounces before it announces a filter, so wait for the emission
    // rather than assuming it is synchronous.
    QTRY_COMPARE(filterSpy.count(), 1);
    QCOMPARE(filterSpy.takeFirst().at(0).toString(), QStringLiteral("invoice"));
    QCOMPARE(bar.query(), QStringLiteral("invoice"));

    // Switching to a document tab empties the field, so the recent query cannot
    // pollute the first document search.
    bar.setMode(FindBar::Mode::FindDocument);
    QVERIFY(bar.query().isEmpty());

    // Coming back has to put it in again: the panel kept filtering by "invoice"
    // the whole time, and an empty box over that filtered list is the bug.
    bar.setMode(FindBar::Mode::RecentSearch);
    QCOMPARE(bar.query(), QStringLiteral("invoice"));

    // Restoring is silent. The panel already holds this exact filter, and
    // re-emitting would restart the content scan that leaving Recent cancelled.
    QVERIFY(filterSpy.isEmpty());
}

void TstFindBar::enteringRecentDoesNotInheritTheDocumentQuery()
{
    FindBar bar; // starts in FindDocument mode
    bar.restoreFindState(QStringLiteral("clause"), false, false, 1, 3);
    QCOMPARE(bar.query(), QStringLiteral("clause"));

    // Opening Recent with no recent search of its own must show an empty box:
    // carrying "clause" over would claim a filter the panel is not applying.
    QSignalSpy filterSpy(&bar, &FindBar::recentFilterChanged);
    bar.setMode(FindBar::Mode::RecentSearch);
    QVERIFY(bar.query().isEmpty());
    QVERIFY(filterSpy.isEmpty());
}

void TstFindBar::pasteTrimsOuterWhitespace()
{
    FindBar bar;
    QLineEdit *field = fieldOf(bar);
    QVERIFY(field);

    QApplication::clipboard()->setText(QStringLiteral("  annual report  \n"));
    field->setText(QStringLiteral("find: "));
    field->setCursorPosition(field->text().size());
    QTest::keyClick(field, Qt::Key_V, Qt::ControlModifier);

    QCOMPARE(field->text(), QStringLiteral("find: annual report"));
}

void TstFindBar::typingPreservesOuterWhitespace()
{
    FindBar bar;
    QLineEdit *field = fieldOf(bar);
    QVERIFY(field);

    QTest::keyClicks(field, QStringLiteral("  annual report  "));

    QCOMPARE(field->text(), QStringLiteral("  annual report  "));
}

QTEST_MAIN(TstFindBar)
#include "tst_find_bar.moc"
