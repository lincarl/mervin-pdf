#include "ui/AnnotPopup.h"

#include "ui/ThemeTokens.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTextCursor>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace mervin {

namespace {
constexpr int kWidth = 260;
constexpr int kGap = 6; // gap below the annotation rect
} // namespace

AnnotPopup::AnnotPopup(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("annotPopup"));
    // A self-contained surface that overrides the dark app chrome QSS so the
    // editor reads as a light note card, like the form inline editors do. The
    // values are the note-card group of the document vocabulary (ThemeTokens.h):
    // deliberately light in BOTH themes, because the card sits on the page.
    const theme::Doc &d = theme::doc();
    setStyleSheet(
        QStringLiteral("#annotPopup{background:%1;border:1px solid %2;border-radius:8px;}"
                       "#annotPopup QPlainTextEdit{background:%3;color:%4;"
                       "border:1px solid %5;border-radius:4px;}"
                       "#annotPopup QLabel{color:%6;}"
                       // Without this the app sheet's QToolButton ink wins and the
                       // close X paints in the dark chrome's #c2c9d6 on cream: 1.6:1.
                       "#annotPopup QToolButton{color:%6;}")
            .arg(theme::css(d.noteCard), theme::css(d.noteCardBorder),
                 theme::css(d.noteCardEditor), theme::css(d.noteCardInk),
                 theme::css(d.noteCardEditorBorder), theme::css(d.noteCardLabelInk)));
    setFixedWidth(kWidth);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 8, 10, 8);
    outer->setSpacing(6);

    auto *headerRow = new QHBoxLayout;
    headerRow->setSpacing(6);
    headerLabel_ = new QLabel(this);
    headerLabel_->setStyleSheet(QStringLiteral("font-weight:600;"));
    headerRow->addWidget(headerLabel_, 1);
    auto *closeBtn = new QToolButton(this);
    closeBtn->setText(QStringLiteral("✕"));
    closeBtn->setAutoRaise(true);
    closeBtn->setToolTip(tr("Close"));
    connect(closeBtn, &QToolButton::clicked, this, [this] { hide(); });
    headerRow->addWidget(closeBtn);
    outer->addLayout(headerRow);

    comment_ = new QPlainTextEdit(this);
    comment_->setPlaceholderText(tr("Add a comment…"));
    comment_->setFixedHeight(72);
    outer->addWidget(comment_);

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
        b->setStyleSheet(theme::swatchStyle(c, false));
        connect(b, &QToolButton::clicked, this, [this, c] {
            refreshSwatchChecks(c);
            emit colorPicked(c);
        });
        swatches_ << b;
        swatchColors_ << c;
        sl->addWidget(b);
    }
    sl->addStretch(1);
    deleteBtn_ = new QToolButton(swatchRow_);
    deleteBtn_->setText(QStringLiteral("🗑"));
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
    QString who = a.author.isEmpty() ? tr("Annotation") : a.author;
    if (a.modifiedMs > 0) {
        const QString when =
            QDateTime::fromMSecsSinceEpoch(a.modifiedMs).toString(QStringLiteral("yyyy-MM-dd"));
        who += QStringLiteral("  ·  ") + when;
    }
    headerLabel_->setText(who);

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

void AnnotPopup::hideEvent(QHideEvent *event)
{
    commit();
    emit dismissed();
    QWidget::hideEvent(event);
}

void AnnotPopup::refreshSwatchChecks(const QColor &color)
{
    for (int i = 0; i < swatches_.size(); ++i) {
        const bool on = swatchColors_[i] == color;
        swatches_[i]->setChecked(on);
        swatches_[i]->setStyleSheet(theme::swatchStyle(swatchColors_[i], on));
    }
}

} // namespace mervin
