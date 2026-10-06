#pragma once

#include <QWidget>
#include <QThreadPool>
#include "recent/ViewState.h"

#include <memory>

class QLabel;

namespace mervin {

class RenderEngine;
class Document;
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
    ~TabPage() override;

    // Open `path`. For an encrypted document, pass the user password; if one is
    // required but missing/wrong, returns false and sets *needsPassword so the
    // caller can prompt and retry. On success the password becomes password().
    bool open(const QString &path, const QString &password = QString(), QString *error = nullptr,
              bool *needsPassword = nullptr);

    // Create a tab without reading its PDF. A recovery checkpoint takes precedence over disk.
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
    // Include duplicate paths when independent edited views have recoverable copies.
    static QStringList recoverablePaths();

    // Release the open Document so the underlying file handle is closed (needed
    // before replacing the file on disk, e.g. an in-place "Save Measurements").
    // The viewer shows a blank page until open() is called again.
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
    static std::shared_ptr<OpenResult> readDocument(RenderEngine *engine, const QString &path,
                                                  const QString &password);
    void installDocument(const std::shared_ptr<OpenResult> &result);
    bool finishResume(const std::shared_ptr<OpenResult> &result);
    void showPlaceholder(const QString &message, bool retry);
    void clearRecovery();
    bool writeRecoveryManifest(QString *error);
    bool adoptRecovery();
    void setPath(const QString &path);
    void stopLoading();
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
    quint64 loadGeneration_ = 0;
    QThreadPool loadPool_;
};

} // namespace mervin
