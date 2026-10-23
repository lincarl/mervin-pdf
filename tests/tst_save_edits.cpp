#include "TestWaits.h"
#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "app/WindowManager.h"
#include "io/AppFiles.h"
#include "io/Background.h"
#include "io/JobContext.h"
#include "render/AnnotModel.h"
#include "render/Document.h"
#include "render/FormModel.h"
#include "render/MeasureContent.h"
#include "render/RenderEngine.h"
#include "recent/PathKey.h"
#include "security/MeasureExport.h"
#include "ui/TabPage.h"
#include "ui/MainWindow.h"
#include "ui/ViewerWidget.h"
#include "Stalls.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <QDir>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QFocusEvent>
#include <QPlainTextEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <algorithm>

using namespace mervin;

namespace {
// Reads fixture bytes so failed or stopped Save can prove the original is unchanged.
QByteArray bytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Adds an editable note used to check snapshot retention and successful Save.
bool edit(TabPage &tab, const QString &text = QStringLiteral("preserved"))
{
    return tab.viewer()->annotModel()->addTextNote(0, {40, 40}, Qt::yellow, {}, text) >= 0;
}

// Collects Save questions and completion while supplying a chosen replacement answer.
struct Outcome {
    std::optional<TabPage::SaveCompletion> completion;
    int questions = 0;
    bool replaceChanged = true;

    // Creates checkpoint and completion callbacks that update this captured outcome.
    TabPage::SaveCallbacks callbacks()
    {
        return {[this](auto valid, auto answer) {
                    ++questions;
                    if (valid())
                        answer(replaceChanged);
                },
                [this](const auto &result) { completion = result; }};
    }
};

// Writes the fixture with `stored` as its /Mervin_Measurements stream, Flate-encoded bytes
// kept exactly as given, as another program or a newer Mervin could write them. With
// `asString` the blob is a PDF string holding `stored` instead.
void writeStoredBlob(const QString &path, const QByteArray &stored, bool asString)
{
    QPDF pdf;
    pdf.processFile(MERVIN_FIXTURE_PDF);
    QPDFObjectHandle blob = QPDFObjectHandle::newString(stored.toStdString());
    if (!asString) {
        blob = pdf.newStream();
        blob.replaceStreamData(stored.toStdString(), QPDFObjectHandle::newName("/FlateDecode"),
                               QPDFObjectHandle::newNull());
    }
    pdf.getRoot().replaceKey("/Mervin_Measurements", blob);
    const QByteArray name = path.toUtf8();
    QPDFWriter writer(pdf, name.constData());
    writer.setStreamDataMode(qpdf_s_preserve);
    writer.setDecodeLevel(qpdf_dl_none);
    writer.write();
}

// A file's stored blob and the number of annotations on page 1.
struct StoredBlob {
    QByteArray bytes; // raw stream data, or the string's value
    bool isString = false;
    int notes = 0;
};

StoredBlob storedBlobAndNotes(const QString &path)
{
    QPDF pdf;
    pdf.processFile(path.toUtf8().constData());
    QPDFObjectHandle blob = pdf.getRoot().getKey("/Mervin_Measurements");
    StoredBlob out;
    if (blob.isStream()) {
        const auto buffer = blob.getRawStreamData();
        out.bytes = QByteArray(reinterpret_cast<const char *>(buffer->getBuffer()),
                               qsizetype(buffer->getSize()));
    } else if (blob.isString()) {
        out.bytes = QByteArray::fromStdString(blob.getStringValue());
        out.isString = true;
    }
    QPDFObjectHandle annots =
        QPDFPageDocumentHelper(pdf).getAllPages().front().getObjectHandle().getKey("/Annots");
    out.notes = annots.isArray() ? annots.getArrayNItems() : 0;
    return out;
}

// Blobs Mervin cannot read: one too large, one that is not measurement data, and valid
// measurement JSON stored as a PDF string, which no Mervin release writes.
void addUnreadableBlobRows()
{
    QTest::addColumn<QByteArray>("stored");
    QTest::addColumn<bool>("asString");
    QTest::newRow("larger than the read limit")
        << qCompress(QByteArray(17 * 1024 * 1024, ' '), 9).mid(4) // zlib data after the size
        << false;
    QTest::newRow("not measurement data") << qCompress(QByteArray("not JSON"), 9).mid(4) << false;
    MeasureDoc doc;
    Measurement mark;
    mark.page = 0;
    mark.kind = MeasureKind::Distance;
    mark.pts = {QPointF(5, 5), QPointF(55, 5)};
    doc.measurements.push_back(mark);
    QTest::newRow("stored as a string") << serializeMeasurements(doc) << true;
}

// Lists private recovery artifacts to detect files left after terminal cleanup.
QStringList recoveryEntries()
{
    return QDir(QDir(ConfigPaths::configDir()).filePath(QStringLiteral("recovery")))
        .entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
}

// Two pages share text and choice fields with authored appearances. Page 1 also
// has an unrelated editor, so refreshing siblings must preserve its pending text.
void writeSharedForm(const QString &path, bool listBox = false)
{
    QPDF pdf;
    pdf.emptyPDF();
    auto font = pdf.makeIndirectObject(QPDFObjectHandle::parse(
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"));
    auto fonts = QPDFObjectHandle::newDictionary();
    fonts.replaceKey("/Helv", font);
    auto resources = QPDFObjectHandle::newDictionary();
    resources.replaceKey("/Font", fonts);
    auto fields = QPDFObjectHandle::newArray();
    auto text = pdf.makeIndirectObject(QPDFObjectHandle::parse(
        "<< /FT /Tx /T (shared-text) /V (Old) /Kids [] >>"));
    auto choice = pdf.makeIndirectObject(QPDFObjectHandle::parse(
        "<< /FT /Ch /Ff 131072 /T (shared-choice) /V (Red) /Opt [(Red) (Green)] /Kids [] >>"));
    if (listBox) {
        choice.replaceKey("/Ff", QPDFObjectHandle::newInteger(0));
        choice.replaceKey("/TI", QPDFObjectHandle::newInteger(1));
    }
    fields.appendItem(text);
    fields.appendItem(choice);
    auto form = QPDFObjectHandle::parse("<< /DA (/Helv 12 Tf 0 g) >>");
    form.replaceKey("/Fields", fields);
    form.replaceKey("/DR", resources);
    pdf.getRoot().replaceKey("/AcroForm", form);
    QPDFPageDocumentHelper pages(pdf);
    for (int p = 0; p < 2; ++p) {
        auto page = pdf.makeIndirectObject(QPDFObjectHandle::parse(
            "<< /Type /Page /MediaBox [0 0 300 300] /Resources << >> /Annots [] >>"));
        pages.addPage(QPDFPageObjectHelper(page), false);
        int index = 0;
        for (auto field : {text, choice}) {
            auto appearance = pdf.newStream(index == 0
                ? "BT /Helv 12 Tf 2 5 Td (Old) Tj ET\n"
                : "BT /Helv 12 Tf 2 5 Td (Red) Tj ET\n");
            appearance.getDict().replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
            appearance.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));
            appearance.getDict().replaceKey("/BBox", QPDFObjectHandle::parse("[0 0 260 20]"));
            appearance.getDict().replaceKey("/Resources", resources);
            auto ap = QPDFObjectHandle::newDictionary();
            ap.replaceKey("/N", appearance);
            auto widget = pdf.makeIndirectObject(QPDFObjectHandle::parse(
                "<< /Type /Annot /Subtype /Widget /F 4 >>"));
            widget.replaceKey("/Parent", field);
            widget.replaceKey("/Rect", QPDFObjectHandle::parse(
                index++ == 0 ? "[20 250 280 270]" : "[20 210 280 230]"));
            widget.replaceKey("/AP", ap);
            if (p == 0)
                widget.replaceKey("/P", page);
            field.getKey("/Kids").appendItem(widget);
            page.getKey("/Annots").appendItem(widget);
        }
        if (p == 0) {
            auto other = pdf.makeIndirectObject(QPDFObjectHandle::parse(
                "<< /Type /Annot /Subtype /Widget /FT /Tx /T (other) /V (Untouched) "
                "/F 4 /Rect [20 170 280 190] >>"));
            fields.appendItem(other);
            page.getKey("/Annots").appendItem(other);
        }
    }
    const QByteArray name = path.toUtf8();
    QPDFWriter writer(pdf, name.constData());
    writer.write();
}

// Inspect saved appearance bytes without a renderer synthesising missing /AP.
QByteArray savedAppearance(QPDFObjectHandle widget)
{
    auto stream = widget.getKey("/AP").getKey("/N");
    if (!stream.isStream())
        return {};
    const auto data = stream.getStreamData(qpdf_dl_all);
    return QByteArray(reinterpret_cast<const char *>(data->getBuffer()), qsizetype(data->getSize()));
}
}

