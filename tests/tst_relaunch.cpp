#include "app/Relaunch.h"
#include "config/ConfigPaths.h"
#include "update/Installer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

using namespace mervin;
using ipc::Message;

namespace mervin::PlatformIntegration {
// The test never runs with an installed package identity.
bool hasPackageIdentity() { return false; }
}

namespace {
// Read actual fixture contents after the production operation finishes.
QByteArray contents(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Write the initial fixture without using the carry implementation.
bool writeFixture(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
}

class TstRelaunch : public QObject
{
    Q_OBJECT

private slots:
    void cleanup();
    void failedStartRetainsUnwrittenLaunches();
    void failedStartRetainsWrittenLaunches();
    void alreadyStartedCopyCannotTakeUnwrittenLaunches();
    void invalidCarryIsNotConsumedOrOverwritten_data();
    void invalidCarryIsNotConsumedOrOverwritten();
    void carryRemovalMustSucceedBeforeDelivery();
#ifdef Q_OS_UNIX
    void unreadableCarrySurvives();
#endif
#ifdef Q_OS_WIN
    void windowsInstallerKeepsLiteralPaths_data();
    void windowsInstallerKeepsLiteralPaths();
    void windowsInstallerRejectsCommandSyntax();
#endif
};

void TstRelaunch::cleanup()
{
    relaunch::setStarterForTests([](const QStringList &) { return true; });
    relaunch::request();
    QVERIFY(relaunch::startIfDue(true));
    relaunch::setStarterForTests({});
    ConfigPaths::setOverrideDir({});
}

// A failed launch must leave both the payload and the reason to retry intact.
void TstRelaunch::failedStartRetainsUnwrittenLaunches()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("next.carried"));
    relaunch::carry(Message::open({QStringLiteral("/one.pdf")}, QStringLiteral("new-window")));
    int starts = 0;
    QStringList passed;
    relaunch::setStarterForTests([&](const QStringList &paths) {
        ++starts;
        passed = paths;
        return false;
    });
    QVERIFY(!relaunch::startIfDue(false));
    QCOMPARE(starts, 1);
    QCOMPARE(passed, QStringList{QStringLiteral("/one.pdf")});
    QVERIFY(relaunch::carrying());
    QVERIFY(relaunch::writeCarried(file));
    const auto launches = relaunch::takeCarried(file);
    QCOMPARE(launches.size(), 1);
    QCOMPARE(launches.first().paths, QStringList{QStringLiteral("/one.pdf")});
    QCOMPARE(launches.first().behavior, QStringLiteral("new-window"));
    relaunch::setStarterForTests([&](const QStringList &paths) {
        ++starts;
        return paths.isEmpty();
    });
    QVERIFY(relaunch::startIfDue(false));
    QCOMPARE(starts, 2);
    QVERIFY(!relaunch::carrying());
    QVERIFY(relaunch::startIfDue(false));
    QCOMPARE(starts, 2);
}

// Persisted data survives a failed restart and the pending start can be retried.
void TstRelaunch::failedStartRetainsWrittenLaunches()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("next.carried"));
    relaunch::carry(Message::open({}, QStringLiteral("new-window")));
    QVERIFY(relaunch::writeCarried(file));
    const QByteArray before = contents(file);
    relaunch::setStarterForTests([](const QStringList &) { return false; });
    QVERIFY(!relaunch::startIfDue(false));
    QVERIFY(relaunch::carrying());
    QCOMPARE(contents(file), before);
    const auto launches = relaunch::takeCarried(file);
    QCOMPARE(launches.size(), 1);
    QCOMPARE(launches.first().behavior, QStringLiteral("new-window"));
    QVERIFY(launches.first().paths.isEmpty());
}

