#pragma once

#include "extract/ExtractPlan.h"
#include "security/PageOps.h"

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

class QAction;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace mervin {

class Document;
class ExtractStrip;
class RenderEngine;
class RowList;
class SaveCheck;

// ExtractPlan-backed editor with reorderable page/range rows and an output thumbnail strip.
// Rows and strip write into the plan; all displayed state is rebuilt from it. Shares RowList
// with MergeDialog. In Delete mode (Document > Delete Pages) the same window lists the pages
// to leave out of a copy. The rows have no order, the strip shows every page and a click
// deletes or keeps one, and the copy is written with PageOps::deletePages, which keeps the
// source's structure and encryption but not the deleted pages' content or form fields.
class ExtractDialog : public QDialog
{
    Q_OBJECT

public:
    /// Carries a worker probe result and its verified open password.
    struct Probe
    {
        PageOps::Status status = PageOps::Status::Failed;
        int pageCount = 0;
        QString password;
        QString error;
    };

    // Runs on a worker before the dialog opens. A known password is tried only
    // when an unauthenticated probe says the source needs one.
    static Probe probeSource(const QString &path, const QString &knownPassword);

    /// Retains editable rows, destination and failure details across worker attempts.
    struct Choices
    {
        enum class Failure { None, Password, Read, Write, ChangedCount, Source, Destination };
        QStringList specs;
        QString output;
        QString password;
        bool outputEdited = false;
        bool openWhenDone = true;
        Failure failure = Failure::None;
        QString error;
    };

    // The existing inline reasons, also used when a hidden owner needs a tray notice.
    static QString failureText(const Choices &choices, ExtractPlan::Mode mode);

    /// Combines captured source facts with borrowed thumbnail context for the dialog.
    struct Source
    {
        ExtractPlan::Mode mode = ExtractPlan::Mode::Extract;
        QString path;            // the file on disk: what qpdf reads
        QString canonicalPath;   // its tab's, which Save as may not name either
        QString password;        // the tab's, tried before the password row is shown
        int viewerPageCount = 0; // used only when qpdf cannot count (locked)
        Probe probe;            // the worker's answer, never computed by this dialog
        // 0-based: the one row holds this page (Save Page As, and in Delete mode the
        // page in view). Without it the row holds every page, first to last
        // (Document > Extract Pages); in Delete mode it starts empty.
        std::optional<int> seedPage;
        bool hasUnsavedEdits = false;
        bool openWhenDone = true; // last choice, from Settings
        QStringList openPaths;    // files open in any tab: Save as may not name one
        Document *doc = nullptr;  // thumbnails; may be null (tests)
        RenderEngine *engine = nullptr;
    };

    // Consumes a completed probe and optional failed-attempt choices. Construction
    // reads no source file. SaveCheck checks its destination on a worker.
    explicit ExtractDialog(const Source &source, QWidget *parent = nullptr,
                           const std::optional<Choices> &choices = {});

    // Valid after acceptance. The caller writes this job in the tab's worker and
    // can reopen the dialog with choices() after a failure.
    ExtractPlan::Job job() const { return job_; }
    /// Captures retry choices independently of the dialog widgets.
    Choices choices() const;
    // The probe's verified password, or the row's chosen password that the worker
    // must verify before writing. Empty when the source is not encrypted.
    QString password() const { return password_; }
    /// Reports whether an accepted copy should open after successful writing.
    bool openWhenDone() const;

private:
    // Checks Save as again, as the disk can have changed since the last answer,
    // then captures the job if the pressed plan still matches the window.
    void accept() override;
    // Confirms an overwrite from the latest SaveCheck answer and returns the job.
    // Password verification and all writes belong to the caller's worker.
    void write();
    // Checks the Save as text off the UI thread (see SaveCheck). While Save as is
    // not edited, the check also steps the default past names in use. refresh()
    // asks for one when the text changes.
    void checkOutput();

