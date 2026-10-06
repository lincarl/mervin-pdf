#include "ui/FindCard.h"
#include "ui/PanelStack.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>

using mervin::FindCard;

// The find card is the whole of find in document: it must open ready for typing,
// search a kept query again when it comes back, close on Escape, step with Enter,
// and keep the caret in the field when the user clicks an option. It talks to the
// viewer only through signals, so these tests drive it with no document.
class TstFindCard : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void openWithPresetSearchesAtOnce();
    void reopeningSearchesTheKeptQuery();
    void openingAnOpenCardDoesNotSearchAgain();
    void escapeInTheFieldCloses();
    void enterStepsOnlyAfterTheQueryIsSearched();
    void steppingSearchesPendingTextFirst();
    void openingTheCardKeepsOtherPanelsInPlace();
    void clickingAnOptionKeepsTheCaretInTheField();
    void countShowsMatchesOrNoResults();
    void pasteTrimsOuterWhitespace();
    void typingPreservesOuterWhitespace();

private:
    QWidget *viewport_ = nullptr;
    mervin::PanelStack *stack_ = nullptr;
    FindCard *card_ = nullptr;

    QLineEdit *field() const { return card_->findChild<QLineEdit *>(QStringLiteral("findField")); }
    QCheckBox *checkBox(const QString &text) const
    {
        for (QCheckBox *box : card_->findChildren<QCheckBox *>())
            if (box->text() == text)
                return box;
        return nullptr;
    }
};

void TstFindCard::init()
{
    // A stand-in for the viewer's viewport: the card is its child, as in TabPage.
    // It takes focus on click like the viewer does, so a click that the card lets
    // through would move focus out of the field here too.
    viewport_ = new QWidget;
    viewport_->setFocusPolicy(Qt::StrongFocus);
    viewport_->resize(1000, 500);
    stack_ = new mervin::PanelStack(viewport_, viewport_);
    card_ = new FindCard(viewport_);
    card_->setStack(stack_);
    stack_->addPanel(card_);
    viewport_->show();
    QVERIFY(QTest::qWaitForWindowExposed(viewport_));
}

void TstFindCard::cleanup()
{
    delete viewport_;
    viewport_ = nullptr;
}

void TstFindCard::openWithPresetSearchesAtOnce()
{
    QSignalSpy search(card_, &FindCard::searchChanged);
    QSignalSpy opened(card_, &FindCard::openChanged);
    QVERIFY(!card_->isOpen());

    card_->open(QStringLiteral("clause"));

    QVERIFY(card_->isOpen());
    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.at(0).at(0).toBool(), true);
    // A preset is the selected text: search it now, without the typing debounce.
    QCOMPARE(search.count(), 1);
    QCOMPARE(search.at(0).at(0).toString(), QStringLiteral("clause"));
    QCOMPARE(field()->selectedText(), QStringLiteral("clause"));
}

void TstFindCard::reopeningSearchesTheKeptQuery()
{
    QSignalSpy search(card_, &FindCard::searchChanged);
    QSignalSpy opened(card_, &FindCard::openChanged);
    card_->open();
    field()->setText(QStringLiteral("invoice"));
    QTRY_COMPARE(search.count(), 1); // debounced while typing

    card_->dismiss();
    QVERIFY(!card_->isOpen());
    QCOMPARE(opened.count(), 2);
    QCOMPARE(opened.at(1).at(0).toBool(), false);
    QCOMPARE(card_->query(), QStringLiteral("invoice")); // the query is kept

    // Closing cleared the highlights, so opening again has to search again.
    card_->open();
    QCOMPARE(search.count(), 2);
    QCOMPARE(search.at(1).at(0).toString(), QStringLiteral("invoice"));
}

void TstFindCard::openingAnOpenCardDoesNotSearchAgain()
{
    card_->open(QStringLiteral("term"));
    QSignalSpy search(card_, &FindCard::searchChanged);
    QSignalSpy opened(card_, &FindCard::openChanged);

    // Ctrl+F while the card is open only selects the query again: re-searching
    // would throw away the current match position.
    card_->open();

    QVERIFY(search.isEmpty());
    QVERIFY(opened.isEmpty());
    QCOMPARE(field()->selectedText(), QStringLiteral("term"));
}

void TstFindCard::escapeInTheFieldCloses()
{
    card_->open(QStringLiteral("term"));
    QSignalSpy opened(card_, &FindCard::openChanged);

    QTest::keyClick(field(), Qt::Key_Escape);

    QVERIFY(!card_->isOpen());
    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.at(0).at(0).toBool(), false);
}