// An installer child has already inherited its input. Report fallback failure
// without erasing launches that a later carry write could still preserve.
void TstRelaunch::alreadyStartedCopyCannotTakeUnwrittenLaunches()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    relaunch::carry(Message::open({QStringLiteral("/late.pdf")}, QStringLiteral("new-tab")));
    relaunch::nextCopyStarted();
    int starts = 0;
    relaunch::setStarterForTests([&](const QStringList &) { ++starts; return true; });
    QVERIFY(!relaunch::startIfDue(false));
    QCOMPARE(starts, 0);
    const QString file = dir.filePath(QStringLiteral("retry.carried"));
    QVERIFY(relaunch::writeCarried(file));
    const auto launches = relaunch::takeCarried(file);
    QCOMPARE(launches.size(), 1);
    QCOMPARE(launches.first().paths, QStringList{QStringLiteral("/late.pdf")});
    QCOMPARE(launches.first().behavior, QStringLiteral("new-tab"));
}

void TstRelaunch::invalidCarryIsNotConsumedOrOverwritten_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::newRow("incomplete JSON") << QByteArray("{\"written\":");
    QTest::newRow("missing metadata") << QByteArray("{\"launches\":[]}");
    QTest::newRow("wrong launch field type") << QByteArray("{\"written\":42,\"launches\":{}}");
}

// A failed decode does not establish that there were no pending launches.
void TstRelaunch::invalidCarryIsNotConsumedOrOverwritten()
{
    QFETCH(QByteArray, bytes);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("damaged.carried"));
    QVERIFY(writeFixture(file, bytes));
    QVERIFY(relaunch::takeCarried(file).isEmpty());
    QCOMPARE(contents(file), bytes);
    relaunch::carry(Message::open({QStringLiteral("/new.pdf")}));
    QVERIFY(!relaunch::writeCarried(file));
    QCOMPARE(contents(file), bytes);
    const QString retry = dir.filePath(QStringLiteral("retry.carried"));
    QVERIFY(relaunch::writeCarried(retry));
    const auto launches = relaunch::takeCarried(retry);
    QCOMPARE(launches.size(), 1);
    QCOMPARE(launches.first().paths, QStringList{QStringLiteral("/new.pdf")});
}

// Read permission alone does not permit acknowledging consumption. A retry
// after removing the deletion blocker must return the message exactly once.
void TstRelaunch::carryRemovalMustSucceedBeforeDelivery()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("queued.carried"));
    relaunch::carry(Message::open({QStringLiteral("/queued.pdf")}, QStringLiteral("new-window")));
    QVERIFY(relaunch::writeCarried(file));
    const QByteArray before = contents(file);
#ifdef Q_OS_WIN
    const std::wstring native = QDir::toNativeSeparators(file).toStdWString();
    const HANDLE held = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(held != INVALID_HANDLE_VALUE);
    auto unblock = qScopeGuard([&] { CloseHandle(held); });
