#include "ui/TabPage.h"

#include "config/Settings.h"
#include "config/ConfigPaths.h"
#include "render/Document.h"
#include "render/MeasureContent.h"
#include "render/MeasureMath.h"
#include "render/RenderEngine.h"
#include "security/MeasureExport.h"
#include "security/DocumentOutput.h"
#include "ui/AnnotPanel.h"
#include "ui/FindCard.h"
#include "ui/MeasurePanel.h"
#include "ui/PanelStack.h"
#include "ui/ViewerWidget.h"

#include <QFileInfo>
#include <QFile>
#include <QCoreApplication>
#include <QEvent>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryFile>
#include <QVBoxLayout>

#include <algorithm>

namespace mervin {

namespace {
struct SourceStamp {
    bool exists = false;
    qint64 size = -1;
    qint64 modified = 0;
    qint64 changed = 0;
    bool operator==(const SourceStamp &) const = default;
};

SourceStamp sourceStamp(const QString &path)
{
    const QFileInfo file(path);
    SourceStamp stamp;
    stamp.exists = file.exists();
    stamp.size = file.size();
    stamp.modified = file.lastModified().toMSecsSinceEpoch();
    stamp.changed = file.fileTime(QFileDevice::FileMetadataChangeTime).toMSecsSinceEpoch();
    return stamp;
}

QString recoveryDirectory()
{
    return QDir(ConfigPaths::configDir()).filePath(QStringLiteral("recovery"));
}

QString createRecoveryFile(QString *error)
{
    const QString directory = recoveryDirectory();
    if (!QDir().mkpath(directory)) {
        if (error)
            *error = QObject::tr("Could not create the document recovery folder.");
        return {};
    }
    QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                     | QFileDevice::ExeOwner);
    QTemporaryFile file(QDir(directory).filePath(QStringLiteral("document-XXXXXX.pdf")));
    if (!file.open()) {
        if (error)
            *error = file.errorString();
        return {};
    }
    file.setAutoRemove(false);
    return file.fileName();
}

// Manifests are adopted once per process, including independent views of the same file.
QSet<QString> claimedRecoveries;

QJsonObject resumeJson(const ViewerWidget::ResumeState &state)
{
    QJsonArray points;
    for (const QPointF &point : state.inProgress)
        points.append(QJsonArray{point.x(), point.y()});
    return {{"page", state.view.page}, {"scale", state.view.scale},
            {"zoom", state.view.zoomMode}, {"rotation", state.view.rotation},
            {"x", state.view.offsetX}, {"y", state.view.offsetY},
            {"single", state.layout.scroll == ViewLayout::Scroll::Single},
            {"spread", state.layout.spread}, {"query", state.query},
            {"case", state.caseSensitive}, {"whole", state.wholeWord},
            {"match", state.currentMatch}, {"tool", static_cast<int>(state.tool)},
            {"measure", state.measureEnabled}, {"comment", state.commentEnabled},
            {"kind", static_cast<int>(state.measureKind)},
            {"unit", static_cast<int>(state.measureUnit)},
            {"precision", state.measurePrecision}, {"line_width", state.measureLineWidth},
            {"points", points}, {"drawing_page", state.inProgressPage}};
}

ViewerWidget::ResumeState readResumeJson(const QJsonObject &json)
{
    ViewerWidget::ResumeState state;
    state.view.page = std::max(0, json.value("page").toInt());
    state.view.scale = json.value("scale").toDouble(1.0);
    state.view.zoomMode = json.value("zoom").toString(QStringLiteral("fit-width"));
    state.view.rotation = json.value("rotation").toInt();
    state.view.offsetX = json.value("x").toDouble();
    state.view.offsetY = json.value("y").toDouble();
    state.layout.scroll = json.value("single").toBool() ? ViewLayout::Scroll::Single
                                                       : ViewLayout::Scroll::Continuous;
    state.layout.spread = json.value("spread").toBool();
    state.query = json.value("query").toString();
    state.caseSensitive = json.value("case").toBool();
    state.wholeWord = json.value("whole").toBool();
    state.currentMatch = json.value("match").toInt(-1);
    state.tool = static_cast<ViewerWidget::ToolMode>(std::clamp(json.value("tool").toInt(), 0, 6));
    state.measureEnabled = json.value("measure").toBool();
    state.commentEnabled = json.value("comment").toBool();
    state.measureKind = static_cast<MeasureKind>(std::clamp(json.value("kind").toInt(), 0, 3));
    state.measureUnit = static_cast<MeasureUnit>(std::clamp(json.value("unit").toInt(), 0, 4));
    state.measurePrecision = std::clamp(json.value("precision").toInt(2), 0, 8);
    state.measureLineWidth = std::clamp(json.value("line_width").toDouble(2.0), 0.25, 10.0);
    state.inProgressPage = json.value("drawing_page").toInt(-1);
    for (const QJsonValue &value : json.value("points").toArray()) {
        const auto point = value.toArray();
        if (point.size() == 2)
            state.inProgress.emplace_back(point[0].toDouble(), point[1].toDouble());
    }
    return state;
}

QJsonObject readManifest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const auto json = QJsonDocument::fromJson(file.readAll()).object();
    const QString snapshot = json.value("snapshot").toString();
    if (json.value("version").toInt() != 1 || json.value("original").toString().isEmpty()
        || snapshot.isEmpty() || QFileInfo(snapshot).fileName() != snapshot
        || !QFileInfo::exists(QFileInfo(path).dir().filePath(snapshot)))
        return {};
    return json;
}