// Checks background Save durability, cancellation, restored edits and completion ownership.
class TstSaveEdits : public QObject
{
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> profile_;

private slots:
    // Creates an isolated profile and resets Save stall hooks for each case.
    void init()
    {
        profile_ = std::make_unique<QTemporaryDir>();
        QVERIFY(profile_->isValid());
        ConfigPaths::setOverrideDir(profile_->path());
        Settings settings;
        settings.autoUpdate = false;
        QVERIFY(settings.save());
        TabPage::setSlowDelayForTesting(0);
    }

    // Releases stalls and settles closes and profile writes before resetting overrides.
    void cleanup()
    {
        job::testing::release();
        job::testing::failWhen({});
        QVERIFY(background::settle(10000));
        QCoreApplication::processEvents();
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(appfiles::flush(10000));
        ConfigPaths::setOverrideDir({});
        TabPage::setSlowDelayForTesting(TabPage::kSlowMs);
        profile_.reset();
    }

    // Direct Save must commit the open editor before its clean-document shortcut.
    void saveCommitsOpenCleanAnnotationEditor()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("annotation.pdf"));
        RenderEngine engine;
        {
            auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
            QVERIFY(doc);
            AnnotModel model(*doc);
            QVERIFY(model.addTextNote(0, {40, 40}, Qt::yellow, {}, QStringLiteral("Before")) >= 0);
            QVERIFY(doc->savePdfTo(path));
        }
        TabPage tab(&engine);
        tab.setAttribute(Qt::WA_DontShowOnScreen);
        tab.resize(900, 800);
        tab.show();
        QVERIFY(tab.open(path));
        auto *viewer = tab.viewer();
        const auto annots = viewer->annotModel()->pageAnnots(0);
        QCOMPARE(annots.size(), size_t(1));
        viewer->setCommentToolEnabled(true);
        viewer->setAnnotSubMode(AnnotSubMode::Note);
        viewer->revealAnnotation(0, annots[0].id);
        auto *editor = viewer->viewport()->findChild<QPlainTextEdit *>();
        QVERIFY(editor && editor->isVisible() && !editor->isReadOnly());
        editor->setPlainText(QStringLiteral("Only the open editor has this text"));
        QVERIFY(!tab.hasUnsavedEdits());
        QVERIFY(!viewer->annotModel()->isDirty());
        QCOMPARE(viewer->annotModel()->annot(0, annots[0].id)->contents, QStringLiteral("Before"));
        Outcome outcome;
        tab.startSave(outcome.callbacks());
        QTRY_VERIFY_WITH_TIMEOUT(outcome.completion.has_value(), 10000);
        QCOMPARE(outcome.completion->result, TabPage::SaveResult::Saved);
        QVERIFY(!tab.hasUnsavedEdits());
        auto reopened = engine.openDocument(path);
        QVERIFY(reopened);
        const auto saved = AnnotModel(*reopened).pageAnnots(0);
        QCOMPARE(saved.size(), size_t(1));
        QCOMPARE(saved[0].contents, QStringLiteral("Only the open editor has this text"));
    }

    void sharedFieldAppearancesPersistWithoutRevisiting_data()
    {
        QTest::addColumn<bool>("listBox");
        QTest::newRow("combo") << false;
        QTest::newRow("list") << true;
    }

    // A cold sibling appearance must be written correctly before any revisit.
    void sharedFieldAppearancesPersistWithoutRevisiting()
    {
        QFETCH(bool, listBox);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("shared.pdf"));
        writeSharedForm(path, listBox);
        RenderEngine engine;
        for (int field = 0; field < 2; ++field) {
            auto doc = engine.openDocument(path);
            QVERIFY(doc);
            FormModel model(*doc);
            QCOMPARE(model.pageFields(0).size(), size_t(3));
            QCOMPARE(model.pageFields(1).size(), size_t(2));
            const QString value = field == 0 ? QStringLiteral("Changed") : QStringLiteral("Green");
            QVERIFY(field == 0 ? model.setTextValue(0, 0, value)
                               : model.setChoiceValue(0, 1, value));
            const QString output = files.filePath(QStringLiteral("saved-%1.pdf").arg(field));
            QVERIFY(doc->savePdfTo(output));
            QPDF before, after;
            before.processFile(path.toUtf8().constData());
            after.processFile(output.toUtf8().constData());
            auto oldPages = QPDFPageDocumentHelper(before).getAllPages();
            auto newPages = QPDFPageDocumentHelper(after).getAllPages();
            QByteArray first;
            for (int p = 0; p < 2; ++p) {
                auto widget = newPages[p].getObjectHandle().getKey("/Annots").getArrayItem(field);
                const auto oldAppearance = savedAppearance(
                    oldPages[p].getObjectHandle().getKey("/Annots").getArrayItem(field));
                const auto appearance = savedAppearance(widget);
                QVERIFY(!appearance.isEmpty());
                QVERIFY(appearance != oldAppearance);
                if (p == 0)
                    first = appearance;
                else
                    QCOMPARE(appearance, first);
                if (field == 1 && listBox) {
                    auto parent = widget.getKey("/Parent");
                    QCOMPARE(parent.getKey("/FT").getName(), std::string("/Ch"));
                    QCOMPARE(parent.getKey("/Ff").getIntValue(), 0);
                    QCOMPARE(parent.getKey("/V").getUTF8Value(), std::string("Green"));
                    QCOMPARE(parent.getKey("/TI").getIntValue(), 1);
                    QCOMPARE(parent.getKey("/Opt").getArrayNItems(), 2);
                    QVERIFY(!widget.hasKey("/Opt"));
                    QVERIFY(!widget.hasKey("/TI"));
                }
            }
        }
    }

    void unreadableMeasurementsSurviveSave_data() { addUnreadableBlobRows(); }

    // Editing either page refreshes sibling controls and both cached page tiles.
    void sharedFormEditorsAndTilesRefreshBeforeSave()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("shared.pdf"));
        writeSharedForm(path);
        RenderEngine engine;
        TabPage tab(&engine);
        tab.setAttribute(Qt::WA_DontShowOnScreen);
        tab.resize(900, 900);
        tab.show();
        QVERIFY(tab.open(path));
        auto *viewer = tab.viewer();
        viewer->setZoomEaseMs(0);
        viewer->setLayoutMode({ViewLayout::Scroll::Continuous, false});
        viewer->setPageTheme(PageTheme::Light);
        viewer->setScale(1);
        viewer->setFormMode(true);
        const auto lineEditors = [&] {
            auto editors = viewer->viewport()->findChildren<QLineEdit *>(Qt::FindDirectChildrenOnly);
            std::sort(editors.begin(), editors.end(), [](QLineEdit *a, QLineEdit *b) {
                return a->y() < b->y();
            });
            return editors;
        };
        QTRY_COMPARE(lineEditors().size(), 3);
        const QRect firstField = lineEditors()[0]->geometry();
        const QRect siblingField = lineEditors()[2]->geometry();
        QCOMPARE(viewer->pageUnder(firstField.center()), 0);
        QCOMPARE(viewer->pageUnder(siblingField.center()), 1);
        viewer->setFormMode(false);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const auto settled = [&] {
            viewer->resetPaintStats();
            viewer->viewport()->grab();
            const auto stats = viewer->paintStats();
            return stats.fresh == 2 && stats.blank == 0 && stats.preview == 0;
        };
        QTRY_VERIFY_WITH_TIMEOUT(settled(), 10000);
        const QImage before = viewer->viewport()->grab().toImage();
        QVERIFY(!before.isNull());
        viewer->setFormMode(true);
        auto editors = lineEditors();
        QCOMPARE(editors.size(), 3);
        editors[1]->setText(QStringLiteral("Independent pending edit"));
        editors[0]->setText(QStringLiteral("Changed shared text"));
        QFocusEvent commit(QEvent::FocusOut);
        QCoreApplication::sendEvent(editors[0], &commit);
        QCOMPARE(editors[2]->text(), QStringLiteral("Changed shared text"));
        QCOMPARE(editors[1]->text(), QStringLiteral("Independent pending edit"));
        QCOMPARE(viewer->formModel()->pageFields(0)[2].value, QStringLiteral("Untouched"));
        auto choices = viewer->viewport()->findChildren<QComboBox *>(Qt::FindDirectChildrenOnly);
        QCOMPARE(choices.size(), 2);
        choices[0]->setCurrentIndex(1);
        QVERIFY(QMetaObject::invokeMethod(choices[0], "activated", Qt::DirectConnection, Q_ARG(int, 1)));
        QCOMPARE(choices[1]->currentText(), QStringLiteral("Green"));
        QCOMPARE(editors[1]->text(), QStringLiteral("Independent pending edit"));
        viewer->setFormMode(false);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY_WITH_TIMEOUT(settled(), 10000);
        const QImage after = viewer->viewport()->grab().toImage();
        QVERIFY(!after.isNull());
        const auto fieldPixels = [](const QImage &image, const QRect &rect) {
            const qreal ratio = image.devicePixelRatio();
            return image.copy(qRound(rect.x() * ratio), qRound(rect.y() * ratio),
                              qRound(rect.width() * ratio), qRound(rect.height() * ratio));
        };
        QVERIFY(fieldPixels(after, firstField) != fieldPixels(before, firstField));
        QVERIFY(fieldPixels(after, siblingField) != fieldPixels(before, siblingField));
        Outcome outcome;
        tab.startSave(outcome.callbacks());
        QTRY_VERIFY_WITH_TIMEOUT(outcome.completion.has_value(), 10000);
        QCOMPARE(outcome.completion->result, TabPage::SaveResult::Saved);
        auto reopened = engine.openDocument(path);
        QVERIFY(reopened);
        FormModel form(*reopened);
        for (int page = 0; page < 2; ++page) {
            QCOMPARE(form.pageFields(page)[0].value, QStringLiteral("Changed shared text"));
            QCOMPARE(form.pageFields(page)[1].value, QStringLiteral("Green"));
        }
        QCOMPARE(form.pageFields(0)[2].value, QStringLiteral("Independent pending edit"));
    }

    // Checks that Save with edits keeps a stored blob Mervin could not read byte for byte,
    // or a string blob as the same string.
    void unreadableMeasurementsSurviveSave()
    {
        QFETCH(QByteArray, stored);
        QFETCH(bool, asString);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("measured.pdf"));
        writeStoredBlob(path, stored, asString);
        const StoredBlob before = storedBlobAndNotes(path);
        QCOMPARE(before.bytes, stored);
        QCOMPARE(before.isString, asString);
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        Outcome outcome;
        tab.startSave(outcome.callbacks());
        QTRY_VERIFY_WITH_TIMEOUT(outcome.completion.has_value(), 10000);
        QCOMPARE(outcome.completion->result, TabPage::SaveResult::Saved);
        const StoredBlob after = storedBlobAndNotes(path);
        QVERIFY(after.notes > before.notes); // the note and its popup were saved
        QCOMPARE(after.bytes, stored);
        QCOMPARE(after.isString, asString);
    }

    void unreadableMeasurementsOfferNoMeasuring_data() { addUnreadableBlobRows(); }

    // Checks that no tool path turns measuring on for a blob Mervin could not read, since
    // Save would keep the stored blob and lose new measurements.
    void unreadableMeasurementsOfferNoMeasuring()
    {
        QFETCH(QByteArray, stored);
        QFETCH(bool, asString);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("measured.pdf"));
        writeStoredBlob(path, stored, asString);
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        ViewerWidget *viewer = tab.viewer();
        viewer->setMeasureMode(true);
        QVERIFY(!viewer->measureToolEnabled());
        auto state = viewer->captureResumeState();
        state.measureEnabled = true;
        state.tool = ViewerWidget::ToolMode::Measure;
        viewer->restoreResumeState(state);
        QVERIFY(!viewer->measureToolEnabled());
        QVERIFY(!viewer->measureMode());
        // A readable document still measures.
        const QString plain = files.filePath(QStringLiteral("plain.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), plain));
        TabPage other(&engine);
        QVERIFY(other.open(plain));
        other.viewer()->setMeasureMode(true);
        QVERIFY(other.viewer()->measureToolEnabled());
    }

    // Checks durable recovery precedes original close and retains full viewer state.
    void loadedSaveRestoresFullStateAndCommitsRecoveryBeforeClosing()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        auto state = tab.viewer()->captureResumeState();
        state.tool = ViewerWidget::ToolMode::Measure;
        state.measureEnabled = true;
        state.measureKind = MeasureKind::Polyline;
        state.inProgressPage = 0;
        state.inProgress = {{20, 20}, {40, 40}};
        state.view.scale = 1.75;
        state.view.zoomMode = QStringLiteral("custom");
        tab.viewer()->restoreResumeState(state);
        const auto expected = tab.viewer()->captureResumeState();
        const auto lifetime = tab.viewer()->document()->lifetime();
        std::atomic<bool> manifestSeen{false}, closeSeen{false};
        job::testing::failWhen([&](job::Step step, const QString &destination) {
            if (step == job::Step::Commit && destination.endsWith(QStringLiteral(".json"))) {
                std::lock_guard<std::timed_mutex> lock(lifetime->mutex);
                manifestSeen = lifetime->document != nullptr;
            }
            if (step == job::Step::Commit && destination == path) {
                std::lock_guard<std::timed_mutex> lock(lifetime->mutex);
                const auto entries = recoveryEntries();
                closeSeen = lifetime->document == nullptr
                    && std::any_of(entries.cbegin(), entries.cend(), [](const auto &name) {
                        return name.endsWith(QStringLiteral(".pdf.json"));
                    });
            }
            return false;
        });
        const int guiSteps = job::testing::stepsOnMainThread();
        Outcome outcome;
        QElapsedTimer action;
        action.start();
        tab.startSave(outcome.callbacks());
        QVERIFY(action.elapsed() < 1000);
        QTRY_VERIFY_WITH_TIMEOUT(outcome.completion.has_value(), 10000);
        QCOMPARE(outcome.completion->result, TabPage::SaveResult::Saved);
        QVERIFY(manifestSeen);
        QVERIFY(closeSeen);
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        QVERIFY(tab.isUsable());
        QVERIFY(!tab.hasUnsavedEdits());
        const auto restored = tab.viewer()->captureResumeState();
        QCOMPARE(restored.inProgress, expected.inProgress);
        QCOMPARE(restored.inProgressPage, expected.inProgressPage);
        QCOMPARE(restored.measureKind, expected.measureKind);
        QCOMPARE(restored.tool, expected.tool);
        QCOMPARE(restored.view.scale, expected.view.scale);
        QCOMPARE(tab.viewer()->annotModel()->pageAnnots(0).size(), size_t(1));
        auto saved = engine.openDocument(path);
        QVERIFY(saved);
        QCOMPARE(AnnotModel(*saved).pageAnnots(0).front().contents, QStringLiteral("preserved"));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
        QCOMPARE(QDir(files.path()).entryList({QStringLiteral(".mervin-save-*")}, QDir::Files | QDir::Hidden).size(), 0);
    }

    // Keeps one captured recovery folder when the profile changes during a held Save.
    void loadedSaveUsesItsCapturedRecoveryDirectory()
    {
        QTemporaryDir files;
        QTemporaryDir changedProfile;
        QVERIFY(files.isValid());
        QVERIFY(changedProfile.isValid());
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        const QByteArray original = bytes(path);
        const QString initialProfile = ConfigPaths::overrideDir();
        const QString initialRecovery = QDir(initialProfile).filePath(QStringLiteral("recovery"));
        const QString changedRecovery = changedProfile.filePath(QStringLiteral("recovery"));
        RenderEngine engine;
        auto tab = std::make_unique<TabPage>(&engine);
        QVERIFY(tab->open(path));
        QVERIFY(edit(*tab));
        engine.shutdown();
        QVERIFY(background::settle(10000));
        Outcome outcome;
        std::atomic<bool> reportedInitial{false}, reportedChanged{false};
        const auto restore = qScopeGuard([&] {
            job::testing::release();
            tab.reset();
            engine.shutdown();
            const bool settled = background::settle(10000);
            const bool closed = Document::waitForCloses(10000);
            const bool flushed = appfiles::flush(10000);
            job::testing::failWhen({});
            QVERIFY(settled && closed && flushed);
            ConfigPaths::setOverrideDir(initialProfile);
        });
        job::testing::holdWhen([path](job::Step step, const QString &file) {
            return step == job::Step::Reading && file == path;
        });
        job::testing::failWhen([&](job::Step step, const QString &file) {
            if (step == job::Step::Writing) {
                if (file == initialRecovery)
                    reportedInitial = true;
                if (file == changedRecovery)
                    reportedChanged = true;
            }
            // Keep the adopted copy so the assertion can inspect its durable location.
            return step == job::Step::Commit && file == path;
        });
        tab->startSave(outcome.callbacks());
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        // The only active worker is held before its first recovery-folder read.
        QCOMPARE(background::running(), 1);
        ConfigPaths::setOverrideDir(changedProfile.path());
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(outcome.completion.has_value(), 10000);
        QCOMPARE(outcome.completion->result, TabPage::SaveResult::Failed);
        QTRY_VERIFY_WITH_TIMEOUT(tab->isUsable(), 10000);
        QVERIFY(background::settle(10000));
        ConfigPaths::setOverrideDir(initialProfile);
        QVERIFY(tab->hasUnsavedEdits());
        QVERIFY(tab->hasRecoverySnapshot());
        QCOMPARE(bytes(path), original);
        QCOMPARE(QFileInfo(tab->recoveryPath()).absolutePath(), initialRecovery);
        QVERIFY(QFileInfo::exists(tab->recoveryPath()));
        QVERIFY(QFileInfo::exists(tab->recoveryPath() + QStringLiteral(".json")));
        QVERIFY(reportedInitial);
        QVERIFY(!reportedChanged);
        QVERIFY(!QFileInfo::exists(changedRecovery));
    }

    // Selects the document gate or its access mutex for a real worker stall.
    void actualDocumentStall_data()
    {
        QTest::addColumn<bool>("gate");
        QTest::newRow("gate") << true;
        QTest::newRow("access") << false;
    }

    // Checks the real Save command returns while document access is held elsewhere.
    void saveCommandRunsOffGuiWithActualAccessStall()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        window.resize(1000, 800);
        window.show();
        QVERIFY(window.openFile(path));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        QTRY_VERIFY(tab->isUsable());
        QVERIFY(edit(*tab));
        const auto lifetime = tab->viewer()->document()->lifetime();
        job::testing::holdWhen([path](job::Step step, const QString &file) {
            return step == job::Step::Reading && file == path;
        });
        std::unique_ptr<testing::Stall<std::mutex>> access;
        connect(tab, &TabPage::stateChanged, &window, [&] {
            if (tab->isBusy() && !access)
                access = std::make_unique<testing::Stall<std::mutex>>(
                    tab->viewer()->document()->accessMutex(), 10000);
        });
        const auto release = qScopeGuard([&] {
            job::testing::release();
            if (access)
                access->release();
        });
        const int guiSteps = job::testing::stepsOnMainThread();
        QElapsedTimer command;
        command.start();
        QVERIFY(QMetaObject::invokeMethod(&window, "saveMeasurements"));
        QVERIFY(command.elapsed() < 1000);
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        QVERIFY(access);
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        auto *still = tab->findChild<QWidget *>(QStringLiteral("operationStill"));
        QVERIFY(still && !still->isHidden());
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(lifetime->holds.load() > 0, 10000);
        tab->stopWaiting();
        QVERIFY(tab->isBusy());
        access->release();
        QTRY_VERIFY_WITH_TIMEOUT(tab->isUsable(), 10000);
        QVERIFY(tab->hasUnsavedEdits());
    }

    // Selects canceled and accepted changed-source questions after an owner move.
    void changedQuestionSurvivesItsQueuedOwnersDestruction_data()
    {
        QTest::addColumn<bool>("replace");
        QTest::newRow("replace") << true;
        QTest::newRow("cancel") << false;
    }

    // Checks a queued replacement question follows the tab past old-window destruction.
    void changedQuestionSurvivesItsQueuedOwnersDestruction()
    {
        QFETCH(bool, replace);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        WindowManager manager;
        const QPointer<MainWindow> first = manager.createWindow();
        MainWindow *second = manager.createWindow();
        QVERIFY(first->openFile(path));
        const QPointer<TabPage> page = first->findChild<TabPage *>();
        QTRY_VERIFY(page && page->isUsable());
        QVERIFY(edit(*page));
        QFile changed(path);
        QVERIFY(changed.open(QIODevice::Append));
        QVERIFY(changed.write("\n% External edit\n") > 0);
        changed.close();
        const QByteArray external = bytes(path);
        QDialog guard(second);
        guard.setWindowModality(Qt::WindowModal);
        guard.show();
        QVERIFY(QMetaObject::invokeMethod(first, "saveMeasurements"));
        QTRY_VERIFY(MainWindow::testing::hasQueuedTabChanges(*first));
        second->adoptTab(first->releaseTab(0));
        delete first.data();
        QVERIFY(!first);
        int questions = 0;
        QPointer<QWidget> shownIn;
        QTimer dialogs;
        connect(&dialogs, &QTimer::timeout, this, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box || box->windowTitle() != QStringLiteral("File changed"))
                return;
            ++questions;
            shownIn = box->parentWidget();
            box->done(replace ? QMessageBox::Yes : QMessageBox::Cancel);
        });
        dialogs.start(5);
        guard.close();
        QTRY_VERIFY_WITH_TIMEOUT(questions == 1 && !page->isBusy(), 10000);
        QCOMPARE(shownIn.data(), second);
        QCOMPARE(page->hasUnsavedEdits(), !replace);
        if (!replace)
            QCOMPARE(bytes(path), external);
        else
            QVERIFY(bytes(path) != external);
    }

    // Selects successful and failed Save reports after an owner move.
    void saveReportSurvivesItsQueuedOwnersDestruction_data()
    {
        QTest::addColumn<bool>("failed");
        QTest::newRow("Saved status") << false;
        QTest::newRow("Save error") << true;
    }

    // Checks Save status is delivered only by the surviving current window.
    void saveReportSurvivesItsQueuedOwnersDestruction()
    {
        QFETCH(bool, failed);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        WindowManager manager;
        const QPointer<MainWindow> first = manager.createWindow();
        MainWindow *second = manager.createWindow();
        QVERIFY(first->openFile(path));
        const QPointer<TabPage> page = first->findChild<TabPage *>();
        QTRY_VERIFY(page && page->isUsable());
        QVERIFY(edit(*page));
        if (failed)
            job::testing::failWhen([path](job::Step step, const QString &file) {
                return step == job::Step::Commit && file == path;
            });
        QDialog guard(second);
        guard.setWindowModality(Qt::WindowModal);
        guard.show();
        QVERIFY(QMetaObject::invokeMethod(first, "saveMeasurements"));
        QTRY_VERIFY_WITH_TIMEOUT(!page->isBusy()
            && MainWindow::testing::hasQueuedTabChanges(*first), 10000);
        second->adoptTab(first->releaseTab(0));
        delete first.data();
        QVERIFY(!first);
        int reports = 0;
        QPointer<QWidget> shownIn;
        QTimer dialogs;
        connect(&dialogs, &QTimer::timeout, this, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box || box->windowTitle() != QStringLiteral("Save"))
                return;
            ++reports;
            shownIn = box->parentWidget();
            box->done(QMessageBox::Ok);
        });
        dialogs.start(5);
        guard.close();
        if (failed) {
            QTRY_COMPARE_WITH_TIMEOUT(reports, 1, 10000);
            QCOMPARE(shownIn.data(), second);
            QVERIFY(page->hasUnsavedEdits());
        } else {
            const QString saved = QStringLiteral("Saved %1").arg(QDir::toNativeSeparators(path));
            QTRY_VERIFY_WITH_TIMEOUT([&] {
                for (auto *label : second->findChildren<QLabel *>())
                    if (label->text() == saved)
                        return true;
                return false;
            }(), 10000);
            QVERIFY(!page->hasUnsavedEdits());
            QCOMPARE(reports, 0);
        }
    }

    // Checks the warning distinguishes successful replacement from failed reopening.
    void reopenFailureReportsTheCommittedOriginal()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        const QByteArray original = bytes(path);
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(window->openFile(path));
        const QPointer<TabPage> page = window->findChild<TabPage *>();
        QTRY_VERIFY(page && page->isUsable());
        QVERIFY(edit(*page));
        std::atomic<bool> reopenFailed{false};
        job::testing::failWhen([path, &reopenFailed, opensAtCommit = -1]
                              (job::Step step, const QString &file) mutable {
            if (step == job::Step::Commit && file == path)
                opensAtCommit = RenderEngine::testing::opens();
            if (opensAtCommit >= 0 && RenderEngine::testing::opens() > opensAtCommit
                && step == job::Step::Reading && file == path) {
                reopenFailed = true;
                return true;
            }
            return false;
        });
        job::testing::holdWhen([&reopenFailed](job::Step step, const QString &file) {
            return reopenFailed && step == job::Step::Reading
                && file.contains(QStringLiteral("/recovery/"));
        });
        const auto release = qScopeGuard([] {
            job::testing::release();
            job::testing::failWhen({});
        });
        QString warning;
        QTimer dialogs;
        connect(&dialogs, &QTimer::timeout, this, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box || box->windowTitle() != QStringLiteral("Save"))
                return;
            warning = box->text();
            box->done(QMessageBox::Ok);
        });
        dialogs.start(5);
        QVERIFY(QMetaObject::invokeMethod(window, "saveMeasurements"));
        QTRY_VERIFY_WITH_TIMEOUT(!warning.isEmpty(), 10000);
        QVERIFY(bytes(path) != original);
        QVERIFY(page->isLoading());
        QVERIFY(page->hasRecoverySnapshot());
        QVERIFY2(warning.startsWith(QStringLiteral(
                     "The file was saved, but could not be reopened.")), qPrintable(warning));
        QVERIFY(!warning.contains(QStringLiteral("remains open")));
        QVERIFY(warning.contains(QDir::toNativeSeparators(page->recoveryPath())));
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(page->isUsable(), 10000);
        QVERIFY(page->hasUnsavedEdits());
    }

    // Checks worker dispatch, independent editing and completion through controlled stalls.
    void actualDocumentStall()
    {
        QFETCH(bool, gate);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        const QByteArray original = bytes(path);
        RenderEngine engine;
        TabPage tab(&engine);
        tab.resize(1000, 800);
        tab.show();
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        const auto lifetime = tab.viewer()->document()->lifetime();
        Document *document = tab.viewer()->document();
        job::testing::holdWhen([path](job::Step step, const QString &file) {
            return step == job::Step::Reading && file == path;
        });
        const int guiSteps = job::testing::stepsOnMainThread();
        Outcome outcome;
        QElapsedTimer action;
        action.start();
        tab.startSave(outcome.callbacks());
        QVERIFY(action.elapsed() < 1000);
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        auto *still = tab.findChild<QWidget *>(QStringLiteral("operationStill"));
        QVERIFY(still && !still->isHidden());
        std::unique_ptr<testing::Stall<std::timed_mutex>> gateStall;
        std::unique_ptr<testing::Stall<std::mutex>> accessStall;
        if (gate)
            gateStall = std::make_unique<testing::Stall<std::timed_mutex>>(lifetime->mutex, 10000);
        else
            accessStall = std::make_unique<testing::Stall<std::mutex>>(document->accessMutex(), 10000);
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(lifetime->holds.load() > 0, 10000);
        int heartbeats = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
        heartbeat.start(10);
        QTRY_VERIFY_WITH_TIMEOUT(heartbeats >= 5, 1000);
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        action.restart();
        tab.stopWaiting();
        QVERIFY(action.elapsed() < 1000);
        QVERIFY(outcome.completion.has_value());
        QCOMPARE(outcome.completion->result, TabPage::SaveResult::Stopped);
        if (!gate) {
            QVERIFY(tab.isBusy());
            auto *stop = tab.findChild<QPushButton *>(QStringLiteral("waitBarStop"));
            QVERIFY(stop && stop->isHidden());
        }
        if (gateStall)
            gateStall->release();
        if (accessStall)
            accessStall->release();
        QTRY_VERIFY_WITH_TIMEOUT(tab.isUsable(), 10000);
        QVERIFY(tab.hasUnsavedEdits());
        QCOMPARE(bytes(path), original);
        QVERIFY(!tab.hasRecoverySnapshot());
    }

    // Checks Stop retains snapshot lock and edits until the held worker releases access.
    void stopDuringSnapshotKeepsTheTrappedNoticeUntilRelease()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        job::testing::holdWhen([](job::Step step, const QString &folder) {
            return step == job::Step::Writing && folder.contains(QStringLiteral("/.save-"));
        });
        Outcome outcome;
        tab.startSave(outcome.callbacks());
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        tab.stopWaiting();
        QVERIFY(tab.isBusy());
        QVERIFY(outcome.completion.has_value());
        auto *text = tab.findChild<QLabel *>(QStringLiteral("waitBarText"));
        auto *stop = tab.findChild<QPushButton *>(QStringLiteral("waitBarStop"));
        QVERIFY(text && text->text().contains(QStringLiteral("document stays locked")));
        QVERIFY(stop && stop->isHidden());
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(tab.isUsable(), 10000);
        QVERIFY(tab.hasUnsavedEdits());
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(recoveryEntries().isEmpty());
    }

    // Selects stops before and during guarded original replacement.
    void stoppedReplacementRecoversAndRetryWaits_data()
    {
        QTest::addColumn<bool>("commitStarted");
        QTest::newRow("before commit") << false;
        QTest::newRow("commit started") << true;
    }

    // Checks recovery remains editable and retry respects the earlier replacement worker.
    void stoppedReplacementRecoversAndRetryWaits()
    {
        QFETCH(bool, commitStarted);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        const QByteArray original = bytes(path);
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        if (commitStarted)
            job::testing::holdCommitWhen([path](const QString &file) { return file == path; });
        else
            job::testing::holdWhen([path](job::Step step, const QString &file) {
                return step == job::Step::Commit && file == path;
            });
        Outcome stopped;
        tab.startSave(stopped.callbacks());
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        QVERIFY(tab.hasRecoverySnapshot());
        const QString firstCopy = tab.recoveryPath();
        tab.stopWaiting();
        QTRY_VERIFY_WITH_TIMEOUT(tab.isUsable(), 10000);
        QVERIFY(tab.hasUnsavedEdits());
        QCOMPARE(stopped.completion->result, TabPage::SaveResult::Stopped);
        QCOMPARE(bytes(path), original);
        QVERIFY(edit(tab, QStringLiteral("newer")));
        Outcome retry;
        tab.startSave(retry.callbacks());
        // Observe noncompletion while the old worker remains held. Busy and the
        // wait notice also describe startup, so neither proves this wait began.
        QTest::qWait(100);
        QVERIFY(tab.isBusy());
        QVERIFY(!retry.completion.has_value());
        QVERIFY(QFileInfo::exists(firstCopy));
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(retry.completion.has_value(), 10000);
        QCOMPARE(retry.completion->result, TabPage::SaveResult::Saved);
        QCOMPARE(retry.questions, commitStarted ? 1 : 0);
        QVERIFY(!tab.hasUnsavedEdits());
        auto saved = engine.openDocument(path);
        QVERIFY(saved);
        const auto notes = AnnotModel(*saved).pageAnnots(0);
        QCOMPARE(notes.size(), size_t(2));
        QCOMPARE(notes.back().contents, QStringLiteral("newer"));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
    }

    // Checks unloaded Save copies retained recovery and asks about external source changes.
    void unloadedSaveDoesNotOpenPdfAndChangedSourceRequiresAnswer()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        QVERIFY(mervin::testing::suspendAndWait(tab));
        QFile changed(path);
        QVERIFY(changed.open(QIODevice::Append));
        QVERIFY(changed.write("\n% External change\n") > 0);
        changed.close();
        const QByteArray external = bytes(path);
        const int opens = RenderEngine::testing::opens();
        Outcome cancel;
        cancel.replaceChanged = false;
        tab.startSave(cancel.callbacks());
        QTRY_VERIFY_WITH_TIMEOUT(cancel.completion.has_value(), 10000);
        QCOMPARE(cancel.completion->result, TabPage::SaveResult::Canceled);
        QCOMPARE(cancel.questions, 1);
        QCOMPARE(bytes(path), external);
        QVERIFY(tab.hasUnsavedEdits());
        QVERIFY(!tab.isLoaded());
        Outcome replace;
        tab.startSave(replace.callbacks());
        QTRY_VERIFY_WITH_TIMEOUT(replace.completion.has_value(), 10000);
        QCOMPARE(replace.completion->result, TabPage::SaveResult::Saved);
        QCOMPARE(replace.questions, 1);
        QCOMPARE(RenderEngine::testing::opens(), opens);
        QVERIFY(!tab.isLoaded());
        QVERIFY(!tab.hasUnsavedEdits());
        auto saved = engine.openDocument(path);
        QVERIFY(saved);
        QCOMPARE(AnnotModel(*saved).pageAnnots(0).front().contents, QStringLiteral("preserved"));
    }

    // Selects automatic resume and Retry during running or queued Save completion.
    void unloadedSaveKeepsItsJobWhenLoadIsRequested_data()
    {
        QTest::addColumn<bool>("retry");
        QTest::addColumn<bool>("stored");
        QTest::newRow("resume during Save") << false << false;
        QTest::newRow("Retry during Save") << true << false;
        QTest::newRow("resume before Save delivery") << false << true;
        QTest::newRow("Retry before Save delivery") << true << true;
    }

    // Checks load requests preserve an unloaded Save and work after its delivery.
    void unloadedSaveKeepsItsJobWhenLoadIsRequested()
    {
        QFETCH(bool, retry);
        QFETCH(bool, stored);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        const QByteArray original = bytes(path);
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        QVERIFY(mervin::testing::suspendAndWait(tab));
        // Stop standing render workers so settle can wait for Save without GUI delivery.
        engine.shutdown();
        QVERIFY(background::settle(10000));
        const QString copy = tab.recoveryPath();
        QVERIFY(!copy.isEmpty());
        const int opens = RenderEngine::testing::opens();
        QSignalSpy resumed(&tab, &TabPage::resumeFinished);
        QSignalSpy finished(&tab, &TabPage::operationFinished);
        job::testing::holdWhen([path](job::Step step, const QString &file) {
            return step == job::Step::Commit && file == path;
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        Outcome outcome;
        tab.startSave(outcome.callbacks());
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        if (stored) {
            job::testing::release();
            QVERIFY(background::settle(10000));
        }
        QVERIFY(tab.isBusy());
        QVERIFY(!outcome.completion.has_value());
        const auto generation = tab.saveGeneration();
        if (retry)
            tab.retryLoad();
        else
            tab.resumeAsync();
        QVERIFY(!tab.isLoading());
        QVERIFY(tab.isBusy());
        QCOMPARE(tab.saveGeneration(), generation);
        QCOMPARE(tab.recoveryPath(), copy);
        QCOMPARE(resumed.size(), 0);
        QCOMPARE(RenderEngine::testing::opens(), opens);
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(outcome.completion.has_value(), 10000);
        QCOMPARE(outcome.completion->result, TabPage::SaveResult::Saved);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(resumed.size(), 0);
        QVERIFY(!tab.isBusy());
        QVERIFY(!tab.isLoading());
        QVERIFY(!tab.isLoaded());
        QVERIFY(!tab.hasUnsavedEdits());
        QVERIFY(bytes(path) != original);
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
        if (retry)
            tab.retryLoad();
        else
            tab.resumeAsync();
        QTRY_COMPARE_WITH_TIMEOUT(resumed.size(), 1, 10000);
        QVERIFY(resumed.front().front().toBool());
        QVERIFY(tab.isUsable());
        const auto notes = tab.viewer()->annotModel()->pageAnnots(0);
        QCOMPARE(notes.size(), size_t(1));
        QCOMPARE(notes.front().contents, QStringLiteral("preserved"));
    }

    // Selects manifest commit, original replacement and reopen failures.
    void failedSaveKeepsEdits_data()
    {
        QTest::addColumn<int>("failure");
        QTest::newRow("manifest commit") << 0;
        QTest::newRow("original commit") << 1;
        QTest::newRow("reopen original") << 2;
    }

    // Checks every failure preserves an editable document or durable recovery.
    void failedSaveKeepsEdits()
    {
        QFETCH(int, failure);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        const QByteArray original = bytes(path);
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        std::atomic<bool> injected{false};
        job::testing::failWhen([failure, path, &injected, opensAtCommit = -1]
                              (job::Step step, const QString &file) mutable {
            if (step == job::Step::Commit && file == path)
                opensAtCommit = RenderEngine::testing::opens();
            bool fail = false;
            if (failure == 0)
                fail = step == job::Step::Commit && file.endsWith(QStringLiteral(".json"));
            else if (failure == 1)
                fail = step == job::Step::Commit && file == path;
            else
                fail = opensAtCommit >= 0 && RenderEngine::testing::opens() > opensAtCommit
                    && step == job::Step::Reading && file == path;
            if (fail)
                injected = true;
            return fail;
        });
        const auto resetFailure = qScopeGuard([] { job::testing::failWhen({}); });
        Outcome outcome;
        tab.startSave(outcome.callbacks());
        QTRY_VERIFY_WITH_TIMEOUT(outcome.completion.has_value(), 10000);
        QVERIFY(injected);
        QCOMPARE(outcome.completion->result, TabPage::SaveResult::Failed);
        QCOMPARE(outcome.completion->originalSaved, failure == 2);
        QTRY_VERIFY_WITH_TIMEOUT(tab.isUsable(), 10000);
        QVERIFY(tab.hasUnsavedEdits());
        QCOMPARE(tab.viewer()->annotModel()->pageAnnots(0).size(), size_t(1));
        QCOMPARE(tab.hasRecoverySnapshot(), failure != 0);
        if (failure != 2)
            QCOMPARE(bytes(path), original);
        if (failure != 0)
            QVERIFY(TabPage::recoverablePaths().contains(path));
        else
            QVERIFY(recoveryEntries().isEmpty());
    }

    // Checks owner deletion abandons a pending approval without stranding its worker.
    void deletingTabAtChangedQuestionWakesItsWorker()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        auto *tab = new TabPage(&engine);
        QVERIFY(tab->open(path));
        QVERIFY(edit(*tab));
        QFile changed(path);
        QVERIFY(changed.open(QIODevice::Append));
        QVERIFY(changed.write("\n% External change\n") > 0);
        changed.close();
        const QByteArray external = bytes(path);
        const QPointer<TabPage> page(tab);
        bool completed = false;
        TabPage::SaveCallbacks callbacks;
        callbacks.changed = [page](auto, auto) { delete page.data(); };
        callbacks.finished = [&](const auto &) { completed = true; };
        tab->startSave(std::move(callbacks));
        QTRY_VERIFY_WITH_TIMEOUT(!page, 10000);
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(!completed);
        QCOMPARE(bytes(path), external);
        QVERIFY(recoveryEntries().isEmpty());
    }

    // Checks direct Stop during viewer detachment preserves the already adopted snapshot.
    void stopFromViewerSignalDuringAdoptionKeepsRecovery()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        const QByteArray original = bytes(path);
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        bool stopped = false;
        connect(tab.viewer(), &ViewerWidget::formModeChanged, &tab, [&](bool) {
            if (tab.isBusy() && tab.hasRecoverySnapshot() && !stopped) {
                stopped = true;
                tab.stopWaiting();
            }
        });
        Outcome outcome;
        tab.startSave(outcome.callbacks());
        QTRY_VERIFY_WITH_TIMEOUT(outcome.completion.has_value(), 10000);
        QCOMPARE(outcome.completion->result, TabPage::SaveResult::Stopped);
        QVERIFY(stopped);
        QTRY_VERIFY_WITH_TIMEOUT(tab.isUsable(), 10000);
        QVERIFY(tab.hasRecoverySnapshot());
        QVERIFY(tab.hasUnsavedEdits());
        QCOMPARE(tab.viewer()->annotModel()->pageAnnots(0).size(), size_t(1));
        QCOMPARE(bytes(path), original);
        QVERIFY(TabPage::recoverablePaths().contains(path));
        QVERIFY(QFileInfo::exists(tab.recoveryPath()));
    }

    // Selects deletion while successful or failed Save restores the tab.
    void deletionDuringDelivery_data()
    {
        QTest::addColumn<bool>("failed");
        QTest::addColumn<bool>("afterRecovery");
        QTest::newRow("successful Save unfreezes") << false << false;
        QTest::newRow("failed Save unfreezes") << true << false;
        QTest::newRow("failed Save recovery finishes") << true << true;
    }

    // Checks callback-driven deletion closes documents and removes owned recovery safely.
    void deletionDuringDelivery()
    {
        QFETCH(bool, failed);
        QFETCH(bool, afterRecovery);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        const QByteArray original = bytes(path);
        RenderEngine engine;
        auto *tab = new TabPage(&engine);
        const QPointer<TabPage> page(tab);
        QVERIFY(tab->open(path));
        QVERIFY(edit(*tab));
        if (failed)
            job::testing::failWhen([path](job::Step step, const QString &file) {
                return step == job::Step::Commit && file == path;
            });
        bool began = false;
        connect(tab, &TabPage::stateChanged, &engine, [&, page] {
            if (!page)
                return;
            began = began || page->isBusy();
            if (began && !page->isBusy() && (!afterRecovery || page->isUsable()))
                delete page.data();
        });
        Outcome outcome;
        tab->startSave(outcome.callbacks());
        QTRY_VERIFY_WITH_TIMEOUT(!page, 10000);
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
        if (failed)
            QCOMPARE(bytes(path), original);
        else
            QVERIFY(bytes(path) != original);
        QCOMPARE(outcome.completion.has_value(), afterRecovery);
    }

    // Selects earlier stopped recovery readers and competing Save outcomes.
    void unloadedSaveRetainsAnAbandonedLoadInput_data()
    {
        QTest::addColumn<bool>("stopFirst");
        QTest::newRow("Save completes while load waits") << false;
        QTest::newRow("stopped Save then retry") << true;
    }

    // Checks retained recovery remains present until every abandoned reader closes.
    void unloadedSaveRetainsAnAbandonedLoadInput()
    {
        QFETCH(bool, stopFirst);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        QVERIFY(mervin::testing::suspendAndWait(tab));
        const QString copy = tab.recoveryPath();
        job::testing::holdWhen([copy, path, stopFirst, first = true](job::Step step, const QString &file) mutable {
            const bool initialLoad = step == job::Step::Reading && file == copy && first;
            if (initialLoad)
                first = false;
            return initialLoad
                || (stopFirst && step == job::Step::Commit && file == path);
        });
        tab.resumeAsync();
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        Outcome first;
        tab.startSave(first.callbacks());
        if (stopFirst) {
            QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 2, 10000);
            tab.stopWaiting();
            QVERIFY(first.completion.has_value());
            QCOMPARE(first.completion->result, TabPage::SaveResult::Stopped);
            QVERIFY(tab.hasUnsavedEdits());
            Outcome retry;
            tab.startSave(retry.callbacks());
            // Startup and waiting for the old commit share the same notice, so
            // allow delivery time before checking that retry has not completed.
            QTest::qWait(100);
            QVERIFY(!retry.completion.has_value());
            QVERIFY(QFileInfo::exists(copy));
            job::testing::release();
            QTRY_VERIFY_WITH_TIMEOUT(retry.completion.has_value(), 10000);
            QCOMPARE(retry.completion->result, TabPage::SaveResult::Saved);
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(first.completion.has_value(), 10000);
            QCOMPARE(first.completion->result, TabPage::SaveResult::Saved);
            QVERIFY(QFileInfo::exists(copy));
            job::testing::release();
        }
        QVERIFY(!tab.hasUnsavedEdits());
        QVERIFY(!tab.isLoaded());
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(!QFileInfo::exists(copy));
        QVERIFY(recoveryEntries().isEmpty());
    }

