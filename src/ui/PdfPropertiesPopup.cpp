#include "ui/PdfPropertiesPopup.h"

#include "ui/Icons.h"
#include "ui/ThemeTokens.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSizePolicy>
#include <QStringList>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextOption>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace mervin {

namespace {
constexpr int kBaseWidth = 360;
constexpr int kMaxWidth = kBaseWidth * 2;
constexpr int kGap = 6;
constexpr int kMaxRowsHeight = 360;
constexpr int kPopupHorizontalMargins = 20;
constexpr int kRowHorizontalMargins = 12;
constexpr int kCopyButtonWidth = 24;
constexpr int kRowSpacing = 6;
constexpr int kScrollbarAllowance = 18;
constexpr int kCloseButtonWidth = 24;
constexpr int kHeaderSpacing = 6;

const QRegularExpression &webLinkRegex()
{
    static const QRegularExpression urlRegex(QStringLiteral(R"((https?://[^\s<>"']+))"));
    return urlRegex;
}

// The popup is a floating card on the popover surface, in both themes. It builds
// its own sheet (rather than living in the app-wide one) because each row bakes
// its ink into rich text, but every colour comes from the shared vocabulary in
// ui/ThemeTokens.h - there is no second definition of "the popover surface" here.
QString popupStyleSheet(const QPalette &pal)
{
    const theme::Chrome t = theme::chrome(pal);
    const QString surface = theme::css(t.popover);
    return QStringLiteral(
               "#pdfPropertiesPopup{background:%1;border:1px solid %2;border-radius:10px;}"
               "#pdfPropertiesPopup QWidget#pdfPropertiesRows,"
               "#pdfPropertiesPopup QWidget#pdfPropertiesViewport{background:%1;}"
               "#pdfPropertiesPopup QWidget#pdfPropertyRow{background:%1;}"
               "#pdfPropertiesPopup QWidget#pdfPropertyRow:hover{background:%3;}"
               "#pdfPropertiesPopup QLabel{color:%4;}"
               "#pdfPropertiesPopup QScrollArea{background:transparent;border:0;}"
               "#pdfPropertiesPopup QToolButton{border:0;border-radius:4px;padding:3px;}"
               "#pdfPropertiesPopup QToolButton:hover{background:%5;}"
               "#pdfPropertiesPopup QToolButton#pdfPropsClose{color:%6;font-weight:700;"
               "font-size:14px;}")
        .arg(surface, theme::css(t.borderPopover), theme::css(t.rowHover), theme::css(t.ink),
             theme::css(t.hover), theme::css(t.inkSoft));
}

int textColumnWidthForPopup(int popupWidth)
{
    return std::max(1, popupWidth - kPopupHorizontalMargins - kRowHorizontalMargins - kCopyButtonWidth
                           - kRowSpacing - kScrollbarAllowance);
}

int preferredPopupWidth(const QStringList &values, const QFontMetrics &metrics, int headingWidth)
{
    int textWidth = 0;
    for (const QString &value : values)
        textWidth = std::max(textWidth, metrics.horizontalAdvance(value));

    const int rowWidth = textWidth + kPopupHorizontalMargins + kRowHorizontalMargins + kCopyButtonWidth
                         + kRowSpacing + kScrollbarAllowance;
    const int headerWidth = headingWidth + kPopupHorizontalMargins + kHeaderSpacing + kCloseButtonWidth;
    return std::min(std::max(rowWidth, headerWidth), kMaxWidth);
}

QString linkifiedPropertyText(const QString &text, const QString &linkCss)
{
    QString html;
    int last = 0;
    auto matches = webLinkRegex().globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const int start = match.capturedStart(1);
        const int end = match.capturedEnd(1);
        const QString url = match.captured(1);
        html += text.mid(last, start - last).toHtmlEscaped();
        html += QStringLiteral("<a style=\"color:%1;text-decoration:underline;\" href=\"").arg(linkCss)
              + url.toHtmlEscaped()
              + QStringLiteral("\">")
              + url.toHtmlEscaped()
              + QStringLiteral("</a>");
        last = end;
    }
    html += text.mid(last).toHtmlEscaped();
    return html;
}

class PropertyRow : public QWidget
{
public:
    using QWidget::QWidget;

    bool hasHeightForWidth() const override
    {
        return layout() && layout()->hasHeightForWidth();
    }

    int heightForWidth(int width) const override
    {
        return layout() ? layout()->heightForWidth(width) : QWidget::heightForWidth(width);
    }

    QSize sizeHint() const override
    {
        QSize hint = QWidget::sizeHint();
        if (width() > 0 && hasHeightForWidth())
            hint.setHeight(heightForWidth(width()));
        return hint;
    }

    QSize minimumSizeHint() const override
    {
        QSize hint = QWidget::minimumSizeHint();
        if (width() > 0 && hasHeightForWidth())
            hint.setHeight(heightForWidth(width()));
        return hint;
    }
};

class PropertyTextEdit : public QTextBrowser
{
public:
    explicit PropertyTextEdit(const QString &text, QWidget *parent = nullptr)
        : QTextBrowser(parent)
    {
        setReadOnly(true);
        setOpenExternalLinks(true);
        setOpenLinks(true);
        contentIsHtml_ = webLinkRegex().match(text).hasMatch();
        content_ = contentIsHtml_
                       ? linkifiedPropertyText(text, theme::css(theme::chrome(palette()).inkLink))
                       : text;
        if (contentIsHtml_)
            setHtml(content_);
        else
            setPlainText(content_);
        setFrameShape(QFrame::NoFrame);
        setFrameStyle(QFrame::NoFrame);
        setLineWidth(0);
        setMidLineWidth(0);
        setViewportMargins(0, 0, 0, 0);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
        setCursor(Qt::IBeamCursor);
        setFocusPolicy(Qt::ClickFocus);
        document()->setDocumentMargin(0);
        QTextOption option = document()->defaultTextOption();
        option.setWrapMode(QTextOption::WrapAnywhere);
        document()->setDefaultTextOption(option);
        viewport()->setCursor(Qt::IBeamCursor);
        viewport()->setAutoFillBackground(false);
        setStyleSheet(QStringLiteral("background:transparent;color:%1;border:none;border-radius:0;padding:0;margin:0;")
                          .arg(theme::css(theme::chrome(palette()).ink)));
        viewport()->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
        QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
    }

    bool hasHeightForWidth() const override
    {
        return true;
    }

    int heightForWidth(int width) const override
    {
        return measureHeight(width);
    }

    QSize sizeHint() const override
    {
        return {textColumnWidthForPopup(kBaseWidth), measureHeight(textColumnWidthForPopup(kBaseWidth))};
    }

    QSize minimumSizeHint() const override
    {
        return {0, measureHeight(textColumnWidthForPopup(kBaseWidth))};
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->matches(QKeySequence::Copy) && textCursor().hasSelection()) {
            copy();
            event->accept();
            return;
        }
        QTextBrowser::keyPressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        viewport()->setCursor(anchorAt(event->pos()).isEmpty() ? Qt::IBeamCursor : Qt::PointingHandCursor);
        QTextBrowser::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        viewport()->setCursor(Qt::IBeamCursor);
        QTextBrowser::leaveEvent(event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QTextBrowser::resizeEvent(event);
        const int wantedHeight = measureHeight(viewport()->width());
        if (height() != wantedHeight)
            setFixedHeight(wantedHeight);
    }

private:
    // Measure the wrapped text height at a given width WITHOUT touching the live
    // displayed document. The earlier implementation called
    // document()->setTextWidth() from these layout queries; because QTextEdit also
    // syncs the live document's text width to its viewport, the two fought and the
    // relayout recursed until the stack overflowed (a crash on any property long
    // enough to widen the popup past kBaseWidth). A throwaway document mirrors the
    // content/font/wrap so measurement has no side effects on the widget.
    int measureHeight(int width) const
    {
        if (width <= 0)
            return fontMetrics().height();
        QTextDocument doc;
        doc.setDefaultFont(font());
        doc.setDocumentMargin(0);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAnywhere);
        doc.setDefaultTextOption(option);
        if (contentIsHtml_)
            doc.setHtml(content_);
        else
            doc.setPlainText(content_);
        doc.setTextWidth(width);
        return static_cast<int>(std::ceil(doc.size().height()));
    }

    QString content_;             // the row's text (linkified HTML if it has a URL)
    bool contentIsHtml_ = false;
};

} // namespace

