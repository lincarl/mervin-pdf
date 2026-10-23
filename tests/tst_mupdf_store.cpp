#include "render/Document.h"
#include "render/RenderEngine.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QImage>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

using namespace mervin;

// Mervin builds MuPDF with the patches in scripts/mupdf-patches (see "MuPDF patches"
// in docs/design.md). Each test here fails or crashes against unpatched MuPDF 1.28.5, so
// a build that lost a patch, or a MuPDF update that needs it reworked, shows up in CI.
class TstMupdfStore : public QObject
{
    Q_OBJECT

private slots:
    void foundValueSurvivesEviction_data();
    void foundValueSurvivesEviction();
    void documentsKeepTheirOwnInlineColourSpace_data();
    void documentsKeepTheirOwnInlineColourSpace();
    void documentsKeepTheirOwnInlineDictionaryKeys();
    void removedEntriesReleaseTheirBudget_data();
    void removedEntriesReleaseTheirBudget();
    // Last, because unpatched MuPDF crashes in it, which would hide later results.
    void newEntryFreedWhileStoreEvicts();
    void newEntryFreedWhileStoreReaps();
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

    /// The number of freed blocks that no longer hold only 0xA5, that is, blocks
    /// something wrote to after MuPDF freed them.
    size_t writtenAfterFree()
    {
        std::lock_guard lock(mutex_);
        size_t written = 0;
        for (void *block : freed_) {
            size_t size = 0;
            std::memcpy(&size, block, sizeof size);
            const auto *payload = static_cast<const unsigned char *>(block) + kHeader;
            if (std::any_of(payload, payload + size, [](unsigned char b) { return b != 0xA5; }))
                ++written;
        }
        return written;
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

    /// Whether the stopped thread stopped waiting for release() after kGiveUp. Its
    /// store call then went on before the test acted, so the test proved nothing.
    bool gaveUp()
    {
        std::lock_guard lock(mutex_);
        return gaveUp_;
    }

private:
    void park(std::unique_lock<std::mutex> &guard)
    {
        inWindow_ = true;
        changed_.notify_all();
        if (!changed_.wait_for(guard, kGiveUp, [this] { return released_; }))
            gaveUp_ = true;
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
    bool gaveUp_ = false;
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

// A value whose drop function stops the dropping thread in the window, so the test
// can act while the store frees the value with its lock released.
struct ParkingValue
{
    fz_storable storable;
    StoreWindow *window;
};

void dropParkingValue(fz_context *ctx, fz_storable *value)
{
    reinterpret_cast<ParkingValue *>(value)->window->park();
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

// The store's reference to this key is the last reference to its owner. Dropping
// the external owner reference while reaping is deferred forces the do_reap path.
struct ReapingKey
{
    int id;
    fz_key_storable *owner;
};

const fz_store_type reapingType = {
    "mervin test reap",
    [](fz_context *, fz_store_hash *hash, void *key) {
        hash->u.pi.i = static_cast<ReapingKey *>(key)->id;
        return 1;
    },
    [](fz_context *ctx, void *key) -> void * {
        if (auto *owner = static_cast<ReapingKey *>(key)->owner)
            fz_keep_key_storable_key(ctx, owner);
        return key;
    },
    [](fz_context *ctx, void *key) {
        if (auto *owner = static_cast<ReapingKey *>(key)->owner)
            fz_drop_key_storable_key(ctx, owner);
    },
    [](fz_context *, void *a, void *b) {
        return int(static_cast<ReapingKey *>(a)->id != static_cast<ReapingKey *>(b)->id);
    },
    [](fz_context *, char *buffer, size_t size, void *) { std::snprintf(buffer, size, "reap key"); },
    [](fz_context *, void *key) {
        const auto *owner = static_cast<ReapingKey *>(key)->owner;
        return int(owner && owner->storable.refs == owner->store_key_refs);
    },
};

QByteArray assemblePdf(const QList<QByteArray> &bodies)
{
    QByteArray pdf = "%PDF-1.7\n";
    QList<int> offsets;
    for (int i = 0; i < bodies.size(); ++i) {
        offsets << pdf.size();
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + bodies[i] + "\nendobj\n";
    }
    const int xrefOff = pdf.size();
    const int n = bodies.size() + 1;
    pdf += "xref\n0 " + QByteArray::number(n) + "\n";
    pdf += "0000000000 65535 f \n";
    for (int off : offsets)
        pdf += QByteArray::number(off).rightJustified(10, '0') + " 00000 n \n";
    pdf += "trailer\n<< /Size " + QByteArray::number(n) + " /Root 1 0 R >>\nstartxref\n"
           + QByteArray::number(xrefOff) + "\n%%EOF\n";
    return pdf;
}

QByteArray stream(const QByteArray &data)
{
    return "<< /Length " + QByteArray::number(data.size()) + " >>\nstream\n" + data
           + "\nendstream";
}

// A page filled with index 1 of the inline colour space [/Indexed /DeviceRGB 1 5 0 R].
// Object 5 holds the two-entry palette: black, then `colour`.
QByteArray indexedColourPdf(QColor colour)
{
    QByteArray palette(3, '\0');
    palette.append(static_cast<char>(colour.red()));
    palette.append(static_cast<char>(colour.green()));
    palette.append(static_cast<char>(colour.blue()));
    return assemblePdf({
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] "
        "/Resources << /ColorSpace << /CS0 [/Indexed /DeviceRGB 1 5 0 R] >> >> "
        "/Contents 4 0 R >>",
        stream("/CS0 cs 1 sc 0 0 100 100 re f"),
        stream(palette),
    });
}

// A page filled with the inline colour space
// [/Separation /Spot /DeviceRGB << /FunctionType 2 ... >>]. Its tint transform is a
// direct dictionary, so MuPDF stores the colour space under a key that holds a
// dictionary of this document and no indirect reference.
QByteArray separationPdf()
{
    return assemblePdf({
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] "
        "/Resources << /ColorSpace << /CS0 [/Separation /Spot /DeviceRGB "
        "<< /FunctionType 2 /Domain [0 1] /C0 [1 1 1] /C1 [1 0 0] /N 1 >>] >> >> "
        "/Contents 4 0 R >>",
        stream("/CS0 cs 1 sc 0 0 100 100 re f"),
    });
}

// The colour at the centre of `document`'s first page rendered at 100%, or an invalid
// colour when it does not render.
QColor centreColour(const Document &document)
{
    const QImage image = document.renderPage(0, 1.0, 0);
    if (image.isNull())
        return {};
    return image.pixelColor(image.width() / 2, image.height() / 2);
}

bool sameColour(const QColor &a, const QColor &b)
{
    return a.isValid() && b.isValid() && std::abs(a.red() - b.red()) <= 8
           && std::abs(a.green() - b.green()) <= 8 && std::abs(a.blue() - b.blue()) <= 8;
}

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
    QVERIFY2(!window.gaveUp(), "the stopped thread went on before the test acted");
    QVERIFY2(evicted, "fz_filter_store did not free the entry");
    QCOMPARE(found, static_cast<void *>(value));
    QCOMPARE(refsLeft, 2);
}

// A store that has to make room leaves its new entry alone once another thread can
// reach it. fz_store_item frees the entries it evicts with the store lock released.
// Unpatched MuPDF has put the new entry into the hash table by then, but not into the
// store's list, so another thread can find it, link it and free it, as closing its
// document does, and the storing thread then links freed memory. The test stops the
// storing thread while it frees the evicted value. If another thread can find the new
// entry then, as patch 0003 allows, the test frees it the way closing a document does.
// The storing thread must not touch it afterwards. Unpatched MuPDF crashes here.
void TstMupdfStore::newEntryFreedWhileStoreEvicts()
{
    // Both outlive the contexts that use them.
    PoisoningAllocator allocator;
    StoreWindow window;
    const fz_alloc_context alloc = allocator.callbacks();
    const fz_locks_context locks = window.callbacks();
    // The store holds one old entry of this size and nothing more.
    constexpr size_t kFull = 1000;
    fz_context *ctx = fz_new_context(&alloc, &locks, kFull);
    QVERIFY(ctx);
    fz_context *workerCtx = fz_clone_context(ctx);
    QVERIFY(workerCtx);

    // Only the store holds the old value, so the next store evicts it.
    auto *old = static_cast<ParkingValue *>(fz_calloc(ctx, 1, sizeof(ParkingValue)));
    FZ_INIT_STORABLE(old, 1, &dropParkingValue);
    old->window = &window;
    TestKey oldKey{1};
    QVERIFY(!fz_store_item(ctx, &oldKey, old, kFull, &hashedType));
    fz_drop_storable(ctx, &old->storable);

    auto *value = static_cast<TestValue *>(fz_calloc(ctx, 1, sizeof(TestValue)));
    FZ_INIT_STORABLE(value, 1, &dropValue);
    TestKey key{2};
    void *existing = nullptr;
    std::thread worker([&] {
        existing = fz_store_item(workerCtx, &key, value, sizeof(TestValue), &hashedType);
        window.finished();
    });

    const bool reachedWindow = window.waitForWindow();
    if (reachedWindow) {
        // Looks the entry up as a render worker would. In unpatched MuPDF this links
        // it into the store's list.
        if (void *early = fz_find_item(ctx, &dropValue, &key, &hashedType))
            fz_drop_storable(ctx, static_cast<fz_storable *>(early));
        // Frees the new entry if it is in the store, whatever its references.
        fz_filter_store(ctx, &isKey, &key, &hashedType);
    }
    window.release();
    worker.join();

    void *stored = fz_find_item(ctx, &dropValue, &key, &hashedType);
    if (stored)
        fz_drop_storable(ctx, static_cast<fz_storable *>(stored));
    const int refsLeft = value->storable.refs;
    const size_t written = allocator.writtenAfterFree();
    fz_drop_storable(ctx, &value->storable);
    fz_drop_context(workerCtx);
    fz_drop_context(ctx);

    QVERIFY2(reachedWindow, "the store did not evict the old entry");
    QVERIFY2(!window.gaveUp(), "the stopped thread went on before the test acted");
    QVERIFY(!existing);
    QCOMPARE(written, size_t(0));
    // Patch 0003 lets the filter free a visible entry in the window. A fix that hides
    // the entry until it is linked stores it after the window instead, and the store
    // then keeps its reference beside ours.
    QVERIFY(!stored || stored == value);
    QCOMPARE(refsLeft, stored ? 2 : 1);
}

// Reaping unlocks through a different branch than size eviction. Keep a caller
// reference to the old value, so size eviction cannot drop it. Only do_reap can
// remove its store entry; its key's final drop parks the worker in that window.
void TstMupdfStore::newEntryFreedWhileStoreReaps()
{
    PoisoningAllocator allocator;
    StoreWindow window;
    const auto alloc = allocator.callbacks();
    const auto locks = window.callbacks();
    constexpr size_t full = 1000;
    fz_context *ctx = fz_new_context(&alloc, &locks, full);
    QVERIFY(ctx);
    fz_context *workerCtx = fz_clone_context(ctx);
    QVERIFY(workerCtx);
    struct Owner { fz_key_storable key_storable; StoreWindow *window; };
    auto *owner = static_cast<Owner *>(fz_calloc(ctx, 1, sizeof(Owner)));
    owner->window = &window;
    FZ_INIT_KEY_STORABLE(owner, 1, +[](fz_context *context, fz_storable *value) {
        reinterpret_cast<Owner *>(value)->window->park();
        fz_free(context, value);
    });
    ReapingKey oldKey{1, &owner->key_storable};
    auto *old = static_cast<TestValue *>(fz_calloc(ctx, 1, sizeof(TestValue)));
    FZ_INIT_STORABLE(old, 1, &dropValue);
    QVERIFY(!fz_store_item(ctx, &oldKey, old, full, &reapingType));
    fz_defer_reap_start(ctx);
    fz_drop_key_storable(ctx, &owner->key_storable);

    ReapingKey key{2, nullptr};
    auto *value = static_cast<TestValue *>(fz_calloc(ctx, 1, sizeof(TestValue)));
    FZ_INIT_STORABLE(value, 1, &dropValue);
    void *existing = nullptr;
    std::thread worker([&] {
        existing = fz_store_item(workerCtx, &key, value, sizeof(TestValue), &reapingType);
        window.finished();
    });
    const bool reached = window.waitForWindow();
    if (reached) {
        if (void *early = fz_find_item(ctx, &dropValue, &key, &reapingType))
            fz_drop_storable(ctx, static_cast<fz_storable *>(early));
        fz_filter_store(ctx, &isKey, &key, &reapingType);
    }
    window.release();
    worker.join();
    fz_defer_reap_end(ctx);
    void *stored = fz_find_item(ctx, &dropValue, &key, &reapingType);
    if (stored)
        fz_drop_storable(ctx, static_cast<fz_storable *>(stored));
    const int refs = value->storable.refs;
    const int oldRefs = old->storable.refs;
    const size_t written = allocator.writtenAfterFree();
    fz_drop_storable(ctx, &old->storable);
    fz_drop_storable(ctx, &value->storable);
    fz_drop_context(workerCtx);
    fz_drop_context(ctx);

    QVERIFY2(reached, "the insertion never reached the deferred reap unlock window");
    QVERIFY(!window.gaveUp());
    QVERIFY(!existing);
    QCOMPARE(oldRefs, 1); // reaped despite the caller's held reference
    QCOMPARE(written, size_t(0));
    QVERIFY(!stored || stored == value);
    QCOMPARE(refs, stored ? 2 : 1);
}

void TstMupdfStore::removedEntriesReleaseTheirBudget_data()
{
    QTest::addColumn<bool>("hashed");
    QTest::newRow("hashed") << true;
    QTest::newRow("scanned") << false;
}

// Removing an entry returns its budget immediately. A later insertion which fits
// must keep its neighbor; real capacity pressure must still evict that neighbor.
void TstMupdfStore::removedEntriesReleaseTheirBudget()
{
    QFETCH(bool, hashed);
    const fz_store_type *type = hashed ? &hashedType : &scannedType;
    fz_context *ctx = fz_new_context(nullptr, nullptr, 1000);
    QVERIFY(ctx);
    TestKey keys[] = {{1}, {2}, {3}, {4}};
    const auto cleanup = qScopeGuard([&] { fz_drop_context(ctx); });
    const auto store = [&](int index, size_t size) {
        auto *value = static_cast<TestValue *>(fz_calloc(ctx, 1, sizeof(TestValue)));
        FZ_INIT_STORABLE(value, 1, &dropValue);
        fz_store_item(ctx, &keys[index], value, size, type);
        fz_drop_storable(ctx, &value->storable);
    };
    const auto contains = [&](int index) {
        auto *found = static_cast<fz_storable *>(fz_find_item(ctx, &dropValue, &keys[index], type));
        const bool exists = found != nullptr;
        fz_drop_storable(ctx, found);
        return exists;
    };
    store(0, 600);
    store(1, 300);
    QVERIFY(contains(0) && contains(1));
    fz_remove_item(ctx, &dropValue, &keys[0], type);
    fz_remove_item(ctx, &dropValue, &keys[0], type); // missing removal spends nothing
    QVERIFY(!contains(0));
    store(2, 600); // 300 + 600 fits the 1000-byte budget
    QVERIFY2(contains(1), "a removed entry's stale size evicted an unrelated live cache entry");
    QVERIFY(contains(2)); // promote it after the neighbor lookup
    store(3, 400); // 300 + 600 + 400 exceeds the actual budget
    QVERIFY(!contains(1));
    QVERIFY(contains(2) && contains(3));
}

void TstMupdfStore::documentsKeepTheirOwnInlineColourSpace_data()
{
    QTest::addColumn<QColor>("first");
    QTest::addColumn<QColor>("second");
    QTest::newRow("red after blue") << QColor(Qt::blue) << QColor(Qt::red);
    QTest::newRow("blue after red") << QColor(Qt::red) << QColor(Qt::blue);
}

// Two open documents that use the same inline colour space array each render with
// their own palette. MuPDF stores the colour space under the array, and unpatched
// MuPDF compares such keys without regard to their document, so the second document
// rendered with the first one's palette.
void TstMupdfStore::documentsKeepTheirOwnInlineColourSpace()
{
    QFETCH(QColor, first);
    QFETCH(QColor, second);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto write = [&dir](const QString &name, const QByteArray &bytes) {
        QFile file(dir.filePath(name));
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    QVERIFY(write(QStringLiteral("first.pdf"), indexedColourPdf(first)));
    QVERIFY(write(QStringLiteral("second.pdf"), indexedColourPdf(second)));

    RenderEngine engine;
    auto firstDocument = engine.openDocument(dir.filePath(QStringLiteral("first.pdf")));
    QVERIFY(firstDocument);
    const QColor firstShown = centreColour(*firstDocument);
    QVERIFY2(sameColour(firstShown, first), qPrintable(firstShown.name()));

    // The first document stays open, so its colour space stays in the shared store.
    auto secondDocument = engine.openDocument(dir.filePath(QStringLiteral("second.pdf")));
    QVERIFY(secondDocument);
    const QColor secondShown = centreColour(*secondDocument);
    QVERIFY2(sameColour(secondShown, second),
             qPrintable(QStringLiteral("%1, expected %2").arg(secondShown.name(), second.name())));
    QVERIFY(sameColour(centreColour(*firstDocument), first));
}

// Two open documents with equal inline colour spaces that hold a dictionary keep
// separate store entries. Unpatched MuPDF compares such keys without regard to their
// document, so the second document got the first one's entry, and comparing the
// dictionaries read the other document's xref, which that document's close frees
// before it removes its store entries. The test loads the colour space through the
// shared store: each document finds its own entry again, and the entries differ.
void TstMupdfStore::documentsKeepTheirOwnInlineDictionaryKeys()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray pdf = separationPdf();
    QByteArray paths[2];
    for (int i = 0; i < 2; ++i) {
        const QString path = dir.filePath(QStringLiteral("doc%1.pdf").arg(i));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly) && file.write(pdf) == pdf.size());
        paths[i] = QFile::encodeName(path);
    }

    fz_context *ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    QVERIFY(ctx);
    pdf_document *docs[2] = {};
    fz_colorspace *first = nullptr;
    fz_colorspace *firstAgain = nullptr;
    fz_colorspace *second = nullptr;
    bool loaded = false;
    bool foundAgain = false;
    bool shared = false;
    fz_var(docs);
    fz_var(first);
    fz_var(firstAgain);
    fz_var(second);
    fz_var(loaded);
    fz_var(foundAgain);
    fz_var(shared);
    // Both documents stay open, so the first one's entry stays in the shared store.
    fz_try(ctx)
    {
        for (int i = 0; i < 2; ++i)
            docs[i] = pdf_open_document(ctx, paths[i].constData());
        const auto inlineColourSpace = [ctx](pdf_document *doc) {
            return pdf_dict_getp(ctx, pdf_lookup_page_obj(ctx, doc, 0),
                                 "Resources/ColorSpace/CS0");
        };
        first = pdf_load_colorspace(ctx, inlineColourSpace(docs[0]));
        firstAgain = pdf_load_colorspace(ctx, inlineColourSpace(docs[0]));
        second = pdf_load_colorspace(ctx, inlineColourSpace(docs[1]));
        // Compared while all three are held, so no address can have been reused.
        foundAgain = firstAgain == first;
        shared = second == first;
        loaded = true;
    }
    fz_always(ctx)
    {
        fz_drop_colorspace(ctx, second);
        fz_drop_colorspace(ctx, firstAgain);
        fz_drop_colorspace(ctx, first);
        pdf_drop_document(ctx, docs[1]);
        pdf_drop_document(ctx, docs[0]);
    }
    fz_catch(ctx)
    {
        qWarning("MuPDF: %s", fz_caught_message(ctx));
    }
    fz_drop_context(ctx);

    QVERIFY2(loaded, "MuPDF could not load the colour spaces");
    QVERIFY2(foundAgain, "the store did not keep the first document's colour space");
    QVERIFY2(!shared, "the second document got the first document's entry");
}

QTEST_GUILESS_MAIN(TstMupdfStore)
#include "tst_mupdf_store.moc"