MeasureKind kindFromString(const QString &s)
{
    const QString t = s.trimmed().toLower();
    if (t == QLatin1String("path") || t == QLatin1String("polyline"))
        return MeasureKind::Polyline;
    if (t == QLatin1String("area"))
        return MeasureKind::Area;
    if (t == QLatin1String("angle"))
        return MeasureKind::Angle;
    return MeasureKind::Distance;
}
} // namespace

struct TabPage::SavedState {
    ViewerWidget::ResumeState viewer;
    SourceStamp source;
    bool restoreTools = false;
};

struct TabPage::OpenResult {
    std::unique_ptr<Document> document;
    std::optional<MeasureDoc> measurements;
    SourceStamp source;
    QString error;
    bool needsPassword = false;
};

QString TabPage::resolvedAnnotAuthor(const QString &configured)
{
    QString author = configured.trimmed();
    if (author.isEmpty()) {
        author = qEnvironmentVariable("USERNAME");
        if (author.isEmpty())
            author = qEnvironmentVariable("USER");
    }
    return author;
}

TabPage::TabPage(RenderEngine *engine, QWidget *parent)
    : QWidget(parent)
    , engine_(engine)
    , saved_(std::make_unique<SavedState>())
{
    loadPool_.setMaxThreadCount(1);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    viewer_ = new ViewerWidget(engine_, this);
    layout->addWidget(viewer_, 1);
    placeholder_ = new QWidget(this);
    auto *placeholderLayout = new QVBoxLayout(placeholder_);
    placeholderLayout->addStretch();
    placeholderLabel_ = new QLabel(placeholder_);
    placeholderLabel_->setObjectName(QStringLiteral("documentLoadStatus"));
    placeholderLabel_->setAlignment(Qt::AlignCenter);
    placeholderLabel_->setWordWrap(true);
    placeholderLabel_->setTextFormat(Qt::PlainText);
    placeholderLayout->addWidget(placeholderLabel_);
    auto *retry = new QPushButton(tr("Retry"), placeholder_);
    retry->setObjectName(QStringLiteral("retryDocumentLoad"));
    retryButton_ = retry;
    placeholderLayout->addWidget(retry, 0, Qt::AlignHCenter);
    placeholderLayout->addStretch();
    connect(retry, &QPushButton::clicked, this, &TabPage::resumeAsync);
    layout->addWidget(placeholder_, 1);
    placeholder_->hide();

    // Floating measure controls: parented to the viewport so they overlap the
    // page (and don't scroll with content). Hidden until measure mode turns on.
    measurePanel_ = new MeasurePanel(viewer_->viewport());
    measurePanel_->hide();
    viewer_->setMeasurePanel(measurePanel_); // let the viewer reject clicks over it

    // Permanent panel <-> viewer wiring (independent of which window owns the
    // tab), so the panel follows the tab through detach/adopt.
    connect(measurePanel_, &MeasurePanel::kindChanged, viewer_, &ViewerWidget::setMeasureKind);
    connect(measurePanel_, &MeasurePanel::unitChanged, viewer_, &ViewerWidget::setMeasureUnit);
    connect(measurePanel_, &MeasurePanel::precisionChanged, viewer_,
            &ViewerWidget::setMeasurePrecision);
    connect(measurePanel_, &MeasurePanel::lineWidthChanged, viewer_,
            &ViewerWidget::setMeasureLineWidth);
    connect(measurePanel_, &MeasurePanel::cursorActiveChanged, viewer_,
            &ViewerWidget::setMeasureCursorActive);
    connect(viewer_, &ViewerWidget::measureCursorActiveChanged, measurePanel_,
            &MeasurePanel::setCursorActive);
    connect(measurePanel_, &MeasurePanel::popupDismissed, viewer_,
            &ViewerWidget::notifyMeasurePanelPopupClosed);
    connect(measurePanel_, &MeasurePanel::calibrateRequested, viewer_,
            &ViewerWidget::beginCalibration);
    connect(measurePanel_, &MeasurePanel::setScaleRequested, viewer_,
            &ViewerWidget::promptSetScale);
    connect(measurePanel_, &MeasurePanel::resetRequested, viewer_,
            &ViewerWidget::resetPageScale);
    connect(measurePanel_, &MeasurePanel::clearRequested, viewer_,
            &ViewerWidget::clearMeasurements);
    connect(measurePanel_, &MeasurePanel::closeRequested, viewer_,
            [this] { viewer_->setMeasureMode(false); });
    connect(viewer_, &ViewerWidget::measureScaleChanged, measurePanel_,
            &MeasurePanel::setScaleText);
    connect(viewer_, &ViewerWidget::measureScaleResettableChanged, measurePanel_,
            &MeasurePanel::setResetVisible);
    connect(viewer_, &ViewerWidget::measurementReadout, measurePanel_, &MeasurePanel::setReadout);
    connect(viewer_, &ViewerWidget::measurementsChanged, measurePanel_,
            &MeasurePanel::setMeasurements);
    // Queued: the X button lives inside the list row that removeMeasurement()
    // rebuilds (QListWidget::clear deletes the row widgets). Deferring the call
    // past the button's clicked() emission avoids deleting the sender mid-signal.
    connect(measurePanel_, &MeasurePanel::removeMeasurementRequested, viewer_,
            &ViewerWidget::removeMeasurement, Qt::QueuedConnection);
    connect(measurePanel_, &MeasurePanel::copyMeasurementRequested, viewer_,
            &ViewerWidget::copyMeasurementValue);
    connect(measurePanel_, &MeasurePanel::measurementHovered, viewer_,
            &ViewerWidget::onMeasurementHovered);
    // The Comment tool window (style + colour for highlights and notes). Created
    // before the PanelStack so both panels can register with the dock below.
    annotPanel_ = new AnnotPanel(viewer_->viewport());
    annotPanel_->hide();
    viewer_->setAnnotPanel(annotPanel_); // let the viewer reject page presses over it
    connect(annotPanel_, &AnnotPanel::modeChanged, viewer_, &ViewerWidget::setAnnotSubMode);
    connect(annotPanel_, &AnnotPanel::highlightStyleChanged, viewer_, &ViewerWidget::setMarkupStyle);
    connect(annotPanel_, &AnnotPanel::closeRequested, viewer_,
            [this] { viewer_->setCommentToolEnabled(false); });
    // Keep the panel's mode selector in sync when the active gesture is taken over
    // by the measuring tool (annotSubModeChanged(Select)) or set programmatically.
    connect(viewer_, &ViewerWidget::annotSubModeChanged, annotPanel_, &AnnotPanel::setMode);

    // Find in document: a card over the top right of the page. It only emits
    // intent; the viewer owns the matches. Closing it ends the search, so the
    // highlights go and typing returns to the page.
    findCard_ = new FindCard(viewer_->viewport());
    viewer_->setFindCard(findCard_);
    connect(findCard_, &FindCard::searchChanged, viewer_, &ViewerWidget::startFind);
    connect(findCard_, &FindCard::findNext, viewer_, &ViewerWidget::findNext);
    connect(findCard_, &FindCard::findPrev, viewer_, &ViewerWidget::findPrev);
    connect(viewer_, &ViewerWidget::findStatusChanged, findCard_, &FindCard::setResultCount);
    connect(findCard_, &FindCard::openChanged, this, [this](bool open) {
        if (open)
            return;
        viewer_->clearFind();
        viewer_->setFocus();
    });

    // Dock: stack the find card and the measure + comment panels vertically (in
    // that order), drag them as a group, top-right by default. Any of them can be
    // open at once; show/hide is signal-driven so the dock reflows on any
    // open/close order (incl. a document switch, which resets tool state and
    // emits the *Changed signals below).
    panelStack_ = new PanelStack(viewer_->viewport(), this);
    findCard_->setStack(panelStack_);
    measurePanel_->setStack(panelStack_);
    annotPanel_->setStack(panelStack_);
    panelStack_->addPanel(findCard_);
    panelStack_->addPanel(measurePanel_);
    panelStack_->addPanel(annotPanel_);
    connect(viewer_, &ViewerWidget::measureModeChanged, this, [this](bool on) {
        measurePanel_->setVisible(on);
        panelStack_->relayout(); // re-stack so the comment panel reflows up/down
    });
    connect(viewer_, &ViewerWidget::commentToolEnabledChanged, this, [this](bool on) {
        annotPanel_->setVisible(on);
        panelStack_->relayout();
    });

    // Seed the panel + viewer from the saved measurement defaults.
    const Settings s = Settings::load();
    const MeasureUnit unit = measure::unitFromString(s.measurementUnit, MeasureUnit::Millimeter);
    const MeasureKind kind = kindFromString(s.measurementType);
    measurePanel_->setUnit(unit);
    measurePanel_->setPrecision(s.measurementPrecision);
    measurePanel_->setLineWidth(s.measurementLineWidth);
    measurePanel_->setKind(kind);
    viewer_->setMeasureUnit(unit);
    viewer_->setMeasurePrecision(s.measurementPrecision);
    viewer_->setMeasureLineWidth(s.measurementLineWidth);
    viewer_->setMeasureKind(kind);
    viewer_->setMeasureSnap(s.measurementSnap);
    // Seed form-fill settings before the document opens, so setDocument can honour
    // them (auto-enter form mode, field highlighting).
    viewer_->setAutoFormFill(s.autoFormFill);
    viewer_->setHighlightFormFields(s.highlightFormFields);

    // Seed annotation defaults: author (fall back to the OS user name so new marks
    // are attributed), and the last-used markup colour + style.
    viewer_->setAnnotAuthor(resolvedAnnotAuthor(s.annotationAuthor));
    // One shared default annotation colour drives both new markups and new sticky
    // notes; it is configured in Settings (the panel no longer carries a picker).
    const QColor markupColor(s.annotationColor);
    if (markupColor.isValid())
        viewer_->setMarkupColor(markupColor);
    const QString style = s.annotationStyle.trimmed().toLower();
    const AnnotType markupStyle = style == QLatin1String("underline")  ? AnnotType::Underline
                                  : style == QLatin1String("strikeout") ? AnnotType::StrikeOut
                                                                        : AnnotType::Highlight;
    viewer_->setMarkupStyle(markupStyle);
    annotPanel_->setHighlightStyle(markupStyle);
    saved_->viewer = viewer_->captureResumeState();
}