PdfPropertiesPopup::PdfPropertiesPopup(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("pdfPropertiesPopup"));
    setAttribute(Qt::WA_StyledBackground, true);
    appliedDark_ = theme::isDark(palette());
    setStyleSheet(popupStyleSheet(palette()));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 8, 10, 8);
    outer->setSpacing(6);

    auto *headerRow = new QHBoxLayout;
    headerRow->setSpacing(6);
    headerLabel_ = new QLabel(tr("Properties"), this);
    headerLabel_->setStyleSheet(QStringLiteral("font-weight:600;"));
    headerRow->addWidget(headerLabel_, 1);

    auto *closeBtn = new QToolButton(this);
    closeBtn->setObjectName(QStringLiteral("pdfPropsClose")); // styled by popupStyleSheet
    closeBtn->setText(QStringLiteral("x"));
    closeBtn->setAutoRaise(true);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setFixedSize(24, 24);
    closeBtn->setToolTip(tr("Close"));
    connect(closeBtn, &QToolButton::clicked, this, [this] { hide(); });
    headerRow->addWidget(closeBtn);
    outer->addLayout(headerRow);

    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->viewport()->setObjectName(QStringLiteral("pdfPropertiesViewport"));
    scroll_->viewport()->setAttribute(Qt::WA_StyledBackground, true);
    rowsWidget_ = new QWidget(scroll_);
    rowsWidget_->setObjectName(QStringLiteral("pdfPropertiesRows"));
    rowsWidget_->setAttribute(Qt::WA_StyledBackground, true);
    rowsLayout_ = new QVBoxLayout(rowsWidget_);
    rowsLayout_->setContentsMargins(0, 0, 0, 0);
    rowsLayout_->setSpacing(2);
    rowsLayout_->addStretch(1);
    scroll_->setWidget(rowsWidget_);
    outer->addWidget(scroll_);

    auto *copyShortcut = new QShortcut(QKeySequence::Copy, this);
    copyShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(copyShortcut, &QShortcut::activated, this, &PdfPropertiesPopup::copySelectedText);

    hide();
}

