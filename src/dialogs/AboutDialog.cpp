#include "dialogs/AboutDialog.h"

#include "mervin_version.h"
#include "update/UpdateChecker.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

AboutDialog::AboutDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("About Mervin PDF"));
    setMinimumWidth(440);

    auto *layout = new QVBoxLayout(this);

    auto *title = new QLabel(QStringLiteral("<h2>%1</h2>").arg(QStringLiteral(MERVIN_APP_NAME)), this);
    layout->addWidget(title);

    auto *version = new QLabel(tr("Version %1").arg(QStringLiteral(MERVIN_VERSION_STRING)), this);
    layout->addWidget(version);

    auto *desc = new QLabel(tr("A lightweight, trustworthy PDF reader for Windows."), this);
    desc->setWordWrap(true);
    layout->addWidget(desc);

    auto *privacy = new QLabel(
        tr("<p><i>Works offline. Your documents and all data - settings, recent files, "
           "OCR language data - stay on this machine. Mervin only reaches the internet "
           "if you turn on update checks.</i></p>"),
        this);
    privacy->setWordWrap(true);
    privacy->setTextFormat(Qt::RichText);
    layout->addWidget(privacy);

    // Required open-source notices (LGPL for Qt, AGPL/commercial for MuPDF), plus
    // the other direct and bundled components.
    auto *licenses = new QLabel(
        tr("<hr><p><b>Open-source components</b></p>"
           "<ul>"
           "<li><b>Qt 6</b> - GNU LGPL v3 (dynamically linked; libraries may be replaced).</li>"
           "<li><b>MuPDF</b> (Artifex) - GNU AGPL v3 or commercial. Rendering, text, OCR.</li>"
           "<li><b>qpdf</b> - Apache 2.0. Security &amp; page operations.</li>"
           "<li><b>Tesseract</b> - Apache 2.0, and <b>Leptonica</b> - BSD. Selection OCR.</li>"
           "<li><b>toml++</b> - MIT. Configuration file.</li>"
           "<li>Bundled by MuPDF: FreeType, HarfBuzz, libjpeg-turbo, OpenJPEG, "
           "jbig2dec (AGPL), zlib.</li>"
           "</ul>"
           "<p>See <i>THIRD_PARTY_LICENSES.md</i> for full licence texts.</p>"),
        this);
    licenses->setWordWrap(true);
    licenses->setTextFormat(Qt::RichText);
    licenses->setOpenExternalLinks(true);
    layout->addWidget(licenses);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    // Manual update check (ActionRole keeps the dialog open). The checker is
    // parented to the application so it outlives this dialog, and deletes itself
    // when the check finishes.
    auto *checkButton = buttons->addButton(tr("Check for Updates"), QDialogButtonBox::ActionRole);
    connect(checkButton, &QPushButton::clicked, this,
            [] { (new mervin::UpdateChecker(qApp))->checkManually(); });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);
}
