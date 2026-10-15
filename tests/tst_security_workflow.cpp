#include "Stalls.h"
#include "TestWaits.h"
#include "app/WindowManager.h"
#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "dialogs/SecurityDialog.h"
#include "io/AppFiles.h"
#include "io/Background.h"
#include "io/JobContext.h"
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "ui/MainWindow.h"
#include "ui/TabPage.h"
#include "ui/ViewerWidget.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <functional>

#ifdef Q_OS_UNIX
#  include <sys/stat.h>
#endif

using namespace mervin;

namespace {

/// Reads exact file contents for source and destination preservation assertions.
QByteArray bytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/// Replaces a local test destination and reports whether every byte was written.
bool writeBytes(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

/// Realizes an isolated test window without showing it on the desktop.
void showWindow(MainWindow &window)
{
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1000, 800);
    window.show();
    QCoreApplication::processEvents();
}

/// Finds the selected tab through the actual window tab widget.
TabPage *currentTab(MainWindow &window)
{
    return qobject_cast<TabPage *>(window.findChild<QTabWidget *>()->currentWidget());
}

// Drives the actual modal controls. Submit each answer once before it can enter
// another event loop, and keep answering nested overwrite questions.
class Interaction
{
public:
    /// Starts modal-control polling with one answer per dialog and guarded nested actions.
    Interaction()
    {
        timer.setInterval(5);
        QObject::connect(&timer, &QTimer::timeout, [&] {
            QWidget *modal = QApplication::activeModalWidget();
            if (intercept && intercept(modal))
                return;
            if (auto *dialog = qobject_cast<SecurityDialog *>(modal)) {
                if (seenSecurity == dialog)
                    return;
                seenSecurity = dialog;
                ++securityCount;
                securityOwners.append(dialog->parentWidget());
                QMetaObject::invokeMethod(dialog, [this, dialog] {
                    const auto dismiss = qScopeGuard([guard = QPointer<SecurityDialog>(dialog)] {
                        if (guard && guard->isVisible())
                            guard->reject();
                    });
                    if (security)
                        security(*dialog);
                    else
                        dialog->reject();
                }, Qt::QueuedConnection);
            } else if (auto *picker = qobject_cast<QFileDialog *>(modal)) {
                if (seenPicker == picker)
                    return;
                seenPicker = picker;
                suggested.append(picker->selectedFiles().value(0));
                if (files.isEmpty()) {
                    unexpected.append(picker->windowTitle());
                    picker->reject();
                    return;
                }
                const QString path = files.takeFirst();
                picker->selectFile(path);
                if (auto *name = picker->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
                    name->setText(QDir::toNativeSeparators(path));
                QMetaObject::invokeMethod(picker, "accept", Qt::QueuedConnection);
            } else if (auto *prompt = qobject_cast<QInputDialog *>(modal)) {
                if (answered == prompt)
                    return;
                answered = prompt;
                ++passwordCount;
                if (passwords.isEmpty()) {
                    unexpected.append(prompt->windowTitle());
                    prompt->reject();
                    return;
                }
                const auto password = passwords.takeFirst();
                if (password) {
                    prompt->setTextValue(*password);
                    prompt->accept();
                } else {
                    prompt->reject();
                }
            } else if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                if (answered == box)
                    return;
                answered = box;
                messages.append(box->text());
                messageOwners.append(box->parentWidget());
                const auto choice = answer ? answer(*box)
                    : answers.isEmpty() ? QMessageBox::NoButton : answers.takeFirst();
                QAbstractButton *button = box->button(choice);
                if (!button) {
                    unexpected.append(box->windowTitle() + QStringLiteral(": ") + box->text());
                    box->reject();
                } else {
                    button->click();
                }
            }
        });
        timer.start();
    }

    QStringList files;
    QList<std::optional<QString>> passwords;
    QList<QMessageBox::StandardButton> answers;
    std::function<QMessageBox::StandardButton(const QMessageBox &)> answer;
    std::function<void(SecurityDialog &)> security;
    std::function<bool(QWidget *)> intercept;
    QStringList messages;
    QStringList suggested;
    QStringList unexpected;
    QList<QPointer<QWidget>> securityOwners;
    QList<QPointer<QWidget>> messageOwners;
    int securityCount = 0;
    int passwordCount = 0;

private:
    QTimer timer;
    QPointer<SecurityDialog> seenSecurity;
    QPointer<QFileDialog> seenPicker;
    QPointer<QWidget> answered;
};

/// Selects distinct passwords, AES-128 and restricted permissions for retry assertions.
void setEncryptionChoices(SecurityDialog &dialog)
{
    dialog.findChild<QLineEdit *>(QStringLiteral("securityUserPassword"))->setText(QStringLiteral("new password"));
    dialog.findChild<QLineEdit *>(QStringLiteral("securityOwnerPassword"))->setText(QStringLiteral("owner guard"));
    dialog.findChild<QComboBox *>(QStringLiteral("securityAlgorithm"))->setCurrentIndex(1);
    for (const QString &name : {QStringLiteral("securityAllowPrint"), QStringLiteral("securityAllowCopy"),
                               QStringLiteral("securityAllowModify"), QStringLiteral("securityAllowAnnotate")})
        dialog.findChild<QCheckBox *>(name)->setChecked(false);
}

/// Checks that failure recovery preserves the chosen output and every encryption control.
void verifyEncryptionChoices(const SecurityDialog &dialog, const QString &output)
{
    QVERIFY(dialog.request());
    QCOMPARE(QDir::fromNativeSeparators(dialog.request()->output), output);
    QCOMPARE(dialog.findChild<QLineEdit *>(QStringLiteral("securityUserPassword"))->text(),
             QStringLiteral("new password"));
    QCOMPARE(dialog.findChild<QLineEdit *>(QStringLiteral("securityOwnerPassword"))->text(),
             QStringLiteral("owner guard"));
    QCOMPARE(dialog.findChild<QComboBox *>(QStringLiteral("securityAlgorithm"))->currentIndex(), 1);
    for (QCheckBox *permission : dialog.findChildren<QCheckBox *>())
        QVERIFY(!permission->isChecked());
}

} // namespace

