#include "ui/FindCard.h"

#include "ui/Icons.h"
#include "ui/PanelStack.h"
#include "ui/SearchLineEdit.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>

namespace mervin {

namespace {
constexpr int kDebounceMs = 200;
constexpr int kFieldWidth = 236;
constexpr int kButtonSize = 28;
constexpr int kGlyphPx = 16;
constexpr int kCountInset = 9; // the count's right edge from the field's outer edge
constexpr int kCountGap = 8;   // between the typed text and the count

// A thin vertical rule between the card's groups (QSS paints it).
QFrame *makeSeparator(QWidget *parent)
{
    auto *sep = new QFrame(parent);
    sep->setObjectName(QStringLiteral("findCardSep"));
    sep->setFixedSize(1, 18);
    return sep;
}

QToolButton *makeIconButton(QWidget *parent, icons::Glyph glyph, const QString &tip)
{
    auto *btn = new QToolButton(parent);
    btn->setFixedSize(kButtonSize, kButtonSize);
    btn->setAutoRaise(true);
    btn->setToolTip(tip);
    icons::setButtonGlyph(btn, glyph, kGlyphPx);
    return btn;
}
} // namespace

FindCard::FindCard(QWidget *viewport)
    : QWidget(viewport)
{
    setObjectName(QStringLiteral("findCard"));
    setAttribute(Qt::WA_StyledBackground, true);
    // The viewport's Dark background role would make Qt derive QPalette::Light ink
    // for every child (see MeasurePanel); anchor the role so the QSS inks apply.
    setBackgroundRole(QPalette::Window);
    setCursor(Qt::ArrowCursor);

    edit_ = new SearchLineEdit(this);
    edit_->setObjectName(QStringLiteral("findField"));
    edit_->setPlaceholderText(tr("Find in document"));
    edit_->setFixedWidth(kFieldWidth);
    edit_->installEventFilter(this);
    searchAction_ = edit_->addAction(QIcon(), QLineEdit::LeadingPosition);
    updateSearchIcon();

    count_ = new QLabel(edit_);
    count_->setObjectName(QStringLiteral("findCount"));
    count_->setAttribute(Qt::WA_TransparentForMouseEvents); // clicks reach the field

    prevBtn_ = makeIconButton(this, icons::Glyph::ChevronUp, tr("Previous match (Shift+Enter)"));
    nextBtn_ = makeIconButton(this, icons::Glyph::ChevronDown, tr("Next match (Enter)"));
    caseCheck_ = new QCheckBox(tr("Match case"), this);
    wordCheck_ = new QCheckBox(tr("Whole word"), this);
    closeBtn_ = makeIconButton(this, icons::Glyph::Close, tr("Close (Esc)"));
    closeBtn_->setObjectName(QStringLiteral("findCardClose"));

    // Clicking an option or a button must leave the caret in the field, so the
    // next keystroke still edits the query. Tab still reaches every control. A
    // click on a control that takes no focus makes Qt walk up to the nearest
    // ancestor that does, which is the viewer; the focus proxy stops that walk at
    // the card, because the field it points to already has focus.
    setFocusProxy(edit_);
    for (QWidget *w : {static_cast<QWidget *>(prevBtn_), static_cast<QWidget *>(nextBtn_),
                       static_cast<QWidget *>(caseCheck_), static_cast<QWidget *>(wordCheck_),
                       static_cast<QWidget *>(closeBtn_)})
        w->setFocusPolicy(Qt::TabFocus);

    // Margins include the 1px QSS border: a 44px card around the 30px field.
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(7, 7, 5, 7);
    layout->setSpacing(2);
    layout->addWidget(edit_);
    layout->addSpacing(4);
    layout->addWidget(prevBtn_);
    layout->addWidget(nextBtn_);
    layout->addSpacing(6);
    layout->addWidget(makeSeparator(this));
    layout->addSpacing(8);
    layout->addWidget(caseCheck_);
    layout->addSpacing(12);
    layout->addWidget(wordCheck_);
    layout->addSpacing(8);
    layout->addWidget(makeSeparator(this));
    layout->addSpacing(6);
    layout->addWidget(closeBtn_);

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(kDebounceMs);
    connect(debounce_, &QTimer::timeout, this, &FindCard::emitSearch);

    // textChanged (not textEdited) so a preset or a cleared field also searches.
    connect(edit_, &QLineEdit::textChanged, this, [this] {
        dirty_ = true;
        debounce_->start();
    });
    connect(prevBtn_, &QToolButton::clicked, this, &FindCard::prev);
    connect(nextBtn_, &QToolButton::clicked, this, &FindCard::next);
    connect(caseCheck_, &QCheckBox::toggled, this, &FindCard::emitSearch);
    connect(wordCheck_, &QCheckBox::toggled, this, &FindCard::emitSearch);
    connect(closeBtn_, &QToolButton::clicked, this, &FindCard::dismiss);

    setResultCount(0, 0);
    hide();
}

void FindCard::open(const QString &preset)
{
    const bool wasOpen = isOpen();
    if (!preset.isEmpty()) {
        QSignalBlocker blocker(edit_);
        edit_->setText(preset);
        dirty_ = true;
    } else if (!wasOpen && !edit_->text().isEmpty()) {
        dirty_ = true; // closing cleared the highlights; search the kept query again
    }
    show();
    raise();
    if (stack_)
        stack_->relayout();
    edit_->setFocus(Qt::ShortcutFocusReason);
    edit_->selectAll();
    if (dirty_)
        emitSearch();
    if (!wasOpen)
        emit openChanged(true);
}

void FindCard::dismiss()
{
    if (!isOpen())
        return;
    debounce_->stop();
    dirty_ = false;
    // Hiding the widget that has focus makes Qt move focus to the next widget in
    // the chain, and in Fill Forms mode the viewer answers that by jumping to a
    // form field. Drop focus first; the owner hands it back to the page.
    if (QWidget *focus = QApplication::focusWidget(); focus && isAncestorOf(focus))
        focus->clearFocus();
    hide();
    if (stack_)
        stack_->relayout();
    emit openChanged(false);
}

QString FindCard::query() const
{
    return edit_->text();
}

bool FindCard::caseSensitive() const
{
    return caseCheck_->isChecked();
}

bool FindCard::wholeWord() const
{
    return wordCheck_->isChecked();
}

void FindCard::setResultCount(int current, int total)
{
    if (edit_->text().isEmpty())
        count_->clear();
    else if (total <= 0)
        count_->setText(tr("No results"));
    else
        //: Match counter in the find field. %1 is the current match, %2 the number
        //: of matches.
        count_->setText(tr("%1 of %2").arg(current).arg(total));
    const bool hasMatches = total > 0;
    prevBtn_->setEnabled(hasMatches);
    nextBtn_->setEnabled(hasMatches);
    placeCount();
}

void FindCard::next()
{
    if (dirty_)
        emitSearch();
    else
        emit findNext();
}

void FindCard::prev()
{
    if (dirty_)
        emitSearch();
    else
        emit findPrev();
}

void FindCard::emitSearch()
{
    debounce_->stop();
    dirty_ = false;
    emit searchChanged(edit_->text(), caseSensitive(), wholeWord());
}

void FindCard::updateSearchIcon()
{
    if (searchAction_)
        searchAction_->setIcon(icons::glyph(icons::Glyph::Search,
                                            palette().color(QPalette::PlaceholderText)));
}

// Keeps the count at the field's right end and stops typed text short of it.
void FindCard::placeCount()
{
    const bool shown = !count_->text().isEmpty();
    count_->setVisible(shown);
    if (!shown) {
        edit_->setTextMargins(0, 0, 0, 0);
        return;
    }
    count_->adjustSize();
    count_->move(edit_->width() - kCountInset - count_->width(),
                 (edit_->height() - count_->height()) / 2);
    edit_->setTextMargins(0, 0, count_->width() + kCountGap, 0);
}

bool FindCard::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == edit_) {
        if (event->type() == QEvent::Resize) {
            placeCount();
        } else if (event->type() == QEvent::KeyPress) {
            auto *ke = static_cast<QKeyEvent *>(event);
            switch (ke->key()) {
            case Qt::Key_Escape:
                dismiss();
                return true;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                if (ke->modifiers() & Qt::ShiftModifier)
                    prev();
                else
                    next();
                return true;
            default:
                break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FindCard::keyPressEvent(QKeyEvent *event)
{
    // Escape from a control reached with Tab closes the card too. Any other key a
    // control leaves unused stops here: passed on, it would reach the viewer and,
    // for example, Backspace would remove the last measuring point. Shortcuts are
    // matched before key presses, so they still work.
    if (event->key() == Qt::Key_Escape)
        dismiss();
    event->accept();
}

void FindCard::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        updateSearchIcon(); // the magnifier follows the card's placeholder ink
    QWidget::changeEvent(event);
}

void FindCard::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    adjustSize();
    if (stack_)
        stack_->relayout();
    placeCount();
}

void FindCard::mousePressEvent(QMouseEvent *event)
{
    // Dragging the card body drags the whole panel stack, as with MeasurePanel.
    // Presses on the field and the buttons are consumed by them.
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        dragLastGlobal_ = event->globalPosition().toPoint();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void FindCard::mouseMoveEvent(QMouseEvent *event)
{
    if (dragging_ && (event->buttons() & Qt::LeftButton)) {
        const QPoint g = event->globalPosition().toPoint();
        if (stack_)
            stack_->nudge(g - dragLastGlobal_);
        dragLastGlobal_ = g;
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void FindCard::mouseReleaseEvent(QMouseEvent *event)
{
    dragging_ = false;
    QWidget::mouseReleaseEvent(event);
}

} // namespace mervin
