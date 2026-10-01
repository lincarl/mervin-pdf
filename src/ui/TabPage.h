#pragma once

#include <QWidget>

#include <memory>

namespace mervin {

class RenderEngine;
class Document;
class ViewerWidget;
class MeasurePanel;
class AnnotPanel;
class PanelStack;

// One open document in a tab: owns the Document and contains its ViewerWidget.
// Created with the window's shared RenderEngine (which must outlive the page).
// The find bar lives in MainWindow (shared, adaptive), not here.
class TabPage : public QWidget
{
    Q_OBJECT

public:
    explicit TabPage(RenderEngine *engine, QWidget *parent = nullptr);
    ~TabPage() override;

    // Open `path`. For an encrypted document, pass the user password; if one is
    // required but missing/wrong, returns false and sets *needsPassword so the
    // caller can prompt and retry. On success the password becomes password().
    bool open(const QString &path, const QString &password = QString(), QString *error = nullptr,
              bool *needsPassword = nullptr);

    // Release the open Document so the underlying file handle is closed (needed
    // before replacing the file on disk, e.g. an in-place "Save Measurements").
    // The viewer shows a blank page until open() is called again.
    void detachDocument();
    bool recoverSnapshot(const QString &snapshot, QString *error);
    bool hasRecoverySnapshot() const { return !recoveryPath_.isEmpty(); }

    ViewerWidget *viewer() const { return viewer_; }
    MeasurePanel *measurePanel() const { return measurePanel_; }
    AnnotPanel *annotPanel() const { return annotPanel_; }
    QString path() const { return path_; }
    QString canonicalPath() const { return canonicalPath_; }
    QString tabTitle() const;      // file name
    QString documentTitle() const; // embedded PDF title, or file name

    // The password that unlocks this tab's file (empty when it is not encrypted),
    // so the qpdf operations that re-read the file from disk do not ask again.
    // setPassword() records one the user typed later that worked. Kept in memory
    // for the tab's lifetime only and never written anywhere (settings, session,
    // recent list, closed-tab history, logs): the decrypted document is already in
    // memory, so also holding its password adds little exposure.
    QString password() const { return password_; }
    void setPassword(const QString &password) { password_ = password; }

private:
    RenderEngine *engine_;
    std::unique_ptr<Document> doc_;
    ViewerWidget *viewer_ = nullptr;
    MeasurePanel *measurePanel_ = nullptr;
    AnnotPanel *annotPanel_ = nullptr;
    PanelStack *panelStack_ = nullptr; // docks measurePanel_ + annotPanel_ as a group
    QString path_;
    QString canonicalPath_;
    QString recoveryPath_;
    QString password_; // see password()
};

} // namespace mervin
