#pragma once

#include <QComboBox>
#include <QFontMetrics>
#include <QLineEdit>
#include <QStyle>
#include <QStyleOption>
#include <QString>

namespace mervin {

// A text-only combo must retain space for its longest choice when a form shrinks
// it. Call after populating the choices so the current style supplies the inset.
inline void fitComboText(QComboBox &combo)
{
    combo.ensurePolished();
    int textWidth = 0;
    for (int row = 0; row < combo.count(); ++row)
        textWidth = qMax(textWidth, combo.fontMetrics().size(Qt::TextSingleLine,
                                                           combo.itemText(row)).width());
    QStyleOptionComboBox option;
    option.initFrom(&combo);
    option.editable = combo.isEditable();
    option.frame = combo.hasFrame();
    const QRect content = combo.style()->subControlRect(
        QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, &combo);
    combo.setMinimumWidth(qMax(combo.minimumWidth(), textWidth + combo.width() - content.width()));
}

// QLineEdit's size hint measures a sample value rather than its placeholder.
// Reserve the full hint plus the current style's frame and text margins.
inline void fitPlaceholder(QLineEdit &edit)
{
    edit.ensurePolished();
    QStyleOptionFrame option;
    option.initFrom(&edit);
    option.lineWidth = edit.hasFrame()
        ? edit.style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &option, &edit) : 0;
    const QRect content = edit.style()->subElementRect(QStyle::SE_LineEditContents, &option, &edit);
    const QMargins margins = edit.textMargins();
    const int padding = edit.width() - content.width() + margins.left() + margins.right() + 4;
    edit.setMinimumWidth(qMax(edit.minimumWidth(),
        edit.fontMetrics().size(Qt::TextSingleLine, edit.placeholderText()).width() + padding));
}

// QFontMetrics::elidedText fits character advances. QLabel also needs room for
// glyph overhang, so check the painted extent before assigning its text.
inline QString elidedLabelText(const QFontMetrics &metrics, const QString &text,
                              Qt::TextElideMode mode, int width)
{
    int available = qMax(0, width);
    QString result = metrics.elidedText(text, mode, available);
    while (!result.isEmpty()) {
        const int overflow = metrics.size(Qt::TextSingleLine, result).width() - width;
        if (overflow <= 0)
            return result;
        available = qMax(0, available - overflow);
        if (!available)
            return {};
        result = metrics.elidedText(text, mode, available);
    }
    return result;
}

} // namespace mervin
