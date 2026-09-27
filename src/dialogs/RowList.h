#pragma once

#include <QListWidget>
#include <QPoint>

#include <functional>

class QHBoxLayout;

namespace mervin {

// The reorderable row list shared by Merge PDFs and Extract Pages: one row widget
// per plan row, laid out as
//
//   grip | # | <the dialog's own columns> | Count | Output | ✕
//
// with a caption strip over the same columns. The dialog owns the plan and
// rebuilds every row from it after each change (so a row index captured at build
// time never goes stale); this class owns the idiom around the rows: dragging a
// row by its grip with a drop marker, the row shortcuts (Ctrl+Shift+Up / Down to
// move, Ctrl+Delete to remove), the ✕, and "focus in a row makes it current".
//
// QListWidget's own InternalMove cannot be used: it moves the QListWidgetItem
// while the visible row is a separate item widget, so the two come apart. A drop
// is handed to onRowDropped instead, for the plan's moveToGap().
class RowList : public QListWidget
{
public:
    // Adds the dialog's own columns (or their captions) to a row (or the header):
    // widgets created with `parent`, added to `columns` in order.
    using Fill = std::function<void(QWidget *parent, QHBoxLayout *columns)>;

    RowList(int countWidth, int outputWidth, QWidget *parent);

    // The caption strip, for the dialog to place directly above the list: "#",
    // the captions `fill` adds, "Count" and "Output". Its insets follow the list's
    // frame, padding and vertical scrollbar, so the captions stay over their
    // columns. Call once.
    QWidget *makeHeader(const Fill &fill);
    // Append a row. Every focusable widget `fill` adds makes the row current when
    // it takes focus, so the side buttons act on the row being typed in.
    void addRow(const Fill &fill);
    void setRowTexts(int row, const QString &count, const QString &output);

    // Called with (row, insertion gap) when a row is dropped: gap 0 is above the
    // first row, count() below the last. It arrives inside QDrag::exec(), whose
    // grip is still on the stack, so rebuild on a later event-loop turn.
    std::function<void(int from, int gap)> onRowDropped;
    std::function<void(int delta)> onMove; // Ctrl+Shift+Up / Down, on the current row
    // Ctrl+Delete, and a ✕ (which first makes its row current, then calls this on
    // the next event-loop turn so the button is not deleted inside its own click).
    std::function<void()> onRemove;

protected:
    bool eventFilter(QObject *o, QEvent *e) override;
    void paintEvent(QPaintEvent *e) override;

private:
    void syncHeaderInsets();
    void startRowDrag(int row);
    void takeIfOurs(QDragMoveEvent *e);
    void handleDrop(QDropEvent *e);
    int gapAt(const QPoint &pos) const;
    int gapY(int gap) const;

    int countWidth_;
    int outputWidth_;
    QHBoxLayout *headerRow_ = nullptr;
    int dropAt_ = -1;   // insertion point under the cursor, -1 when not dragging
    int dragRow_ = -1;  // row whose grip is held, -1 when none
    QPoint dragOrigin_; // where that press landed, for the drag threshold
};

} // namespace mervin
