#include "ui/FindBar.h"

#include "ui/Icons.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QTimer>
#include <QToolButton>

#include <memory>

namespace mervin {

namespace {
constexpr int kDebounceMs = 200;

class SearchLineEdit final : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->matches(QKeySequence::Paste)) {
            insertTrimmedClipboardText();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        std::unique_ptr<QMenu> menu(createStandardContextMenu());
        const QList<QKeySequence> pasteKeys = QKeySequence::keyBindings(QKeySequence::Paste);
        for (QAction *action : menu->actions()) {
            bool isPaste = false;
            for (const QKeySequence &shortcut : action->shortcuts()) {
                for (const QKeySequence &key : pasteKeys) {
                    if (shortcut.matches(key) == QKeySequence::ExactMatch) {
                        isPaste = true;
                        break;
                    }
                }
                if (isPaste)
                    break;
            }
            if (isPaste) {
                QObject::disconnect(action, nullptr, nullptr, nullptr);
                connect(action, &QAction::triggered, this,
                        [this] { insertTrimmedClipboardText(); });
                break;
            }
        }
        menu->exec(event->globalPos());
    }

private:
    void insertTrimmedClipboardText()
    {
        if (!isReadOnly())
            insert(QApplication::clipboard()->text().trimmed());
    }
};

// A monochrome magnifier, tinted to the caller's (muted) colour so it reads on
// both light and dark themes.
QIcon makeSearchIcon(const QColor &color)
{
    return mervin::icons::glyph(mervin::icons::Glyph::Search, color);
}
} // namespace

FindBar::FindBar(QWidget *parent)
    : QWidget(parent)
{
    setAutoFillBackground(true);
    // Scopes the theme's find-bar rules (row border, muted labels) and lets the
    // app-level sheet paint them on this plain QWidget.
    setObjectName(QStringLiteral("findBar"));
    setAttribute(Qt::WA_StyledBackground, true);

    // ---- shared search field ----
    edit_ = new SearchLineEdit(this);
    edit_->setPlaceholderText(tr("Find in document"));
    edit_->setClearButtonEnabled(true);
    edit_->setMinimumWidth(200);
    edit_->setMaximumWidth(360);
    edit_->installEventFilter(this);
    // Leading magnifier glyph (matches md-easy's search field).
    searchAction_ = edit_->addAction(QIcon(), QLineEdit::LeadingPosition);
    updateSearchIcon();

    // ---- FindDocument controls ----
    prevBtn_ = new QToolButton(this);
    prevBtn_->setText(tr("Previous"));
    prevBtn_->setToolTip(tr("Previous match (Shift+Enter)"));
    prevBtn_->setAutoRaise(true);

    nextBtn_ = new QToolButton(this);
    nextBtn_->setText(tr("Next"));
    nextBtn_->setToolTip(tr("Next match (Enter)"));
    nextBtn_->setAutoRaise(true);

    countLabel_ = new QLabel(this);
    countLabel_->setMinimumWidth(64);
    countLabel_->setAlignment(Qt::AlignCenter);

    caseCheck_ = new QCheckBox(tr("Match case"), this);
    wordCheck_ = new QCheckBox(tr("Whole word"), this);

    // ---- RecentSearch controls ----
    recentControls_ = new QWidget(this);
    recentControls_->setObjectName(QStringLiteral("recentScopeBar"));
    auto *rcLayout = new QHBoxLayout(recentControls_);
    rcLayout->setContentsMargins(0, 0, 0, 0);
    rcLayout->setSpacing(0);

    nameBtn_ = new QToolButton(recentControls_);
    nameBtn_->setText(tr("Name"));
    nameBtn_->setCheckable(true);
    nameBtn_->setChecked(true);
    nameBtn_->setAutoRaise(false);

    contentsBtn_ = new QToolButton(recentControls_);
    contentsBtn_->setText(tr("Inside documents"));
    contentsBtn_->setCheckable(true);
    contentsBtn_->setAutoRaise(false);

    // Drives the segmented control's outer corner rounding (see mervin::Theme).
    nameBtn_->setProperty("segpos", "first");
    contentsBtn_->setProperty("segpos", "last");

    scopeGroup_ = new QButtonGroup(this);
    scopeGroup_->addButton(nameBtn_, 0);
    scopeGroup_->addButton(contentsBtn_, 1);
    scopeGroup_->setExclusive(true);

    rcLayout->addWidget(nameBtn_);
    rcLayout->addWidget(contentsBtn_);

    // ---- main layout ----
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 5, 6, 5);
    layout->setSpacing(6);
    layout->addWidget(edit_);
    // FindDocument controls
    layout->addWidget(countLabel_);
    layout->addWidget(prevBtn_);
    layout->addWidget(nextBtn_);
    layout->addWidget(caseCheck_);
    layout->addWidget(wordCheck_);
    // RecentSearch controls (hidden initially)
    layout->addWidget(recentControls_);
    layout->addStretch(1);

    recentControls_->setVisible(false);

    // ---- debounce timer ----
    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(kDebounceMs);
    connect(debounce_, &QTimer::timeout, this, &FindBar::emitSearch);

    // textChanged (not textEdited) so the clear button also triggers a search.
    connect(edit_, &QLineEdit::textChanged, this, &FindBar::onTextChanged);
    connect(prevBtn_, &QToolButton::clicked, this, &FindBar::findPrev);
    connect(nextBtn_, &QToolButton::clicked, this, &FindBar::findNext);
    connect(caseCheck_, &QCheckBox::toggled, this, &FindBar::emitSearch);
    connect(wordCheck_, &QCheckBox::toggled, this, &FindBar::emitSearch);
    connect(scopeGroup_, &QButtonGroup::idClicked, this, [this](int) {
        updateRecentPlaceholder();
        emitSearch();
    });

    setResultCount(0, 0);
}

