#include "ui/ThemeCard.h"

#include "ui/Theme.h"
#include "ui/ThemeTokens.h"

#include <QApplication>
#include <QEnterEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <utility>

namespace mervin {

ThemeCard::ThemeCard(const QString &label, PaintPreview paintPreview, QWidget *parent)
    : QRadioButton(label, parent), paintPreview_(std::move(paintPreview))
{
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor);
}

QSize ThemeCard::sizeHint() const
{
    return QSize(std::max(150, minimumSizeHint().width()), 168);
}

QSize ThemeCard::minimumSizeHint() const
{
    return QSize(fontMetrics().horizontalAdvance(text()) + 48, 168);
}

bool ThemeCard::hitButton(const QPoint &point) const
{
    return rect().contains(point);
}

void ThemeCard::enterEvent(QEnterEvent *event)
{
    QRadioButton::enterEvent(event);
    update();
}

void ThemeCard::leaveEvent(QEvent *event)
{
    QRadioButton::leaveEvent(event);
    update();
}

void ThemeCard::paintEvent(QPaintEvent *)
{
    // The application palette, not this button's: platform themes such as GTK
    // give radio buttons a palette of their own, with a black accent.
    const QPalette pal = QApplication::palette();
    const QColor accent = Theme::appliedAccent();
    const auto colors = accent.isValid() ? theme::chrome(pal, accent) : theme::chrome(pal);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled()) painter.setOpacity(0.5);

    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colors.bar);
    painter.drawRoundedRect(card, 6, 6);
    if (isChecked() || underMouse() || isDown()) {
        painter.setBrush(isDown() ? colors.pressed
                                 : isChecked() ? colors.accentWash : colors.hover);
        painter.drawRoundedRect(card, 6, 6);
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(isChecked() ? colors.accent : colors.borderPush, 1));
    painter.drawRoundedRect(card, 6, 6);

    const QRectF well(6.5, 6.5, width() - 13.0, height() - 44.0);
    painter.setPen(QPen(colors.border, 1));
    painter.setBrush(colors.well);
    painter.drawRoundedRect(well, 3, 3);
    if (paintPreview_) {
        painter.save();
        paintPreview_(painter, well);
        painter.restore();
    }

    painter.setPen(colors.inkPrimary);
    painter.drawText(QRectF(11, height() - 34, width() - 43, 25),
                     Qt::AlignVCenter | Qt::AlignLeft, text());

    const QPointF radioCenter(width() - 18, height() - 21);
    painter.setPen(QPen(isChecked() ? colors.accent : colors.inkSoft, 1));
    painter.setBrush(isChecked() ? colors.accent : Qt::transparent);
    painter.drawEllipse(radioCenter, 6.5, 6.5);
    if (isChecked()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(colors.onAccent, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath tick;
        tick.moveTo(radioCenter + QPointF(-3.2, 0));
        tick.lineTo(radioCenter + QPointF(-0.8, 2.4));
        tick.lineTo(radioCenter + QPointF(3.5, -2.5));
        painter.drawPath(tick);
    }

    if (hasFocus()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(colors.accent, 1, Qt::DashLine));
        painter.drawRoundedRect(card.adjusted(2.5, 2.5, -2.5, -2.5), 4, 4);
    }
}

} // namespace mervin
