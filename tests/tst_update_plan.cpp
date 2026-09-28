// The updater's pure half: reading GitHub's latest-release answer, choosing the
// asset that updates each kind of install, and recognising Windows installs.
// A wrong answer here downloads the wrong package, or none, with no visible
// error, so each rule is pinned with a case that must match and one that must not.

#include "update/UpdatePlan.h"

#include <QtTest>

using namespace mervin::update;

namespace {

// Trimmed from the real v1.63.3 answer of the releases/latest endpoint.
const QByteArray kRelease163 = R"({
  "tag_name": "v1.63.3",
  "html_url": "https://github.com/lincarl/mervin-pdf/releases/tag/v1.63.3",
  "assets": [
    {"name": "mervin-pdf-1.63.3.x86_64.rpm",
     "digest": "sha256:4adf91c186849e83212906c9d499f11c3a1e226d9a52182a97e68d3058c082b6",
     "browser_download_url": "https://github.com/lincarl/mervin-pdf/releases/download/v1.63.3/mervin-pdf-1.63.3.x86_64.rpm"},
    {"name": "mervin-pdf_1.63.3_ubuntu26.04_amd64.deb",
     "digest": "sha256:f3381e519d54035b1906ea5a91285be571b3ce1116feb1664b0aaf7a7a8dfaae",
     "browser_download_url": "https://github.com/lincarl/mervin-pdf/releases/download/v1.63.3/mervin-pdf_1.63.3_ubuntu26.04_amd64.deb"},
    {"name": "MervinPDF-1.63.3-x86_64.AppImage",
     "digest": "sha256:FE0A990B4585645A2689ADF4E856BDE2D823FB69DDF3712F01FE832FFA32F760",
     "browser_download_url": "https://github.com/lincarl/mervin-pdf/releases/download/v1.63.3/MervinPDF-1.63.3-x86_64.AppImage"},
    {"name": "MervinPDF-1.63.3.msi",
     "digest": "sha256:52abec9c86b9772acc69f5bb2b9c2d333ba0952becc9d017d1af74121f57144b",
     "browser_download_url": "https://github.com/lincarl/mervin-pdf/releases/download/v1.63.3/MervinPDF-1.63.3.msi"},
    {"name": "MervinPDF-Setup-1.63.3.exe",
     "digest": "sha256:9181bb65e1e0297a30012fd62735a8996e32df0986af2acbdaca31bf21fbd7e0",
     "browser_download_url": "https://github.com/lincarl/mervin-pdf/releases/download/v1.63.3/MervinPDF-Setup-1.63.3.exe"},
    {"name": "SHA256SUMS",
     "browser_download_url": "https://github.com/lincarl/mervin-pdf/releases/download/v1.63.3/SHA256SUMS"}
  ]
})";

// A downloadable, verifiable asset: the rules under test are about the name.
ReleaseAsset named(const QString &name)
{
    return {name, QStringLiteral("https://example.invalid/") + name, QStringLiteral("ab12")};
}

} // namespace

class TestUpdatePlan : public QObject
{
    Q_OBJECT

private slots:
    void parseVersion_data();
    void parseVersion();
    void parsesTheRealRelease();
    void rejectsReleasesWithoutAVersion_data();
    void rejectsReleasesWithoutAVersion();
    void assetForEachKind_data();
    void assetForEachKind();
    void assetForIgnoresOtherArchitectures_data();
    void assetForIgnoresOtherArchitectures();
    void debPrefersThisUbuntuRelease();
    void assetForNeedsADigestAndAPlainName();
    void windowsPackageKind_data();
    void windowsPackageKind();
    void linuxPackageKind_data();
    void linuxPackageKind();
};

void TestUpdatePlan::parseVersion_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QVersionNumber>("expected");
    QTest::newRow("tag") << "v1.63.3" << QVersionNumber(1, 63, 3);
    QTest::newRow("bare") << "1.63.3" << QVersionNumber(1, 63, 3);
    QTest::newRow("upper-case v, padded") << " V2.0.1 " << QVersionNumber(2, 0, 1);
    QTest::newRow("not a version") << "latest" << QVersionNumber();
}

