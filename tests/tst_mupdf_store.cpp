#include <mupdf/fitz.h>

#include <QTest>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// Mervin builds MuPDF with the patches in scripts/mupdf-patches (see "MuPDF patches"
// in docs/design.md). Each test here fails against unpatched MuPDF 1.28.5, so a build
// that lost a patch, or a MuPDF update that needs it reworked, shows up in CI.
class TstMupdfStore : public QObject
{
    Q_OBJECT

private slots:
    void foundValueSurvivesEviction_data();
    void foundValueSurvivesEviction();
};

namespace {

// Bounds each wait of the thread handshake below, so a broken handshake fails the
// test instead of hanging it. A passing run never waits this long.
constexpr std::chrono::seconds kGiveUp{30};

// A MuPDF allocator that fills freed blocks with 0xA5 and frees them only when it is
// destroyed. A pointer read from a freed store item then comes back as
// 0xA5A5A5A5A5A5A5A5, whatever else has been allocated since.
class PoisoningAllocator
{
public:
    PoisoningAllocator() = default;
    PoisoningAllocator(const PoisoningAllocator &) = delete;
    PoisoningAllocator &operator=(const PoisoningAllocator &) = delete;
    ~PoisoningAllocator()
    {
        for (void *block : freed_)
            std::free(block);
    }

    fz_alloc_context callbacks() { return {this, &allocate, &reallocate, &release}; }

    /// The number of blocks MuPDF has freed so far.
    size_t freedCount()
    {
        std::lock_guard lock(mutex_);
        return freed_.size();
    }

private:
    // Each block starts with its payload size. 16 bytes keep the payload aligned as
    // malloc's result is.
    static constexpr size_t kHeader = 16;

    static void *allocate(void *, size_t size)
    {
        auto *block = static_cast<unsigned char *>(std::malloc(kHeader + size));
        if (!block)
            return nullptr;
        std::memcpy(block, &size, sizeof size);
        return block + kHeader;
    }

    static void *reallocate(void *self, void *old, size_t size)
    {
        if (!old)
            return allocate(self, size);
        auto *block = static_cast<unsigned char *>(std::realloc(
            static_cast<unsigned char *>(old) - kHeader, kHeader + size));
        if (!block)
            return nullptr;
        std::memcpy(block, &size, sizeof size);
        return block + kHeader;
    }

    static void release(void *self, void *payload)
    {
        if (!payload)
            return;
        unsigned char *block = static_cast<unsigned char *>(payload) - kHeader;
        size_t size = 0;
        std::memcpy(&size, block, sizeof size);
        std::memset(payload, 0xA5, size);
        auto *allocator = static_cast<PoisoningAllocator *>(self);
        std::lock_guard lock(allocator->mutex_);
        allocator->freed_.push_back(block);
    }

    std::mutex mutex_;
    std::vector<void *> freed_;
};

// MuPDF lock callbacks that stop one thread in a critical window of the store.
// An armed thread stops right after it releases FZ_LOCK_ALLOC holding a reference to
// the watched value it has just found. Stock MuPDF reads the found fz_item again after
// that unlock (fz_find_item, and fz_store_item's duplicate branch). A thread can also
// stop itself with park(), for example from a drop function that the store calls with
// the lock released. Either thread stays there until the test calls release().
class StoreWindow
{
public:
    fz_locks_context callbacks() { return {this, &lockCallback, &unlockCallback}; }

    /// Stops the calling thread at the window once `watched` has `refs` references.
    void arm(const fz_storable *watched, int refs)
    {
        std::lock_guard lock(mutex_);
        armed_ = std::this_thread::get_id();
        watched_ = watched;
        refs_ = refs;
    }

    /// Stops the calling thread in the window until release().
    void park()
    {
        std::unique_lock guard(mutex_);
        park(guard);
    }

    /// Records that the stopped thread's store call returned.
    void finished()
    {
        std::lock_guard lock(mutex_);
        armed_ = {};
        finished_ = true;
        changed_.notify_all();
    }

    /// Waits until the thread is in the window (true) or finished without reaching
    /// it (false).
    bool waitForWindow()
    {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, kGiveUp, [this] { return inWindow_ || finished_; });
        return inWindow_;
    }