#ifdef Q_OS_UNIX
    // Checks a changed alias retains its new key before the worker scope releases reservations.
    void retargetedAliasKeepsItsNewCanonicalAdmission()
    {
        QTemporaryDir files;
        const QString first = files.filePath(QStringLiteral("first.pdf"));
        const QString second = files.filePath(QStringLiteral("second.pdf"));
        const QString alias = files.filePath(QStringLiteral("alias.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), first));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), second));
        QVERIFY(QFile::link(first, alias));
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(alias));
        QVERIFY(edit(tab));
        const QByteArray initial = bytes(first);
        QVERIFY(QFile::remove(alias));
        QVERIFY(QFile::link(second, alias));
        QFile changed(second);
        QVERIFY(changed.open(QIODevice::Append));
        QVERIFY(changed.write("\n% Different target\n") > 0);
        changed.close();
        Outcome outcome;
        tab.startSave(outcome.callbacks());
        QTRY_VERIFY_WITH_TIMEOUT(outcome.completion.has_value(), 10000);
        QCOMPARE(outcome.completion->result, TabPage::SaveResult::Saved);
        QCOMPARE(outcome.questions, 1);
        QCOMPARE(tab.canonicalPath(), second);
        QCOMPARE(bytes(first), initial);
        job::State otherWriter;
        const job::Scope scope(otherWriter);
        QVERIFY(!job::reserveDestinations({fileKey(QFileInfo(second))}));
        QVERIFY(job::reserveDestinations({fileKey(QFileInfo(first))}));
        auto saved = engine.openDocument(second);
        QVERIFY(saved);
        QCOMPARE(AnnotModel(*saved).pageAnnots(0).front().contents, QStringLiteral("preserved"));
    }
#endif
};

QTEST_MAIN(TstSaveEdits)
#include "tst_save_edits.moc"
