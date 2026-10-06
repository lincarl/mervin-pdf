#pragma once

#include <QCoreApplication>
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
    Q_DECLARE_TR_FUNCTIONS(mervin::OpenPdfDialog)

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
// native pre-validation hook. Its own tr() context, rather than QFileDialog's,
// matches the Windows class, so both platforms share one set of translations.
class OpenPdfDialog final : public QFileDialog
{
    Q_DECLARE_TR_FUNCTIONS(mervin::OpenPdfDialog)

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
