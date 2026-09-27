#pragma once

#include "extract/ExtractPlan.h"

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

// Document > Extract Pages.
//
// The old flow was three modals: a text prompt for the range, a native save
// dialog, and an "Open it now?" box. Nothing showed what the new file would
// contain until it was written, and a bad range was only reported after the fact.
//
// This dialog shows the extract plan instead: one output file, built from rows
// of one page or range each, reordered like the rows of Merge PDFs (the same
// RowList). The strip under them is the result drawn in output order, with a bad
// row as a red cell where it would land. Every edit, in a row or in the strip,
// goes into the plan, and everything on screen is rebuilt from it. All
// arithmetic lives in ExtractPlan (mervin_core, unit-tested); this class wires
// widgets to it.
class ExtractDialog : public QDialog
{
    Q_OBJECT

public:
    struct Source
    {
        QString path;            // the file on disk: what qpdf reads
        QString password;        // the tab's, tried before the password row is shown
        int viewerPageCount = 0; // used only when qpdf cannot count (locked)
        int currentPage = 0;     // 0-based; seeds the first row
        bool hasUnsavedEdits = false;
        bool openWhenDone = true; // last choice, from Settings
        QStringList openPaths;    // files open in any tab: Save as may not name one
        Document *doc = nullptr;  // thumbnails; may be null (tests)
        RenderEngine *engine = nullptr;
    };

    // Probes `source.path` with qpdf before anything is shown: the page count, the
    // password row and a read failure all shape the dialog. An encrypted file that
    // `source.password` unlocks is treated like an unencrypted one: no row appears.
    explicit ExtractDialog(const Source &source, QWidget *parent = nullptr);

    // Valid after exec() == Accepted, by which time the file has been written:
    // accept() writes it and stays open on a failure, so the rows survive it.
    ExtractPlan::Job job() const { return job_; }
    // Verified: the Source's password when it worked, else the one typed in the
    // row. Empty when the file is not encrypted.
    QString password() const { return password_; }
    bool openWhenDone() const;

private:
    // Verify the password, confirm an overwrite, then write the file. Closes only
    // once the write succeeded; a failure goes on the error line instead.
    void accept() override;

    // Tear down and rebuild every row from plan_, refresh(), then make `selectRow`
    // (clamped) current and reveal it in the strip. Field puts the caret at the
    // end of that row's field and SelectAll selects its text; Keep does what Field
    // does only when focus was in the list, whose fields the rebuild replaces, and
    // otherwise leaves it.
    enum class Focus { Keep, Field, SelectAll };
    void rebuild(int selectRow, Focus focus);
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
    void expand(const ExtractPlan::RunKey &run);
    void updateActions(); // the strip's context menu follows its current cell
    void browseForOutput();
    QString problem() const; // the error line's text: the first problem only

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
};

} // namespace mervin
