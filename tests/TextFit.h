#pragma once

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCommonStyle>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStyle>
#include <QStyleOption>
#include <QTextDocument>
#include <QToolButton>
#include <QWidget>

#include <algorithm>
#include <cmath>

// Geometry checks for standard Qt text controls after the dialog has been shown
// and its layout has settled. Custom-painted controls need their own assertions.
// Editable values may scroll. Fixed labels and empty-input hints should fit.
namespace textfit {
namespace detail {

inline QSize textSize(const QFont &font, const QString &text, int flags = 0)
{
    return QFontMetrics(font).size(flags | Qt::TextExpandTabs, text);
}

inline QString describe(const QWidget &widget)
{
    QStringList path;
    for (const QWidget *p = &widget; p; p = p->parentWidget()) {
        path.prepend(QString::fromLatin1(p->metaObject()->className())
                         + (p->objectName().isEmpty() ? QString()
                                                    : QStringLiteral("[%1]").arg(p->objectName())));
        if (p->isWindow())
            break;
    }
    return path.join('/');
}

inline void compare(QStringList &errors, const QWidget &widget, const QString &text,
                    QSize needed, QSize available, const QString &part = {}, bool horizontalCanScroll = false)
{
    // Tests may name one control whose product design explicitly permits
    // horizontal elision. The nonempty reason belongs beside the scenario.
    // This never exempts its children or vertically clipped text.
    const bool allowElision = !widget.property("textFitAllowElision").toString().isEmpty();
    constexpr int roundingTolerance = 1;
    if ((!allowElision && !horizontalCanScroll && needed.width() > available.width() + roundingTolerance)
        || needed.height() > available.height() + roundingTolerance) {
        QString displayText = text;
        displayText.replace('\n', QStringLiteral("\\n"));
        errors.append(QStringLiteral("%1 %2 text=\"%3\" needs %4x%5, available %6x%7")
                          .arg(describe(widget), part, displayText)
                          .arg(needed.width()).arg(needed.height())
                          .arg(available.width()).arg(available.height()));
    }
}

inline void label(QStringList &errors, const QLabel &label)
{
    if (label.text().isEmpty())
        return;
    QRect area = label.contentsRect();
    area.adjust(label.margin(), label.margin(), -label.margin(), -label.margin());
    int indent = label.indent();
    if (indent < 0)
        indent = label.frameWidth()
            ? std::max(0, QFontMetrics(label.font()).horizontalAdvance('x') / 2 - label.margin()) : 0;
    const Qt::Alignment alignment = QStyle::visualAlignment(
        label.text().isRightToLeft() ? Qt::RightToLeft : Qt::LeftToRight, label.alignment());
    if (alignment & Qt::AlignLeft)
        area.setLeft(area.left() + indent);
    if (alignment & Qt::AlignRight)
        area.setRight(area.right() - indent);
    if (alignment & Qt::AlignTop)
        area.setTop(area.top() + indent);
    if (alignment & Qt::AlignBottom)
        area.setBottom(area.bottom() - indent);

    QSize needed;
    const bool rich = label.textFormat() == Qt::RichText
        || (label.textFormat() == Qt::AutoText && Qt::mightBeRichText(label.text()));
    const bool documentLayout = rich || label.textFormat() == Qt::MarkdownText
        || (label.textInteractionFlags() & (Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard))
        || label.focusPolicy() != Qt::NoFocus;
    if (documentLayout) {
        QTextDocument document;
        document.setDefaultFont(label.font());
        document.setDocumentMargin(0);
        QTextOption option;
        option.setWrapMode(label.wordWrap() ? QTextOption::WordWrap : QTextOption::ManualWrap);
        document.setDefaultTextOption(option);
        if (label.textFormat() == Qt::MarkdownText)
            document.setMarkdown(label.text());
        else if (rich)
            document.setHtml(label.text());
        else
            document.setPlainText(label.text());
        document.setTextWidth(label.wordWrap() ? std::max(1, area.width()) : -1);
        needed = QSize(static_cast<int>(std::ceil(document.idealWidth())),
                       static_cast<int>(std::ceil(document.size().height())));
    } else {
        int flags = 0;
        if (label.buddy())
            flags |= Qt::TextShowMnemonic;
        if (label.wordWrap())
            flags |= Qt::TextWordWrap;
        // The height constraint must not suppress overflowing wrapped lines.
        needed = QFontMetrics(label.font()).boundingRect(
            QRect(0, 0, std::max(1, area.width()), 100000), flags, label.text()).size();
    }
    compare(errors, label, label.text(), needed, area.size());
}

inline void button(QStringList &errors, const QAbstractButton &button)
{
    if (button.text().isEmpty())
        return;
    QSize needed = textSize(button.font(), button.text(), Qt::TextShowMnemonic);
    const QSize icon = button.icon().isNull() ? QSize(0, 0)
                                            : button.icon().actualSize(button.iconSize());
    if (const auto *tool = qobject_cast<const QToolButton *>(&button)) {
        Qt::ToolButtonStyle mode = tool->toolButtonStyle();
        if (mode == Qt::ToolButtonFollowStyle)
            mode = static_cast<Qt::ToolButtonStyle>(tool->style()->styleHint(QStyle::SH_ToolButtonStyle));
        if (mode == Qt::ToolButtonIconOnly)
            return;
        if (!icon.isEmpty() && mode == Qt::ToolButtonTextUnderIcon) {
            needed.setWidth(std::max(needed.width(), icon.width()));
            needed.rheight() += icon.height() + 4;
        } else if (!icon.isEmpty() && mode == Qt::ToolButtonTextBesideIcon) {
            needed.rwidth() += icon.width() + 4;
            needed.setHeight(std::max(needed.height(), icon.height()));
        }
        QStyleOptionToolButton option;
        option.initFrom(tool);
        option.text = tool->text();
        option.icon = tool->icon();
        option.iconSize = tool->iconSize();
        option.toolButtonStyle = mode;
        if (tool->menu())
            option.features |= QStyleOptionToolButton::HasMenu;
        if (tool->popupMode() == QToolButton::MenuButtonPopup)
            option.features |= QStyleOptionToolButton::MenuButtonPopup;
        needed = tool->style()->sizeFromContents(QStyle::CT_ToolButton, &option, needed, tool);
        compare(errors, button, button.text(), needed, button.size());
        return;
    }

    QStyleOptionButton option;
    option.initFrom(&button);
    option.text = button.text();
    option.icon = button.icon();
    option.iconSize = button.iconSize();
    QStyle::SubElement element;
    if (qobject_cast<const QCheckBox *>(&button))
        element = QStyle::SE_CheckBoxContents;
    else if (qobject_cast<const QRadioButton *>(&button))
        element = QStyle::SE_RadioButtonContents;
    else if (const auto *push = qobject_cast<const QPushButton *>(&button)) {
        element = QStyle::SE_PushButtonContents;
        if (push->isDefault())
            option.features |= QStyleOptionButton::DefaultButton;
        if (push->autoDefault())
            option.features |= QStyleOptionButton::AutoDefaultButton;
        if (push->isFlat())
            option.features |= QStyleOptionButton::Flat;
        if (push->menu()) {
            option.features |= QStyleOptionButton::HasMenu;
            needed.rwidth() += button.style()->pixelMetric(QStyle::PM_MenuButtonIndicator, &option, &button);
        }
    } else {
        return; // A custom-painted QAbstractButton needs a dedicated assertion.
    }
    if (!icon.isEmpty()) {
        needed.rwidth() += icon.width() + 4;
        needed.setHeight(std::max(needed.height(), icon.height()));
    }
    const QRect area = button.style()->subElementRect(element, &option, &button);
    compare(errors, button, button.text(), needed, area.size());
}

inline void combo(QStringList &errors, const QComboBox &combo)
{
    if (combo.isEditable())
        return; // Its editable value may scroll, like an ordinary QLineEdit.
    QStyleOptionComboBox option;
    option.initFrom(&combo);
    option.editable = false;
    option.frame = combo.hasFrame();
    option.iconSize = combo.iconSize();
    const QRect area = combo.style()->subControlRect(
        QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, &combo);
    for (int row = 0; row < combo.count(); ++row) {
        const QString text = combo.itemText(row);
        if (text.isEmpty())
            continue;
        const QVariant fontData = combo.itemData(row, Qt::FontRole);
        const QFont font = fontData.isValid() ? qvariant_cast<QFont>(fontData).resolve(combo.font())
                                            : combo.font();
        QSize needed = textSize(font, text);
        // QStyle draws an item's icon before the text with four pixels of gap.
        if (!combo.itemIcon(row).isNull()) {
            needed.rwidth() += combo.iconSize().width() + 4;
            needed.setHeight(std::max(needed.height(), combo.iconSize().height()));
        }
        compare(errors, combo, text, needed, area.size(), QStringLiteral("option %1").arg(row));
    }
}

inline void editor(QStringList &errors, const QLineEdit &editor)
{
    const bool spinValue = qobject_cast<QAbstractSpinBox *>(editor.parentWidget()) != nullptr;
    const bool scrollableValue = !spinValue && !editor.text().isEmpty();
    const QString text = spinValue ? editor.text()
        : scrollableValue ? editor.displayText() : editor.placeholderText();
    if (text.isEmpty())
        return;
    QStyleOptionFrame option;
    option.initFrom(&editor);
    option.lineWidth = editor.hasFrame()
        ? editor.style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &option, &editor) : 0;
    QRect area = editor.style()->subElementRect(QStyle::SE_LineEditContents, &option, &editor);
    const QMargins margins = editor.textMargins();
    area = area.marginsRemoved(margins);
    const auto *clearAction = editor.findChild<QAction *>(QStringLiteral("_q_qlineeditclearaction"));
    for (const QAbstractButton *action : editor.findChildren<QAbstractButton *>(QString(), Qt::FindDirectChildrenOnly)) {
        const auto *tool = qobject_cast<const QToolButton *>(action);
        // Qt reserves no space for the clear button once text becomes empty,
        // including the short interval while its fade-out is still visible.
        if (editor.text().isEmpty() && clearAction && tool && tool->defaultAction() == clearAction)
            continue;
        if (action->isVisibleTo(&editor)) {
            const int gap = editor.style()->pixelMetric(QStyle::PM_LineEditIconMargin, nullptr, &editor);
            area.setWidth(area.width() - action->width() - gap);
        }
    }
    // QLineEdit's text layout leaves a two-pixel horizontal margin at each end.
    area.adjust(2, 0, -2, 0);
    compare(errors, editor, text, textSize(editor.font(), text), area.size(),
            spinValue ? QStringLiteral("value")
                : scrollableValue ? QStringLiteral("scrollable value") : QStringLiteral("placeholder"),
            scrollableValue);
}

inline void plainEditor(QStringList &errors, const QPlainTextEdit &editor)
{
    // The editable document may scroll, but a placeholder is painted directly
    // into the viewport and cannot be scrolled to reveal its remaining lines.
    if (!editor.document()->isEmpty() || editor.placeholderText().isEmpty())
        return;
    const int margin = static_cast<int>(editor.document()->documentMargin());
    const QRect area = editor.viewport()->rect().adjusted(margin, margin, 0, 0);
    const QSize needed = QFontMetrics(editor.font()).boundingRect(
        QRect(0, 0, std::max(1, area.width()), 100000), Qt::TextWordWrap,
        editor.placeholderText()).size();
    compare(errors, editor, editor.placeholderText(), needed, area.size(), QStringLiteral("placeholder"));
}

inline void group(QStringList &errors, const QGroupBox &group)
{
    if (group.title().isEmpty())
        return;
    QStyleOptionGroupBox option;
    option.initFrom(&group);
    option.text = group.title();
    option.textAlignment = group.alignment();
    option.subControls = QStyle::SC_GroupBoxLabel | QStyle::SC_GroupBoxFrame;
    if (group.isCheckable())
        option.subControls |= QStyle::SC_GroupBoxCheckBox;
    if (group.isFlat())
        option.features |= QStyleOptionFrame::Flat;
    QRect area = group.style()->subControlRect(
        QStyle::CC_GroupBox, &option, QStyle::SC_GroupBoxLabel, &group);
    if (group.style()->inherits("QStyleSheetStyle")) {
        // QStyleSheetStyle expands the title to its QWindowsStyle parent size
        // while painting. That inherited QCommonStyle calculation adds room
        // beyond the advance-based rectangle returned by subControlRect.
        QCommonStyle native;
        const QSize paintedSize = native.subControlRect(
            QStyle::CC_GroupBox, &option, QStyle::SC_GroupBoxLabel, &group).size();
        area.setSize(area.size().expandedTo(paintedSize));
    }
    area = area.intersected(group.rect());
    compare(errors, group, group.title(), textSize(group.font(), group.title(), Qt::TextShowMnemonic),
            area.size(), QStringLiteral("title"));
}

inline void list(QStringList &errors, const QListWidget &list)
{
    // Document lists may intentionally scroll horizontally. Fixed navigation
    // lists cannot recover clipped translated titles that way.
    if (list.horizontalScrollBarPolicy() != Qt::ScrollBarAlwaysOff)
        return;
    for (int row = 0; row < list.count(); ++row) {
        QListWidgetItem *item = list.item(row);
        if (item->isHidden() || item->text().isEmpty() || list.itemWidget(item))
            continue;
        QStyleOptionViewItem option;
        option.initFrom(&list);
        option.rect = list.visualItemRect(item);
        option.font = item->font().resolve(list.font());
        option.fontMetrics = QFontMetrics(option.font);
        option.text = item->text();
        option.features = QStyleOptionViewItem::HasDisplay;
        option.displayAlignment = Qt::Alignment(item->textAlignment());
        if (!item->icon().isNull()) {
            option.features |= QStyleOptionViewItem::HasDecoration;
            option.icon = item->icon();
            const int smallIcon = list.style()->pixelMetric(QStyle::PM_SmallIconSize, &option, &list);
            option.decorationSize = list.iconSize().isValid() ? list.iconSize()
                                                            : QSize(smallIcon, smallIcon);
        }
        if (item->data(Qt::CheckStateRole).isValid())
            option.features |= QStyleOptionViewItem::HasCheckIndicator;
        QRect area = list.style()->subElementRect(QStyle::SE_ItemViewItemText, &option, &list);
        // visualItemRect may be wider than the viewport even when sideways
        // scrolling is disabled. Only vertical offscreen rows remain reachable.
        area.setLeft(std::max(0, area.left()));
        area.setRight(std::min(list.viewport()->width() - 1, area.right()));
        compare(errors, list, item->text(), textSize(option.font, item->text()), area.size(),
                QStringLiteral("row %1").arg(row));
    }
}

} // namespace detail

