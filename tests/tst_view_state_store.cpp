#include "recent/ViewState.h"
#include "recent/ViewStateStore.h"

#include <QTemporaryDir>
#include <QTest>

#include <optional>

using mervin::ViewState;
using mervin::ViewStateStore;

class TstViewStateStore : public QObject
{
    Q_OBJECT

private slots:
    void putThenGet();
    void getMissingReturnsNullopt();
    void putReplacesExisting();
#ifdef Q_OS_WIN
    void keyIsCaseInsensitiveOnWindows();
#endif
    void retentionEvictsLeastRecentlyUpdated();
    void keptStatesAreNeitherEvictedNorCounted();
    void loadKeepsKeptStates();
    void removeDeletesEntry();
    void jsonRoundTrip();
};

static ViewState makeState(int page, const QString &zoom, double scale, int rot,
                           double ox = 0.0, double oy = 0.0)
{
    ViewState s;
    s.page = page;
    s.zoomMode = zoom;
    s.scale = scale;
    s.rotation = rot;
    s.offsetX = ox;
    s.offsetY = oy;
    return s;
}

void TstViewStateStore::putThenGet()
{
    ViewStateStore s;
    s.put(QStringLiteral("C:/a.pdf"), makeState(7, QStringLiteral("custom"), 1.5, 90), 100);
    const auto got = s.get(QStringLiteral("C:/a.pdf"));
    QVERIFY(got.has_value());
    QCOMPARE(got->page, 7);
    QCOMPARE(got->zoomMode, QStringLiteral("custom"));
    QCOMPARE(got->scale, 1.5);
    QCOMPARE(got->rotation, 90);
}

void TstViewStateStore::getMissingReturnsNullopt()
{
    ViewStateStore s;
    QVERIFY(!s.get(QStringLiteral("C:/none.pdf")).has_value());
}

void TstViewStateStore::putReplacesExisting()
{
    ViewStateStore s;
    s.put(QStringLiteral("C:/a.pdf"), makeState(1, QStringLiteral("fit-width"), 1.0, 0), 100);
    s.put(QStringLiteral("C:/a.pdf"), makeState(9, QStringLiteral("fit-page"), 1.0, 270), 200);
    QCOMPARE(s.count(), 1);
    const auto got = s.get(QStringLiteral("C:/a.pdf"));
    QVERIFY(got.has_value());
    QCOMPARE(got->page, 9);
    QCOMPARE(got->rotation, 270);
}

#ifdef Q_OS_WIN
void TstViewStateStore::keyIsCaseInsensitiveOnWindows()
{
    ViewStateStore s;
    s.put(QStringLiteral("C:/Docs/Report.pdf"), makeState(5, QStringLiteral("fit-width"), 1.0, 0), 100);
    const auto got = s.get(QStringLiteral("c:\\docs\\report.pdf"));
    QVERIFY(got.has_value());
    QCOMPARE(got->page, 5);
}
#endif

void TstViewStateStore::retentionEvictsLeastRecentlyUpdated()
{
    ViewStateStore s(2);
    s.put(QStringLiteral("C:/a.pdf"), makeState(1, QStringLiteral("fit-width"), 1.0, 0), 100);
    s.put(QStringLiteral("C:/b.pdf"), makeState(2, QStringLiteral("fit-width"), 1.0, 0), 200);
    s.put(QStringLiteral("C:/c.pdf"), makeState(3, QStringLiteral("fit-width"), 1.0, 0), 300);
    QCOMPARE(s.count(), 2);
    QVERIFY(!s.get(QStringLiteral("C:/a.pdf")).has_value()); // oldest update evicted
    QVERIFY(s.get(QStringLiteral("C:/b.pdf")).has_value());
    QVERIFY(s.get(QStringLiteral("C:/c.pdf")).has_value());
}

