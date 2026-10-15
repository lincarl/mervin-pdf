#pragma once

#include <QString>
#include <QSet>
#include <QtGlobal>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

// What a background job of a tab is doing, so the tab can say what it waits for and
// stop waiting, and so a job that was stopped never replaces a file afterwards. A
// thread blocked in a file call cannot be interrupted, so stopping a job only means
// that it gives up at its next step and never commits. The call it is in still runs
// until the disk answers.
//
// A job opens a Scope for its State on its own thread. The code that reads and writes
// document files (AtomicPdfWriter, DocumentOutput::replace, the qpdf sources and
// RenderEngine::open) reports through the free functions below, which act on the
// state of the calling thread's scope. Outside a scope nothing is reported to a job
// and every step goes ahead, unless a test hook holds it or makes it fail.
namespace mervin::job {

using ClaimId = quint64;
class OpenPath;

// What a job is doing, for the wording of its tab.
enum class Step
{
    None,
    Reading,  // the path is the file being read
    Writing,  // the path is the folder being written to
    Printing, // the path is the printer's name
    Commit,   // the path is the file being replaced
};

// Shared by a tab and its job. The job reports from its thread, and the tab reads and
// stops it from the UI thread. Each call holds the state's mutex only to read or
// change a few fields, so none of them waits for the job.
class State
{
public:
    ~State();
    // What the job was doing when stop() was called.
    enum class Stop
    {
        Done,       // its scope had closed, so it had run to the end
        Stopped,    // it gives up at its next step and commits nothing
        Holding,    // it holds the shown document until the read it waits for returns
        Committing, // a commit had begun, and it completes
    };

    // Stops the job, from any thread. Never waits. A later call answers again from
    // what the job is doing then.
    Stop stop();
    bool stopped() const { return stopped_.load(); }

    // The job is about to replace a file. False once stopped, and then it must not.
    // Otherwise stop() answers Committing until endCommit().
    bool beginCommit();
    // The replace returned.
    void endCommit();

    void report(Step step, const QString &path);
    // A piece written or a page printed.
    void progressed() { ticks_.fetch_add(1, std::memory_order_relaxed); }
    // The job took or released the shown document (Document::Hold).
    void setHolding(bool on);
    // Reserve document use before taking Document::Hold. Stop and this reservation
    // share the mutex, so a stopped job cannot begin using the restored viewer.
    bool beginHolding();

    struct Snapshot
    {
        Step step = Step::None;
        QString path;
        quint64 ticks = 0; // progressed() calls so far
        bool holding = false;
        bool stopped = false;
    };
    // For the tab's poll.
    Snapshot snapshot() const;
    // Atomically reserves output keys against tabs admitted in this process. Keys
    // were resolved on the worker. Stop releases them unless a commit already began.
    bool reserveDestinations(const QSet<QString> &keys);
    // In-place Save may keep its own admission. The captured identity must still
    // be the sole admission of this key when the worker reserves it.
    bool reserveOwnedDestination(const QString &key, ClaimId owner);
    // Transfers a sole reservation into an admission before the scope releases
    // it. The original captured admission must still identify its owner.
    std::unique_ptr<OpenPath> admitReservedDestination(const QString &key,
                                                     const QString &ownerKey, ClaimId owner);

private:
    friend class Scope;