TabPage::~TabPage()
{
    stopLoading();
    // Child widgets otherwise die after the Document member.
    delete viewer_;
    viewer_ = nullptr;
    doc_.reset();
    clearRecovery();
}

void TabPage::stopLoading()
{
    ++loadGeneration_;
    loading_ = false;
    loadPool_.clear();
    loadPool_.waitForDone();
    // A finished worker can still have a queued result owning an open recovery PDF.
    // Destroy that result before removing checkpoint files, especially on Windows.
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
}

std::shared_ptr<TabPage::OpenResult> TabPage::readDocument(RenderEngine *engine,
                                                         const QString &path,
                                                         const QString &password)
{
    auto result = std::make_shared<OpenResult>();
    result->source = sourceStamp(path);
    result->document = engine->openDocument(path, password, &result->error, &result->needsPassword);
    if (!result->document)
        return result;
    if (result->document->hasMervinMeasurements()) {
        if (auto blob = MeasureExport::readMervinBlob(path, password)) {
            MeasureDoc measurements;
            if (parseMeasurements(*blob, &measurements))
                result->measurements = std::move(measurements);
        }
    }
    return result;
}

void TabPage::installDocument(const std::shared_ptr<OpenResult> &result)
{
    viewer_->setDocument(nullptr);
    doc_ = std::move(result->document);
    viewer_->setDocument(doc_.get());
    if (result->measurements) {
        const auto &md = *result->measurements;
        viewer_->loadMeasurements(md.measurements, md.overridesModel(), md.unit,
                                  md.precision, md.lineWidth);
        measurePanel_->setUnit(md.unit);
        measurePanel_->setPrecision(md.precision);
        measurePanel_->setLineWidth(md.lineWidth);
    }
    placeholder_->hide();
    viewer_->show();
}

