#pragma once

// Test helpers that hold up the app's own work the way a share that stopped
// answering does, so a test can check that the screen thread does not wait for it.

#include "render/Document.h"

#include <QFile>
#include <QString>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#ifdef Q_OS_UNIX
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace mervin::testing {

// Holds `held` on another thread, as a render or a search stuck reading a document
// from a share that stopped answering holds the document's lock, until released or
// for at most `ms` milliseconds. The constructor returns once the lock is taken.
// `Mutex` is any type with lock() and unlock(), such as the document's lifetime gate
// (a std::timed_mutex) or its access lock (a std::mutex).
template <class Mutex>
class Stall
{
public:
    explicit Stall(Mutex &held, int ms = 3000)
        : thread_([this, &held, ms] {
            std::lock_guard<Mutex> holding(held);
            std::unique_lock<std::mutex> lock(mutex_);
            locked_ = true;
            changed_.notify_all();
            changed_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return released_; });
        })
    {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return locked_; });
    }
    ~Stall()
    {
        release();
        thread_.join();
    }
    Stall(const Stall &) = delete;
    Stall &operator=(const Stall &) = delete;

    void release()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool locked_ = false;
    bool released_ = false;
    std::thread thread_; // last: it uses the members above
};

// Takes a document the way a save job does, as a lockable for Stall. On the Stall's
// thread it takes Document::Hold, then the access lock, which savePdfTo keeps while
// it writes. Meanwhile a query of the document on the screen thread is refused and
// counted (Document::testing::refusals()), and a call that takes the access lock
// waits.
struct SavingJob
{
    std::shared_ptr<DocumentLifetime> lifetime;
    std::optional<Document::Hold> hold;
    bool answered = false; // the job holds the document and its own call was answered

    void lock()
    {
        hold.emplace(lifetime);
        Document *document = hold->document();
        answered = document && document->isPdf();
        if (document)
            document->accessMutex().lock();
    }
    void unlock()
    {
        if (Document *document = hold->document())
            document->accessMutex().unlock();
        hold.reset();
    }
};

#ifdef Q_OS_UNIX
// Opening a named pipe waits for a writer, as a read from a share that stopped
// answering waits for the share. This lets every such open through, by opening and
// closing the writing end, once released or after `ms` milliseconds. The reader then
// sees an empty file. Unix only: tests that use it are compiled out on Windows.
class PipeWriter
{
public:
    explicit PipeWriter(const QString &path, int ms = 30000)
        : thread_([this, path = QFile::encodeName(path), ms] {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return released_; });
            while (!stopped_) {
                lock.unlock();
                const int fd = ::open(path.constData(), O_WRONLY | O_NONBLOCK);
                if (fd >= 0)
                    ::close(fd);
                lock.lock();
                changed_.wait_for(lock, std::chrono::milliseconds(10), [this] { return stopped_; });
            }
        })
    {
    }
    ~PipeWriter()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = stopped_ = true;
            changed_.notify_all();
        }
        thread_.join();
    }
    PipeWriter(const PipeWriter &) = delete;
    PipeWriter &operator=(const PipeWriter &) = delete;

    void release()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool released_ = false;
    bool stopped_ = false;
    std::thread thread_; // last: it uses the members above
};
#endif

} // namespace mervin::testing
