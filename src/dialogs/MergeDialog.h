#pragma once

#include "merge/MergePlan.h"

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>

class QLabel;
class QLineEdit;
class QPushButton;

namespace mervin {

class RowList;

// MergePlan-backed editor: each row contributes a file/page range in output order. Rebuild rows
// after every mutation so captured row indices remain valid.
class MergeDialog : public QDialog
{
    Q_OBJECT

public:
    // Add nonempty initialPath as a normal editable row, probed with qpdf and initialPassword.
    // initialPageCount is only a fallback when qpdf opens but reports no pages.
    explicit MergeDialog(const QString &initialPath, int initialPageCount,
                         const QString &initialPassword = QString(),
                         QWidget *parent = nullptr);

    // Files open in the app's tabs. The viewer holds each one open, so the merged
    // file may not replace one; the error line says so before Merge is pressed.
    void setOpenFiles(const QStringList &paths);

    // Valid after exec() == Accepted, by which time the merged file is written:
    // accept() writes it and stays open on a failure, so the plan survives it.
    QList<PageOps::MergeInput> inputs() const { return plan_.inputs(); }
    QString outputPath() const { return outputPath_; }

    // Probe each path and append it as a row, in the order given. Rows that
    // cannot be merged are still added, marked and blocking, so the user can see
    // and remove them. This is what "Add Files…" calls once the picker returns,
    // and the seam a file drop would use.
    void addPaths(const QStringList &paths);

private:
    // Validate, confirm an overwrite, then write the merged file. Closes only
    // once the write succeeded; a failure goes on the error line instead.
    void accept() override;
    void resizeEvent(QResizeEvent *event) override;

    void addFiles();
    void removeCurrent();
    void duplicateCurrent();
    void moveCurrent(int delta);
    void browseForOutput();

    // Probe `path` with qpdf and build the row it deserves (page count, or a
    // Locked / Unreadable state with the backend's message as its tooltip). An
    // encrypted file is tried with `password` when one is given; if it opens, the
    // row is Ok and keeps the password. Added files have none, so they stay Locked.
    static MergePlan::Entry probeEntry(const QString &path, const QString &password = QString());

    // Tear down and rebuild every row from plan_, then refresh the summary, the
    // error line and the enabled states. `selectRow` is reselected afterwards.
    void rebuild(int selectRow = -1);
    void refreshFooter();
    void reelideNames();     // fit each row's file name to its actual width
    int currentRow() const;
    QString startDirectory() const;

    MergePlan plan_;
    QString outputPath_;
    bool outputEdited_ = false; // stop re-deriving the name once the user typed one
    QStringList openKeys_;      // normalizePathKey() of every file open in a tab
    QString writeError_;        // the last failed write, until the plan or Save as changes
    QString writeErrorDetail_;  // qpdf's message for it, the error line's tooltip

    RowList *list_ = nullptr;
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