void TabPage::setPath(const QString &path)
{
    const QFileInfo fi(path);
    path_ = fi.absoluteFilePath();
    canonicalPath_ = fi.canonicalFilePath();
    if (canonicalPath_.isEmpty())
        canonicalPath_ = path_;
}

bool TabPage::open(const QString &path, const QString &password, QString *error,
                   bool *needsPassword)
{
    stopLoading();
    const auto result = readDocument(engine_, path, password);
    if (needsPassword)
        *needsPassword = result->needsPassword;
    if (!result->document) {
        if (error)
            *error = result->error;
        return false;
    }
    // Save writes the file and then opens it again in this tab. An open find card
    // keeps its search across that, without moving the view.
    const ViewerWidget::ResumeState before = saved_->viewer;
    installDocument(result);
    if (findCard_->isOpen() && !before.query.isEmpty())
        viewer_->restoreFind(before.query, before.caseSensitive, before.wholeWord,
                             before.currentMatch);
    // Copy before setPath/clearRecovery, since callers may pass tab-owned strings.
    const QString verifiedPassword = password;
    setPath(path);
    password_ = verifiedPassword;
    saved_->source = result->source;
    clearRecovery();
    retainedEdits_ = false;
    saved_->viewer = viewer_->captureResumeState();
    saved_->restoreTools = true;
    emit stateChanged();
    return true;
}

