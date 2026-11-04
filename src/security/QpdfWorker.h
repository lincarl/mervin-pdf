#pragma once

#include "security/PageOps.h"
#include "render/MeasureContent.h"

#include <QByteArray>
#include <QStringList>

#include <functional>
#include <optional>

namespace mervin::qpdfWorker {

// Per application process. Admission covers the entire helper lifetime, including
// cleanup after cancellation. See docs/QPDF-WORKERS.md for platform differences.
inline constexpr int kMaxWorkers = 2;
inline constexpr quint64 kMemoryBytes = 1024ULL * 1024 * 1024;
inline constexpr int kCpuSeconds = 60;
inline constexpr int kWallMilliseconds = 120000;
inline constexpr qsizetype kMaxRequestBytes = 32 * 1024 * 1024;
inline constexpr qsizetype kMaxFrameBytes = 64 * 1024;
inline constexpr quint64 kMaxOutputBytes = 1024ULL * 1024 * 1024;
inline constexpr int kMaxSplitParts = 10000;

enum class Operation { Info, Decrypt, Encrypt, Probe, Delete, Rotate, Merge, Split,
                       Flatten, Embed, Keep, Blob };

/// Values captured by the caller. Passwords travel only through anonymous pipes.
struct Request {
    Operation operation = Operation::Info;
    QString path, output, password, userPassword, ownerPassword, baseName;
    QList<int> pages;
    QList<PageOps::MergeInput> inputs;
    int angle = 0;
    bool relative = true;
    QpdfService::Algorithm algorithm = QpdfService::Algorithm::AES256;
    QpdfService::Permissions permissions;
    std::vector<RenderMeasurement> marks;
    std::optional<MeasureDoc> measurements;
    bool keepStoredBlob = false;
    bool failRetarget = false; // explicit algorithm fault injection, never an isolation bypass
};

struct Result {
    QpdfService::Status status = QpdfService::Status::Failed;
    QString error;
    int failedIndex = -1;
    int count = -1;
    bool encrypted = false;
    QString sourceKey;
    QpdfService::Info info;
    std::optional<QByteArray> blob;
    QStringList outputs; // parent-confirmed commits only
    PageOps::testing::SplitProjection projection;
};

struct Callbacks {
    std::function<bool(const QStringList &)> mayWrite;
    std::function<bool(const QString &)> mayCommit;
};

/// Runs one bounded helper, with cancellable admission and parent-owned publication.
Result run(const Request &request, const Callbacks &callbacks = {});

/// True only in the internal helper after OS limits have been installed.
bool inWorker();

/// Handles the internal mode before QApplication, profile or single-instance setup.
int main(int argc, char **argv);

/// A translated, visible reason for a resource, timeout, protocol or helper failure.
QString failureText();

/// Reports a source step, retaining its original merge index even for repeated paths.
bool reading(const QString &path, bool again = false, int sourceIndex = -1);

/// Opens, streams and finishes one output through the parent's atomic writer.
void openOutput(const QString &path);
void writeOutput(const unsigned char *data, size_t length);
void finishOutput();

namespace testing {
/// Tightens parent bounds for enforcement tests. Values above production limits clamp.
/// Zero restores the production defaults. These hooks cannot disable isolation.
void setBounds(quint64 outputBytes = 0, int wallMilliseconds = 0);
/// Observes real helper admission. The callback must not wait or start another job.
void setStartedHook(std::function<void(qint64)> hook);
/// Number of live admitted helpers, including helpers being stopped.
int activeWorkers();
}

} // namespace mervin::qpdfWorker