void TestUpdatePlan::parseVersion()
{
    QFETCH(QString, text);
    QFETCH(QVersionNumber, expected);
    QCOMPARE(mervin::update::parseVersion(text), expected);
}

void TestUpdatePlan::parsesTheRealRelease()
{
    const std::optional<Release> r = parseRelease(kRelease163);
    QVERIFY(r);
    QCOMPARE(r->tag, QStringLiteral("v1.63.3"));
    QCOMPARE(r->version, QVersionNumber(1, 63, 3));
    QVERIFY(r->version > QVersionNumber(1, 63, 2));
    QVERIFY(!(r->version > QVersionNumber(1, 63, 3)));
    QCOMPARE(r->pageUrl,
             QStringLiteral("https://github.com/lincarl/mervin-pdf/releases/tag/v1.63.3"));
    QCOMPARE(r->assets.size(), 6);

    // Digests lose their "sha256:" prefix and are lower-cased to compare with
    // QCryptographicHash's hex; an asset without one keeps an empty digest.
    const ReleaseAsset &appImage = r->assets.at(2);
    QCOMPARE(appImage.name, QStringLiteral("MervinPDF-1.63.3-x86_64.AppImage"));
    QCOMPARE(appImage.sha256,
             QStringLiteral("fe0a990b4585645a2689adf4e856bde2d823fb69ddf3712f01fe832ffa32f760"));
    QVERIFY(r->assets.at(5).sha256.isEmpty());
}

void TestUpdatePlan::rejectsReleasesWithoutAVersion_data()
{
    QTest::addColumn<QByteArray>("json");
    QTest::newRow("empty") << QByteArray();
    QTest::newRow("not json") << QByteArray("<html>rate limited</html>");
    QTest::newRow("API error body") << QByteArray(R"({"message": "Not Found"})");
    QTest::newRow("tag is not a version") << QByteArray(R"({"tag_name": "nightly", "assets": []})");
}

void TestUpdatePlan::rejectsReleasesWithoutAVersion()
{
    QFETCH(QByteArray, json);
    QVERIFY(!parseRelease(json));
}

void TestUpdatePlan::assetForEachKind_data()
{
    QTest::addColumn<PackageKind>("kind");
    QTest::addColumn<QString>("expected"); // empty: no asset
    QTest::newRow("NSIS") << PackageKind::NsisSetup << "MervinPDF-Setup-1.63.3.exe";
    QTest::newRow("MSI") << PackageKind::Msi << "MervinPDF-1.63.3.msi";
    QTest::newRow("AppImage") << PackageKind::AppImage << "MervinPDF-1.63.3-x86_64.AppImage";
    QTest::newRow("deb") << PackageKind::Deb << "mervin-pdf_1.63.3_ubuntu26.04_amd64.deb";
    QTest::newRow("rpm") << PackageKind::Rpm << "mervin-pdf-1.63.3.x86_64.rpm";
    QTest::newRow("dev build") << PackageKind::None << QString();
}

void TestUpdatePlan::assetForEachKind()
{
    QFETCH(PackageKind, kind);
    QFETCH(QString, expected);
    const QList<ReleaseAsset> assets = parseRelease(kRelease163)->assets;
    const std::optional<ReleaseAsset> a = assetFor(assets, kind, QStringLiteral("26.04"));
    QCOMPARE(a ? a->name : QString(), expected);

    // A release that lacks this kind's asset offers nothing to download.
    const QList<ReleaseAsset> sumsOnly{parseRelease(kRelease163)->assets.last()};
    QVERIFY(!assetFor(sumsOnly, kind, QStringLiteral("26.04")));
}