void TabPage::restoreViewer()
{
    viewer_->restoreResumeState(saved_->viewer);
    // A search stays only while the find card is open. One restored behind a
    // closed card (from a recovery checkpoint, say) would leave highlights that
    // nothing on screen explains.
    if (!findCard_->isOpen() && !viewer_->findQuery().isEmpty())
        viewer_->clearFind();
}

bool TabPage::recoverSnapshot(const QString &snapshot, QString *error)
{
    stopLoading();
    const QString privateCopy = createRecoveryFile(error);
    if (privateCopy.isEmpty())
        return false;
    if (!DocumentOutput::replace(snapshot, privateCopy, error)) {
        QFile::remove(privateCopy);
        return false;
    }
    auto result = readDocument(engine_, privateCopy, password_);
    if (!result->document) {
        if (error)
            *error = result->error;
        QFile::remove(privateCopy);
        return false;
    }
    if (doc_)
        saved_->viewer = viewer_->captureResumeState();
    saved_->restoreTools = true;
    const QString previousSnapshot = recoveryPath_;
    const QString previousManifest = recoveryManifest_;
    recoveryPath_ = privateCopy;
    recoveryManifest_ = privateCopy + QStringLiteral(".json");
    if (!writeRecoveryManifest(error)) {
        result.reset(); // close the private copy before removal on Windows
        QFile::remove(privateCopy);
        recoveryPath_ = previousSnapshot;
        recoveryManifest_ = previousManifest;
        return false;
    }
    installDocument(result);
    restoreViewer();
    retainedEdits_ = true;
    if (previousSnapshot.isEmpty() || QFile::remove(previousSnapshot)) {
        if (!previousManifest.isEmpty()) {
            claimedRecoveries.remove(previousManifest);
            QFile::remove(previousManifest);
        }
    }
    if (snapshot != previousSnapshot)
        QFile::remove(snapshot);
    emit stateChanged();
    return true;
}

void TabPage::clearRecovery()
{
    if (!recoveryManifest_.isEmpty()) {
        claimedRecoveries.remove(recoveryManifest_);
        QFile::remove(recoveryManifest_);
        recoveryManifest_.clear();
    }
    if (!recoveryPath_.isEmpty()) {
        QFile::remove(recoveryPath_);
        recoveryPath_.clear();
    }
}

