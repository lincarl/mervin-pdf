#include "ui/WaitBar.h"

#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

#include <algorithm>

namespace mervin {

namespace {
// The bar's height without its bottom border, the room beside its text and between
// the text and the button.
constexpr int kBarHeight = 42;
constexpr int kSidePadding = 16;
constexpr int kTextToButton = 14;
} // namespace

QString fitFileSentence(const FileSentence &compose, const QString &name, const QString &folder,
                        const QFontMetrics &metrics, int width)
{
    // `text` at most `cap` pixels wide, with the breaks above, as a bidirectional
    // isolate, so a path keeps its order in a right-to-left sentence.
    const auto part = [&metrics](const QString &text, int cap, bool path) {
        // elidedText() measures without rounding, so it can shorten a text that
        // horizontalAdvance() says is `cap` wide.
        const QString shown = metrics.horizontalAdvance(text) <= cap
                                  ? text
                                  : metrics.elidedText(text, Qt::ElideMiddle, cap);
        const auto separator = [path](QChar c) { return path && (c == u'/' || c == u'\\'); };
        QString out = QChar(0x2068); // first strong isolate
        bool leading = true;         // in the separators the text starts with
        for (qsizetype i = 0; i < shown.size(); ++i) {
            const QChar c = shown.at(i);
            out += c;
            leading = leading && separator(c);
            if (i + 1 == shown.size())
                break;
            if (leading || c == QChar(0x2026) || separator(shown.at(i + 1)))
                out += QChar(0x2060); // word joiner, no break here
            else if (separator(c))
                out += QChar(0x200B); // zero width space, a break here
        }
        return out + QChar(0x2069); // pop directional isolate
    };
    const auto sentence = [&](int nameCap, int folderCap) {
        return compose(part(name, nameCap, false), part(folder, folderCap, true));
    };
    const auto fits = [&](const QString &text) {
        return metrics.boundingRect(QRect(0, 0, width, 1 << 20), Qt::TextWordWrap, text).width()
            <= width;
    };
    const int nameWidth = metrics.horizontalAdvance(name);
    const int folderWidth = metrics.horizontalAdvance(folder);
    if (width <= 0 || fits(sentence(nameWidth, folderWidth)))
        return sentence(nameWidth, folderWidth);
    // The widest cap up to `widest` at which `fitsAt` holds, or 0.
    const auto widestFitting = [](int widest, const auto &fitsAt) {
        if (fitsAt(widest))
            return widest;
        int fitting = 0;
        int tooWide = widest;
        while (tooWide - fitting > 1) {
            const int cap = (fitting + tooWide) / 2;
            (fitsAt(cap) ? fitting : tooWide) = cap;
        }
        return fitting;
    };
    // Each part beside the other shortened to nothing, so only its own pieces count.
    const int nameCap = widestFitting(nameWidth, [&](int cap) { return fits(sentence(cap, 0)); });
    const int folderCap =
        widestFitting(folderWidth, [&](int cap) { return fits(sentence(0, cap)); });
    // A translation can put pieces of both on one line. Then both are shortened alike.
    const int cap = widestFitting(std::max(nameCap, folderCap), [&](int cap) {
        return fits(sentence(std::min(nameCap, cap), std::min(folderCap, cap)));
    });
    return sentence(std::min(nameCap, cap), std::min(folderCap, cap));
}

WaitBar::WaitBar(const QString &stopText, QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("waitBar"));
    setAttribute(Qt::WA_StyledBackground, true);
    // The stylesheet draws a 1 px line along the bottom edge, below the margins.
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(kSidePadding, 0, kSidePadding, 1);
    layout->setSpacing(kTextToButton);
    text_ = new QLabel(this);
    text_->setObjectName(QStringLiteral("waitBarText"));
    text_->setWordWrap(true);
    text_->setTextFormat(Qt::PlainText);
    text_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    text_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    text_->installEventFilter(this); // fits the text to its width
    layout->addWidget(text_, 1);
    stop_ = new QPushButton(stopText, this);
    stop_->setObjectName(QStringLiteral("waitBarStop"));
    layout->addWidget(stop_, 0, Qt::AlignVCenter);
    setMinimumHeight(kBarHeight + 1);
    hide();
}

void WaitBar::setNotice(FileSentence compose, const QString &name, const QString &folder)
{
    compose_ = std::move(compose);
    name_ = name;
    folder_ = folder;
    fitText();
}

void WaitBar::place(int left, int width)
{
    // The layout's height for a width follows the text as fitted now. The label's
    // resize fits the text to the new width, which places the bar again if it then
    // needs another height.
    setGeometry(left, 0, width, std::max(minimumHeight(), heightForWidth(width)));
}

void WaitBar::fitText()
{
    if (!compose_)
        return;
    const QString text = fitFileSentence(compose_, name_, folder_, text_->fontMetrics(),
                                         text_->contentsRect().width());
    if (text == text_->text())
        return;
    text_->setText(text);
    if (!isHidden())
        place(x(), width());
}

bool WaitBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == text_ && (event->type() == QEvent::Resize || event->type() == QEvent::FontChange))
        fitText();
    return QWidget::eventFilter(watched, event);
}

} // namespace mervin
