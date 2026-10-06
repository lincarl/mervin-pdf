#include "ui/RecentSearchField.h"

#include "ui/Icons.h"
#include "ui/ThemeTokens.h"

#include <QAction>
#include <QButtonGroup>
#include <QEvent>
#include <QFrame>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>

#include <algorithm>

namespace mervin {

namespace {
constexpr int kClearSize = 22;  // the clear button's square
constexpr int kClearGlyph = 14; // Lucide x inside it
constexpr int kRightInset = 6;  // from the field's outer right edge
constexpr int kTextGap = 6;     // between the typed text and the clear button
constexpr qreal kLine = 3;      // the progress line's thickness
constexpr qreal kRuleGap = 4;   // the line ends this far before the rule
} // namespace

RecentSearchField::RecentSearchField(QWidget *parent)
    : SearchLineEdit(parent)
{
    setObjectName(QStringLiteral("recentSearch"));
    searchAction_ = addAction(QIcon(), QLineEdit::LeadingPosition);
    updateSearchIcon();

    // The clear button keeps its place while hidden, so typing never moves the
    // scope toggles.
    clearBtn_ = new QToolButton(this);
    clearBtn_->setObjectName(QStringLiteral("recentSearchClear"));
    clearBtn_->setFixedSize(kClearSize, kClearSize);
    clearBtn_->setFocusPolicy(Qt::NoFocus);
    clearBtn_->setCursor(Qt::ArrowCursor);
    clearBtn_->setToolTip(tr("Clear"));
    icons::setButtonGlyph(clearBtn_, icons::Glyph::Close, kClearGlyph);
    clearBtn_->hide();
    connect(clearBtn_, &QToolButton::clicked, this, [this] {
        clear();
        setFocus(Qt::OtherFocusReason);
    });
    connect(this, &QLineEdit::textChanged, this,
            [this](const QString &text) { clearBtn_->setVisible(!text.isEmpty()); });

    rule_ = new QFrame(this);
    rule_->setObjectName(QStringLiteral("recentSearchRule"));
    rule_->setFixedSize(1, 16);

    const struct
    {
        Scope scope;
        const char *label;
        const char *tip;
    } defs[] = {
        {Scope::Names, QT_TR_NOOP("Names"), QT_TR_NOOP("Search file names")},
        {Scope::Contents, QT_TR_NOOP("Contents"), QT_TR_NOOP("Search inside documents")},
        {Scope::All, QT_TR_NOOP("All"), QT_TR_NOOP("Search file names, then inside documents")},
    };
    scopeGroup_ = new QButtonGroup(this);
    scopeGroup_->setExclusive(true);
    for (const auto &d : defs) {
        auto *btn = new QToolButton(this);
        btn->setObjectName(QStringLiteral("recentScope"));
        btn->setText(tr(d.label));
        btn->setToolTip(tr(d.tip));
        btn->setCheckable(true);
        btn->setAutoRaise(true);
        // Tab still reaches the toggles; a click leaves the caret in the field.
        btn->setFocusPolicy(Qt::TabFocus);
        btn->setCursor(Qt::ArrowCursor);
        scopeGroup_->addButton(btn, static_cast<int>(d.scope));
        scopeBtns_[static_cast<int>(d.scope)] = btn;
    }
    scopeBtns_[static_cast<int>(Scope::Names)]->setChecked(true);
    connect(scopeGroup_, &QButtonGroup::idClicked, this,
            [this](int id) { setScope(static_cast<Scope>(id)); });

    updateScopeLook();
}

void RecentSearchField::setScope(Scope scope)
{
    scopeBtns_[static_cast<int>(scope)]->setChecked(true);
    if (scope_ == scope)
        return;
    scope_ = scope;
    updateScopeLook();
    emit scopeChanged(scope_);
}

void RecentSearchField::setProgress(qreal fraction)
{
    const qreal next = fraction < 0 ? -1 : std::min<qreal>(fraction, 1);
    if (qFuzzyCompare(next + 2, progress_ + 2))
        return;
    progress_ = next;
    update();
}

QString RecentSearchField::settingValue(Scope scope)
{
    switch (scope) {
    case Scope::Contents: return QStringLiteral("contents");
    case Scope::All: return QStringLiteral("all");
    case Scope::Names: break;
    }
    return QStringLiteral("names");
}

RecentSearchField::Scope RecentSearchField::scopeFromSetting(const QString &value)
{
    if (value == QLatin1String("contents"))
        return Scope::Contents;
    if (value == QLatin1String("all"))
        return Scope::All;
    return Scope::Names;
}

// The selected toggle reads in bold (the QSS colours it). Each toggle is as wide
// as its bold label, so switching never shifts the others.
void RecentSearchField::updateScopeLook()
{
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    QFont bold = small;
    bold.setBold(true);
    const QFontMetrics boldMetrics(bold);
    for (QToolButton *btn : scopeBtns_) {
        btn->setFont(btn->isChecked() ? bold : small);
        btn->setFixedSize(boldMetrics.horizontalAdvance(btn->text()) + 10, kClearSize);
    }
    switch (scope_) {
    case Scope::Names: setPlaceholderText(tr("Search file names")); break;
    case Scope::Contents: setPlaceholderText(tr("Search inside documents")); break;
    case Scope::All: setPlaceholderText(tr("Search names and contents")); break;
    }
    layoutRightEnd();
}

// Right end, from the right: the three toggles, a thin rule, the clear button.
// Typed text stops short of the clear button.
void RecentSearchField::layoutRightEnd()
{
    int x = width() - kRightInset;
    const int cy = height() / 2;
    for (int i = 2; i >= 0; --i) {
        QToolButton *btn = scopeBtns_[i];
        x -= btn->width();
        btn->move(x, cy - btn->height() / 2);
    }
    x -= 4 + rule_->width();
    rule_->move(x, cy - rule_->height() / 2);
    x -= 2 + clearBtn_->width();
    clearBtn_->move(x, cy - clearBtn_->height() / 2);
    setTextMargins(0, 0, width() - x + kTextGap, 0);
}

void RecentSearchField::updateSearchIcon()
{
    if (searchAction_)
        searchAction_->setIcon(icons::glyph(icons::Glyph::Search,
                                            palette().color(QPalette::PlaceholderText)));
}

bool RecentSearchField::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::FontChange:
        updateScopeLook();
        break;
    case QEvent::PaletteChange:
    case QEvent::StyleChange:
        updateSearchIcon();
        break;
    default:
        break;
    }
    return SearchLineEdit::event(event);
}