/// Checks worker I/O, retained choices and Security result delivery through real controls.
class TstSecurityWorkflow : public QObject
{
    Q_OBJECT

public:
    /// Uses Qt file pickers so tests can drive and inspect their controls.
    TstSecurityWorkflow()
    {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    }

private slots:
    /// Creates an isolated profile and disables background application policies for each case.
    void init()
    {
        profile_ = std::make_unique<QTemporaryDir>();
        QVERIFY(profile_->isValid());
        ConfigPaths::setOverrideDir(profile_->path());
        Settings settings;
        settings.autoUpdate = false;
        settings.closeToTray = false;
        settings.unloadInactiveMinutes = 0;
        QVERIFY(settings.save());
    }

    /// Releases job hooks and settles background writes before removing the test profile.
    void cleanup()
    {
        job::testing::release();
        job::testing::failWhen({});
        QVERIFY(background::settle(10000));
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(appfiles::flush(10000));
        ConfigPaths::setOverrideDir({});
        profile_.reset();
    }

    /// Checks that metadata reads return promptly and keep another tab editable.
    void securityInformationRunsOnAWorker()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("plan.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, source));
        auto *page = window.findChild<TabPage *>();
        const int guiSteps = job::testing::stepsOnMainThread();
        job::testing::holdWhen([source](job::Step step, const QString &path) {
            return step == job::Step::Reading && path == source;
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        Interaction dialogs;
        dialogs.security = [&](SecurityDialog &dialog) {
            QCOMPARE(dialog.parentWidget(), &window);
            QCOMPARE(dialog.windowModality(), Qt::WindowModal);
            dialog.reject();
        };
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(QMetaObject::invokeMethod(&window, "openSecurity"));
        QVERIFY(elapsed.elapsed() < 1000);
        QTRY_COMPARE(job::testing::held(), 1);
        QVERIFY(page->isBusy());
        QCOMPARE(dialogs.securityCount, 0);
        job::testing::release();
        QTRY_COMPARE(dialogs.securityCount, 1);
        QTRY_VERIFY(page->isUsable());
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// Covers correct and incorrect answers to the security information password prompt.
    void informationPasswordRetry_data()
    {
        QTest::addColumn<bool>("correct");
        QTest::newRow("correct") << true;
        QTest::newRow("wrong") << false;
    }

    /// Checks the single password retry and its final information result.
    void informationPasswordRetry()
    {
        QFETCH(bool, correct);
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("encrypted.pdf"));
        QCOMPARE(QpdfService().encrypt(QStringLiteral(MERVIN_FIXTURE_PDF), source, {},
                     QStringLiteral("source password"), {}, QpdfService::Algorithm::AES256, {}),
                 QpdfService::Status::Ok);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, source, false, -1, true, QStringLiteral("source password")));
        auto *page = window.findChild<TabPage *>();
        page->setPassword({});
        const int guiSteps = job::testing::stepsOnMainThread();
        std::atomic<int> reads{0};
        job::testing::failWhen([&](job::Step step, const QString &path) {
            if (step == job::Step::Reading && path == source)
                ++reads;
            return false;
        });
        const auto clearHook = qScopeGuard([] { job::testing::failWhen({}); });
        Interaction dialogs;
        dialogs.passwords.append(correct ? QStringLiteral("source password") : QStringLiteral("wrong"));
        QString information;
        dialogs.security = [&](SecurityDialog &dialog) {
            for (QLabel *label : dialog.findChildren<QLabel *>())
                if (label->textFormat() == Qt::RichText)
                    information = label->text();
            dialog.reject();
        };
        QVERIFY(QMetaObject::invokeMethod(&window, "openSecurity"));
        QTRY_COMPARE(dialogs.securityCount, 1);
        QCOMPARE(dialogs.passwordCount, 1);
        QCOMPARE(reads.load(), 2);
        QVERIFY(information.contains(correct ? QStringLiteral("<b>Encrypted</b>")
                                             : QStringLiteral("A password is required.")));
        QCOMPARE(page->password(), correct ? QStringLiteral("source password") : QString());
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// Checks that the result opens using its new password and retains chosen restrictions.
    void encryptedResultOpensWithItsNewPassword()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("plan.pdf"));
        const QString output = files.filePath(QStringLiteral("protected.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        const QByteArray original = bytes(source);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, source));
        const int guiSteps = job::testing::stepsOnMainThread();
        Interaction dialogs;
        dialogs.files.append(output);
        dialogs.answers.append(QMessageBox::Yes);
        dialogs.security = [&](SecurityDialog &dialog) {
            setEncryptionChoices(dialog);
            dialog.findChild<QPushButton *>(QStringLiteral("securityEncrypt"))->click();
        };
        QVERIFY(QMetaObject::invokeMethod(&window, "openSecurity"));
        QTRY_COMPARE(window.tabCount(), 2);
        QTRY_VERIFY(currentTab(window) && currentTab(window)->isUsable());
        QCOMPARE(window.currentTabPath(), output);
        QCOMPARE(dialogs.passwordCount, 0);
        QCOMPARE(dialogs.securityCount, 1);
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        QpdfService::Info info;
        QCOMPARE(QpdfService().readInfo(output, QStringLiteral("new password"), info), QpdfService::Status::Ok);
        QVERIFY(info.encrypted);
        QCOMPARE(info.keyLengthBits, 128);
        QVERIFY(!info.permissions.canPrint && !info.permissions.canCopy
                && !info.permissions.canModify && !info.permissions.canAnnotate);
        QCOMPARE(bytes(source), original);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// Covers password removal and owner-restriction removal.
    void decryptAndStripWriteAnUnrestrictedCopy_data()
    {
        QTest::addColumn<bool>("strip");
        QTest::newRow("remove password") << false;
        QTest::newRow("strip restrictions") << true;
    }

    /// Checks that accepted requests write usable, unencrypted and unrestricted copies.
    void decryptAndStripWriteAnUnrestrictedCopy()
    {
        QFETCH(bool, strip);
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("protected.pdf"));
        const QString output = files.filePath(QStringLiteral("clear.pdf"));
        QpdfService::Permissions permissions{false, false, false, false};
        QCOMPARE(QpdfService().encrypt(QStringLiteral(MERVIN_FIXTURE_PDF), source, {},
                     QStringLiteral("source password"), QStringLiteral("owner"),
                     QpdfService::Algorithm::AES256, permissions), QpdfService::Status::Ok);
        const QByteArray original = bytes(source);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, source, false, -1, true, QStringLiteral("source password")));
        auto *page = window.findChild<TabPage *>();
        Interaction dialogs;
        dialogs.files.append(output);
        if (strip)
            dialogs.answers.append(QMessageBox::Yes);
        dialogs.answers.append(QMessageBox::No);
        dialogs.security = [&](SecurityDialog &dialog) {
            dialog.findChild<QPushButton *>(strip ? QStringLiteral("securityStrip")
                                                  : QStringLiteral("securityDecrypt"))->click();
        };
        QVERIFY(QMetaObject::invokeMethod(&window, "openSecurity"));
        QTRY_VERIFY(QFileInfo::exists(output));
        QTRY_VERIFY(page->isUsable() && dialogs.answers.isEmpty());
        QpdfService::Info info;
        QCOMPARE(QpdfService().readInfo(output, {}, info), QpdfService::Status::Ok);
        QVERIFY(!info.encrypted);
        QVERIFY(info.permissions.canPrint && info.permissions.canCopy
                && info.permissions.canModify && info.permissions.canAnnotate);
        QCOMPARE(bytes(source), original);
        QCOMPARE(dialogs.passwordCount, 0);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// Checks that failed writes retain choices and destination before a successful retry.
    void failedWriteReopensTheChoicesAndCanRetry()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("plan.pdf"));
        const QString output = files.filePath(QStringLiteral("chosen.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, source));
        job::testing::failWhen([folder = files.path()](job::Step step, const QString &path) {
            return step == job::Step::Writing && path == folder;
        });
        const auto clearHook = qScopeGuard([] { job::testing::failWhen({}); });
        Interaction dialogs;
        dialogs.files << output << output;
        dialogs.answers << QMessageBox::Ok << QMessageBox::No;
        dialogs.security = [&](SecurityDialog &dialog) {
            if (dialogs.securityCount == 1) {
                setEncryptionChoices(dialog);
            } else {
                QCOMPARE(dialog.findChild<QLineEdit *>(QStringLiteral("securityUserPassword"))->text(), QStringLiteral("new password"));
                QCOMPARE(dialog.findChild<QLineEdit *>(QStringLiteral("securityOwnerPassword"))->text(), QStringLiteral("owner guard"));
                QCOMPARE(dialog.findChild<QComboBox *>(QStringLiteral("securityAlgorithm"))->currentIndex(), 1);
                for (QCheckBox *permission : dialog.findChildren<QCheckBox *>())
                    QVERIFY(!permission->isChecked());
                QVERIFY(!QFileInfo::exists(output));
                job::testing::failWhen({});
            }
            dialog.findChild<QPushButton *>(QStringLiteral("securityEncrypt"))->click();
        };
        QVERIFY(QMetaObject::invokeMethod(&window, "openSecurity"));
        QTRY_VERIFY(QFileInfo::exists(output));
        QTRY_VERIFY(dialogs.answers.isEmpty());
        QCOMPARE(dialogs.securityCount, 2);
        QCOMPARE(dialogs.suggested.size(), 2);
        QCOMPARE(dialogs.suggested.last(), output);
        QpdfService::Info info;
        QCOMPARE(QpdfService().readInfo(output, QStringLiteral("new password"), info), QpdfService::Status::Ok);
        QCOMPARE(info.keyLengthBits, 128);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// Covers changes after absent or existing approval and inside the replacement question.
    void changedDestinationRequiresFreshApproval_data()
    {
        QTest::addColumn<bool>("existing");
        QTest::addColumn<bool>("duringQuestion");
        QTest::newRow("absent output while source waits") << false << false;
        QTest::newRow("existing output while source waits") << true << false;
        QTest::newRow("existing output during confirmation") << true << true;
    }

    /// Preserves competing bytes, retains choices and obtains new consent on explicit retry.
    void changedDestinationRequiresFreshApproval()
    {
        QFETCH(bool, existing);
        QFETCH(bool, duringQuestion);
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        const QString output = files.filePath(QStringLiteral("output.pdf"));
        const QByteArray original("original contents");
        const QByteArray changed("changed contents!");
        QCOMPARE(original.size(), changed.size());
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        if (existing)
            QVERIFY(writeBytes(output, original));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, source));
        const int guiSteps = job::testing::stepsOnMainThread();
        const auto release = qScopeGuard([] { job::testing::release(); });
        Interaction dialogs;
        dialogs.files << output << output;
        int questions = 0;
        int completions = 0;
        bool changedWritten = false;
        bool checkedRetained = false;
        QString failure;
        dialogs.answer = [&](const QMessageBox &box) {
            if (box.objectName() == QStringLiteral("securityReplace")) {
                if (++questions == 1 && duringQuestion)
                    changedWritten = writeBytes(output, changed);
                return QMessageBox::Yes;
            }
            if (box.windowTitle() == QStringLiteral("Done")) {
                ++completions;
                return QMessageBox::No;
            }
            failure = box.text();
            return QMessageBox::Ok;
        };
        dialogs.security = [&](SecurityDialog &dialog) {
            if (dialogs.securityCount == 1) {
                setEncryptionChoices(dialog);
                if (!duringQuestion)
                    job::testing::holdWhen([source](job::Step step, const QString &path) {
                        return step == job::Step::Reading
                            && QDir::fromNativeSeparators(path) == source;
                    });
            } else {
                verifyEncryptionChoices(dialog, output);
                QCOMPARE(bytes(output), changed);
                checkedRetained = true;
            }
            dialog.findChild<QPushButton *>(QStringLiteral("securityEncrypt"))->click();
        };
        QVERIFY(QMetaObject::invokeMethod(&window, "openSecurity"));
        if (!duringQuestion) {
            QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
            QCOMPARE(questions, existing ? 1 : 0);
            changedWritten = writeBytes(output, changed);
            job::testing::release();
        }
        QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 10000);
        QVERIFY(changedWritten);
        QVERIFY(checkedRetained);
        QCOMPARE(dialogs.securityCount, 2);
        QCOMPARE(questions, existing ? 2 : 1);
        QVERIFY(failure.contains(QStringLiteral("The destination changed.")));
        QCOMPARE(dialogs.suggested.size(), 2);
        QCOMPARE(QDir::fromNativeSeparators(dialogs.suggested.last()), output);
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        QpdfService::Info info;
        QCOMPARE(QpdfService().readInfo(output, QStringLiteral("new password"), info),
                 QpdfService::Status::Ok);
        QCOMPARE(info.keyLengthBits, 128);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// A password retry must keep the initial absence even when no request carried it yet.
    void passwordRetryRetainsAbsentDestinationConsent()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        const QString output = files.filePath(QStringLiteral("output.pdf"));
        const QByteArray winner("another completed writer");
        QCOMPARE(QpdfService().encrypt(QStringLiteral(MERVIN_FIXTURE_PDF), source, {},
                     QStringLiteral("source password"), {}, QpdfService::Algorithm::AES256, {}),
                 QpdfService::Status::Ok);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, source, false, -1, true, QStringLiteral("source password")));
        currentTab(window)->setPassword({});
        const int guiSteps = job::testing::stepsOnMainThread();
        Interaction dialogs;
        dialogs.files << output << output;
        dialogs.passwords << QStringLiteral("wrong") << QStringLiteral("source password");
        bool attemptedWinner = false;
        bool winnerWritten = false;
        bool checkedRetained = false;
        int questions = 0;
        int completions = 0;
        QString failure;
        dialogs.intercept = [&](QWidget *modal) {
            if (!attemptedWinner && dialogs.passwordCount == 1 && qobject_cast<QInputDialog *>(modal)) {
                attemptedWinner = true;
                winnerWritten = writeBytes(output, winner);
            }
            return false;
        };
        dialogs.answer = [&](const QMessageBox &box) {
            if (box.objectName() == QStringLiteral("securityReplace")) {
                ++questions;
                return QMessageBox::Yes;
            }
            if (box.windowTitle() == QStringLiteral("Done")) {
                ++completions;
                return QMessageBox::No;
            }
            failure = box.text();
            return QMessageBox::Ok;
        };
        dialogs.security = [&](SecurityDialog &dialog) {
            const auto labels = dialog.findChildren<QLabel *>();
            QVERIFY(std::any_of(labels.cbegin(), labels.cend(), [](QLabel *label) {
                return label->text().contains(QStringLiteral("A password is required."));
            }));
            if (dialogs.securityCount == 1) {
                setEncryptionChoices(dialog);
            } else {
                verifyEncryptionChoices(dialog, output);
                QCOMPARE(bytes(output), winner);
                QCOMPARE(questions, 0);
                QCOMPARE(completions, 0);
                checkedRetained = true;
            }
            dialog.findChild<QPushButton *>(QStringLiteral("securityEncrypt"))->click();
        };
        QVERIFY(QMetaObject::invokeMethod(&window, "openSecurity"));
        QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 10000);
        QVERIFY(winnerWritten);
        QVERIFY(checkedRetained);
        QCOMPARE(dialogs.securityCount, 2);
        QCOMPARE(dialogs.passwordCount, 2);
        QCOMPARE(questions, 1);
        QVERIFY(failure.contains(QStringLiteral("The destination changed.")));
        QCOMPARE(QDir::fromNativeSeparators(dialogs.suggested.last()), output);
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        QpdfService::Info info;
        QCOMPARE(QpdfService().readInfo(output, QStringLiteral("new password"), info),
                 QpdfService::Status::Ok);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// Covers unreadable destinations before confirmation and immediately before publication.
    void failedDestinationCheckRetainsChoices_data()
    {
        QTest::addColumn<bool>("finalRead");
        QTest::newRow("initial capture") << false;
        QTest::newRow("final comparison") << true;
    }

    /// Reports an actionable error and allows a retained request to choose another output.
    void failedDestinationCheckRetainsChoices()
    {
        QFETCH(bool, finalRead);
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        const QString output = files.filePath(QStringLiteral("output.pdf"));
        const QString alternative = files.filePath(QStringLiteral("alternative.pdf"));
        const QByteArray original("keep the unchecked destination");
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        QVERIFY(writeBytes(output, original));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, source));
        const int guiSteps = job::testing::stepsOnMainThread();
        const auto failRead = [&] {
            job::testing::failWhen([output](job::Step step, const QString &path) {
                return step == job::Step::Reading && QDir::fromNativeSeparators(path) == output;
            });
        };
        const auto clearHook = qScopeGuard([] { job::testing::failWhen({}); });
        Interaction dialogs;
        dialogs.files << output << alternative;
        int questions = 0;
        int completions = 0;
        bool checkedRetained = false;
        QString failure;
        dialogs.answer = [&](const QMessageBox &box) {
            if (box.objectName() == QStringLiteral("securityReplace")) {
                ++questions;
                if (finalRead)
                    failRead();
                return QMessageBox::Yes;
            }
            if (box.windowTitle() == QStringLiteral("Done")) {
                ++completions;
                return QMessageBox::No;
            }
            failure = box.text();
            return QMessageBox::Ok;
        };
        dialogs.security = [&](SecurityDialog &dialog) {
            if (dialogs.securityCount == 1) {
                setEncryptionChoices(dialog);
                if (!finalRead)
                    failRead();
            } else {
                verifyEncryptionChoices(dialog, output);
                QCOMPARE(bytes(output), original);
                checkedRetained = true;
                job::testing::failWhen({});
            }
            dialog.findChild<QPushButton *>(QStringLiteral("securityEncrypt"))->click();
        };
        QVERIFY(QMetaObject::invokeMethod(&window, "openSecurity"));
        QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 10000);
        QVERIFY(checkedRetained);
        QCOMPARE(dialogs.securityCount, 2);
        QCOMPARE(questions, finalRead ? 1 : 0);
        QCOMPARE(failure, QStringLiteral("Could not check the destination. Try again or choose another name."));
        QVERIFY(!failure.contains(job::notRunError()));
        QCOMPARE(bytes(output), original);
        QCOMPARE(dialogs.suggested.size(), 2);
        QCOMPARE(QDir::fromNativeSeparators(dialogs.suggested.last()), output);
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        QpdfService::Info info;
        QCOMPARE(QpdfService().readInfo(alternative, QStringLiteral("new password"), info),
                 QpdfService::Status::Ok);
        QCOMPARE(info.keyLengthBits, 128);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// Covers cancellation in both the initial snapshot and final comparison.
    void stopDuringDestinationConsentRead_data()
    {
        QTest::addColumn<bool>("finalRead");
        QTest::newRow("initial capture") << false;
        QTest::newRow("final comparison") << true;
    }

    /// Stop between hash chunks keeps the source edits and old destination without late UI.
    void stopDuringDestinationConsentRead()
    {
        QFETCH(bool, finalRead);
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        const QString output = files.filePath(QStringLiteral("output.pdf"));
        const QByteArray original(3 * 1024 * 1024, 'x');
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        QVERIFY(writeBytes(output, original));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, source));
        auto *page = currentTab(window);
        page->viewer()->loadMeasurements(
            {{0, MeasureKind::Distance, {{15, 80}, {85, 80}}}}, {}, MeasureUnit::Millimeter, 2, 2);
        page->viewer()->setMeasurePrecision(3);
        QVERIFY(page->hasUnsavedEdits());
        const int running = background::running();
        const int guiSteps = job::testing::stepsOnMainThread();
        const auto armHold = [&] {
            job::testing::holdWhen([output, reads = 0](job::Step step, const QString &path) mutable {
                return step == job::Step::Reading && QDir::fromNativeSeparators(path) == output
                    && ++reads == 3;
            });
        };
        const auto release = qScopeGuard([] { job::testing::release(); });
        Interaction dialogs;
        dialogs.files.append(output);
        int questions = 0;
        dialogs.answer = [&](const QMessageBox &box) {
            if (box.objectName() == QStringLiteral("securityReplace")) {
                ++questions;
                if (finalRead)
                    armHold();
                return QMessageBox::Yes;
            }
            return QMessageBox::NoButton;
        };
        dialogs.security = [&](SecurityDialog &dialog) {
            if (!finalRead)
                armHold();
            setEncryptionChoices(dialog);
            dialog.findChild<QPushButton *>(QStringLiteral("securityEncrypt"))->click();
        };
        QVERIFY(QMetaObject::invokeMethod(&window, "openSecurity"));
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        QVERIFY(page->isBusy());
        page->stopWaiting();
        QVERIFY(page->isUsable() && page->hasUnsavedEdits());
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(background::running() <= running, 10000);
        QTest::qWait(50);
        QCOMPARE(bytes(output), original);
        QCOMPARE(dialogs.securityCount, 1);
        QCOMPARE(questions, finalRead ? 1 : 0);
        QCOMPARE(dialogs.messages.size(), questions);
        QCOMPARE(page->viewer()->committedMeasurements().size(), size_t(1));
        QCOMPARE(page->viewer()->measurePrecision(), 3);
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// Checks that reopening after write failure preserves the earlier information read error.
    void failedInformationStaysVisibleAfterAFailedWrite()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, source));
        job::testing::failWhen([source, reads = 0](job::Step step, const QString &path) mutable {
            return step == job::Step::Reading && path == source && ++reads == 1;
        });
        const auto clearHook = qScopeGuard([] { job::testing::failWhen({}); });
        Interaction dialogs;
        dialogs.files.append(files.filePath(QStringLiteral("protected.pdf")));
        dialogs.answers.append(QMessageBox::Ok);
        dialogs.security = [&](SecurityDialog &dialog) {
            const auto labels = dialog.findChildren<QLabel *>();
            QVERIFY(std::any_of(labels.cbegin(), labels.cend(), [](QLabel *label) {
                return label->text().contains(QStringLiteral("Could not read security information."));
            }));
            QVERIFY(std::none_of(labels.cbegin(), labels.cend(), [](QLabel *label) {
                return label->text().contains(QStringLiteral("not encrypted"));
            }));
            if (dialogs.securityCount == 1) {
                job::testing::failWhen([folder = files.path()](job::Step step, const QString &path) {
                    return step == job::Step::Writing && path == folder;
                });
                dialog.findChild<QPushButton *>(QStringLiteral("securityEncrypt"))->click();
            } else {
                dialog.reject();
            }
        };
        QVERIFY(QMetaObject::invokeMethod(&window, "openSecurity"));
        QTRY_COMPARE(dialogs.securityCount, 2);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// Covers Stop at metadata reads, source reads and destination writes.
    void stopSuppressesTheProbeOrWrite_data()
    {
        QTest::addColumn<QString>("phase");
        QTest::newRow("information read") << QStringLiteral("probe");
        QTest::newRow("write source") << QStringLiteral("source");
        QTest::newRow("write destination") << QStringLiteral("destination");
    }

    /// Checks usable edited source state, unchanged source bytes and no output or late UI after Stop.
    void stopSuppressesTheProbeOrWrite()
    {
        QFETCH(QString, phase);
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("plan.pdf"));
        const QString output = files.filePath(QStringLiteral("protected.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        const QByteArray original = bytes(source);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, source));
        auto *page = currentTab(window);
        page->viewer()->loadMeasurements(
            {{0, MeasureKind::Distance, {{15, 80}, {85, 80}}}}, {}, MeasureUnit::Millimeter, 2, 2);
        page->viewer()->setMeasurePrecision(3);
        QVERIFY(page->hasUnsavedEdits());
        const int running = background::running();
        job::testing::holdWhen([source, folder = files.path(), phase, reads = 0](
                                  job::Step step, const QString &path) mutable {
            if (phase == QStringLiteral("destination"))
                return step == job::Step::Writing && path == folder;
            if (step != job::Step::Reading || path != source)
                return false;
            return phase == QStringLiteral("probe") || ++reads > 1;
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        Interaction dialogs;
        if (phase != QStringLiteral("probe")) {
            dialogs.files.append(output);
            dialogs.security = [&](SecurityDialog &dialog) {
                setEncryptionChoices(dialog);
                dialog.findChild<QPushButton *>(QStringLiteral("securityEncrypt"))->click();
            };
        }
        QVERIFY(QMetaObject::invokeMethod(&window, "openSecurity"));
        QTRY_COMPARE(job::testing::held(), 1);
        QVERIFY(page->isBusy());
        QVERIFY(!qobject_cast<SecurityDialog *>(QApplication::activeModalWidget()));
        page->stopWaiting();
        QVERIFY(page->isUsable() && page->hasUnsavedEdits());
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(background::running() <= running, 10000);
        QTest::qWait(50);
        QCOMPARE(dialogs.securityCount, phase == QStringLiteral("probe") ? 0 : 1);
        QVERIFY(dialogs.messages.isEmpty());
        QVERIFY(dialogs.unexpected.isEmpty());
        QVERIFY(!QFileInfo::exists(output));
        QCOMPARE(bytes(source), original);
        QVERIFY(page->hasUnsavedEdits());
        QCOMPARE(page->viewer()->committedMeasurements().size(), size_t(1));
        QCOMPARE(page->viewer()->measurePrecision(), 3);
    }

    /// Covers source paths, open documents and their aliases as protected destinations.
    void sourceAndOpenDestinationsAreRefused_data()
    {
        QTest::addColumn<QString>("destination");
        QTest::newRow("source") << QStringLiteral("source");
        QTest::newRow("another open file") << QStringLiteral("open");
#ifdef Q_OS_UNIX
        QTest::newRow("alias of another open file") << QStringLiteral("alias");
#endif
    }

    /// Checks that protected destinations retain their exact original bytes.
    void sourceAndOpenDestinationsAreRefused()
    {
        QFETCH(QString, destination);
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        const QString other = files.filePath(QStringLiteral("other.pdf"));
        const QString alias = files.filePath(QStringLiteral("alias.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), other));
        if (destination == QStringLiteral("alias"))
            QVERIFY(QFile::link(other, alias));
        const QString output = destination == QStringLiteral("source") ? source
                             : destination == QStringLiteral("open") ? other : alias;
        const QByteArray before = bytes(output);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, other));
        QVERIFY(testing::openAndWait(window, source));
        Interaction dialogs;
        dialogs.files.append(output);
        dialogs.answers.append(QMessageBox::Ok);
        dialogs.security = [&](SecurityDialog &dialog) {
            if (dialogs.securityCount == 1)
                dialog.findChild<QPushButton *>(QStringLiteral("securityEncrypt"))->click();
            else
                dialog.reject();
        };
        QVERIFY(QMetaObject::invokeMethod(&window, "openSecurity"));
        QTRY_COMPARE(dialogs.securityCount, 2);
        QTRY_VERIFY(dialogs.answers.isEmpty());
        QVERIFY(dialogs.messages.contains(QStringLiteral("The destination is open in a tab.")));
        QCOMPARE(bytes(output), before);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// Covers information and failed writes after tab moves or owner deletion.
    void queuedResultsFollowOrDieWithTheirTab_data()
    {
        QTest::addColumn<bool>("failure");
        QTest::addColumn<bool>("moved");
        QTest::newRow("information follows moved tab") << false << true;
        QTest::newRow("failed write follows moved tab") << true << true;
        QTest::newRow("information dies with tab") << false << false;
        QTest::newRow("failed write dies with tab") << true << false;
    }

    /// Checks current-owner delivery for moved tabs and suppression when the tab is destroyed.
    void queuedResultsFollowOrDieWithTheirTab()
    {
        QFETCH(bool, failure);
        QFETCH(bool, moved);
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        const QString kept = files.filePath(QStringLiteral("kept.pdf"));
        const QString output = files.filePath(QStringLiteral("protected.pdf"));
        for (const QString &path : {source, kept})
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        WindowManager manager;
        QPointer<MainWindow> first(manager.createWindow());
        MainWindow *second = manager.createWindow();
        showWindow(*first);
        showWindow(*second);
        QVERIFY(testing::openAndWait(*second, kept));
        QVERIFY(testing::openAndWait(*first, source));
        QPointer<TabPage> page(currentTab(*first));
        job::testing::holdWhen([source, failure, reads = 0](
                                  job::Step step, const QString &path) mutable {
            return step == job::Step::Reading && path == source && (!failure || ++reads > 1);
        });
        if (failure)
            job::testing::failWhen([folder = files.path()](job::Step step, const QString &path) {
                return step == job::Step::Writing && path == folder;
            });
        const auto clearHooks = qScopeGuard([] {
            job::testing::release();
            job::testing::failWhen({});
        });
        Interaction dialogs;
        if (failure) {
            dialogs.files.append(output);
            if (moved)
                dialogs.answers.append(QMessageBox::Ok);
        }
        dialogs.security = [&](SecurityDialog &dialog) {
            if (failure && dialogs.securityCount == 1) {
                setEncryptionChoices(dialog);
                dialog.findChild<QPushButton *>(QStringLiteral("securityEncrypt"))->click();
            } else {
                QCOMPARE(dialog.parentWidget(), second);
                QCOMPARE(currentTab(*second), page.data());
                if (failure)
                    QCOMPARE(dialog.findChild<QLineEdit *>(QStringLiteral("securityUserPassword"))->text(),
                             QStringLiteral("new password"));
                dialog.reject();
            }
        };
        QVERIFY(QMetaObject::invokeMethod(first, "openSecurity"));
        QTRY_COMPARE(job::testing::held(), 1);
        QDialog guard;
        guard.setWindowModality(Qt::ApplicationModal);
        guard.show();
        job::testing::release();
        QTRY_VERIFY(page->isUsable());
        QCOMPARE(dialogs.securityCount, failure ? 1 : 0);
        if (moved)
            manager.mergeTab(first, 0, second, -1);
        delete first.data();
        guard.close();
        if (moved) {
            QTRY_COMPARE(dialogs.securityCount, failure ? 2 : 1);
            QCOMPARE(dialogs.securityOwners.last().data(), static_cast<QWidget *>(second));
            QVERIFY(page && page->window() == second);
            if (failure)
                QCOMPARE(dialogs.messageOwners.last().data(), static_cast<QWidget *>(second));
        } else {
            QTest::qWait(100);
            QVERIFY(!page);
            QCOMPARE(dialogs.securityCount, failure ? 1 : 0);
            QVERIFY(dialogs.messages.isEmpty());
        }
        QVERIFY(dialogs.unexpected.isEmpty());
        QVERIFY(!QFileInfo::exists(output));
    }

    /// Covers information and failed writes around canceled and confirmed Quit.
    void queuedResultsRespectQuit_data()
    {
        QTest::addColumn<bool>("failure");
        QTest::addColumn<bool>("cancel");
        QTest::newRow("information after canceled quit") << false << true;
        QTest::newRow("failure after canceled quit") << true << true;
        QTest::newRow("information during confirmed quit") << false << false;
        QTest::newRow("failure during confirmed quit") << true << false;
    }

    /// Checks resumed delivery after canceled Quit and no UI after confirmed shutdown.
    void queuedResultsRespectQuit()
    {
        QFETCH(bool, failure);
        QFETCH(bool, cancel);
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        const QString kept = files.filePath(QStringLiteral("kept.pdf"));
        for (const QString &path : {source, kept})
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        WindowManager manager;
        QPointer<MainWindow> first(manager.createWindow());
        QPointer<MainWindow> second(manager.createWindow());
        showWindow(*first);
        showWindow(*second);
        QVERIFY(testing::openAndWait(*first, source));
        QVERIFY(testing::openAndWait(*second, kept));
        QPointer<TabPage> page(currentTab(*first));
        if (cancel) {
            auto *viewer = currentTab(*second)->viewer();
            viewer->loadMeasurements({{0, MeasureKind::Distance, {{15, 80}, {85, 80}}}},
                                     {}, MeasureUnit::Millimeter, 2, 2);
            viewer->setMeasurePrecision(3);
            QVERIFY(currentTab(*second)->hasUnsavedEdits());
        }
        job::testing::holdWhen([source, failure, reads = 0](job::Step step, const QString &path) mutable {
            return step == job::Step::Reading && path == source && (!failure || ++reads > 1);
        });
        if (failure)
            job::testing::failWhen([folder = files.path()](job::Step step, const QString &path) {
                return step == job::Step::Writing && path == folder;
            });
        const auto clearHooks = qScopeGuard([] {
            job::testing::release();
            job::testing::failWhen({});
        });
        Interaction dialogs;
        if (failure)
            dialogs.files.append(files.filePath(QStringLiteral("protected.pdf")));
        bool quitAnswered = false;
        bool deliveredAfterShutdown = false;
        dialogs.answer = [&](const QMessageBox &box) {
            if (box.windowTitle() == QStringLiteral("Unsaved changes")) {
                quitAnswered = true;
                return QMessageBox::Cancel;
            }
            return QMessageBox::Ok;
        };
        dialogs.intercept = [&](QWidget *modal) {
            if (manager.isClosing() && (qobject_cast<SecurityDialog *>(modal)
                    || qobject_cast<QMessageBox *>(modal) || qobject_cast<QInputDialog *>(modal)))
                deliveredAfterShutdown = true;
            return false;
        };
        dialogs.security = [&](SecurityDialog &dialog) {
            if (failure && dialogs.securityCount == 1)
                dialog.findChild<QPushButton *>(QStringLiteral("securityEncrypt"))->click();
            else
                dialog.reject();
        };
        QVERIFY(QMetaObject::invokeMethod(first, "openSecurity"));
        QTRY_COMPARE(job::testing::held(), 1);
        QDialog guard;
        guard.setWindowModality(Qt::ApplicationModal);
        guard.show();
        job::testing::release();
        QTRY_VERIFY(page->isUsable());
        manager.quitAll();
        guard.close();
        if (cancel) {
            QTRY_VERIFY(quitAnswered);
            QVERIFY(!manager.isClosing());
            QTRY_COMPARE(dialogs.securityCount, failure ? 2 : 1);
            QVERIFY(first && second && page);
            QCOMPARE(dialogs.messages.size(), failure ? 2 : 1);
        } else {
            QTRY_VERIFY(manager.isClosing());
            QTRY_VERIFY(!first && !second && !page);
            QTest::qWait(50);
        }
        QVERIFY(!deliveredAfterShutdown);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// Covers owner deletion during Security, passwords, output and confirmation questions.
    void modalOwnerCanBeDestroyed_data()
    {
        QTest::addColumn<QString>("phase");
        QTest::newRow("security dialog") << QStringLiteral("dialog");
        QTest::newRow("password prompt") << QStringLiteral("password");
        QTest::newRow("output picker") << QStringLiteral("picker");
        QTest::newRow("strip question") << QStringLiteral("strip");
        QTest::newRow("replacement question") << QStringLiteral("replace");
    }

    /// Checks that all nested Security dialogs tolerate deletion of their owner.
    void modalOwnerCanBeDestroyed()
    {
        QFETCH(QString, phase);
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        const QString output = files.filePath(QStringLiteral("output.pdf"));
        const QByteArray original("keep existing destination");
        if (phase == QStringLiteral("replace"))
            QVERIFY(writeBytes(output, original));
        if (phase == QStringLiteral("password"))
            QCOMPARE(QpdfService().encrypt(QStringLiteral(MERVIN_FIXTURE_PDF), source, {},
                         QStringLiteral("source password"), {}, QpdfService::Algorithm::AES256, {}),
                     QpdfService::Status::Ok);
        else
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        WindowManager manager;
        QPointer<MainWindow> owner(manager.createWindow());
        manager.createWindow();
        showWindow(*owner);
        QVERIFY(testing::openAndWait(*owner, source, false, -1, true,
                    phase == QStringLiteral("password") ? QStringLiteral("source password") : QString()));
        if (phase == QStringLiteral("password"))
            currentTab(*owner)->setPassword({});
        bool destroyed = false;
        bool replacementHadOwner = false;
        Interaction dialogs;
        if (phase == QStringLiteral("replace"))
            dialogs.files.append(output);
        dialogs.intercept = [&](QWidget *modal) {
            if (destroyed)
                return false;
            const bool matches = phase == QStringLiteral("dialog") ? qobject_cast<SecurityDialog *>(modal) != nullptr
                : phase == QStringLiteral("password") ? qobject_cast<QInputDialog *>(modal) != nullptr
                : phase == QStringLiteral("picker") ? qobject_cast<QFileDialog *>(modal) != nullptr
                : phase == QStringLiteral("replace") ? modal && modal->objectName() == QStringLiteral("securityReplace")
                : qobject_cast<QMessageBox *>(modal) != nullptr;
            if (!matches)
                return false;
            if (phase == QStringLiteral("replace"))
                replacementHadOwner = modal->parentWidget() == owner
                    && modal->windowModality() == Qt::WindowModal;
            destroyed = true;
            delete owner.data();
            return true;
        };
        dialogs.security = [&](SecurityDialog &dialog) {
            dialog.findChild<QPushButton *>(phase == QStringLiteral("strip")
                ? QStringLiteral("securityStrip") : QStringLiteral("securityEncrypt"))->click();
        };
        QVERIFY(QMetaObject::invokeMethod(owner, "openSecurity"));
        QTRY_VERIFY(destroyed);
        QVERIFY(!owner);
        if (phase == QStringLiteral("replace")) {
            QVERIFY(replacementHadOwner);
            // The surviving manager keeps its render workers running. Only the
            // deleted owner's document must finish closing here.
            QTRY_COMPARE_WITH_TIMEOUT(Document::closing(), 0, 10000);
            QTest::qWait(50);
            QCOMPARE(bytes(output), original);
            QCOMPARE(dialogs.securityCount, 1);
            QVERIFY(dialogs.messages.isEmpty());
        }
        QVERIFY(dialogs.unexpected.isEmpty());
    }

#ifdef Q_OS_UNIX
    /// Checks that a newly redirected source cannot cause its target to be overwritten.
    void changedSourceAliasCannotOverwriteItsTarget()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        const QString target = files.filePath(QStringLiteral("target.pdf"));
        for (const QString &path : {source, target})
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        const QByteArray original = bytes(target);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, source));
        const int guiSteps = job::testing::stepsOnMainThread();
        Interaction dialogs;
        dialogs.files.append(target);
        dialogs.answers << QMessageBox::Yes << QMessageBox::Ok;
        dialogs.security = [&](SecurityDialog &dialog) {
            if (dialogs.securityCount == 1) {
                QVERIFY(QFile::remove(source));
                QVERIFY(QFile::link(target, source));
                setEncryptionChoices(dialog);
                dialog.findChild<QPushButton *>(QStringLiteral("securityEncrypt"))->click();
            } else {
                dialog.reject();
            }
        };
        QVERIFY(QMetaObject::invokeMethod(&window, "openSecurity"));
        QTRY_VERIFY(dialogs.messages.size() >= 2);
        QCOMPARE(bytes(target), original);
        QTRY_COMPARE(dialogs.securityCount, 2);
        QTRY_VERIFY(dialogs.answers.isEmpty());
        QVERIFY(dialogs.messages.contains(QStringLiteral("The destination is open in a tab.")));
        QCOMPARE(bytes(source), original);
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// Checks that a blocked named-pipe metadata read leaves the GUI responsive.
    void securityProbeReturnsWhileItsSourceDoesNotAnswer()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("plan.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1000, 800);
        window.show();
        QVERIFY(testing::openAndWait(window, source));
        auto *page = window.findChild<TabPage *>();
        QVERIFY(page && page->isUsable());
        QVERIFY(QFile::remove(source));
        QCOMPARE(::mkfifo(QFile::encodeName(source).constData(), 0600), 0);
        testing::PipeWriter writer(source, 3000);
        const int running = background::running();
        int dialogs = 0;
        QTimer reject;
        connect(&reject, &QTimer::timeout, this, [&] {
            if (auto *dialog = qobject_cast<SecurityDialog *>(QApplication::activeModalWidget())) {
                ++dialogs;
                dialog->reject();
            }
        });
        reject.start(5);
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(QMetaObject::invokeMethod(&window, "openSecurity"));
        QVERIFY2(elapsed.elapsed() < 1000, qPrintable(QStringLiteral("Security took %1 ms").arg(elapsed.elapsed())));
        QVERIFY(page->isBusy());
        page->stopWaiting();
        writer.release();
        QTRY_VERIFY_WITH_TIMEOUT(background::running() <= running, 10000);
        QTest::qWait(50);
        QCOMPARE(dialogs, 0);
        QVERIFY(page->isUsable());
    }
#endif

private:
    std::unique_ptr<QTemporaryDir> profile_;
};

QTEST_MAIN(TstSecurityWorkflow)
#include "tst_security_workflow.moc"
