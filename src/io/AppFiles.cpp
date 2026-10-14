#include "io/AppFiles.h"

#include "io/Background.h"
#include "session/SessionEnd.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
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
    bool waited = false; // a caller waits for this write's own result
    quint64 serial = 0;  // tells that caller when it is done
    bool recovery = false;
};

std::mutex mutex;
std::condition_variable changed; // notified when a job finishes or the queue empties
std::deque<Job> jobs;            // guarded by mutex
bool working = false;            // guarded by mutex: a thread is draining jobs
quint64 lastSerial = 0;          // guarded by mutex
std::set<quint64> outstanding;   // guarded by mutex: the serials of jobs not finished yet
bool held = false;               // guarded by mutex: see testing::hold
std::function<void(const QString &, bool)> observer; // guarded by mutex: see testing::observe
// Guarded by mutex: why recent jobs failed, by serial, for writeAndWait. Old ones are
// dropped, as nobody waits for most jobs.
std::map<quint64, QString> failures;
constexpr std::size_t kFailuresKept = 64;

// Performs `job` and returns why it failed, or an empty string.
QString perform(const Job &job, bool *failed)
{
    *failed = false;
    if (job.remove) {
        if (job.recovery && !sessionend::beginRecoveryRemoval())
            return {};
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
        std::function<void(const QString &, bool)> notify;
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [] { return !held; });
            if (jobs.empty()) {
                working = false;
                changed.notify_all();
                return;
            }
            job = std::move(jobs.front());
            jobs.pop_front();
            notify = observer;
        }
        bool failed = false;
        const QString error = perform(job, &failed);
        if (notify)
            notify(job.path, job.remove);
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
        // A newer write of a file makes the last job waiting for it unneeded when
        // that job is a write nobody waits for, so it goes. The new one queues last,
        // after everything asked for before it, so the files change in the order
        // asked and repeated saves of a file do not pile up while the folder stalls.
        if (!job.remove) {
            const auto last = std::find_if(jobs.rbegin(), jobs.rend(), [&job](const Job &waiting) {
                return waiting.path == job.path;
            });
            if (last != jobs.rend() && !last->remove && !last->waited) {
                outstanding.erase(last->serial);
                jobs.erase(std::next(last).base());
            }
        }
        jobs.push_back(std::move(job));
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
    const quint64 serial = add({path, data, false, true});
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

void removeRecovery(const QString &path)
{
    if (!path.isEmpty())
        add({.path = path, .remove = true, .recovery = true});
}

namespace testing {

void hold(bool on)
{
    std::lock_guard<std::mutex> lock(mutex);
    held = on;
    changed.notify_all();
}

void observe(std::function<void(const QString &, bool)> done)
{
    std::lock_guard<std::mutex> lock(mutex);
    observer = std::move(done);
}

} // namespace testing

bool flush(int ms)
{
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::milliseconds(ms),
                            [] { return jobs.empty() && !working; });
}

} // namespace mervin::appfiles