    mutable std::mutex mutex_;
    Step step_ = Step::None;          // guarded by mutex_
    QString path_;                    // guarded by mutex_
    bool committing_ = false;         // guarded by mutex_
    bool holding_ = false;            // guarded by mutex_
    int scopes_ = 0;                  // guarded by mutex_: scopes open on this state
    bool ended_ = false;              // guarded by mutex_: the last of them closed
    QSet<QString> destinations_;      // guarded by mutex_: owned output reservations
    std::atomic<bool> stopped_{false}; // set under mutex_, read without it
    std::atomic<quint64> ticks_{0};
};

// Makes `state` the current job's state on this thread until the scope ends. The
// state counts as Done once its last scope ended. It must outlive the scope. Scopes
// nest, and the previous state is current again when an inner one ends.
class Scope
{
public:
    explicit Scope(State &state);
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    State &state_;
    State *previous_;
};

// The current job was stopped. False outside a scope.
bool stopped();

// A tab's admission, keyed by its known path without filesystem work. Admission
// and output reservations use the same short memory-only critical section.
class OpenPath
{
public:
    // Adds a tab admission unless output ownership already blocks this key.
    explicit OpenPath(const QString &key);
    // Releases this tab identity without changing other admissions of the key.
    ~OpenPath();
    OpenPath(const OpenPath &) = delete;
    OpenPath &operator=(const OpenPath &) = delete;
    bool admitted() const { return admitted_; }
    const QString &key() const { return key_; }
    // Returns the stable tab admission identity used to authorize in-place Save.
    ClaimId identity() const { return identity_; }

private:
    friend class State;
    // Adopts an admission already transferred from the current job reservation.
    OpenPath(const QString &key, ClaimId identity);
    QString key_;
    bool admitted_ = false;
    ClaimId identity_ = 0;
};

// Reserves every output before the current job can write any of them. False when
// stopped or when a tab already owns one of these keys. Reservations last through
// commit, and otherwise release on Stop or when the last Scope closes.
bool reserveDestinations(const QSet<QString> &keys);
// Reserves in-place output for the sole captured tab owner in the current scope.
bool reserveOwnedDestination(const QString &key, ClaimId owner);
// Memory-only query for a key already normalized by the caller's worker.
bool destinationInUse(const QString &key);

// Reserve live document use for the current job. False once stopped. Release the
// Document::Hold before endHolding(), so Stop keeps the still picture until then.
bool beginHolding();
void endHolding();

// Report a step of the current job, before it starts. Each returns whether to go on,
// which is false once the job stopped or when a test made the step fail
// (testing::failWhen).
bool reading(const QString &file);
bool writing(const QString &folder);
bool printing(const QString &printer);

// Reports a step that goes on after the thread reported another one, as when qpdf
// reads its source between the pieces it writes. Does nothing while `step` and `path`
// are still the last report on this thread, so a loop may call it for every piece.
// The test hooks apply as to the reports above. It cannot fail, so the caller checks
// stopped() itself.
void switchTo(Step step, const QString &path);

// Asked right before the commit that replaces `destination`. The commit syncs the new
// file to disk and then renames it, so a stop during the sync answers Committing too.
// False once the job stopped, or when a test made the commit fail, and then nothing
// may be replaced. Otherwise the commit goes ahead and endCommit() follows once it
// returned.
bool beginCommit(const QString &destination);
// Submits a printer's completed pages with the same Stop/commit ordering while
// retaining Printing as the reported step. Follow with endCommit after it returns.
bool beginPrintingCommit(const QString &printer);
void endCommit();

// Counts progress for the current job, which keeps its tab from saying that the disk
// is not answering.
void progressed();

// The error of a step that did not run because its job stopped or a test made it
// fail. A stopped job's result is dropped, so nobody reads it and it is not
// translated.
QString notRunError();

// Orders the writes of one destination file in the process, so a writer that was
// stopped while stuck cannot replace the file after a newer writer did. A writer takes
// it before it opens the file for writing and keeps it until the replace returned.
class DestinationLock
{
public:
    // Reports writing to the folder of `path`, then takes the lock for that file. The
    // key is fileKey(), the canonical folder or file normalized for comparison, which
    // reads the disk on the calling thread. While another writer holds the file, this
    // waits in short steps and gives up once the current job stops. A stopped job
    // never takes a lock. Outside a scope, as on the GUI thread, it waits until the
    // lock is free, so the GUI thread must not write a file that a job may still hold.
    explicit DestinationLock(const QString &path);
    // Releases the lock.
    ~DestinationLock();
    DestinationLock(const DestinationLock &) = delete;
    DestinationLock &operator=(const DestinationLock &) = delete;

    // False only when the job stopped before this took the lock.
    bool owns() const { return owns_; }

private:
    QString key_;
    bool owns_ = false;
};

// For tests only. The hooks act inside and outside a scope. A hold blocks any thread
// but the GUI thread, which it would freeze. The functions given here run under the
// hooks' own lock and must not call them.
namespace testing {
// Blocks each step whose report `match` accepts until release(), as on a disk that
// stopped answering. A commit is held before it begins, so a stop meanwhile refuses it.
// DestinationLock's own report of the folder skips the hooks, so a writer held at its
// Writing step already owns its destination.
void holdWhen(std::function<bool(Step, const QString &)> match);
// Blocks each commit whose destination `match` accepts once it has begun, as in a
// rename on a disk that stopped answering, so a stop meanwhile answers Committing.
void holdCommitWhen(std::function<bool(const QString &)> match);
// The steps that the holds block right now.
int held();
// Lets every held step go, and holds nothing more until the next holdWhen() or
// holdCommitWhen().
void release();
// Makes each step whose report `match` accepts fail, until called with an empty
// function.
void failWhen(std::function<bool(Step, const QString &)> match);
// Steps reported on the GUI thread since the process started, in a scope or not.
// A job body that runs there by mistake shows up here.
int stepsOnMainThread();
} // namespace testing

} // namespace mervin::job
