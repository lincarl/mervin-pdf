#include "security/QpdfSource.h"

#include "io/JobContext.h"

#include <qpdf/Constants.h>
#include <qpdf/FileInputSource.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QUtil.hh>

#include <exception>
#include <memory>
#include <string>

namespace mervin {

namespace {

// qpdf's own file source, with each read reported to the current job.
class ReportedFileSource final : public FileInputSource
{
public:
    explicit ReportedFileSource(const QString &path)
        : FileInputSource(path.toUtf8().constData()), path_(path)
    {
    }

    // Every read goes through here, findAndSkipNextEOL() included.
    size_t read(char *data, size_t length) override
    {
        job::switchTo(job::Step::Reading, path_);
        return FileInputSource::read(data, length);
    }

private:
    const QString path_;
};

// Opens `path` in a new QPDF with `password` as its exact bytes, as QPDF::processFile()
// does, and sets `pdf`. False when qpdf rejects the password. Other errors throw.
bool open(std::unique_ptr<QPDF> &pdf, const QString &path, const std::string &password)
{
    auto opened = std::make_unique<QPDF>();
    try {
        opened->processInputSource(std::make_shared<ReportedFileSource>(path),
                                   password.empty() ? nullptr : password.c_str());
    } catch (const QPDFExc &e) {
        if (e.getErrorCode() == qpdf_e_password)
            return false;
        throw;
    }
    pdf = std::move(opened);
    return true;
}

} // namespace

QpdfService::Status openQpdf(const QString &path, const QString &password,
                             std::unique_ptr<QPDF> &pdf, QString *error)
{
    using Status = QpdfService::Status;
    const auto failed = [error](const QString &reason) {
        if (error)
            *error = reason;
        return Status::Failed;
    };
    if (!job::reading(path))
        return failed(job::notRunError());
    const std::string utf8 = password.toStdString();
    // Only a password with a character beyond ASCII that PDFDocEncoding also has, such
    // as "café", has a second form to try. A file cannot be keyed with a character that
    // PDFDocEncoding lacks.
    std::string pdfDoc;
    const bool twoForms = QUtil::utf8_to_pdf_doc(utf8, pdfDoc) && pdfDoc != utf8;
    try {
        if (open(pdf, path, utf8))
            return Status::Ok;
        if (twoForms) {
            // The second attempt reads the file again.
            if (!job::reading(path))
                return failed(job::notRunError());
            if (open(pdf, path, pdfDoc))
                return Status::Ok;
        }
    } catch (const std::exception &e) {
        return failed(QString::fromUtf8(e.what()));
    }
    if (error)
        *error = QpdfService::tr("A password is required to open this document.");
    return Status::NeedsPassword;
}

} // namespace mervin
