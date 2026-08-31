#include "config/Settings.h"
#include "dialogs/SettingsDialog.h"

#include <QPushButton>
#include <QTest>

class TstSettingsDialog : public QObject
{
    Q_OBJECT

private slots:
    void defaultPdfControlIsWindowsOnly();
};

void TstSettingsDialog::defaultPdfControlIsWindowsOnly()
{
    SettingsDialog dialog(mervin::Settings{});
    auto *button =
        dialog.findChild<QPushButton *>(QStringLiteral("setDefaultPdfAppButton"));

#ifdef Q_OS_WIN
    QVERIFY(button);
    QCOMPARE(button->text(), QStringLiteral("Set as Default PDF App"));
#else
    QVERIFY(!button);
#endif
}

QTEST_MAIN(TstSettingsDialog)
#include "tst_settings_dialog.moc"
