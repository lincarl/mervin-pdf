// Performance guardrail for startup: launches the real MervinPDF binary with
// `--profile <tempdir> --quit-after-startup` and measures whole-process
// runtime (spawn to exit). The temp profile keeps the run isolated from the
// user's real settings/session AND from any running installed instance (a
// profile forms its own single-instance group), so the numbers are stable no
// matter what the developer has open. Run 1 includes first-run profile
// creation; later runs are warm starts. Printed numbers are the deliverable -
// the QVERIFY ceiling only catches runaway regressions.
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <cstdio>

class TstPerfStartup : public QObject
{
    Q_OBJECT

private slots:
    void startupTime();
    void startupTimeWithRestoredSession();
};

void TstPerfStartup::startupTime()
{
    const QString exe = QStringLiteral(MERVIN_APP_EXE);
    if (!QFileInfo::exists(exe))
        QSKIP("MervinPDF executable not built");

    QTemporaryDir profile;
    QVERIFY(profile.isValid());

    double first = 0, best = 1e18;
    for (int i = 0; i < 3; ++i) {
        QProcess proc;
        QElapsedTimer t;
        t.start();
        proc.start(exe, {QStringLiteral("--profile"), profile.path(),
                         QStringLiteral("--quit-after-startup")});
        QVERIFY2(proc.waitForStarted(10000), "MervinPDF failed to start");
        QVERIFY2(proc.waitForFinished(30000), "MervinPDF did not exit after startup");
        const double ms = t.nsecsElapsed() / 1e6;
        QCOMPARE(proc.exitStatus(), QProcess::NormalExit);
        QCOMPARE(proc.exitCode(), 0);
        if (i == 0)
            first = ms;
        best = std::min(best, ms);
        std::printf("startup run %d: %6.0f ms\n", i + 1, ms);
        std::fflush(stdout);
    }
    std::printf("startup: first=%.0f ms  best=%.0f ms\n", first, best);
    std::fflush(stdout);

    QVERIFY2(best < 15000, "startup grossly regressed (>15 s)");
}

void TstPerfStartup::startupTimeWithRestoredSession()
{
    // A startup that actually opens documents. The empty-profile run above times
    // process and window bring-up but says nothing about the session restore,
    // which is where a multi-document start spends nearly all of its time - so
    // neither guardrail used to cover the work that makes a many-document start
    // slow. --quit-after-startup waits for the last document of the batch, so this
    // number covers the whole restore rather than just the first paint.
    const QString exe = QStringLiteral(MERVIN_APP_EXE);
    if (!QFileInfo::exists(exe))
        QSKIP("MervinPDF executable not built");

    const QDir examples(QStringLiteral(MERVIN_EXAMPLES_DIR));
    QStringList docs;
    for (const char *name : {"schematic.pdf", "form_comment.pdf", "images.pdf", "example1.pdf"}) {
        const QString path = examples.filePath(QLatin1String(name));
        if (!QFileInfo::exists(path))
            QSKIP("optional local PDF fixtures are not present");
        docs << path;
    }

    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    const QString sessionFile = QDir(profile.path()).filePath(QStringLiteral("session.json"));

    // All but one document come from the session; the last is passed on the command
    // line, as a double-click would. That split is also what makes the run
    // verifiable: only a launch that really opened documents rewrites the session
    // to hold all four, so this cannot quietly degrade into timing a startup that
    // restores nothing.
    const QStringList fromSession = docs.mid(0, docs.size() - 1);
    const QString fromCli = docs.last();

    double first = 0, best = 1e18;
    for (int i = 0; i < 3; ++i) {
        // Rewritten every run: the app keeps the session up to date as it opens
        // documents, and each run has to start from the same state.
        QJsonArray open;
        for (const QString &p : fromSession)
            open.append(p);
        QJsonObject root;
        root.insert(QStringLiteral("open"), open);
        root.insert(QStringLiteral("active"), fromSession.last());
        QFile f(sessionFile);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.close();

        QProcess proc;
        QElapsedTimer t;
        t.start();
        proc.start(exe, {QStringLiteral("--profile"), profile.path(),
                         QStringLiteral("--quit-after-startup"), fromCli});
        QVERIFY2(proc.waitForStarted(10000), "MervinPDF failed to start");
        QVERIFY2(proc.waitForFinished(60000), "MervinPDF did not exit after startup");
        const double ms = t.nsecsElapsed() / 1e6;
        QCOMPARE(proc.exitStatus(), QProcess::NormalExit);
        QCOMPARE(proc.exitCode(), 0);
        if (i == 0)
            first = ms;
        best = std::min(best, ms);
        std::printf("startup+%d docs run %d: %6.0f ms\n", int(docs.size()), i + 1, ms);
        std::fflush(stdout);
    }
    std::printf("startup+session: first=%.0f ms  best=%.0f ms\n", first, best);
    std::fflush(stdout);

    // Prove the timed runs actually opened the documents: the session the app left
    // behind must list all four (the three it restored plus the command-line one),
    // which only happens once each has a tab.
    QFile after(sessionFile);
    QVERIFY(after.open(QIODevice::ReadOnly));
    const QJsonArray left =
        QJsonDocument::fromJson(after.readAll()).object().value(QStringLiteral("open")).toArray();
    after.close();
    QCOMPARE(left.size(), docs.size());
    QStringList names;
    for (const QJsonValue &v : left)
        names << QFileInfo(v.toString()).fileName();
    for (const QString &d : docs)
        QVERIFY2(names.contains(QFileInfo(d).fileName()),
                 qPrintable(QStringLiteral("%1 missing from the session the app wrote: %2")
                                .arg(QFileInfo(d).fileName(), names.join(QLatin1Char(',')))));

    QVERIFY2(best < 20000, "startup with a restored session grossly regressed (>20 s)");
}

QTEST_GUILESS_MAIN(TstPerfStartup)
#include "tst_perf_startup.moc"
