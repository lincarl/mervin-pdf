#pragma once

#include "merge/MergePlan.h"

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>
#include <memory>
#include <map>

class QLabel;
class QLineEdit;
class QPushButton;

namespace mervin {

class RowList;
class SaveCheck;
namespace background { class SerialQueue; }
namespace job { class State; }

// MergePlan-backed editor. Each row contributes a file/page range in output order.
// Structural changes rebuild rows so captured indices remain valid. Probe answers
// refresh existing widgets without interrupting a drag or an active range editor.
class MergeDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Conflict { None, Input, OpenTab };
    // Add nonempty initialPath as a normal editable row, probed with qpdf and initialPassword.
    // initialPageCount is retained for callers; the background probe determines
    // the authoritative count.
    explicit MergeDialog(const QString &initialPath, int initialPageCount,
                         const QString &initialPassword = QString(),
                         QWidget *parent = nullptr);
    // Reopens a failed merge from its previously probed plan without reading the
    // inputs again. The output and row ranges remain editable.
    MergeDialog(const MergePlan &plan, const QString &output, const QString &error,
                int failedIndex, Conflict conflict = Conflict::None, QWidget *parent = nullptr);
    /// Stops outstanding probe requests without waiting for their workers.
    ~MergeDialog() override;

    // Shared failure summary for a reopened plan and its tray notification.
    static QString failureSummary(const MergePlan &plan, const QString &output,
                                  int failedIndex, Conflict conflict);

    // Files open in the app's tabs. Merge cannot replace these destinations;
    // the error line explains the conflict before Merge is pressed.
    void setOpenFiles(const QStringList &paths);

    // Valid after Accepted. The caller writes this plan on its worker.
    QList<PageOps::MergeInput> inputs() const { return plan_.inputs(); }
    /// Returns the probed rows and editable ranges for the accepted worker request.
    MergePlan plan() const { return plan_; }
    /// Reports whether any input row still awaits its background probe.
    bool reading() const { return plan_.hasReading(); }
    QString outputPath() const { return outputPath_; }

    // Probe each path and append it as a row, in the order given. Rows that
    // cannot be merged are still added, marked and blocking, so the user can see
    // and remove them. This is what "Add Files…" calls once the picker returns,
    // and the seam a file drop would use.
    void addPaths(const QStringList &paths);

private:
    // Validates the plan, then checks the current Save as choice before accepting
    // the unchanged request for a worker write.
    void accept() override;
    /// Stops probe states and clears queued probes before closing the dialog.
    void done(int result) override;
    // Confirm an overwrite, then close with the validated plan.
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
    struct ProbeBatch;
    /// Appends a Reading row and schedules its probe on the volume queue.
    void queueProbe(const QString &path, const QString &password,
                    const std::shared_ptr<ProbeBatch> &batch = {}, int index = 0);
    /// Resolves the matching rows and reports surviving failures once per batch.
    void finishProbe(quint64 ticket, const MergePlan::Entry &entry,
                     const std::shared_ptr<ProbeBatch> &batch, int index);
    // Refreshes existing widgets so a late probe cannot delete an active drag grip.
    void refreshRows();

    // Tear down and rebuild every row from plan_, then refresh the summary, the
    // error line and the enabled states. `selectRow` is reselected afterwards.
    void rebuild(int selectRow = -1);
    void refreshFooter();
    void reelideNames();     // fit each row's file name to its actual width
    int currentRow() const;
    QString startDirectory() const;

    MergePlan plan_;
    std::map<QString, std::unique_ptr<background::SerialQueue>> probes_;
    std::map<quint64, std::shared_ptr<job::State>> probeStates_;
    quint64 nextProbe_ = 0;
    bool closed_ = false;
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
    QList<QLabel *> badges_;
    QList<QLineEdit *> specEditors_;
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
