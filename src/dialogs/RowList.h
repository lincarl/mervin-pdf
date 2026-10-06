#pragma once

#include <QCoreApplication>
#include <QListWidget>
#include <QPoint>
#include <QStringList>

#include <functional>

class QHBoxLayout;

namespace mervin {

// Shared Merge/Extract row UI: grip, index, dialog columns, count, output and remove button.
// Dialogs own plans and rebuild rows after edits. This class handles focus, drag markers,
// Ctrl+Shift+Up/Down and Ctrl+Delete. Route drops to onRowDropped/moveToGap:
// QListWidget::InternalMove moves items separately from their row widgets.
class RowList : public QListWidget
{
    // Its own context: without Q_OBJECT, tr() would be QListWidget's.
    Q_DECLARE_TR_FUNCTIONS(mervin::RowList)

public:
    // Adds the dialog's own columns (or their captions) to a row (or the header):
    // widgets created with `parent`, added to `columns` in order.
    using Fill = std::function<void(QWidget *parent, QHBoxLayout *columns)>;

    // The Count and Output columns get these widths, or more when their
    // translated captions need it.
    RowList(int countWidth, int outputWidth, QWidget *parent);

    // `minimum`, or wider when one of `texts` needs more room in `font`, plus
    // `padding`. Column widths fit English with room to spare; this lets a longer
    // translation widen a column instead of being clipped, since a QLabel clips
    // rather than elides.
    static int widthFor(int minimum, const QFont &font, const QStringList &texts, int padding);
    // The caption strip's font: `base` at the weight Theme gives the captions.
    static QFont captionFont(const QFont &base);

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
