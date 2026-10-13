#include "io/AppFiles.h"

#include "io/Background.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <set>
#include <mutex>

namespace mervin::appfiles {

namespace {

struct Job
{
    QString path;
    QByteArray data;
    bool remove = false;
    quint64 serial = 0; // tells a caller waiting for this write when it is done
};

std::mutex mutex;
std::condition_variable changed; // notified when a job finishes or the queue empties
std::deque<Job> jobs;            // guarded by mutex
bool working = false;            // guarded by mutex: a thread is draining jobs
quint64 lastSerial = 0;          // guarded by mutex
// Guarded by mutex: the serials of jobs not finished yet. A write replaced in place
// takes a newer serial but keeps its place, so serials do not finish in order.
std::set<quint64> outstanding;
// Guarded by mutex: why recent jobs failed, by serial, for writeAndWait. Old ones are
// dropped, as nobody waits for most jobs.
std::map<quint64, QString> failures;
constexpr std::size_t kFailuresKept = 64;

// Performs `job` and returns why it failed, or an empty string.
QString perform(const Job &job, bool *failed)
{
    *failed = false;
    if (job.remove) {
        if (QFileInfo::exists(job.path) && !QFile::remove(job.path)) {
            qWarning() << "Could not remove" << job.path;
            *failed = true;
        }
        return {};
    }
    QDir().mkpath(QFileInfo(job.path).absolutePath());
    QSaveFile file(job.path);
    if (!file.open(QIODevice::WriteOnly) || file.write(job.data) != job.data.size()
        || !file.commit()) {
        qWarning() << "Could not save" << job.path << ":" << file.errorString();
        *failed = true;
        return file.errorString();
    }
    return {};
}

void drain()
{
    for (;;) {
        Job job;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (jobs.empty()) {
                working = false;
                changed.notify_all();
                return;
            }
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        bool failed = false;
        const QString error = perform(job, &failed);
        std::lock_guard<std::mutex> lock(mutex);
        outstanding.erase(job.serial);
        if (failed) {
            failures[job.serial] = error;
            if (failures.size() > kFailuresKept)
                failures.erase(failures.begin());
        }
        changed.notify_all();
    }
}

// Queues `job` and returns its serial.
quint64 add(Job job)
{
    bool start = false;
    quint64 serial = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        serial = job.serial = ++lastSerial;
        // The last job waiting for the file, if it is a write, takes the new data in
        // place, so the files keep the order they were asked for. Anything else,
        // such as a removal after that write, waits its turn.
        const auto last = std::find_if(jobs.rbegin(), jobs.rend(),
                                       [&job](const Job &waiting) { return waiting.path == job.path; });
        if (!job.remove && last != jobs.rend() && !last->remove) {
            last->data = std::move(job.data);
            outstanding.erase(last->serial);
            last->serial = serial;
        } else {
            jobs.push_back(std::move(job));
        }
        outstanding.insert(serial);
        start = !std::exchange(working, true);
    }
    if (start)
        background::start(drain);
    return serial;
}

} // namespace

void write(const QString &path, const QByteArray &data)
{
    if (!path.isEmpty())
        add({path, data, false});
}

Wait writeAndWait(const QString &path, const QByteArray &data, int ms, QString *error)
{
    const quint64 serial = add({path, data, false});
    std::unique_lock<std::mutex> lock(mutex);
    if (!changed.wait_for(lock, std::chrono::milliseconds(ms),
                          [serial] { return !outstanding.contains(serial); }))
        return Wait::Pending;
    const auto failure = failures.find(serial);
    if (failure == failures.end())
        return Wait::Written;
    if (error)
        *error = failure->second;
    failures.erase(failure);
    return Wait::Failed;
}

void remove(const QString &path)
{
    if (!path.isEmpty())
        add({path, {}, true});
}

bool flush(int ms)
{
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::milliseconds(ms),
                            [] { return jobs.empty() && !working; });
}

} // namespace mervin::appfiles