    // Tear down and rebuild every row from plan_, refresh(), then make `selectRow`
    // (clamped) current and, when `reveal`, scroll the strip to it. Field puts the
    // caret at the end of that row's field and SelectAll selects its text; Keep
    // does what Field does only when focus was in the list, whose fields the
    // rebuild replaces, and otherwise leaves it.
    enum class Focus { Keep, Field, SelectAll };
    void rebuild(int selectRow, Focus focus, bool reveal = true);
    // A row's field, typed or pasted. Text with a comma or semicolon is split
    // into rows by applySplit() on the next event-loop turn.
    void specChanged(int row, const QString &text);
    void applySplit(); // the pending split, if any: every further piece a row below
    void refresh(); // row columns, strip, labels, Save default, error line, buttons
    // Side buttons and the strip's highlight follow the list's current row;
    // `reveal` also scrolls the strip to it when none of its cells are in view.
    void syncCurrentRow(bool reveal);
    void addRange();
    void moveCurrent(int delta);
    void removeCurrent();
    void removeCells(const ExtractPlan::Cell &cell);
    // In Delete mode, delete a kept page or keep a deleted one. The strip stays
    // where it is, since the click was on a page in view.
    void togglePage(const ExtractPlan::Cell &cell);
    void expand(const ExtractPlan::RunKey &run);
    void updateActions(); // the strip's context menu follows its current cell
    /// Runs the destination picker with guarded owner lifetime and window modality.
    void browseForOutput();
    // The error line's text: the first problem only. Empty when there is none, and
    // nothing while the check of the Save as text has not answered.
    std::optional<QString> problem() const;
    QString planProblem() const;                 // the problems before Save as
    std::optional<QString> saveAsProblem() const; // Save as, once its check answered
    // The error line and the button. Until the Save as check answers, a reason
    // about Save as stays, so a quick check does not make it flicker;
    // SaveCheck::slow() says when it takes long.
    void showProblem();

    Source source_;
    ExtractPlan plan_;
    ExtractPlan::Job job_;
    QString password_;                    // the one that unlocked the file
    QList<ExtractPlan::RunKey> expanded_; // opened folds; a resize or editing the run closes one
    std::optional<int> pendingFlat_;      // the strip cell to make current on the next refresh
    bool outputEdited_ = false;           // stop re-deriving Save as once the user typed
    bool passwordRejected_ = false;       // until the password field is edited again
    bool readError_ = false;              // qpdf could not open the file at all
    QString readErrorDetail_;             // qpdf's message, the error line's tooltip
    QString writeError_;                  // the last failed write, until Save as changes
    QString writeErrorDetail_;            // qpdf's message for it, the tooltip
    // The last Save as check's answer: the text it is for, and what it found.
    struct Checked
    {
        QString text;
        ExtractPlan::Destination facts;
    };
    std::optional<Checked> checked_;
    QString checking_;    // the text the last check asked for was for
    QString defaultName_; // plan_.fileName() when the default Save as text was set
    std::optional<ExtractPlan::Job> pressed_; // what Extract was pressed for, until written
    SaveCheck *saveCheck_ = nullptr;
    bool saveAsReasonShown_ = false; // the error line's reason is about Save as

    // Row fields carry the generation they were built in and ignore their edits
    // once it is behind: a rebuild replaced them, or a pending split outdated them.
    int fieldsGen_ = 0;
    struct PendingSplit
    {
        int row = 0;  // the field typed in...
        int gen = 0;  // ...and its generation
        QString text; // its latest text, split when applySplit() runs
    };
    std::optional<PendingSplit> split_;

    RowList *list_ = nullptr;
    QList<QLineEdit *> specs_; // one per row, rebuilt with the list
    QLabel *of_ = nullptr;
    // Extract mode only, as are removeAct_ and expandAct_.
    QPushButton *upBtn_ = nullptr;
    QPushButton *downBtn_ = nullptr;
    QPushButton *removeBtn_ = nullptr;
    ExtractStrip *strip_ = nullptr;
    QLabel *summary_ = nullptr;
    QLabel *error_ = nullptr;
    QLineEdit *passwordEdit_ = nullptr; // only when no known password unlocks the file
    QLineEdit *output_ = nullptr;
    QCheckBox *openWhenDone_ = nullptr;
    QPushButton *acceptBtn_ = nullptr;
    QAction *removeAct_ = nullptr;
    QAction *expandAct_ = nullptr;
    QAction *deleteAct_ = nullptr; // the strip's Delete Page and Keep Page, in
    QAction *keepAct_ = nullptr;   // Delete mode only
};

} // namespace mervin