    /// Lets the stopped thread continue its store call.
    void release()
    {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

private:
    void park(std::unique_lock<std::mutex> &guard)
    {
        inWindow_ = true;
        changed_.notify_all();
        changed_.wait_for(guard, kGiveUp, [this] { return released_; });
    }

    static void lockCallback(void *self, int lock)
    {
        static_cast<StoreWindow *>(self)->locks_[lock].lock();
    }

    static void unlockCallback(void *user, int lock)
    {
        auto *self = static_cast<StoreWindow *>(user);
        self->locks_[lock].unlock();
        if (lock != FZ_LOCK_ALLOC)
            return;
        std::unique_lock guard(self->mutex_);
        // The armed thread takes its reference under FZ_LOCK_ALLOC, so the count
        // reaches `refs_` only at the unlock that follows the lookup. Earlier unlocks,
        // such as the allocation at the start of fz_store_item, pass.
        if (self->armed_ != std::this_thread::get_id() || self->watched_->refs != self->refs_)
            return;
        self->armed_ = {};
        self->park(guard);
    }

    std::mutex locks_[FZ_LOCK_MAX];
    std::mutex mutex_;
    std::condition_variable changed_;
    std::thread::id armed_;
    const fz_storable *watched_ = nullptr;
    int refs_ = 0;
    bool inWindow_ = false;
    bool finished_ = false;
    bool released_ = false;
};

struct TestValue
{
    fz_storable storable;
};

struct TestKey
{
    int id;
};

void dropValue(fz_context *ctx, fz_storable *value)
{
    fz_free(ctx, value);
}

int hashKey(fz_context *, fz_store_hash *hash, void *key)
{
    hash->u.pi.i = static_cast<TestKey *>(key)->id;
    return 1;
}

void *keepKey(fz_context *, void *key)
{
    return key;
}

void dropKey(fz_context *, void *)
{
}

int compareKeys(fz_context *, void *a, void *b)
{
    return static_cast<TestKey *>(a)->id != static_cast<TestKey *>(b)->id;
}

void formatKey(fz_context *, char *buffer, size_t size, void *key)
{
    std::snprintf(buffer, size, "(test key %d)", static_cast<TestKey *>(key)->id);
}

int isKey(fz_context *, void *wanted, void *key)
{
    return key == wanted;
}

// Hashed keys take fz_find_item's hash table path, as pattern tiles do. Unhashed keys
// take its scan of the whole store, as direct PDF object keys do.
const fz_store_type hashedType = {"mervin test hashed", &hashKey, &keepKey, &dropKey,
                                  &compareKeys, &formatKey, nullptr};
const fz_store_type scannedType = {"mervin test scanned", nullptr, &keepKey, &dropKey,
                                   &compareKeys, &formatKey, nullptr};

} // namespace

void TstMupdfStore::foundValueSurvivesEviction_data()
{
    QTest::addColumn<bool>("hashed");
    QTest::addColumn<bool>("duplicate");
    QTest::newRow("lookup by hash") << true << false;
    QTest::newRow("lookup by scan") << false << false;
    QTest::newRow("duplicate store") << true << true;
}

// A store lookup returns the value it found, with a reference taken, even when another
// thread frees the store entry right after the lookup released the store lock. MuPDF
// frees entries that way when a document closes (fz_filter_store), while render
// workers look up the document's pattern tiles. Unpatched MuPDF reads the value from
// the freed entry and returns 0xA5A5A5A5A5A5A5A5 here.
void TstMupdfStore::foundValueSurvivesEviction()
{
    QFETCH(bool, hashed);
    QFETCH(bool, duplicate);
    const fz_store_type *type = hashed ? &hashedType : &scannedType;

    // Both outlive the contexts that use them.
    PoisoningAllocator allocator;
    StoreWindow window;
    const fz_alloc_context alloc = allocator.callbacks();
    const fz_locks_context locks = window.callbacks();
    fz_context *ctx = fz_new_context(&alloc, &locks, FZ_STORE_DEFAULT);
    QVERIFY(ctx);
    fz_context *workerCtx = fz_clone_context(ctx);
    QVERIFY(workerCtx);
    // The duplicate branch warns about the duplicate.
    fz_set_warning_callback(workerCtx, nullptr, nullptr);

    auto *value = static_cast<TestValue *>(fz_calloc(ctx, 1, sizeof(TestValue)));
    FZ_INIT_STORABLE(value, 1, &dropValue);
    TestKey key{7};
    // The store takes its own reference.
    QVERIFY(!fz_store_item(ctx, &key, value, sizeof(TestValue), type));

    // The duplicate case stores a second value under an equal key, which finds the
    // first entry instead.
    auto *second = static_cast<TestValue *>(fz_calloc(ctx, 1, sizeof(TestValue)));
    FZ_INIT_STORABLE(second, 1, &dropValue);
    TestKey equalKey{7};

    void *found = nullptr;
    std::thread worker([&] {
        // Three references: ours, the store's and the one the lookup takes.
        window.arm(&value->storable, 3);
        found = duplicate ? fz_store_item(workerCtx, &equalKey, second, sizeof(TestValue), type)
                          : fz_find_item(workerCtx, &dropValue, &equalKey, type);
        window.finished();
    });

    const bool reachedWindow = window.waitForWindow();
    bool evicted = false;
    if (reachedWindow) {
        // Free the entries as closing a document does, whatever their reference count.
        const size_t freedBefore = allocator.freedCount();
        fz_filter_store(ctx, &isKey, &key, type);
        evicted = allocator.freedCount() > freedBefore;
        if (void *left = fz_find_item(ctx, &dropValue, &key, type)) {
            evicted = false;
            fz_drop_storable(ctx, static_cast<fz_storable *>(left));
        }
    }
    window.release();
    worker.join();

    // The lookup took its reference under the lock in either build, so ours and the
    // lookup's are left.
    const int refsLeft = value->storable.refs;
    if (refsLeft == 2)
        fz_drop_storable(ctx, &value->storable);
    fz_drop_storable(ctx, &value->storable);
    fz_drop_storable(ctx, &second->storable);
    fz_drop_context(workerCtx);
    fz_drop_context(ctx);

    QVERIFY2(reachedWindow, "the store call returned without unlocking after its lookup");
    QVERIFY2(evicted, "fz_filter_store did not free the entry");
    QCOMPARE(found, static_cast<void *>(value));
    QCOMPARE(refsLeft, 2);
}

QTEST_GUILESS_MAIN(TstMupdfStore)
#include "tst_mupdf_store.moc"