void TstFindCard::enterStepsOnlyAfterTheQueryIsSearched()
{
    card_->open();
    QSignalSpy search(card_, &FindCard::searchChanged);
    QSignalSpy next(card_, &FindCard::findNext);
    QSignalSpy prev(card_, &FindCard::findPrev);

    // Enter right after typing searches now instead of waiting for the debounce,
    // and does not step: there is nothing to step through yet.
    QTest::keyClicks(field(), QStringLiteral("rate"));
    QTest::keyClick(field(), Qt::Key_Return);
    QCOMPARE(search.count(), 1);
    QVERIFY(next.isEmpty());

    card_->setResultCount(1, 3);
    QTest::keyClick(field(), Qt::Key_Return);
    QCOMPARE(next.count(), 1);
    QTest::keyClick(field(), Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(prev.count(), 1);
    QCOMPARE(search.count(), 1);
}

void TstFindCard::steppingSearchesPendingTextFirst()
{
    card_->open(QStringLiteral("old"));
    card_->setResultCount(1, 4);
    QSignalSpy search(card_, &FindCard::searchChanged);
    QSignalSpy next(card_, &FindCard::findNext);

    // F3 and the chevrons go through next(). Right after typing, stepping would
    // walk the previous query's matches, so the new text is searched instead.
    field()->setText(QStringLiteral("new"));
    card_->next();
    QCOMPARE(search.count(), 1);
    QCOMPARE(search.at(0).at(0).toString(), QStringLiteral("new"));
    QVERIFY(next.isEmpty());

    card_->next();
    QCOMPARE(next.count(), 1);
}

void TstFindCard::openingTheCardKeepsOtherPanelsInPlace()
{
    // A narrower panel below the card in the same stack, like the Measure panel.
    auto *panel = new QWidget(viewport_);
    panel->setFixedSize(300, 120);
    stack_->addPanel(panel);
    panel->show();
    stack_->relayout();
    const int rightEdge = panel->geometry().right();
    QCOMPARE(rightEdge, viewport_->width() - 12 - 1); // 12px from the viewport edge

    card_->open();
    QVERIFY(card_->width() > panel->width());
    // The stack is right-aligned: the wider card must not drag the panel left.
    QCOMPARE(panel->geometry().right(), rightEdge);
    QCOMPARE(card_->geometry().right(), rightEdge);
    QVERIFY(panel->geometry().top() > card_->geometry().bottom());
}

void TstFindCard::clickingAnOptionKeepsTheCaretInTheField()
{
    viewport_->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(viewport_));
    card_->open(QStringLiteral("term"));
    QVERIFY(field()->hasFocus());
    QSignalSpy search(card_, &FindCard::searchChanged);

    QCheckBox *matchCase = checkBox(QStringLiteral("Match case"));
    QVERIFY(matchCase);
    QTest::mouseClick(matchCase, Qt::LeftButton, {}, QPoint(4, matchCase->height() / 2));

    QVERIFY(matchCase->isChecked());
    QCOMPARE(search.count(), 1);
    QCOMPARE(search.at(0).at(1).toBool(), true);
    // The bug this pins: a click used to move focus to the checkbox, so the next
    // keystroke went nowhere and the field lost its active look.
    QVERIFY(field()->hasFocus());
}

void TstFindCard::countShowsMatchesOrNoResults()
{
    card_->open();
    auto *count = card_->findChild<QLabel *>(QStringLiteral("findCount"));
    QVERIFY(count);
    QToolButton *next = nullptr;
    for (QToolButton *b : card_->findChildren<QToolButton *>())
        if (b->toolTip().startsWith(QStringLiteral("Next match")))
            next = b;
    QVERIFY(next);

    card_->setResultCount(0, 0); // empty field: no count at all
    QVERIFY(count->text().isEmpty());
    QVERIFY(!next->isEnabled());

    field()->setText(QStringLiteral("zzz"));
    card_->setResultCount(0, 0);
    QCOMPARE(count->text(), QStringLiteral("No results"));
    QVERIFY(!next->isEnabled());

    card_->setResultCount(2, 5);
    QCOMPARE(count->text(), QStringLiteral("2 of 5"));
    QVERIFY(next->isEnabled());
    // The count sits inside the field, and typed text stops short of it.
    QVERIFY(field()->textMargins().right() > count->width());
}

// The field is a SearchLineEdit, shared with the Recent page: a pasted line
// searches for its text, not for the newline copied with it.
void TstFindCard::pasteTrimsOuterWhitespace()
{
    card_->open();
    QApplication::clipboard()->setText(QStringLiteral("  annual report  \n"));
    field()->setText(QStringLiteral("find: "));
    field()->setCursorPosition(field()->text().size());
    QTest::keyClick(field(), Qt::Key_V, Qt::ControlModifier);

    QCOMPARE(field()->text(), QStringLiteral("find: annual report"));
}

void TstFindCard::typingPreservesOuterWhitespace()
{
    card_->open();
    QTest::keyClicks(field(), QStringLiteral("  annual report  "));

    QCOMPARE(field()->text(), QStringLiteral("  annual report  "));
}

QTEST_MAIN(TstFindCard)
#include "tst_find_card.moc"
