#include "render/ZoomAnimation.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace mervin {

bool ZoomAnimation::start(Snapshot snapshot, const QRectF &representativeFinal, int durationMs)
{
    reset();
    const auto rep = snapshot.from.constFind(snapshot.repPage);
    if (durationMs <= 0 || !representativeFinal.isValid()
        || rep == snapshot.from.constEnd() || rep->width() <= 0.0)
        return false;
    const double ratio = representativeFinal.width() / rep->width();
    if (!(ratio > 0.0) || std::abs(std::log(ratio)) < std::log(1.05))
        return false;
    ratio_ = ratio;
    durationMs_ = std::clamp(static_cast<int>(durationMs * std::abs(std::log(ratio))
                                             / std::log(1.25)), 90, 260);
    snapshot_ = std::move(snapshot);
    active_ = true;
    clock_.start();
    return true;
}

bool ZoomAnimation::finished() const
{
    return active_ && forcedT_ < 0.0 && clock_.elapsed() >= durationMs_;
}

QRectF ZoomAnimation::rect(int page, const QRectF &finalRect,
                           const QRectF &representativeFinal) const
{
    if (!active_ || finalRect.width() <= 0.0)
        return finalRect;
    QRectF from = finalRect;
    const auto captured = snapshot_.from.constFind(page);
    if (captured != snapshot_.from.constEnd()) {
        from = *captured;
    } else {
        // Extrapolate newly visible pages from the nearest captured page.
        const auto rep = snapshot_.from.constFind(snapshot_.repPage);
        if (rep != snapshot_.from.constEnd() && representativeFinal.isValid()) {
            const double a = rep->width() / representativeFinal.width();
            from = QRectF(a * finalRect.x() + (rep->x() - a * representativeFinal.x()),
                          a * finalRect.y() + (rep->y() - a * representativeFinal.y()),
                          a * finalRect.width(), a * finalRect.height());
        }
    }
    if (from.width() <= 0.0)
        return finalRect;
    const double t = forcedT_ >= 0.0 ? forcedT_ : double(clock_.elapsed()) / durationMs_;
    const double e = 1.0 - std::pow(1.0 - std::clamp(t, 0.0, 1.0), 3.0);
    // Geometric size progression avoids a lunge at the start of large zooms.
    const double u = (std::pow(ratio_, e) - 1.0) / (ratio_ - 1.0);
    return QRectF(from.x() + (finalRect.x() - from.x()) * u,
                  from.y() + (finalRect.y() - from.y()) * u,
                  from.width() + (finalRect.width() - from.width()) * u,
                  from.height() + (finalRect.height() - from.height()) * u);
}

QTransform ZoomAnimation::transform(const QRectF &finalRect, const QRectF &drawnRect)
{
    if (!finalRect.isValid() || !drawnRect.isValid())
        return {};
    // Layout rounds each axis separately, so overlays must use both drawn scales.
    const double ax = drawnRect.width() / finalRect.width();
    const double ay = drawnRect.height() / finalRect.height();
    QTransform xf;
    xf.translate(drawnRect.x() - ax * finalRect.x(), drawnRect.y() - ay * finalRect.y());
    xf.scale(ax, ay);
    return xf;
}

} // namespace mervin
