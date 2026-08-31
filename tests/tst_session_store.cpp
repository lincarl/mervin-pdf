#include "session/SessionStore.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using mervin::SessionStore;

class TstSessionStore : public QObject
{
    Q_OBJECT

private slots:
    void roundTrip();
    void loadMissingIsEmpty();
    void emptyClears();
    void activePathRoundTrip();
    void activePathAbsentFromOlderFile();
    void activePathOutsideOpenSetIsDropped();
    void clearDropsActivePath();
};

void TstSessionStore::roundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("session.json"));

    SessionStore a;
    a.setPaths({QStringLiteral("C:/a.pdf"), QStringLiteral("C:/docs/b.pdf")});
    QVERIFY(a.save(file));

    SessionStore b;
    QVERIFY(b.load(file));
    QCOMPARE(b.paths().size(), 2);
    QCOMPARE(b.paths()[0], QStringLiteral("C:/a.pdf"));
    QCOMPARE(b.paths()[1], QStringLiteral("C:/docs/b.pdf"));
}

void TstSessionStore::loadMissingIsEmpty()
{
    SessionStore s;
    s.setPaths({QStringLiteral("C:/x.pdf")});
    QVERIFY(!s.load(QStringLiteral("C:/no/such/mervin-session-test.json")));
    QVERIFY(s.paths().isEmpty()); // load clears even on failure
}

void TstSessionStore::emptyClears()
{
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("session.json"));
    SessionStore a;
    a.setPaths({QStringLiteral("C:/a.pdf")});
    QVERIFY(a.save(file));
    a.clear();
    QVERIFY(a.save(file));

    SessionStore b;
    QVERIFY(b.load(file));
    QVERIFY(b.paths().isEmpty());
}

void TstSessionStore::activePathRoundTrip()
{
    // The document on screen is what restore opens first, so it has to survive the
    // round trip - otherwise every start reopens the wrong document first.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("session.json"));

    SessionStore a;
    a.setPaths({QStringLiteral("C:/a.pdf"), QStringLiteral("C:/docs/b.pdf")});
    a.setActivePath(QStringLiteral("C:/docs/b.pdf"));
    QVERIFY(a.save(file));

    SessionStore b;
    QVERIFY(b.load(file));
    QCOMPARE(b.paths().size(), 2);
    QCOMPARE(b.activePath(), QStringLiteral("C:/docs/b.pdf"));
}

void TstSessionStore::activePathAbsentFromOlderFile()
{
    // A session.json written before the field existed must still load, with no
    // active document rather than a refusal or a bogus one.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("session.json"));
    QFile f(file);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{\"open\":[\"C:/a.pdf\",\"C:/b.pdf\"]}");
    f.close();

    SessionStore s;
    QVERIFY(s.load(file));
    QCOMPARE(s.paths().size(), 2);
    QCOMPARE(s.paths()[0], QStringLiteral("C:/a.pdf"));
    QVERIFY(s.activePath().isEmpty());
}

void TstSessionStore::activePathOutsideOpenSetIsDropped()
{
    // A recorded active document that is not in the open set is stale (the entry
    // was closed, or the file is gone). Ignore it instead of planning a restore
    // around a document that will not be opened.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("session.json"));

    SessionStore a;
    a.setPaths({QStringLiteral("C:/a.pdf")});
    a.setActivePath(QStringLiteral("C:/gone.pdf"));
    QVERIFY(a.save(file));

    SessionStore b;
    QVERIFY(b.load(file));
    QCOMPARE(b.paths().size(), 1);
    QVERIFY(b.activePath().isEmpty());
}

void TstSessionStore::clearDropsActivePath()
{
    SessionStore s;
    s.setPaths({QStringLiteral("C:/a.pdf")});
    s.setActivePath(QStringLiteral("C:/a.pdf"));
    s.clear();
    QVERIFY(s.paths().isEmpty());
    QVERIFY(s.activePath().isEmpty());
}

QTEST_GUILESS_MAIN(TstSessionStore)
#include "tst_session_store.moc"