QStringList TabPage::recoverablePaths()
{
    QStringList paths;
    const QDir directory(recoveryDirectory());
    for (const QString &name : directory.entryList({QStringLiteral("*.json")}, QDir::Files,
                                                  QDir::Time)) {
        const auto manifest = readManifest(directory.filePath(name));
        if (!manifest.isEmpty())
            paths.append(manifest.value("original").toString());
    }
    return paths;
}

bool TabPage::adoptRecovery()
{
    const QDir directory(recoveryDirectory());
    for (const QString &name : directory.entryList({QStringLiteral("*.json")}, QDir::Files,
                                                  QDir::Time)) {
        const QString path = directory.filePath(name);
        if (claimedRecoveries.contains(path))
            continue;
        const auto manifest = readManifest(path);
        if (manifest.value("original").toString() != path_
            && manifest.value("canonical").toString() != canonicalPath_)
            continue;
        recoveryManifest_ = path;
        recoveryPath_ = directory.filePath(manifest.value("snapshot").toString());
        saved_->viewer = readResumeJson(manifest.value("view").toObject());
        saved_->restoreTools = true;
        const auto stamp = manifest.value("source").toObject();
        saved_->source.exists = stamp.value("exists").toBool();
        saved_->source.size = stamp.value("size").toVariant().toLongLong();
        saved_->source.modified = stamp.value("modified").toVariant().toLongLong();
        saved_->source.changed = stamp.value("changed").toVariant().toLongLong();
        retainedEdits_ = true;
        claimedRecoveries.insert(path);
        return true;
    }
    return false;
}

bool TabPage::writeRecoveryManifest(QString *error)
{
    const SourceStamp &stamp = saved_->source;
    const QJsonObject json{
        {"version", 1}, {"original", path_}, {"canonical", canonicalPath_},
        {"snapshot", QFileInfo(recoveryPath_).fileName()},
        {"view", resumeJson(saved_->viewer)},
        {"source", QJsonObject{{"exists", stamp.exists}, {"size", QString::number(stamp.size)},
                               {"modified", QString::number(stamp.modified)},
                               {"changed", QString::number(stamp.changed)}}}};
    QSaveFile file(recoveryManifest_);
    file.setDirectWriteFallback(false);
    const QByteArray data = QJsonDocument(json).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly)
        || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(data) != data.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    claimedRecoveries.insert(recoveryManifest_);
    return true;
}

void TabPage::initializeSuspended(const QString &path)
{
    initializeSuspended(path, viewer_->captureResumeState().view);
}

void TabPage::initializeSuspended(const QString &path, const ViewState &state)
{
    if (doc_ || loading_)
        return;
    setPath(path);
    saved_->viewer = viewer_->captureResumeState();
    saved_->viewer.view = state;
    adoptRecovery();
    showPlaceholder(tr("Document is unloaded."), false);
    emit stateChanged();
}

void TabPage::showPlaceholder(const QString &message, bool retry)
{
    viewer_->hide();
    placeholderLabel_->setText(message);
    retryButton_->setVisible(retry);
    placeholder_->show();
}

bool TabPage::hasUnsavedEdits() const
{
    return retainedEdits_ || !recoveryPath_.isEmpty() || (doc_ && viewer_->hasUnsavedEdits());
}

ViewState TabPage::capturedViewState() const
{
    return doc_ ? viewer_->captureResumeState().view : saved_->viewer.view;
}

void TabPage::setSavedViewState(const ViewState &state)
{
    // A crash checkpoint is newer than the ordinary last-saved view-state file.
    if (!doc_ && !recoveryPath_.isEmpty())
        return;
    if (doc_)
        saved_->viewer = viewer_->captureResumeState();
    saved_->viewer.view = state;
    if (doc_)
        restoreViewer();
}

bool TabPage::sourceChangedOnDisk() const
{
    return sourceStamp(path_) != saved_->source;
}

