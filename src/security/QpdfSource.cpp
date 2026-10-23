#include "security/QpdfSource.h"

#include "io/JobContext.h"

#include <qpdf/Constants.h>
#include <qpdf/FileInputSource.hh>
#include <qpdf/Pl_Flate.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QUtil.hh>

#include <exception>
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace mervin {

namespace {

// qpdf's own file source, with each read reported to the current job.
class ReportedFileSource final : public FileInputSource
{
public:
    explicit ReportedFileSource(std::shared_ptr<QpdfReadState> state)
        : FileInputSource(state->path.toUtf8().constData()), state_(std::move(state))
    {
        FileInputSource::seek(0, SEEK_END);
        size_ = FileInputSource::tell();
        FileInputSource::rewind();
    }

    // Every read goes through here, findAndSkipNextEOL() included.
    size_t read(char *data, size_t length) override
    {
        job::switchTo(job::Step::Reading, state_->path);
        try {
            const qpdf_offset_t offset = FileInputSource::tell();
            const size_t got = FileInputSource::read(data, length);
            // Reading past the original EOF is normal. Losing bytes that were present
            // when this handle opened is not, even if qpdf can write a partial stream.
            const auto available = std::max<qpdf_offset_t>(0, size_ - offset);
            if (got < std::min<unsigned long long>(length, available))
                throw std::runtime_error("source file became shorter while reading");
            return got;
        } catch (const std::exception &e) {
            failed(e);
        }
    }

    /// Keeps failed seeks attributable to the source as well as failed reads.
    void seek(qpdf_offset_t offset, int whence) override
    {
        try {
            FileInputSource::seek(offset, whence);
        } catch (const std::exception &e) {
            failed(e);
        }
    }

private:
    /// Records the first failure before qpdf can catch and replace its exception.
    [[noreturn]] void failed(const std::exception &e)
    {
        if (state_->error.isEmpty())
            state_->error = QString::fromUtf8(e.what());
        throw QpdfReadError(state_->path, state_->error);
    }

    std::shared_ptr<QpdfReadState> state_;
    qpdf_offset_t size_ = 0;
};

// Opens `path` in a new QPDF with `password` as its exact bytes, as QPDF::processFile()
// does, and sets `pdf`. False when qpdf rejects the password. Other errors throw.
bool open(std::unique_ptr<QPDF> &pdf, const std::shared_ptr<QpdfReadState> &state,
          const std::string &password)
{
    auto opened = std::make_unique<QPDF>();
    try {
        opened->processInputSource(std::make_shared<ReportedFileSource>(state),
                                   password.empty() ? nullptr : password.c_str());
        checkQpdfWarnings(*opened, state.get());
        state->throwIfFailed();
    } catch (const QPDFExc &e) {
        checkQpdfWarnings(*opened, state.get());
        state->throwIfFailed();
        if (e.getErrorCode() == qpdf_e_password)
            return false;
        throw;
    } catch (const std::exception &) {
        checkQpdfWarnings(*opened, state.get());
        state->throwIfFailed();
        throw;
    }
    pdf = std::move(opened);
    return true;
}

} // namespace

QpdfReadError::QpdfReadError(const QString &path, const QString &message)
    : std::runtime_error(message.toStdString()), path_(path)
{
}

void QpdfReadState::throwIfFailed() const
{
    if (!error.isEmpty())
        throw QpdfReadError(path, error);
}

QString qpdfFailurePath(const std::exception &error)
{
    if (const auto *source = dynamic_cast<const QpdfReadError *>(&error))
        return source->path();
    if (const auto *qpdf = dynamic_cast<const QPDFExc *>(&error))
        return QString::fromUtf8(qpdf->getFilename().c_str());
    return {};
}

void checkQpdfWarnings(QPDF &pdf, QpdfReadState *state)
{
    // These are the structured error code and exact message construction in qpdf
    // 11.10 and 12. A filename or object description containing the same words
    // must not turn an unrelated repair warning into a limit failure.
    constexpr std::string_view prefix = "error decoding stream data for object ";
    constexpr std::string_view suffix = ": PL_Flate memory limit exceeded";
    for (const QPDFExc &warning : pdf.getWarnings()) {
        const std::string &detail = warning.getMessageDetail();
        if (warning.getErrorCode() != qpdf_e_damaged_pdf || !detail.starts_with(prefix)
            || !detail.ends_with(suffix))
            continue;
        const QString reason = QString::fromUtf8(warning.what());
        if (state) {
            if (state->error.isEmpty())
                state->error = reason;
            state->throwIfFailed();
        }
        const QString path = QString::fromUtf8(warning.getFilename().c_str());
        throw QpdfReadError(path, reason);
    }
}

QpdfService::Status openQpdf(const QString &path, const QString &password,
                             std::unique_ptr<QPDF> &pdf, QString *error,
                             std::shared_ptr<QpdfReadState> *readState)
{
    using Status = QpdfService::Status;
    const auto failed = [error](const QString &reason) {
        if (error)
            *error = reason;
        return Status::Failed;
    };
    if (!job::reading(path))
        return failed(job::notRunError());
    // qpdf stores this setting globally. Set it once before any application decoder
    // starts, and never change it while other workers may be decoding streams.
    static std::once_flag configured;
    std::call_once(configured, [] { Pl_Flate::memory_limit(kMaxQpdfFlateBytes); });
    const auto state = std::make_shared<QpdfReadState>();
    state->path = path;
    if (readState)
        *readState = state;
    const std::string utf8 = password.toStdString();
    // Only a password with a character beyond ASCII that PDFDocEncoding also has, such
    // as "café", has a second form to try. A password with a character that
    // PDFDocEncoding lacks has only its UTF-8 form.
    std::string pdfDoc;
    const bool twoForms = QUtil::utf8_to_pdf_doc(utf8, pdfDoc) && pdfDoc != utf8;
    try {
        if (open(pdf, state, utf8))
            return Status::Ok;
        if (twoForms) {
            // The second attempt reads the file again.
            if (!job::reading(path))
                return failed(job::notRunError());
            if (open(pdf, state, pdfDoc))
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