void TestUpdatePlan::assetForIgnoresOtherArchitectures_data()
{
    QTest::addColumn<PackageKind>("kind");
    QTest::addColumn<QString>("name");
    QTest::newRow("AppImage") << PackageKind::AppImage << "MervinPDF-1.64.0-aarch64.AppImage";
    QTest::newRow("deb") << PackageKind::Deb << "mervin-pdf_1.64.0_ubuntu26.04_arm64.deb";
    QTest::newRow("rpm") << PackageKind::Rpm << "mervin-pdf-1.64.0.aarch64.rpm";
    QTest::newRow("NSIS is not the MSI") << PackageKind::NsisSetup << "MervinPDF-1.64.0.msi";
    QTest::newRow("MSI is not the NSIS") << PackageKind::Msi << "MervinPDF-Setup-1.64.0.exe";
    QTest::newRow("rpm is not the deb") << PackageKind::Rpm << "mervin-pdf_1.64.0_ubuntu26.04_amd64.deb";
}

void TestUpdatePlan::assetForIgnoresOtherArchitectures()
{
    QFETCH(PackageKind, kind);
    QFETCH(QString, name);
    QVERIFY(!assetFor({named(name)}, kind, QStringLiteral("26.04")));
}

void TestUpdatePlan::debPrefersThisUbuntuRelease()
{
    const QList<ReleaseAsset> assets{
        named(QStringLiteral("mervin-pdf_1.64.0_ubuntu26.04_amd64.deb")),
        named(QStringLiteral("mervin-pdf_1.64.0_ubuntu26.10_amd64.deb")),
    };
    QCOMPARE(assetFor(assets, PackageKind::Deb, QStringLiteral("26.10"))->name,
             QStringLiteral("mervin-pdf_1.64.0_ubuntu26.10_amd64.deb"));
    QCOMPARE(assetFor(assets, PackageKind::Deb, QStringLiteral("26.04"))->name,
             QStringLiteral("mervin-pdf_1.64.0_ubuntu26.04_amd64.deb"));
    // Not Ubuntu (Debian reports "13"): the first .deb is still the best guess.
    QCOMPARE(assetFor(assets, PackageKind::Deb, QStringLiteral("13"))->name,
             QStringLiteral("mervin-pdf_1.64.0_ubuntu26.04_amd64.deb"));
}

void TestUpdatePlan::assetForNeedsADigestAndAPlainName()
{
    const QString name = QStringLiteral("MervinPDF-Setup-1.64.0.exe");
    QVERIFY(assetFor({named(name)}, PackageKind::NsisSetup, {}));

    // Nothing unverifiable gets run as an installer.
    ReleaseAsset undigested = named(name);
    undigested.sha256.clear();
    QVERIFY(!assetFor({undigested}, PackageKind::NsisSetup, {}));

    // The name becomes a path under the updates folder, so it must stay there.
    QVERIFY(!assetFor({named(QStringLiteral("MervinPDF-Setup-/../../evil.exe"))},
                      PackageKind::NsisSetup, {}));
    QVERIFY(!assetFor({named(QStringLiteral("MervinPDF-Setup-..\\..\\evil.exe"))},
                      PackageKind::NsisSetup, {}));
}

