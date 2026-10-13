#pragma once

#include "merge/MergePlan.h"

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

class QLabel;
class QLineEdit;
class QPushButton;

namespace mervin {

class RowList;
class SaveCheck;

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
    // write() writes it and keeps the dialog open on a failure, so the plan survives it.
    QList<PageOps::MergeInput> inputs() const { return plan_.inputs(); }
    QString outputPath() const { return outputPath_; }

    // Probe each path and append it as a row, in the order given. Rows that
    // cannot be merged are still added, marked and blocking, so the user can see
    // and remove them. This is what "Add Files…" calls once the picker returns,
    // and the seam a file drop would use.
    void addPaths(const QStringList &paths);

private:
    // Validate, then check Save as again, as the disk can have changed since the
    // last answer, and write once that answers (write()) if the rows and Save as
    // are still what Merge was pressed for.
    void accept() override;
    // Confirm an overwrite, then write the merged file. Closes only once the write
    // succeeded; a failure goes on the error line instead.
    void write();
    // Checks the Save as text off the UI thread (see SaveCheck). While Save as is
    // not edited, the check also steps the default past names in use.
    // refreshFooter() asks for one when the text or the files change.
    void checkOutput();
    // The error line's text: the first problem only. Empty when there is none, and
    // nothing while the check of the Save as text has not answered.
    std::optional<QString> problem() const;
    std::optional<QString> saveAsProblem() const; // Save as, once its check answered
    // The error line and Merge. Until the Save as check answers, a reason about
    // Save as stays, so a quick check does not make it flicker; SaveCheck::slow()
    // says when it takes long.
    void showProblem();
    // The rows' files and ranges, which a write depends on.
    QStringList files() const;
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
    // The last Save as check's answer.
    struct Checked
    {
        QString text;       // the Save as text it is for
        QString canonical;  // the merged file's canonical path, empty when it does not exist
        QString key;        // normalizePathKey() of the merged file, symlinks resolved
        QStringList inputs; // each row's file's canonical path, in row order; for a write
    };
    std::optional<Checked> checked_;
    QString checking_;        // the text the last check asked for was for...
    QStringList checkedFiles_; // ...and files() then
    QString defaultPath_;     // plan_.plainOutputPath() when the default text was set
    // What Merge was pressed for, until written: the Save as text and files().
    std::optional<std::pair<QString, QStringList>> pressed_;
    SaveCheck *saveCheck_ = nullptr;
    bool saveAsReasonShown_ = false; // the error line's reason is about Save as

    int specWidth_ = 0;          // the Pages column: its caption and fields
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