#else
    const auto permissions = QFile::permissions(dir.path());
    QVERIFY(QFile::setPermissions(dir.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    auto unblock = qScopeGuard([&] { QFile::setPermissions(dir.path(), permissions); });
#endif
    QVERIFY(relaunch::takeCarried(file).isEmpty());
    QCOMPARE(contents(file), before);
#ifdef Q_OS_WIN
    CloseHandle(held);
#else
    QVERIFY(QFile::setPermissions(dir.path(), permissions));
#endif
    unblock.dismiss();
    const auto launches = relaunch::takeCarried(file);
    QCOMPARE(launches.size(), 1);
    QCOMPARE(launches.first().paths, QStringList{QStringLiteral("/queued.pdf")});
    QCOMPARE(launches.first().behavior, QStringLiteral("new-window"));
    QVERIFY(relaunch::takeCarried(file).isEmpty());
    QVERIFY(!QFile::exists(file));
}

#ifdef Q_OS_UNIX
// An existing file can be unreadable even when its parent permits deletion.
void TstRelaunch::unreadableCarrySurvives()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("unreadable.carried"));
    relaunch::carry(Message::open({QStringLiteral("/private.pdf")}));
    QVERIFY(relaunch::writeCarried(file));
    QVERIFY(QFile::setPermissions(file, QFileDevice::WriteOwner));
    const auto restore = qScopeGuard([&] {
        QFile::setPermissions(file, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    });
    QFile probe(file);
    QVERIFY(!probe.open(QIODevice::ReadOnly));
    QVERIFY(relaunch::takeCarried(file).isEmpty());
    QVERIFY(QFile::exists(file));
    relaunch::carry(Message::open({QStringLiteral("/later.pdf")}));
    QVERIFY(!relaunch::writeCarried(file));
    QVERIFY(QFile::setPermissions(file, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    QVERIFY(relaunch::writeCarried(file));
    const auto launches = relaunch::takeCarried(file);
    QCOMPARE(launches.size(), 2);
    QCOMPARE(launches.at(0).paths, QStringList{QStringLiteral("/private.pdf")});
    QCOMPARE(launches.at(1).paths, QStringList{QStringLiteral("/later.pdf")});
}
#endif

#ifdef Q_OS_WIN
void TstRelaunch::windowsInstallerKeepsLiteralPaths_data()
{
    QTest::addColumn<int>("installerExit");
    QTest::addColumn<int>("profileKind");
    QTest::newRow("success with literal profile") << 0 << 1;
    QTest::newRow("cancelled install still relaunches") << 1602 << 1;
    QTest::newRow("failed install still relaunches") << 1603 << 1;
    QTest::newRow("drive-root profile") << 0 << 2;
    QTest::newRow("default profile") << 0 << 0;
}

// Real cmd.exe invokes harmless test helpers. No MSI or installed app runs.
// Literal percent and exclamation components must survive every command, and
// cleanup must leave a directory with the expanded spelling untouched.
void TstRelaunch::windowsInstallerKeepsLiteralPaths()
{
    QFETCH(int, installerExit);
    QFETCH(int, profileKind);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString literal = QStringLiteral("literal %MERVIN_TEST_EXPAND% ! & (^x)");
    const QString tools = dir.filePath(literal + QStringLiteral(" tools"));
    const QString staging = dir.filePath(literal + QStringLiteral(" staging"));
    QString otherStaging = staging;
    otherStaging.replace(QStringLiteral("%MERVIN_TEST_EXPAND%"), QStringLiteral("expanded"));
    QVERIFY(QDir().mkdir(tools));
    QVERIFY(QDir().mkdir(staging));
    QVERIFY(QDir().mkdir(otherStaging));
    const QString package = QDir(staging).filePath(QStringLiteral("update.msi"));
    const QString otherPackage = QDir(otherStaging).filePath(QStringLiteral("update.msi"));
    QVERIFY(writeFixture(package, "fixture package"));
    QVERIFY(writeFixture(otherPackage, "do not delete"));
    const QString installer = QDir(tools).filePath(QStringLiteral("fake-installer.exe"));
    const QString executable = QDir(tools).filePath(QStringLiteral("fake-relaunch.exe"));
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), installer));
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), executable));
    const QString profile = profileKind == 2 ? QDir(dir.path()).rootPath()
        : profileKind == 1 ? dir.filePath(literal + QStringLiteral(" profile")) : QString();
    ConfigPaths::setOverrideDir(profile);
    const QString installLog = dir.filePath(QStringLiteral("installed.json"));
    const QString relaunchLog = dir.filePath(QStringLiteral("relaunched.json"));
    QProcess helper;
    QVERIFY(update::configureWindowsInstaller(helper, package, executable, installer));
    auto environment = helper.processEnvironment();
    environment.insert(QStringLiteral("MERVIN_TEST_EXPAND"), QStringLiteral("expanded"));
    environment.insert(QStringLiteral("MERVIN_TEST_INSTALL_LOG"), installLog);
    environment.insert(QStringLiteral("MERVIN_TEST_RELAUNCH_LOG"), relaunchLog);
    environment.insert(QStringLiteral("MERVIN_TEST_INSTALL_EXIT"), QString::number(installerExit));
    helper.setProcessEnvironment(environment);
    helper.start();
    QVERIFY(helper.waitForStarted());
    QVERIFY2(helper.waitForFinished(15000), qPrintable(helper.errorString()));
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(relaunchLog), 15000);
    const QJsonObject installed = QJsonDocument::fromJson(contents(installLog)).object();
    const QJsonObject relaunched = QJsonDocument::fromJson(contents(relaunchLog)).object();
    // Both helpers commit their log before exiting. Wait for their recorded PIDs
    // too, so the test leaves no detached child or open executable behind.
    for (const QJsonObject &log : {installed, relaunched}) {
        const DWORD pid = DWORD(log.value(QStringLiteral("pid")).toInteger());
        QVERIFY(pid != 0);
        if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
            const auto close = qScopeGuard([&] { CloseHandle(process); });
            QCOMPARE(WaitForSingleObject(process, 10000), DWORD(WAIT_OBJECT_0));
        }
    }
    QCOMPARE(installed.value(QStringLiteral("package")).toString(), QStringLiteral("fixture package"));
    const QJsonArray installArgs = installed.value(QStringLiteral("arguments")).toArray();
    QCOMPARE(installArgs.size(), 4);
    QCOMPARE(installArgs.at(0).toString(), QStringLiteral("/i"));
    QCOMPARE(installArgs.at(1).toString(), QDir::toNativeSeparators(package));
    QCOMPARE(installArgs.at(2).toString(), QStringLiteral("/passive"));
    QCOMPARE(installArgs.at(3).toString(), QStringLiteral("/norestart"));
    const QJsonArray relaunchArgs = relaunched.value(QStringLiteral("arguments")).toArray();
    QCOMPARE(relaunchArgs.size(), profile.isEmpty() ? 0 : 2);
    if (!profile.isEmpty()) {
        QCOMPARE(relaunchArgs.at(0).toString(), QStringLiteral("--profile"));
        QCOMPARE(relaunchArgs.at(1).toString(), QDir::toNativeSeparators(profile));
    }
    QVERIFY(!QFile::exists(package));
    QVERIFY(!QDir(staging).exists());
    QCOMPARE(contents(otherPackage), QByteArray("do not delete"));
}

