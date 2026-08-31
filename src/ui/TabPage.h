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
    // caller can prompt and retry.
    bool open(const QString &path, const QString &password = QString(), QString *error = nullptr,
              bool *needsPassword = nullptr);

    // Release the open Document so the underlying file handle is closed (needed
    // before replacing the file on disk, e.g. an in-place "Save Measurements").
    // The viewer shows a blank page until open() is called again.
    void detachDocument();

    ViewerWidget *viewer() const { return viewer_; }
    MeasurePanel *measurePanel() const { return measurePanel_; }
    AnnotPanel *annotPanel() const { return annotPanel_; }
    QString path() const { return path_; }
    QString canonicalPath() const { return canonicalPath_; }
    QString tabTitle() const;      // file name
    QString documentTitle() const; // embedded PDF title, or file name

private:
    RenderEngine *engine_;
    std::unique_ptr<Document> doc_;
    ViewerWidget *viewer_ = nullptr;
    MeasurePanel *measurePanel_ = nullptr;
    AnnotPanel *annotPanel_ = nullptr;
    PanelStack *panelStack_ = nullptr; // docks measurePanel_ + annotPanel_ as a group
    QString path_;
    QString canonicalPath_;
};

} // namespace mervin
