#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

class QWidget;

#ifndef Q_OS_WIN
#include <QFileDialog>
#endif

namespace mervin {

#ifdef Q_OS_WIN

// Windows' native picker, used as an input surface only. The text in its file
// name field is captured before Mervin decides whether to open local files or
// download an HTTP(S) URL.
class OpenPdfDialog final
{
public:
    explicit OpenPdfDialog(QWidget *parent = nullptr);

    int exec();
    QString internetUrl() const { return internetUrl_; }
    QStringList selectedFiles() const { return selectedFiles_; }
    static constexpr bool usesNativeDialog() { return true; }

private:
    QWidget *parent_ = nullptr;
    QString internetUrl_;
    QStringList selectedFiles_;
};

#else

// The URL-capturing behavior relies on the Windows IFileDialog event API.
// Other platforms retain the in-process picker until they gain an equivalent
// native pre-validation hook.
class OpenPdfDialog final : public QFileDialog
{
public:
    explicit OpenPdfDialog(QWidget *parent = nullptr);

    QString internetUrl() const { return internetUrl_; }
    void accept() override;
    static constexpr bool usesNativeDialog() { return false; }

private:
    QString internetUrl_;
};

#endif

} // namespace mervin
