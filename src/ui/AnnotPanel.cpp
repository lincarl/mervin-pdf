#include "ui/AnnotPanel.h"

#include "ui/PanelStack.h"

#include <QButtonGroup>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>

#include <iterator>

namespace mervin {

namespace {
// Clear the checked button of an exclusive group. An exclusive QButtonGroup will
// not let you un-check its current button directly, so drop exclusivity for the
// toggle and restore it - leaving the group with nothing checked.
void clearChecked(QButtonGroup *group)
{
    if (QAbstractButton *b = group->checkedButton()) {
        group->setExclusive(false);
        b->setChecked(false);
        group->setExclusive(true);
    }
}
} // namespace

AnnotPanel::AnnotPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("measurePanel")); // reuse the measure panel's QSS surface
    setAttribute(Qt::WA_StyledBackground, true);
    // Same as MeasurePanel: anchor the background role so child labels resolve to
    // WindowText rather than QPalette::Light (see ViewerWidget's constructor).
    setBackgroundRole(QPalette::Window);
    setCursor(Qt::ArrowCursor);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 8, 10, 10);
    outer->setSpacing(8);

    // Header: title (drag handle) + close.
    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    auto *title = new QLabel(tr("Comment"), this);
    title->setObjectName(QStringLiteral("measureTitle")); // same panel-title ink
    QFont tf = title->font();
    tf.setBold(true);
    title->setFont(tf);
    auto *closeBtn = new QToolButton(this);
    closeBtn->setObjectName(QStringLiteral("measureClearX")); // square, muted, frameless
    closeBtn->setFixedSize(22, 22);
    closeBtn->setText(QStringLiteral("✕"));
    closeBtn->setAutoRaise(true);
    closeBtn->setToolTip(tr("Close comment tool"));
    connect(closeBtn, &QToolButton::clicked, this, &AnnotPanel::closeRequested);
    header->addWidget(title);
    header->addStretch(1);
    header->addWidget(closeBtn);
    outer->addLayout(header);

    // Mode selector, stacked as a vertical list: Select (pointer) / Comment
    // (sticky note). The third gesture, Markup, has no button of its own - it is
    // armed by picking one of the style buttons below. Exclusive, but with NEITHER
    // checked while Markup is active (cleared via setMode), so the checked-set is
    // really "Select, Comment, or one of the three styles", never two at once.
    modeGroup_ = new QButtonGroup(this);
    modeGroup_->setExclusive(true);
    struct ModeDef { AnnotSubMode mode; const char *label; const char *tip; };
    const ModeDef modes[] = {
        {AnnotSubMode::Select, QT_TR_NOOP("Select"),
         QT_TR_NOOP("Pointer: select and copy text (no annotation)")},
        {AnnotSubMode::Note, QT_TR_NOOP("Comment"),
         QT_TR_NOOP("Click the page to drop a sticky-note comment")},
    };
    for (const ModeDef &m : modes) {
        auto *b = new QToolButton(this);
        b->setText(tr(m.label));
        b->setToolTip(tr(m.tip));
        b->setCheckable(true);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); // full-width list
        modeGroup_->addButton(b, static_cast<int>(m.mode));
        outer->addWidget(b); // one button per row -> vertical list
    }
    connect(modeGroup_, &QButtonGroup::idClicked, this,
            [this](int id) { emit modeChanged(static_cast<AnnotSubMode>(id)); });

    // Markup style: full-name buttons in one horizontal row, below Comment. Picking
    // one is what selects the Markup sub-mode, so a style click emits BOTH the style
    // and modeChanged(Markup); the viewer round-trips back through setMode, which
    // clears Select/Comment. Default-checked (Highlight) to match the tool's open
    // state (the viewer arms Markup when the Comment tool opens).
    // Three mutually exclusive choices, so this is the app's segmented control -
    // the same element the measuring panel's kind row uses, and the only treatment
    // in which "selected" is unmistakable.
    auto *styleBar = new QWidget(this);
    styleBar->setObjectName(QStringLiteral("segmentBar"));
    auto *styleRow = new QHBoxLayout(styleBar);
    styleRow->setContentsMargins(0, 0, 0, 0);
    styleRow->setSpacing(0); // segments share their borders
    styleGroup_ = new QButtonGroup(this);
    styleGroup_->setExclusive(true);
    struct StyleDef { AnnotType type; const char *label; const char *tip; };
    const StyleDef styles[] = {
        {AnnotType::Highlight, QT_TR_NOOP("Highlight"), QT_TR_NOOP("Highlight the selected text")},
        {AnnotType::Underline, QT_TR_NOOP("Underline"), QT_TR_NOOP("Underline the selected text")},
        {AnnotType::StrikeOut, QT_TR_NOOP("Strike out"), QT_TR_NOOP("Strike out the selected text")},
    };
    const int styleCount = static_cast<int>(std::size(styles));
    for (int i = 0; i < styleCount; ++i) {
        const StyleDef &d = styles[i];
        auto *b = new QToolButton(styleBar);
        b->setText(tr(d.label));
        b->setCheckable(true);
        b->setToolTip(tr(d.tip));
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        // Corner rounding and shared borders come from `segpos` (Qt QSS does not
        // reliably match :first-child/:last-child on plain child widgets).
        b->setProperty("segpos", i == 0 ? QStringLiteral("first")
                                        : i == styleCount - 1 ? QStringLiteral("last")
                                                              : QStringLiteral("mid"));
        styleGroup_->addButton(b, static_cast<int>(d.type));
        styleRow->addWidget(b, 1); // equal-width segments across the row
        if (d.type == AnnotType::Highlight)
            b->setChecked(true);
    }
    connect(styleGroup_, &QButtonGroup::idClicked, this, [this](int id) {
        activeStyle_ = static_cast<AnnotType>(id);
        emit highlightStyleChanged(activeStyle_);
        // Picking a style arms Markup; let the viewer drive the round-trip that
        // un-checks Select/Comment (skip when already in Markup - just a restyle).
        if (currentMode_ != AnnotSubMode::Markup)
            emit modeChanged(AnnotSubMode::Markup);
    });
    outer->addWidget(styleBar);

    // No colour picker here (see class doc): new marks use the default-colour Setting.
    adjustSize();
}