// The Recent favourites keep their view state however old it is, and the cap
// applies to the other files alone.
void TstViewStateStore::keptStatesAreNeitherEvictedNorCounted()
{
    ViewStateStore s(2);
    s.setKept({QStringLiteral("C:/./star.pdf")}); // any spelling of the path
    s.put(QStringLiteral("C:/star.pdf"), makeState(1, QStringLiteral("fit-width"), 1.0, 0), 100);
    s.put(QStringLiteral("C:/a.pdf"), makeState(2, QStringLiteral("fit-width"), 1.0, 0), 200);
    s.put(QStringLiteral("C:/b.pdf"), makeState(3, QStringLiteral("fit-width"), 1.0, 0), 300);
    QCOMPARE(s.count(), 3);
    s.put(QStringLiteral("C:/c.pdf"), makeState(4, QStringLiteral("fit-width"), 1.0, 0), 400);
    QCOMPARE(s.count(), 3);
    QVERIFY(s.get(QStringLiteral("C:/star.pdf")).has_value());
    QVERIFY(!s.get(QStringLiteral("C:/a.pdf")).has_value()); // the oldest of the others
    s.setRetention(1);
    QCOMPARE(s.count(), 2);
    QVERIFY(s.get(QStringLiteral("C:/star.pdf")).has_value());
    QVERIFY(s.get(QStringLiteral("C:/c.pdf")).has_value());

    // No longer kept, it counts again from the next prune, and is the oldest.
    s.setKept({});
    QCOMPARE(s.count(), 2);
    s.put(QStringLiteral("C:/d.pdf"), makeState(5, QStringLiteral("fit-width"), 1.0, 0), 500);
    QCOMPARE(s.count(), 1);
    QVERIFY(s.get(QStringLiteral("C:/d.pdf")).has_value());
}

void TstViewStateStore::loadKeepsKeptStates()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("viewstate.json"));
    ViewStateStore a;
    a.put(QStringLiteral("C:/star.pdf"), makeState(1, QStringLiteral("fit-width"), 1.0, 0), 100);
    a.put(QStringLiteral("C:/a.pdf"), makeState(2, QStringLiteral("fit-width"), 1.0, 0), 200);
    a.put(QStringLiteral("C:/b.pdf"), makeState(3, QStringLiteral("fit-width"), 1.0, 0), 300);
    QVERIFY(a.save(file));

    ViewStateStore b(1);
    b.setKept({QStringLiteral("C:/star.pdf")});
    QVERIFY(b.load(file));
    QCOMPARE(b.count(), 2);
    QVERIFY(b.get(QStringLiteral("C:/star.pdf")).has_value());
    QVERIFY(b.get(QStringLiteral("C:/b.pdf")).has_value());
}

void TstViewStateStore::removeDeletesEntry()
{
    ViewStateStore s;
    s.put(QStringLiteral("C:/a.pdf"), makeState(1, QStringLiteral("fit-width"), 1.0, 0), 100);
    QVERIFY(s.remove(QStringLiteral("C:/a.pdf")));
    QCOMPARE(s.count(), 0);
    QVERIFY(!s.remove(QStringLiteral("C:/a.pdf")));
}

void TstViewStateStore::jsonRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("viewstate.json"));

    ViewStateStore a;
    a.put(QStringLiteral("C:/a.pdf"),
          makeState(12, QStringLiteral("custom"), 2.25, 180, 0.3, 0.65), 100);
    QVERIFY(a.save(file));

    ViewStateStore b;
    QVERIFY(b.load(file));
    const auto got = b.get(QStringLiteral("C:/a.pdf"));
    QVERIFY(got.has_value());
    QCOMPARE(got->page, 12);
    QCOMPARE(got->zoomMode, QStringLiteral("custom"));
    QCOMPARE(got->scale, 2.25);
    QCOMPARE(got->rotation, 180);
    QCOMPARE(got->offsetX, 0.3);
    QCOMPARE(got->offsetY, 0.65);
}

QTEST_GUILESS_MAIN(TstViewStateStore)
#include "tst_view_state_store.moc"
