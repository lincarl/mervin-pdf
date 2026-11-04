#include <QTest>

#include <mujs.h>

#include <cstdlib>
#include <memory>

namespace {

using State = std::unique_ptr<js_State, decltype(&js_freestate)>;

/// Fails one chosen allocation, then allows exception handling and cleanup.
struct Allocations
{
    int remaining = -1;
    bool failed = false;
};

void *allocate(void *context, void *pointer, int size)
{
    auto &state = *static_cast<Allocations *>(context);
    if (size == 0) {
        std::free(pointer);
        return nullptr;
    }
    if (state.remaining == 0) {
        state.remaining = -1;
        state.failed = true;
        return nullptr;
    }
    if (state.remaining > 0)
        --state.remaining;
    return std::realloc(pointer, static_cast<size_t>(size));
}

/// Counts collection of an array value after its last reference is removed.
void finalize(js_State *, void *value)
{
    ++*static_cast<int *>(value);
}

/// C-style frame so MuJS longjmp never crosses a C++ object's destructor.
bool defineProperty(js_State *state)
{
    // MuJS's C macro relies on an implicit void-pointer conversion.
    if (setjmp(*static_cast<jmp_buf *>(js_savetry(state)))) {
        js_pop(state, 1);
        return false;
    }
    js_pushnumber(state, 7);
    js_defproperty(state, 0, "defined", 0);
    js_endtry(state);
    return true;
}

} // namespace

/// Checks the patched engine without enabling JavaScript in PDF documents.
class TstMujs : public QObject
{
    Q_OBJECT
private slots:
    void longRegularExpression();
    void numericProperty();
    void failedArrayConversion_data();
    void failedArrayConversion();
};

/// Global matching must advance beyond the old 16-bit lastIndex field.
void TstMujs::longRegularExpression()
{
    State state(js_newstate(nullptr, nullptr, 0), js_freestate);
    QVERIFY(state);
    QCOMPARE(js_dostring(state.get(),
        "var text = Array(70001).join('a') + 'b'; var pattern = /b/g;"
        "if (!pattern.test(text) || pattern.lastIndex !== 70001) throw Error('index truncated');"
        "if (pattern.test(text) || pattern.lastIndex !== 0) throw Error('match did not end');"), 0);
}

/// Out-of-range numeric keys remain ordinary properties without an int cast.
void TstMujs::numericProperty()
{
    State state(js_newstate(nullptr, nullptr, 0), js_freestate);
    QVERIFY(state);
    QCOMPARE(js_dostring(state.get(),
        "var a = []; a[1e100] = 7; a[-1] = 8;"
        "if (a[1e100] !== 7 || a[-1] !== 8 || a.length !== 0) throw Error('bad property');"), 0);
}

void TstMujs::failedArrayConversion_data()
{
    QTest::addColumn<int>("allocation");
    for (int allocation = 0; allocation < 12; ++allocation)
        QTest::newRow(qPrintable(QString::number(allocation))) << allocation;
}

/// Failed flattening must roll back migrated values and retain other properties.
void TstMujs::failedArrayConversion()
{
    QFETCH(int, allocation);
    Allocations allocations;
    int finalized = 0;
    State state(js_newstate(allocate, &allocations, 0), js_freestate);
    QVERIFY(state);
    js_newarray(state.get());
    js_pushnull(state.get()); // Prototype for the userdata.
    js_newuserdata(state.get(), "probe", &finalized, finalize);
    js_setindex(state.get(), 0, 0);
    for (int index = 1; index < 16; ++index) {
        js_pushnumber(state.get(), index);
        js_setindex(state.get(), 0, index);
    }
    js_pushnumber(state.get(), 42);
    js_setproperty(state.get(), 0, "keep");
    allocations.remaining = allocation;
    QVERIFY(!defineProperty(state.get()));
    QVERIFY(allocations.failed);
    allocations.remaining = -1;
    js_getproperty(state.get(), 0, "keep");
    QCOMPARE(js_tonumber(state.get(), -1), 42.0);
    js_pop(state.get(), 1);
    js_pushundefined(state.get());
    js_setindex(state.get(), 0, 0);
    js_gc(state.get(), 0);
    QCOMPARE(finalized, 1);
    QVERIFY(defineProperty(state.get()));
    js_getindex(state.get(), 0, 15);
    QCOMPARE(js_tonumber(state.get(), -1), 15.0);
}

QTEST_GUILESS_MAIN(TstMujs)
#include "tst_mujs.moc"