// Invalid path syntax must not escape a quoted environment reference in cmd.
void TstRelaunch::windowsInstallerRejectsCommandSyntax()
{
    QProcess process;
    QVERIFY(!update::configureWindowsInstaller(process, QStringLiteral("C:/bad\"name.msi"),
                                               QStringLiteral("C:/app.exe"), QStringLiteral("C:/installer.exe")));
    ConfigPaths::setOverrideDir(QStringLiteral("C:/bad\nprofile"));
    QVERIFY(!update::configureWindowsInstaller(process, QStringLiteral("C:/package.msi"),
                                               QStringLiteral("C:/app.exe"), QStringLiteral("C:/installer.exe")));
}

// A process fixture records the actual argv and bytes seen by the two children.
int runWindowsFixture(const QStringList &arguments)
{
    const bool installing = arguments.value(0) == QLatin1String("/i");
    QJsonObject log;
    log.insert(QStringLiteral("pid"), QCoreApplication::applicationPid());
    log.insert(QStringLiteral("arguments"), QJsonArray::fromStringList(arguments));
    if (installing)
        log.insert(QStringLiteral("package"), QString::fromUtf8(contents(arguments.value(1))));
    QSaveFile out(qEnvironmentVariable(installing ? "MERVIN_TEST_INSTALL_LOG" : "MERVIN_TEST_RELAUNCH_LOG"));
    const QByteArray bytes = QJsonDocument(log).toJson(QJsonDocument::Compact);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit())
        return 99;
    return installing ? qEnvironmentVariableIntValue("MERVIN_TEST_INSTALL_EXIT") : 0;
}
#endif

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIsSet("MERVIN_TEST_INSTALL_LOG"))
        return runWindowsFixture(QCoreApplication::arguments().mid(1));
#endif
    TstRelaunch test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_relaunch.moc"