void PdfPropertiesPopup::showFor(const PdfItemProperties &properties)
{
    lastProps_ = properties;
    clearRows();
    const int popupWidth = preferredPopupWidth(properties.values, fontMetrics(),
                                               headerLabel_->fontMetrics().horizontalAdvance(headerLabel_->text()));
    setFixedWidth(popupWidth);
    const int textColumnWidth = textColumnWidthForPopup(popupWidth);
    for (const QString &value : properties.values)
        addPropertyRow(value, textColumnWidth);

    rowsWidget_->updateGeometry();
    const int rowsHeight = std::min(rowsWidget_->sizeHint().height(), kMaxRowsHeight);
    scroll_->setFixedHeight(std::max(36, rowsHeight));
    scroll_->verticalScrollBar()->setValue(0);
    QTimer::singleShot(0, this, [this] {
        if (scroll_)
            scroll_->verticalScrollBar()->setValue(0);
    });
    show();
    raise();
}

void PdfPropertiesPopup::positionNear(const QRect &itemWidgetRect)
{
    QWidget *p = parentWidget();
    if (!p)
        return;

    adjustSize();
    int x = itemWidgetRect.left();
    int y = itemWidgetRect.bottom() + kGap;
    if (y + height() > p->height() && itemWidgetRect.top() - kGap - height() >= 0)
        y = itemWidgetRect.top() - kGap - height();

    x = std::clamp(x, kGap, std::max(kGap, p->width() - width() - kGap));
    y = std::clamp(y, kGap, std::max(kGap, p->height() - height() - kGap));
    move(x, y);
}

void PdfPropertiesPopup::hideEvent(QHideEvent *event)
{
    emit dismissed();
    QWidget::hideEvent(event);
}

void PdfPropertiesPopup::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange) {
        // Only re-skin when the light/dark state actually flips. Calling
        // setStyleSheet() re-resolves the widget palette and posts ANOTHER
        // PaletteChange, so re-applying it unconditionally here recurses
        // setStyleSheet -> PaletteChange -> changeEvent -> setStyleSheet until the
        // stack overflows (it crashed the app the moment any properties popup was
        // built). Gating on a real theme change makes the re-entrant PaletteChange
        // a no-op, which stops the loop after one pass.
        const bool dark = theme::isDark(palette());
        if (dark != appliedDark_) {
            appliedDark_ = dark;
            setStyleSheet(popupStyleSheet(palette()));
            // The rows bake their ink / link colour / copy-icon tint at build
            // time, so a theme switch while the popup is on screen would leave
            // dark-on-dark text; rebuild from the shown properties. Hidden popups
            // rebuild on the next showFor() anyway.
            if (isVisible())
                showFor(lastProps_);
        }
    }
    QWidget::changeEvent(event);
}

void PdfPropertiesPopup::clearRows()
{
    while (rowsLayout_->count() > 1) {
        QLayoutItem *item = rowsLayout_->takeAt(0);
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
}

void PdfPropertiesPopup::copySelectedText() const
{
    if (!rowsWidget_)
        return;

    const auto textEdits = rowsWidget_->findChildren<QTextEdit *>();
    for (QTextEdit *textEdit : textEdits) {
        const QString selected = textEdit->textCursor().selectedText();
        if (!selected.isEmpty()) {
            QGuiApplication::clipboard()->setText(selected);
            return;
        }
    }
}

void PdfPropertiesPopup::addPropertyRow(const QString &text, int textColumnWidth)
{
    const theme::Chrome t = theme::chrome(palette());
    auto *row = new PropertyRow(rowsWidget_);
    row->setObjectName(QStringLiteral("pdfPropertyRow"));
    row->setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(4, 3, 8, 3);
    layout->setSpacing(6);

    auto *copyBtn = new QToolButton(row);
    copyBtn->setIcon(icons::glyph(icons::Glyph::Copy, t.inkSoft));
    copyBtn->setIconSize(QSize(16, 16));
    copyBtn->setAutoRaise(true);
    copyBtn->setCursor(Qt::PointingHandCursor);
    copyBtn->setFixedSize(24, 24);
    copyBtn->setToolTip(tr("Copy property"));
    connect(copyBtn, &QToolButton::clicked, this, [text] {
        QGuiApplication::clipboard()->setText(text);
    });
    layout->addWidget(copyBtn, 0, Qt::AlignTop);

    auto *textEdit = new PropertyTextEdit(text, row);
    textEdit->setMinimumWidth(0);
    textEdit->setFixedHeight(textEdit->heightForWidth(textColumnWidth));
    layout->addWidget(textEdit, 1);

    rowsLayout_->insertWidget(rowsLayout_->count() - 1, row);
}

} // namespace mervin
