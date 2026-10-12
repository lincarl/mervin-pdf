#include "ui/AnnotPopup.h"

#include "ui/Icons.h"
#include "ui/ThemeTokens.h"

#include <QDateTime>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QTextCursor>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace mervin {

namespace {
constexpr int kWidth = 260;
constexpr int kGap = 6; // gap below the annotation rect
constexpr int kMargin = 10;        // left and right padding inside the card
constexpr int kHeaderSpacing = 6;  // between the header text and the close button
// The close and delete buttons match the Comment panel's close X.
constexpr int kIconButton = 22;
constexpr int kIconGlyph = 16;
} // namespace

AnnotPopup::AnnotPopup(QWidget *parent) : QWidget(parent)
{
    // The same floating-panel surface as the Comment and Measure panels (Theme.cpp's
    // QWidget#measurePanel rules), so the card follows the UI theme and stays opaque
    // over any page. Without WA_StyledBackground a plain QWidget never paints its
    // QSS background, and the header and buttons would sit directly on the page.
    setObjectName(QStringLiteral("measurePanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    // Same as AnnotPanel: anchor the background role so child labels resolve to
    // WindowText rather than QPalette::Light (see ViewerWidget's constructor).
    setBackgroundRole(QPalette::Window);
    // The card is a child of the viewport, so without this it would inherit the
    // page's tool cursor (I-beam, pointing hand or crosshair) over its buttons and
    // swatches. The comment editor keeps its own I-beam.
    setCursor(Qt::ArrowCursor);
    setFixedWidth(kWidth);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(kMargin, 8, kMargin, 8);
    outer->setSpacing(6);

    auto *headerRow = new QHBoxLayout;
    headerRow->setSpacing(kHeaderSpacing);
    headerLabel_ = new QLabel(this);
    headerLabel_->setObjectName(QStringLiteral("measureTitle")); // the panel-title ink
    QFont headerFont = headerLabel_->font();
    headerFont.setBold(true);
    headerLabel_->setFont(headerFont);
    // A long author name or a large UI font moves the date to a second line rather
    // than clipping it: the card has a fixed width.
    headerLabel_->setWordWrap(true);
    headerRow->addWidget(headerLabel_, 1);
    auto *closeBtn = new QToolButton(this);
    closeBtn->setObjectName(QStringLiteral("measureClearX")); // square, muted, frameless
    closeBtn->setFixedSize(kIconButton, kIconButton);
    icons::setButtonGlyph(closeBtn, icons::Glyph::Close, kIconGlyph);
    closeBtn->setAutoRaise(true);
    //: Tooltip of the button that closes the annotation card (verb).
    closeBtn->setToolTip(tr("Close"));
    connect(closeBtn, &QToolButton::clicked, this, [this] { hide(); });
    headerRow->addWidget(closeBtn, 0, Qt::AlignTop); // beside the first header line
    outer->addLayout(headerRow);

    comment_ = new QPlainTextEdit(this);
    comment_->setPlaceholderText(tr("Add a comment…"));
    comment_->setFixedHeight(72);
    outer->addWidget(comment_);
    // The swatches and buttons take no focus on click, so Qt would hand the keyboard
    // to the nearest ancestor that does, the viewer. The proxy stops that walk at
    // the card while the comment has focus, so typing continues after a colour pick.
    setFocusProxy(comment_);

    // Colour swatches + delete on one row.
    swatchRow_ = new QWidget(this);
    auto *sl = new QHBoxLayout(swatchRow_);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->setSpacing(6);
    for (const QColor &c : annot::palette()) {
        auto *b = new QToolButton(swatchRow_);
        b->setFixedSize(20, 20);
        b->setCheckable(true);
        b->setToolTip(c.name());
        connect(b, &QToolButton::clicked, this, [this, c] {
            refreshSwatchChecks(c);
            const bool typing = comment_->hasFocus();
            emit colorPicked(c);
            // Applying the colour refreshes the window, which gives the keyboard to
            // the page. Hand it back, so typing continues where it was.
            if (typing)
                comment_->setFocus();
        });
        swatches_ << b;
        swatchColors_ << c;
        sl->addWidget(b);
    }
    sl->addStretch(1);
    deleteBtn_ = new QToolButton(swatchRow_);
    deleteBtn_->setObjectName(QStringLiteral("measureClearX")); // same muted square as the X
    deleteBtn_->setFixedSize(kIconButton, kIconButton);
    icons::setButtonGlyph(deleteBtn_, icons::Glyph::Delete, kIconGlyph);
    deleteBtn_->setAutoRaise(true);
    deleteBtn_->setToolTip(tr("Delete annotation"));
    connect(deleteBtn_, &QToolButton::clicked, this, [this] {
        committedText_ = comment_->toPlainText(); // suppress a commit on the ensuing hide
        emit deleteRequested();
    });
    sl->addWidget(deleteBtn_);
    outer->addWidget(swatchRow_);

    hide();
}

void AnnotPopup::showFor(const Annotation &a, bool allowEdit)
{
    //: Header of an annotation's card when the annotation names no author.
    const QString who = a.author.isEmpty() ? tr("Annotation") : a.author;
    if (a.modifiedMs > 0) {
        const QString when =
            QDateTime::fromMSecsSinceEpoch(a.modifiedMs).toString(QStringLiteral("yyyy-MM-dd"));
        //: Header of an annotation's card. %1 is the author (or "Annotation"), %2
        //: the date it was last changed.
        headerLabel_->setText(tr("%1  ·  %2").arg(who, when));
    } else {
        headerLabel_->setText(who);
    }
    // Sizing the card ignores word wrap and guesses the header's height from its
    // length. The card's width is fixed, so give the header the height it needs at
    // that width: one line when it fits, two when it wraps.
    headerLabel_->setFixedHeight(
        headerLabel_->heightForWidth(kWidth - 2 * kMargin - kHeaderSpacing - kIconButton));

    {
        QSignalBlocker block(comment_);
        comment_->setPlainText(a.contents);
    }
    committedText_ = a.contents;

    const bool editable = allowEdit && a.editable();
    comment_->setReadOnly(!editable);
    swatchRow_->setVisible(editable);
    refreshSwatchChecks(a.color);

    show();
    raise();
}

void AnnotPopup::positionNear(const QRect &annotWidgetRect)
{
    anchorRect_ = annotWidgetRect;
    QWidget *p = parentWidget();
    if (!p)
        return;
    adjustSize();
    int x = annotWidgetRect.left();
    int y = annotWidgetRect.bottom() + kGap;
    // Flip above the annotation if there's no room below.
    if (y + height() > p->height() && annotWidgetRect.top() - kGap - height() >= 0)
        y = annotWidgetRect.top() - kGap - height();
    x = std::clamp(x, kGap, std::max(kGap, p->width() - width() - kGap));
    y = std::clamp(y, kGap, std::max(kGap, p->height() - height() - kGap));
    move(x, y);
}

void AnnotPopup::commit()
{
    if (!comment_ || comment_->isReadOnly())
        return;
    const QString text = comment_->toPlainText();
    if (text != committedText_) {
        committedText_ = text;
        emit commentEdited(text);
    }
}

void AnnotPopup::focusComment()
{
    if (comment_ && !comment_->isReadOnly()) {
        comment_->setFocus();
        comment_->moveCursor(QTextCursor::End);
    }
}

void AnnotPopup::mousePressEvent(QMouseEvent *event)
{
    // A click on the card's padding or header must not reach the page below, where
    // the press would close the card, start a text selection or drop another note,
    // and the release would reopen the annotation under the card.
    event->accept();
}

void AnnotPopup::mouseReleaseEvent(QMouseEvent *event)
{
    event->accept();
}

void AnnotPopup::hideEvent(QHideEvent *event)
{
    commit();
    emit dismissed();
    QWidget::hideEvent(event);
}

void AnnotPopup::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    // A UI theme switch while the card is open: re-pick the swatch rings for the
    // new chrome.
    if (event->type() == QEvent::PaletteChange)
        refreshSwatchChecks(checkedColor_);
}

void AnnotPopup::refreshSwatchChecks(const QColor &color)
{
    checkedColor_ = color;
    const bool dark = theme::isDark(palette());
    for (int i = 0; i < swatches_.size(); ++i) {
        const bool on = swatchColors_[i] == color;
        swatches_[i]->setChecked(on);
        swatches_[i]->setStyleSheet(theme::swatchStyle(swatchColors_[i], on, dark));
    }
}

} // namespace mervin