void AnnotPanel::setMode(AnnotSubMode mode)
{
    if (!modeGroup_ || !styleGroup_)
        return;
    currentMode_ = mode;
    QSignalBlocker blockMode(modeGroup_);
    QSignalBlocker blockStyle(styleGroup_);
    if (mode == AnnotSubMode::Markup) {
        // A style is active: clear Select/Comment, (re-)check the active style.
        clearChecked(modeGroup_);
        if (QAbstractButton *b = styleGroup_->button(static_cast<int>(activeStyle_)))
            b->setChecked(true);
    } else {
        // Select or Comment: check that mode button, de-select all three styles.
        if (QAbstractButton *b = modeGroup_->button(static_cast<int>(mode)))
            b->setChecked(true);
        clearChecked(styleGroup_);
    }
}

void AnnotPanel::setHighlightStyle(AnnotType type)
{
    if (!styleGroup_ || !isTextMarkup(type))
        return;
    activeStyle_ = type;
    // Only reflect the swatch while Markup is the live mode; in Select/Comment the
    // style buttons stay de-selected (the choice is remembered in activeStyle_).
    if (currentMode_ != AnnotSubMode::Markup)
        return;
    if (QAbstractButton *b = styleGroup_->button(static_cast<int>(type))) {
        QSignalBlocker block(styleGroup_);
        b->setChecked(true);
    }
}

void AnnotPanel::mousePressEvent(QMouseEvent *event)
{
    // Drag the panel body to move the whole dock as a group; control presses are
    // consumed by the buttons and never reach here.
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        dragLastGlobal_ = event->globalPosition().toPoint();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void AnnotPanel::mouseMoveEvent(QMouseEvent *event)
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

void AnnotPanel::mouseReleaseEvent(QMouseEvent *event)
{
    dragging_ = false;
    QWidget::mouseReleaseEvent(event);
}

void AnnotPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    adjustSize();
    if (stack_)
        stack_->relayout(); // dock places this panel (and re-stacks the others)
}

} // namespace mervin