void TestUpdatePlan::windowsPackageKind_data()
{
    QTest::addColumn<QString>("exeDir");
    QTest::addColumn<QString>("registered");
    QTest::addColumn<bool>("hasUninstaller");
    QTest::addColumn<PackageKind>("expected");

    // applicationDirPath() uses forward slashes; the registry holds native paths.
    const QString exeDir = QStringLiteral("C:/Users/ana/AppData/Local/Mervin PDF");
    QTest::newRow("NSIS") << exeDir << "C:\\Users\\ana\\AppData\\Local\\Mervin PDF" << true
                          << PackageKind::NsisSetup;
    // WiX writes [INSTALLFOLDER], which ends in a backslash.
    QTest::newRow("MSI") << exeDir << "C:\\Users\\ana\\AppData\\Local\\Mervin PDF\\" << false
                         << PackageKind::Msi;
    QTest::newRow("path case differs") << exeDir << "c:\\users\\ANA\\appdata\\local\\mervin pdf\\"
                                       << false << PackageKind::Msi;
    QTest::newRow("dev build") << QStringLiteral("C:/projects/mervin-pdf/build/x64-release")
                               << "C:\\Users\\ana\\AppData\\Local\\Mervin PDF\\" << false
                               << PackageKind::None;
    QTest::newRow("copied install folder") << QStringLiteral("D:/Tools/Mervin PDF")
                                           << "C:\\Users\\ana\\AppData\\Local\\Mervin PDF" << true
                                           << PackageKind::None;
    QTest::newRow("never installed") << exeDir << QString() << true << PackageKind::None;
}

void TestUpdatePlan::windowsPackageKind()
{
    QFETCH(QString, exeDir);
    QFETCH(QString, registered);
    QFETCH(bool, hasUninstaller);
    QFETCH(PackageKind, expected);
    QCOMPARE(mervin::update::windowsPackageKind(exeDir, registered, hasUninstaller), expected);
}

void TestUpdatePlan::linuxPackageKind_data()
{
    QTest::addColumn<QString>("exePath");
    QTest::addColumn<QString>("appImage");
    QTest::addColumn<QString>("appDir");
    QTest::addColumn<QString>("buildFormat");
    QTest::addColumn<PackageKind>("expected");

    const QString mount = QStringLiteral("/tmp/.mount_MervinAbc");
    const QString image = QStringLiteral("/home/ana/Apps/MervinPDF-1.63.3-x86_64.AppImage");
    // linuxdeploy's AppRun execs the binary inside the mounted image.
    QTest::newRow("AppImage") << mount + "/usr/bin/MervinPDF" << image << mount << QString()
                              << PackageKind::AppImage;
    // Started from another AppImage's terminal: its variables leak in, and must
    // not make this copy overwrite that other program.
    const QString otherMount = QStringLiteral("/tmp/.mount_CursorXyz");
    const QString otherImage = QStringLiteral("/home/ana/Apps/Cursor.AppImage");
    QTest::newRow("deb under another AppImage") << "/usr/bin/MervinPDF" << otherImage << otherMount
                                                << "deb" << PackageKind::Deb;
    QTest::newRow("dev build under another AppImage")
        << "/home/ana/dev/build/MervinPDF" << otherImage << otherMount << QString()
        << PackageKind::None;
    QTest::newRow("mount name is only a prefix") << mount + "Def/usr/bin/MervinPDF" << image << mount
                                                 << QString() << PackageKind::None;
    QTest::newRow("APPIMAGE without APPDIR") << mount + "/usr/bin/MervinPDF" << image << QString()
                                             << QString() << PackageKind::None;
    QTest::newRow("deb") << "/usr/bin/MervinPDF" << QString() << QString() << "deb"
                         << PackageKind::Deb;
    QTest::newRow("rpm") << "/usr/bin/MervinPDF" << QString() << QString() << "rpm"
                         << PackageKind::Rpm;
    QTest::newRow("package build in its build tree")
        << "/home/ana/dev/build/MervinPDF" << QString() << QString() << "deb" << PackageKind::None;
    QTest::newRow("source install") << "/usr/bin/MervinPDF" << QString() << QString() << QString()
                                    << PackageKind::None;
}

void TestUpdatePlan::linuxPackageKind()
{
    QFETCH(QString, exePath);
    QFETCH(QString, appImage);
    QFETCH(QString, appDir);
    QFETCH(QString, buildFormat);
    QFETCH(PackageKind, expected);
    QCOMPARE(mervin::update::linuxPackageKind(exePath, appImage, appDir, buildFormat), expected);
}

QTEST_APPLESS_MAIN(TestUpdatePlan)
#include "tst_update_plan.moc"