// The caller opens each relevant state, including stack pages and dynamic
// messages. Offscreen descendants of scroll areas retain their own full layout
// size and are checked without treating the viewport boundary as text clipping.
inline QStringList check(QWidget &root)
{
    QStringList errors;
    QList<QWidget *> widgets = root.findChildren<QWidget *>();
    widgets.prepend(&root);
    for (QWidget *widget : widgets) {
        if (!widget->isVisibleTo(&root))
            continue;
        QWidget *parent = widget->parentWidget();
        const auto *scroll = parent ? qobject_cast<QAbstractScrollArea *>(parent->parentWidget()) : nullptr;
        const bool inViewport = scroll && scroll->viewport() == parent;
        if (widget != &root && !widget->isWindow() && parent && !inViewport
            && !parent->rect().adjusted(-1, -1, 1, 1).contains(widget->geometry())) {
            const QRect geometry = widget->geometry();
            errors.append(QStringLiteral("%1 extends outside its parent, geometry %2,%3 %4x%5, parent %6x%7")
                              .arg(detail::describe(*widget))
                              .arg(geometry.x()).arg(geometry.y()).arg(geometry.width()).arg(geometry.height())
                              .arg(parent->width()).arg(parent->height()));
        }
        if (const auto *label = qobject_cast<QLabel *>(widget))
            detail::label(errors, *label);
        else if (const auto *button = qobject_cast<QAbstractButton *>(widget))
            detail::button(errors, *button);
        else if (const auto *combo = qobject_cast<QComboBox *>(widget))
            detail::combo(errors, *combo);
        else if (const auto *editor = qobject_cast<QLineEdit *>(widget))
            detail::editor(errors, *editor);
        else if (const auto *editor = qobject_cast<QPlainTextEdit *>(widget))
            detail::plainEditor(errors, *editor);
        else if (const auto *group = qobject_cast<QGroupBox *>(widget))
            detail::group(errors, *group);
        else if (const auto *list = qobject_cast<QListWidget *>(widget))
            detail::list(errors, *list);
    }
    return errors;
}

} // namespace textfit
