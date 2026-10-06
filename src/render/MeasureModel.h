#pragma once

// Per-page manual/calibrated scales override embedded /VP metadata.
// MeasureContent serializes them with the document's editable measurements.

#include "render/MeasureMath.h"
#include "render/MeasureTypes.h"

#include <QString>

#include <unordered_map>

namespace mervin {

class MeasureModel
{
public:
    // The user drew a line of length `lengthPoints` (in PDF points) over a known
    // dimension of `trueLength` in `unit`.  units-per-point = trueLength / length.
    static MeasureScale fromCalibration(double lengthPoints, double trueLength, MeasureUnit unit)
    {
        MeasureScale s;
        if (lengthPoints <= 0.0 || trueLength <= 0.0)
            return s;
        const double mm = trueLength * measure::mmPerUnit(unit);
        s.mmPerPointX = s.mmPerPointY = mm / lengthPoints;
        s.source = MeasureSource::Calibrated;
        s.label = measure::deriveRatioLabel(s.mmPerPointX);
        return s;
    }

    // Manual ratio "1:denom" (paper:real). At 1:1 a PDF point is mm-per-point.
    static MeasureScale fromRatio(int denom)
    {
        MeasureScale s;
        if (denom <= 0)
            return s;
        s.mmPerPointX = s.mmPerPointY = measure::kMmPerPoint * denom;
        s.source = MeasureSource::Manual;
        s.label = QStringLiteral("1:%1").arg(denom);
        return s;
    }

    void setOverride(int page, const MeasureScale &s) { overrides_[page] = s; }
    void clearOverride(int page) { overrides_.erase(page); }
    void clearAll() { overrides_.clear(); }

    bool hasOverride(int page) const { return overrides_.find(page) != overrides_.end(); }
    // True when any page carries a manual/calibrated override (something worth
    // persisting), even if no measurement has been committed.
    bool hasAnyOverride() const { return !overrides_.empty(); }
    MeasureScale override(int page) const
    {
        auto it = overrides_.find(page);
        return it == overrides_.end() ? MeasureScale{} : it->second;
    }

private:
    std::unordered_map<int, MeasureScale> overrides_;
};

} // namespace mervin
