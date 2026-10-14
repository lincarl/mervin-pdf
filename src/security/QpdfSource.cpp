#include "security/QpdfSource.h"

#include "io/JobContext.h"

#include <qpdf/FileInputSource.hh>
#include <qpdf/QPDF.hh>

#include <memory>

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

} // namespace

void processReportedFile(QPDF &pdf, const QString &path, const char *password)
{
    pdf.processInputSource(std::make_shared<ReportedFileSource>(path), password);
}

} // namespace mervin