void FindBar::setMode(Mode mode)
{
    if (mode_ == mode)
        return;
    mode_ = mode;

    const bool recent = (mode == Mode::RecentSearch);
    if (recent)
        updateRecentPlaceholder();
    else
        edit_->setPlaceholderText(tr("Find in document"));

    prevBtn_->setVisible(!recent);
    nextBtn_->setVisible(!recent);
    countLabel_->setVisible(!recent);
    caseCheck_->setVisible(!recent);
    wordCheck_->setVisible(!recent);

    recentControls_->setVisible(recent);

    // A debounce armed in the outgoing mode would fire into the incoming one and
    // emit the wrong signal for the field's new contents (restoreFindState() stops
    // it for the same reason).
    debounce_->stop();

    if (recent) {
        // Put this view's own query back. RecentFilesPanel kept filtering by it the
        // whole time we were in a document - nothing clears the panel on the way out
        // - so leaving the field empty here showed an empty box over a filtered
        // list. Blocked, because the panel already holds exactly this filter:
        // re-emitting would only churn, and would restart the content scan that
        // leaving the Recent view deliberately cancelled.
        {
            QSignalBlocker blk(edit_);
            edit_->setText(recentQuery_);
        }
        dirty_ = false;
        // Reset document-find state visually.
        setResultCount(0, 0);
    } else {
        // Entering document mode: park the recent query so it survives the round
        // trip, then clear it so it doesn't pollute the first document search.
        recentQuery_ = edit_->text();
        QSignalBlocker blk(edit_);
        edit_->clear();
        dirty_ = false;
    }
}

void FindBar::activate(const QString &preset)
{
    if (mode_ == Mode::FindDocument && !preset.isEmpty()) {
        QSignalBlocker blocker(edit_);
        edit_->setText(preset);
        dirty_ = true;
        emitSearch();
    }
    edit_->setFocus();
    edit_->selectAll();
}

QString FindBar::query() const
{
    return edit_->text();
}

bool FindBar::caseSensitive() const
{
    return caseCheck_->isChecked();
}

bool FindBar::wholeWord() const
{
    return wordCheck_->isChecked();
}

void FindBar::restoreFindState(const QString &query, bool caseSensitive, bool wholeWord,
                               int current, int total)
{
    // Block signals so seeding the field doesn't trigger a fresh search (which
    // would discard the viewer's current-match position).
    debounce_->stop();
    {
        QSignalBlocker b1(edit_);
        QSignalBlocker b2(caseCheck_);
        QSignalBlocker b3(wordCheck_);
        edit_->setText(query);
        caseCheck_->setChecked(caseSensitive);
        wordCheck_->setChecked(wholeWord);
    }
    dirty_ = false;
    setResultCount(current, total);
}

void FindBar::setResultCount(int current, int total)
{
    if (edit_->text().isEmpty()) {
        countLabel_->clear();
    } else if (total <= 0) {
        countLabel_->setText(tr("No results"));
    } else {
        countLabel_->setText(tr("%1 of %2").arg(current).arg(total));
    }
    const bool hasMatches = total > 0;
    prevBtn_->setEnabled(hasMatches);
    nextBtn_->setEnabled(hasMatches);
}

void FindBar::updateRecentPlaceholder()
{
    edit_->setPlaceholderText(contentsBtn_->isChecked() ? tr("Search inside documents")
                                                         : tr("Search file names"));
}

void FindBar::updateSearchIcon()
{
    if (searchAction_)
        searchAction_->setIcon(makeSearchIcon(palette().color(QPalette::PlaceholderText)));
}

void FindBar::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange)
        updateSearchIcon();
    QWidget::changeEvent(event);
}

void FindBar::onTextChanged()
{
    dirty_ = true;
    debounce_->start();
}

void FindBar::emitSearch()
{
    debounce_->stop();
    dirty_ = false;
    if (mode_ == Mode::RecentSearch)
        emit recentFilterChanged(edit_->text(), contentsBtn_->isChecked());
    else
        emit searchChanged(edit_->text(), caseSensitive(), wholeWord());
}

bool FindBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == edit_ && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        switch (ke->key()) {
        case Qt::Key_Escape:
            emit escapePressed();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (mode_ == Mode::FindDocument) {
                if (dirty_)
                    emitSearch();
                else if (ke->modifiers() & Qt::ShiftModifier)
                    emit findPrev();
                else
                    emit findNext();
                return true;
            }
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace mervin
