#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QRectF>
#include <QTransform>

namespace mervin {

// Visual interpolation only; the viewer installs the final layout before starting.
class ZoomAnimation
{
public:
    struct Snapshot {
        QHash<int, QRectF> from;
        int repPage = -1;
    };

    bool start(Snapshot snapshot, const QRectF &representativeFinal, int durationMs);
    void reset() { *this = ZoomAnimation{}; }
    bool active() const { return active_; }
    bool finished() const;
    int representativePage() const { return snapshot_.repPage; }
    const QHash<int, QRectF> &capturedRects() const { return snapshot_.from; }
    void setProgressForTest(double t) { forcedT_ = t; } // negative restores clock-driven progress
    QRectF rect(int page, const QRectF &finalRect, const QRectF &representativeFinal) const;
    static QTransform transform(const QRectF &finalRect, const QRectF &drawnRect);

private:
    Snapshot snapshot_;
    bool active_ = false;
    double ratio_ = 1.0;
    int durationMs_ = 0;
    double forcedT_ = -1.0;
    QElapsedTimer clock_;
};

} // namespace mervin
