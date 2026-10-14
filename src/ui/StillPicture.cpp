#include "ui/StillPicture.h"

#include "ui/ThemeTokens.h"

#include <QPainter>

namespace mervin {

StillPicture::StillPicture(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("operationStill"));
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setFocusPolicy(Qt::NoFocus);
}

void StillPicture::setPicture(const QPixmap &picture)
{
    picture_ = picture;
    update();
}

void StillPicture::setVeil(const QRect &bar)
{
    if (bar == bar_)
        return;
    bar_ = bar;
    update();
}

void StillPicture::paintEvent(QPaintEvent *)
{
    const theme::Chrome t = theme::chrome(palette());
    QPainter painter(this);
    painter.fillRect(rect(), t.canvas);
    // At its natural size, as the pixmap carries the device pixel ratio it was taken at.
    painter.drawPixmap(QPoint(0, 0), picture_);
    if (!bar_.isEmpty())
        painter.fillRect(QRect(bar_.left(), bar_.bottom() + 1, bar_.width(),
                               height() - bar_.bottom() - 1),
                         t.veil);
}

} // namespace mervin
