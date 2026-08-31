#pragma once

#include "merge/MergePlan.h"

#include <QDialog>
#include <QList>
#include <QPoint>
#include <QString>
#include <QStringList>

class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace mervin {

// Document -> Merge PDFs.
//
// The old flow was two native file pickers: getOpenFileNames() returned the
// files in *its* sort order (not click order) while the title claimed "in
// order", the open document was silently prepended, and the whole of every file
// was always taken. Nothing about the result was visible before it was written.
//
// This dialog shows the merge plan instead. Each row is one (file, page range)
// segment; the row's position in the list is its position in the output, and
// every row states how many pages it contributes and which output pages those
// become. The footer states the total. Order and selection are read, not
// imagined.
//
// All of the arithmetic lives in MergePlan (mervin_core, unit-tested); this
// class is the widgets around it, and rebuilds the rows wholesale after every
// mutation so build-time row indices can never go stale.
class MergeDialog : public QDialog
{
    Q_OBJECT

public:
    // `initialPath` is the document the user had open (may be empty), added as an
    // ordinary first row - removable and reorderable like any other, which is the
    // point. It is probed with qpdf like every other row; `initialPageCount` (the
    // viewer's count) is only a fallback for the case where qpdf opens the file
    // but reports no pages.
    explicit MergeDialog(const QString &initialPath, int initialPageCount,
                         QWidget *parent = nullptr);

    // Valid after exec() == Accepted.
    QList<PageOps::MergeInput> inputs() const { return plan_.inputs(); }
    QString outputPath() const { return outputPath_; }

    // Probe each path and append it as a row, in the order given. Rows that
    // cannot be merged are still added, marked and blocking, so the user can see
    // and remove them. This is what "Add Files…" calls once the picker returns,
    // and the seam a file drop would use.
    void addPaths(const QStringList &paths);

private:
    void accept() override;
    // Watches each row's page-range editor for focus (so the row the user is
    // typing in becomes the current row, and Remove / Duplicate / Move act on it)
    // and the list's viewport for resize (so the column captions stay over their
    // columns when the scrollbar appears).
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

    void addFiles();
    void removeCurrent();
    void duplicateCurrent();
    void moveCurrent(int delta);
    void browseForOutput();

    // Probe `path` with qpdf and build the row it deserves (page count, or a
    // Locked / Unreadable state with the backend's message as its tooltip).
    static MergePlan::Entry probeEntry(const QString &path);

    // Tear down and rebuild every row from plan_, then refresh the summary, the
    // error line and the enabled states. `selectRow` is reselected afterwards.
    void startRowDrag(int row);
    void rebuild(int selectRow = -1);
    void refreshFooter();
    void syncHeaderInsets(); // keep the captions over the columns
    void reelideNames();     // fit each row's file name to its actual width
    int currentRow() const;
    QString startDirectory() const;

    MergePlan plan_;
    QString outputPath_;
    bool outputEdited_ = false; // stop re-deriving the name once the user typed one
    int dragRow_ = -1;          // row whose grip is held, -1 when none
    QPoint dragOrigin_;         // where that press landed, for the drag threshold

    QListWidget *list_ = nullptr;
    QHBoxLayout *headerRow_ = nullptr;
    QList<QLabel *> nameLabels_; // one per row, rebuilt with the list
    QStringList nameTexts_;      // their untruncated text, for re-eliding
    QLabel *summary_ = nullptr;
    QLabel *error_ = nullptr;
    QLineEdit *outputEdit_ = nullptr;
    QPushButton *removeBtn_ = nullptr;
    QPushButton *upBtn_ = nullptr;
    QPushButton *downBtn_ = nullptr;
    QPushButton *duplicateBtn_ = nullptr;
    QPushButton *mergeBtn_ = nullptr;
};

} // namespace mervin
