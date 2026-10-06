#pragma once

#include <QList>

namespace mervin {

// Row reordering shared by MergePlan and ExtractPlan, whose dialogs move rows the
// same way (RowList: Move Up / Move Down, and a drag between rows). Both return
// the row's new index, or -1 when nothing moved, so a caller can treat a boundary
// or a no-op drop as "leave everything as it is".

// Move row `i` by `delta` slots; -1 when that would leave the list.
template<typename T>
int moveRow(QList<T> &rows, int i, int delta)
{
    const int to = i + delta;
    if (i < 0 || i >= rows.size() || to < 0 || to >= rows.size() || delta == 0)
        return -1;
    rows.move(i, to);
    return to;
}

// Move row `from` to the insertion point `gap`, where gap 0 is above the first
// row and gap size() is below the last: the positions a drag hovers between.
// Counted before the row is lifted out, so every gap below `from` shifts up by one
// on the way; dropping a row immediately above or below itself is therefore a
// no-op.
template<typename T>
int moveRowToGap(QList<T> &rows, int from, int gap)
{
    if (from < 0 || from >= rows.size() || gap < 0 || gap > rows.size())
        return -1;
    // Lifting the row out closes the gap it occupied, so everything below it
    // shifts up one.
    const int to = gap > from ? gap - 1 : gap;
    if (to == from)
        return -1;
    rows.move(from, to);
    return to;
}

} // namespace mervin