bool TabPage::suspend(QString *error)
{
    if (loading_) {
        if (error)
            *error = tr("The document is still loading.");
        return false;
    }
    if (!doc_)
        return true;
    viewer_->commitActiveFormEditor();
    viewer_->commitActiveAnnotEditor();
    saved_->viewer = viewer_->captureResumeState();
    saved_->restoreTools = true;
    const bool dirty = hasUnsavedEdits() || !saved_->viewer.inProgress.empty();
    QString previousSnapshot;
    QString previousManifest;
    if (dirty) {
        const QString nextSnapshot = createRecoveryFile(error);
        if (nextSnapshot.isEmpty())
            return false;
        if (!DocumentOutput::snapshot(*doc_, viewer_->measurementDocument(), nextSnapshot,
                                      password_, error)) {
            QFile::remove(nextSnapshot);
            return false;
        }
        if (!QFile::setPermissions(nextSnapshot, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            QFile::remove(nextSnapshot);
            if (error)
                *error = tr("Could not protect the document recovery copy.");
            return false;
        }
        const QString oldSnapshot = recoveryPath_;
        const QString oldManifest = recoveryManifest_;
        recoveryPath_ = nextSnapshot;
        recoveryManifest_ = nextSnapshot + QStringLiteral(".json");
        if (!writeRecoveryManifest(error)) {
            QFile::remove(nextSnapshot);
            QFile::remove(recoveryManifest_);
            recoveryPath_ = oldSnapshot;
            recoveryManifest_ = oldManifest;
            return false;
        }
        previousSnapshot = oldSnapshot;
        previousManifest = oldManifest;
        retainedEdits_ = true;
    }
    detachDocument();
    // MuPDF may still be reading the previous checkpoint until detachDocument closes its
    // file handle. Keep its manifest if removal fails so that file remains discoverable.
    if (previousSnapshot.isEmpty() || QFile::remove(previousSnapshot)) {
        if (!previousManifest.isEmpty()) {
            claimedRecoveries.remove(previousManifest);
            QFile::remove(previousManifest);
        }
    }
    showPlaceholder(tr("Document is unloaded."), false);
    emit stateChanged();
    return true;
}

bool TabPage::finishResume(const std::shared_ptr<OpenResult> &result)
{
    loading_ = false;
    if (!result->document) {
        showPlaceholder(result->error.isEmpty() ? tr("Could not load this document.") : result->error,
                        true);
        emit stateChanged();
        return false;
    }
    if (recoveryPath_.isEmpty())
        saved_->source = result->source;
    installDocument(result);
    if (!saved_->restoreTools) {
        const auto view = saved_->viewer.view;
        const auto layout = saved_->viewer.layout;
        saved_->viewer = viewer_->captureResumeState();
        saved_->viewer.view = view;
        saved_->viewer.layout = layout;
        saved_->restoreTools = true;
    }
    restoreViewer();
    measurePanel_->setKind(saved_->viewer.measureKind);
    measurePanel_->setUnit(saved_->viewer.measureUnit);
    measurePanel_->setPrecision(saved_->viewer.measurePrecision);
    measurePanel_->setLineWidth(saved_->viewer.measureLineWidth);
    emit stateChanged();
    return true;
}

bool TabPage::resume(QString *error, bool *needsPassword)
{
    if (needsPassword)
        *needsPassword = false;
    if (doc_)
        return true;
    stopLoading();
    const auto result = readDocument(engine_, recoveryPath_.isEmpty() ? path_ : recoveryPath_, password_);
    if (error)
        *error = result->error;
    if (needsPassword)
        *needsPassword = result->needsPassword;
    return finishResume(result);
}

void TabPage::resumeAsync()
{
    if (doc_ || loading_)
        return;
    loading_ = true;
    const quint64 generation = ++loadGeneration_;
    showPlaceholder(tr("Loading"), false);
    emit stateChanged();
    const QString source = recoveryPath_.isEmpty() ? path_ : recoveryPath_;
    const QString password = password_;
    loadPool_.start([this, generation, source, password] {
        auto result = readDocument(engine_, source, password);
        QMetaObject::invokeMethod(this, [this, generation, result] {
            if (generation != loadGeneration_)
                return;
            const bool success = finishResume(result);
            emit resumeFinished(success, result->error, result->needsPassword);
        }, Qt::QueuedConnection);
    });
}

void TabPage::detachDocument()
{
    stopLoading();
    if (doc_) {
        saved_->viewer = viewer_->captureResumeState();
        saved_->restoreTools = true;
    }
    viewer_->setDocument(nullptr); // clear the viewer's raw Document* first
    doc_.reset();                  // then drop the owner, closing the file handle
}

QString TabPage::tabTitle() const
{
    return QFileInfo(path_).fileName();
}

QString TabPage::documentTitle() const
{
    const QString t = doc_ ? doc_->title() : QString();
    return t.isEmpty() ? tabTitle() : t;
}

} // namespace mervin
