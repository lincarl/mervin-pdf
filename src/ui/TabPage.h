#pragma once

#include <QWidget>
#include "recent/ViewState.h"

#include <memory>

class QLabel;

namespace mervin {

class RenderEngine;
class Document;
class DocumentOpener;
class ViewerWidget;
class MeasurePanel;
class AnnotPanel;
class FindCard;
class PanelStack;

// One open document in a tab: owns the Document and contains its ViewerWidget.
// Created with the window's shared RenderEngine (which must outlive the page).
// Its find card floats over the viewer, so each tab keeps its own search.
class TabPage : public QWidget
{
    Q_OBJECT

public:
    // The author stamped on new comments: the configured name, or the OS user
    // name when it is empty. Settings shows the fallback as its placeholder.
    static QString resolvedAnnotAuthor(const QString &configured);

    explicit TabPage(RenderEngine *engine, QWidget *parent = nullptr);
    // Never waits for the document: a load or render stuck on a drive that stopped
    // answering would hang the window. The document closes on a background thread.
    ~TabPage() override;

    // Open `path`. For an encrypted document, pass the user password; if one is
    // required but missing/wrong, returns false and sets *needsPassword so the
    // caller can prompt and retry. On success the password becomes password().
    bool open(const QString &path, const QString &password = QString(), QString *error = nullptr,
              bool *needsPassword = nullptr);

    // Create a tab without reading its PDF or touching the disk for it: `path` is
    // taken as the canonical path, as the session and Recent store it. A recovery
    // checkpoint takes precedence over disk.
    void initializeSuspended(const QString &path);
    void initializeSuspended(const QString &path, const ViewState &state);
    bool isLoaded() const { return doc_ != nullptr; }
    bool isLoading() const { return loading_; }
    bool isSuspended() const { return !doc_ && !loading_; }
    bool suspend(QString *error = nullptr);
    bool resume(QString *error = nullptr, bool *needsPassword = nullptr);
    void resumeAsync();
    bool hasUnsavedEdits() const;
    ViewState capturedViewState() const;
    void setSavedViewState(const ViewState &state);
    bool sourceChangedOnDisk() const;
    QString recoveryPath() const { return recoveryPath_; }
    // A document with a recovery copy: the path it was opened with, and its
    // canonical path, which the session stores.
    struct RecoverableDocument
    {
        QString original;
        QString canonical;
    };
    // The documents with a recovery copy, duplicates included when independent
    // edited views have recoverable copies. Reads the recovery folder, which
    // startup does once; tabs restored lazily adopt the copies found then.
    static QList<RecoverableDocument> recoverableDocuments();
    // Their canonical paths, or original ones where no canonical path was recorded.
    static QStringList recoverablePaths();

    // Release the open Document and close its file here, waiting for any work in it
    // (needed before replacing the file on disk, e.g. an in-place "Save
    // Measurements"). The viewer shows a blank page until open() is called again.
    void detachDocument();
    bool recoverSnapshot(const QString &snapshot, QString *error);
    bool hasRecoverySnapshot() const { return !recoveryPath_.isEmpty(); }

    ViewerWidget *viewer() const { return viewer_; }
    MeasurePanel *measurePanel() const { return measurePanel_; }
    AnnotPanel *annotPanel() const { return annotPanel_; }
    FindCard *findCard() const { return findCard_; }
    QString path() const { return path_; }
    QString canonicalPath() const { return canonicalPath_; }
    QString tabTitle() const;      // file name
    QString documentTitle() const; // embedded PDF title, or file name

    // Verified password for this tab, reused by disk operations. Retain only in memory for the
    // tab lifetime; never persist or log it.
    QString password() const { return password_; }
    void setPassword(const QString &password) { password_ = password; }

signals:
    void stateChanged();
    void resumeFinished(bool success, const QString &error, bool needsPassword);

private:
    struct SavedState;
    struct OpenResult;
    struct Load;
    static std::shared_ptr<OpenResult> readDocument(DocumentOpener &opener, const QString &path,
                                                  const QString &password);
    void installDocument(const std::shared_ptr<OpenResult> &result);
    bool finishResume(const std::shared_ptr<OpenResult> &result);
    void showPlaceholder(const QString &message, bool retry);
    void clearRecovery();
    bool writeRecoveryManifest(QString *error);
    bool adoptRecovery();
    // The tab's path and canonical path. `resolve` follows links on the disk;
    // without it `path` is taken as canonical.
    void setPath(const QString &path, bool resolve = true);
    void finishLoad(const std::shared_ptr<Load> &load);
    // Drops the load in progress, waiting for it, for the callers that read or
    // replace the document's file on this thread next.
    void stopLoading();
    // Takes the document away from the viewer and the tab, for its caller to close.
    std::unique_ptr<Document> releaseDocument();
    void restoreViewer(); // restoreResumeState from saved_, without a search the card hides

    RenderEngine *engine_;
    std::unique_ptr<Document> doc_;
    ViewerWidget *viewer_ = nullptr;
    MeasurePanel *measurePanel_ = nullptr;
    AnnotPanel *annotPanel_ = nullptr;
    FindCard *findCard_ = nullptr;
    PanelStack *panelStack_ = nullptr; // docks findCard_, measurePanel_ and annotPanel_
    QString path_;
    QString canonicalPath_;
    QString recoveryPath_;
    QString recoveryManifest_;
    QString password_; // see password()
    std::unique_ptr<SavedState> saved_;
    QWidget *placeholder_ = nullptr;
    ::QLabel *placeholderLabel_ = nullptr;
    QWidget *retryButton_ = nullptr;
    bool loading_ = false;
    bool retainedEdits_ = false;
    std::shared_ptr<Load> load_; // the resumeAsync() load in progress
};

} // namespace mervin