// The progress line sits on the field's inside bottom edge, under the frame drawn
// by the style sheet: a faint track, so the line reads as a bar from the first
// moment, and the filled part in the focus ring's accent. It spans the text and
// the clear button and ends before the rule, never under the scope toggles. It
// repaints only when progress arrives; nothing moves on its own.
void RecentSearchField::paintEvent(QPaintEvent *event)
{
    SearchLineEdit::paintEvent(event);
    if (progress_ < 0)
        return;
    const theme::Chrome t = theme::chrome(palette());
    const QColor accent = theme::legibleAccent(t.accent, t.dark);
    const bool ring = hasFocus();
    const qreal border = ring ? 2 : 1;   // the focus ring, or the resting edge
    const qreal radius = t.dark ? 6 : 8; // as Theme's control radius
    const QRectF inner = QRectF(rect()).adjusted(border, border, -border, -border);
    // Against the focus ring the line reaches 1px into it: the ring is the same
    // accent, so the overlap is invisible, and at 125% or 150% scaling no half
    // pixel of background is left between the two.
    const qreal overlap = ring ? 1 : 0;
    const QRectF reach = inner.adjusted(-overlap, -overlap, overlap, overlap);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(reach, radius - border + overlap, radius - border + overlap);
    p.setClipPath(clip);
    p.setPen(Qt::NoPen);
    QColor track = accent;
    track.setAlphaF(0.28);
    const qreal end = rule_->geometry().left() - kRuleGap;
    const QRectF line(inner.left(), inner.bottom() - kLine, end - inner.left(), kLine + overlap);
    p.setBrush(track);
    p.drawRoundedRect(line, kLine / 2, kLine / 2);
    p.setBrush(accent);
    p.drawRoundedRect(QRectF(line.left(), line.top(), line.width() * progress_, line.height()),
                      kLine / 2, kLine / 2);
}

void RecentSearchField::resizeEvent(QResizeEvent *event)
{
    SearchLineEdit::resizeEvent(event);
    layoutRightEnd();
}

} // namespace mervin
