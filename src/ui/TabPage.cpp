#include "ui/TabPage.h"

#include "config/Settings.h"
#include "config/ConfigPaths.h"
#include "io/AppFiles.h"
#include "io/Background.h"
#include "render/Document.h"
#include "render/MeasureContent.h"
#include "render/MeasureMath.h"
#include "render/RenderEngine.h"
#include "recent/PathKey.h"
#include "security/MeasureExport.h"
#include "security/DocumentOutput.h"
#include "ui/AnnotPanel.h"
#include "ui/FindCard.h"
#include "ui/MeasurePanel.h"
#include "ui/PanelStack.h"
#include "ui/StillPicture.h"
#include "ui/ViewerWidget.h"
#include "ui/WaitBar.h"

#include <QFileInfo>
#include <QFile>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryFile>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <condition_variable>
#include <mutex>

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
            *error = TabPage::tr("Could not create the document recovery folder.");
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

// The manifests TabPage::recoverableDocuments() found, by path, so a tab restored
// lazily adopts its recovery copy without reading the folder on the UI thread.
// Manifests written later belong to the tab that wrote them.
QList<QPair<QString, QJsonObject>> foundManifests;

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

// How often a tab reads what its load reports.
constexpr int kPollMs = 250;
int slowDelayMs = TabPage::kSlowMs;

// The slow notice for document `name` read from `folder`, for a label `width` wide (see
// fitFileSentence).
QString slowNotice(const QString &name, const QString &folder, const QFontMetrics &metrics,
                   int width)
{
    //: Shown in place of the page when the drive has not answered for a few seconds
    //: while the document loads. Below it is the Stop waiting button. %1 is the file
    //: name (plan.pdf), %2 the folder it is read from (/nas/docs, or a network share
    //: folder on Windows).
    const QString sentence = TabPage::tr("Opening %1 from %2. The drive is not answering.");
    return fitFileSentence(
        [&sentence](const QString &file, const QString &in) { return sentence.arg(file, in); },
        name, folder, metrics, width);
}

// The wait bar's sentence for an operation that reads the file `name` in `folder`, or
// with an empty name, that saves to `folder` (see fitFileSentence).
FileSentence waitSentence(const QString &name)
{
    if (name.isEmpty()) {
        //: Shown in a bar over the document when the drive has not answered for a few
        //: seconds while a tool such as Rotate Pages or Split writes its files, or reads
        //: a file in the folder it writes to. On its right is the Stop waiting button.
        //: %1 is the folder (/nas/docs, or a network share folder on Windows).
        const QString sentence = TabPage::tr("Saving to %1. The drive is not answering.");
        return [sentence](const QString &, const QString &in) { return sentence.arg(in); };
    }
    //: Shown in a bar over the document when the drive has not answered for a few
    //: seconds while a tool such as Rotate Pages reads a file. On its right is the Stop
    //: waiting button. %1 is the file name (plan.pdf), %2 the folder it is read from
    //: (/nas/docs, or a network share folder on Windows).
    const QString sentence = TabPage::tr("Reading %1 from %2. The drive is not answering.");
    return [sentence](const QString &file, const QString &in) { return sentence.arg(file, in); };
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
    OwnedDocument document;
    std::optional<MeasureDoc> measurements;
    SourceStamp source;
    QString canonical;
    std::unique_ptr<job::OpenPath> admission;
    QString error;
    bool needsPassword = false;
};

// The tab's background job, the load that resumeAsync() starts or an operation
// (startOperation), shared by the tab and the job's thread. The tab never destroys a
// running job. It takes the result, waits for a load (stopLoading) or lets go of the
// job (abandon), and the job ends on its own. An operation keeps its result in what
// its work captured, so its job has no `result`, and it opens no document.
struct TabPage::Job {
    enum class Phase {
        Reading, // the job reads the document, or the operation runs
        Closing, // the tab let go of the job, and what it read is being closed
        Stored,  // `result` waits for the tab, or the operation has returned
        Ended,   // nothing is left open, as the tab took the result or it was closed
    };

    explicit Job(const QString &source) : source(source) {}

    // With `mutex` held.
    void setPhase(Phase next)
    {
        phase = next;
        changed.notify_all();
    }
    // Ends the job once what it read is closed, on the thread that closed it, then
    // removes the recovery copy a closed tab left for it.
    void endClosed()
    {
        std::unique_lock<std::mutex> lock(mutex);
        const QString file = std::exchange(removeAfter, QString());
        setPhase(Phase::Ended);
        lock.unlock();
        appfiles::remove(file);
    }
    // Blocks until the job has ended. Only for another job's thread.
    void waitUntilEnded()
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [this] { return phase == Phase::Ended; });
    }

    const QString source;               // the file the load reads; empty for an operation
    job::State state;                   // what the job reports, and its stop
    std::mutex mutex;
    std::condition_variable changed;    // notified when `phase` changes
    Phase phase = Phase::Reading;       // guarded by mutex
    std::shared_ptr<OpenResult> result; // guarded by mutex: set while Stored
    bool abandoned = false;             // guarded by mutex: the tab let go of the job
    QString removeAfter;                // guarded by mutex: a recovery copy, see ~TabPage
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
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    viewer_ = new ViewerWidget(engine_, this);
    layout->addWidget(viewer_, 1);
    // Stands in for the pages, on their canvas, while the document is unloaded, loads
    // or could not be loaded.
    placeholder_ = new QWidget(this);
    placeholder_->setObjectName(QStringLiteral("documentPlaceholder"));
    placeholder_->setAttribute(Qt::WA_StyledBackground, true);
    auto *placeholderLayout = new QVBoxLayout(placeholder_);
    placeholderLayout->setSpacing(6);
    placeholderLayout->addStretch();
    placeholderLabel_ = new QLabel(placeholder_);
    placeholderLabel_->setObjectName(QStringLiteral("documentLoadStatus"));
    placeholderLabel_->setAlignment(Qt::AlignCenter);
    placeholderLabel_->setWordWrap(true);
    placeholderLabel_->setTextFormat(Qt::PlainText);
    placeholderLabel_->installEventFilter(this); // fits the slow notice to its width
    placeholderLayout->addWidget(placeholderLabel_);
    //: Button that tries to load the document again.
    auto *retry = new QPushButton(tr("Retry"), placeholder_);
    retry->setObjectName(QStringLiteral("retryDocumentLoad"));
    retryButton_ = retry;
    //: Button beside a notice that the drive is not answering. Under the notice of a
    //: document that loads, it gives up the load and leaves the document unloaded. In
    //: the bar over a document that a tool such as Rotate Pages works on, it gives up
    //: the tool's work, so the document can be used again.
    const QString stopText = tr("Stop waiting");
    auto *stop = new QPushButton(stopText, placeholder_);
    stop->setObjectName(QStringLiteral("stopDocumentLoad"));
    stopButton_ = stop;
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(6);
    buttons->addStretch();
    buttons->addWidget(retry);
    buttons->addWidget(stop);
    buttons->addStretch();
    placeholderLayout->addLayout(buttons);
    placeholderLayout->addStretch();
    connect(retry, &QPushButton::clicked, this, &TabPage::retryLoad);
    connect(stop, &QPushButton::clicked, this, &TabPage::stopWaiting);
    layout->addWidget(placeholder_, 1);
    placeholder_->hide();
    // While an operation runs, the still picture takes the viewer's place and the wait
    // bar floats over its top.
    still_ = new StillPicture(this);
    layout->addWidget(still_, 1);
    still_->hide();
    waitBar_ = new WaitBar(stopText, this);
    waitBar_->installEventFilter(this); // the veil follows the bar
    connect(waitBar_->stopButton(), &QPushButton::clicked, this, &TabPage::stopWaiting);
    slowPoll_ = new QTimer(this);
    slowPoll_->setInterval(kPollMs);
    connect(slowPoll_, &QTimer::timeout, this, &TabPage::pollJob);

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
    // Child widgets otherwise die after the Document member.
    delete viewer_;
    viewer_ = nullptr;
    // The manifest goes now, so the recovery copy is never offered again. The copy
    // itself goes once no document has it open, as Windows cannot remove an open file.
    const QString snapshot = std::exchange(recoveryPath_, QString());
    clearRecovery();
    if (job_ && !doc_) {
        // A load still reading gives up at its next step. The copy goes once what the
        // load read is closed.
        job_->state.stop();
        abandon(job_, snapshot);
        return;
    }
    if (job_) {
        // The operation starts no further commit. It does not use the document,
        // which closes as below. A commit already started can finish.
        job_->state.stop();
        abandon(job_, {});
    }
    if (!doc_)
        appfiles::remove(snapshot);
    else if (snapshot.isEmpty())
        Document::closeInBackground(std::move(doc_));
    else
        Document::closeInBackground(std::move(doc_), [snapshot] { appfiles::remove(snapshot); });
}

void TabPage::abandon(const std::shared_ptr<Job> &job, const QString &removeAfter)
{
    std::unique_ptr<Document> document;
    bool ended = false;
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        job->abandoned = true;
        if (job->phase == Job::Phase::Stored) {
            if (const auto result = std::exchange(job->result, nullptr))
                document.reset(result->document.release());
            job->setPhase(document ? Job::Phase::Closing : Job::Phase::Ended);
        }
        ended = job->phase == Job::Phase::Ended;
        if (!ended && !removeAfter.isEmpty())
            job->removeAfter = removeAfter;
    }
    // A stored document closes in the background and the job ends after it, as one the
    // job closes itself, so a file left for removal goes only once nothing has it open.
    if (document)
        Document::closeInBackground(std::move(document), [job] { job->endClosed(); });
    if (ended)
        appfiles::remove(removeAfter);
}

void TabPage::stopLoading()
{
    loading_ = false;
    stopSlowClock();
    const std::shared_ptr<Job> job = std::exchange(job_, nullptr);
    if (!job)
        return;
    job->state.stop(); // it gives up at its next step
    std::shared_ptr<OpenResult> result;
    {
        std::unique_lock<std::mutex> lock(job->mutex);
        job->changed.wait(lock, [&job] {
            return job->phase == Job::Phase::Stored || job->phase == Job::Phase::Ended;
        });
        result = std::move(job->result);
        job->setPhase(Job::Phase::Ended);
    }
    // Closes here, before the caller removes or replaces a file it may have open. A load
    // the user stopped ends only once what it read is closed.
    if (result)
        delete result->document.release();
}

std::shared_ptr<TabPage::OpenResult> TabPage::readDocument(DocumentOpener &opener,
                                                         const QString &path,
                                                         const QString &password)
{
    auto result = std::make_shared<OpenResult>();
    result->source = sourceStamp(path);
    result->document.reset(
        opener.open(path, password, &result->error, &result->needsPassword).release());
    if (!result->document)
        return result;
    result->document->hasForm(); // cache the answer before the viewer installs it
    if (result->document->hasMervinMeasurements()) {
        if (auto blob = MeasureExport::readMervinBlob(path, password)) {
            MeasureDoc measurements;
            if (parseMeasurements(*blob, &measurements))
                result->measurements = std::move(measurements);
        }
    }
    return result;
}

void TabPage::installDocument(const std::shared_ptr<OpenResult> &result,
                              std::function<void()> afterPrevious)
{
    viewer_->setDocument(nullptr);
    // Closing waits for render work in the document, which can be stuck on a drive
    // that stopped answering.
    if (doc_)
        Document::closeInBackground(std::move(doc_), std::move(afterPrevious));
    else if (afterPrevious)
        afterPrevious();
    doc_.reset(result->document.release());
    loadStopped_ = false;
    viewer_->setDocument(doc_.get());
    if (result->measurements) {
        const auto &md = *result->measurements;
        viewer_->loadMeasurements(md.measurements, md.overridesModel(), md.unit,
                                  md.precision, md.lineWidth);
        measurePanel_->setUnit(md.unit);
        measurePanel_->setPrecision(md.precision);
        measurePanel_->setLineWidth(md.lineWidth);
    }
    stopSlowClock();
    placeholder_->hide();
    viewer_->show();
}

void TabPage::setPath(const QString &path, bool resolve)
{
    const QFileInfo fi(path);
    path_ = fi.absoluteFilePath();
    canonicalPath_ = resolve ? fi.canonicalFilePath() : QDir::cleanPath(path_);
    if (canonicalPath_.isEmpty())
        canonicalPath_ = path_;
}

bool TabPage::claimPath(const QString &path)
{
    const QString key = normalizePathKey(path);
    if (openPath_ && openPath_->key() == key)
        return true;
    auto claim = std::make_unique<job::OpenPath>(key);
    if (!claim->admitted())
        return false;
    openPath_ = std::move(claim);
    return true;
}

QString TabPage::pathBeingWrittenError()
{
    //: Reason when opening a page-tool output is refused until the tool finishes
    //: writing it. This is an active write reservation, not a password or permission.
    return tr("This file is being written. Try again when the operation finishes.");
}

bool TabPage::open(const QString &path, const QString &password, QString *error,
                   bool *needsPassword)
{
    stopLoading();
    const auto result = readDocument(*engine_->opener(), path, password);
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
    auto result = readDocument(*engine_->opener(), privateCopy, password_);
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
        // Close the private copy here, before its removal, as Windows cannot remove an
        // open file.
        delete result->document.release();
        QFile::remove(privateCopy);
        recoveryPath_ = previousSnapshot;
        recoveryManifest_ = previousManifest;
        return false;
    }
    // The document shown before can have the previous copy open, so that goes once it
    // closed. The manifest stays if the removal fails, so the copy remains
    // discoverable.
    if (!previousManifest.isEmpty())
        claimedRecoveries.remove(previousManifest);
    installDocument(result, [previousSnapshot, previousManifest] {
        if ((previousSnapshot.isEmpty() || QFile::remove(previousSnapshot))
            && !previousManifest.isEmpty())
            QFile::remove(previousManifest);
    });
    restoreViewer();
    retainedEdits_ = true;
    if (snapshot != previousSnapshot)
        QFile::remove(snapshot);
    emit stateChanged();
    return true;
}

void TabPage::clearRecovery()
{
    // On the background thread that writes the app's files, after the writes
    // asked for before.
    if (!recoveryManifest_.isEmpty()) {
        claimedRecoveries.remove(recoveryManifest_);
        appfiles::remove(recoveryManifest_);
        recoveryManifest_.clear();
    }
    if (!recoveryPath_.isEmpty()) {
        appfiles::remove(recoveryPath_);
        recoveryPath_.clear();
    }
}

QList<TabPage::RecoverableDocument> TabPage::recoverableDocuments()
{
    QList<RecoverableDocument> documents;
    foundManifests.clear();
    const QDir directory(recoveryDirectory());
    for (const QString &name : directory.entryList({QStringLiteral("*.json")}, QDir::Files,
                                                  QDir::Time)) {
        const QString path = directory.filePath(name);
        const auto manifest = readManifest(path);
        if (manifest.isEmpty())
            continue;
        foundManifests.append({path, manifest});
        documents.append({manifest.value("original").toString(),
                          manifest.value("canonical").toString()});
    }
    return documents;
}

QStringList TabPage::recoverablePaths()
{
    // The canonical path, as the session stores it, so a document both restored
    // and recovered gets one tab: lazy tabs take their path as canonical.
    QStringList paths;
    for (const RecoverableDocument &document : recoverableDocuments())
        paths.append(document.canonical.isEmpty() ? document.original : document.canonical);
    return paths;
}

bool TabPage::adoptRecovery()
{
    const QDir directory(recoveryDirectory());
    for (qsizetype i = 0; i < foundManifests.size(); ++i) {
        const QString path = foundManifests.at(i).first;
        const QJsonObject manifest = foundManifests.at(i).second;
        if (claimedRecoveries.contains(path))
            continue;
        if (manifest.value("original").toString() != path_
            && manifest.value("canonical").toString() != canonicalPath_)
            continue;
        // Adopted once: the tab keeps the copy or removes it, so it is never
        // offered again, even after the tab lets it go.
        foundManifests.removeAt(i);
        recoveryManifest_ = path;
        recoveryPath_ = directory.filePath(manifest.value("snapshot").toString());
        // The cached manifest knows the original identity even if its drive is
        // away. Keep this key separate from the private snapshot that gets read.
        const QString canonical = manifest.value("canonical").toString();
        if (!canonical.isEmpty())
            canonicalPath_ = QDir::cleanPath(canonical);
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

void TabPage::initializeSuspended(const QString &path, const ViewState &state, bool recover)
{
    if (doc_ || loading_)
        return;
    setPath(path, /*resolve=*/false);
    saved_->viewer = viewer_->captureResumeState();
    saved_->viewer.view = state;
    if (recover)
        adoptRecovery();
    showPlaceholder(tr("Document is unloaded."), PlaceholderButton::None);
    emit stateChanged();
}

void TabPage::initializeFresh(const QString &path, const FreshOpen &options)
{
    if (doc_ || loading_)
        return;
    freshOpen_ = options;
    initializeSuspended(path, options.view.value_or(viewer_->captureResumeState().view),
                        /*recover=*/!options.allowDuplicate);
}

void TabPage::showPlaceholder(const QString &message, PlaceholderButton button)
{
    viewer_->hide();
    placeholderLabel_->setText(message);
    retryButton_->setVisible(button == PlaceholderButton::Retry);
    stopButton_->setVisible(button == PlaceholderButton::StopWaiting);
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
    // A crash checkpoint is newer than the ordinary last-saved view-state file. A busy
    // tab keeps the view it shows, as moving the viewer would read the document.
    if ((!doc_ && !recoveryPath_.isEmpty()) || busy_)
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
    if (busy_)
        return false; // the window does not unload a busy tab, so this says nothing
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
    // The document closes on a background thread, as a render stuck reading it from
    // a share that stopped answering would otherwise hold up the window. It can have
    // the previous checkpoint open until then, so that goes after it. Its manifest
    // stays if the removal fails, so that file remains discoverable.
    if (!previousManifest.isEmpty())
        claimedRecoveries.remove(previousManifest);
    if (previousSnapshot.isEmpty() && previousManifest.isEmpty()) {
        Document::closeInBackground(releaseDocument());
    } else {
        Document::closeInBackground(releaseDocument(), [previousSnapshot, previousManifest] {
            if ((previousSnapshot.isEmpty() || QFile::remove(previousSnapshot))
                && !previousManifest.isEmpty())
                QFile::remove(previousManifest);
        });
    }
    showPlaceholder(tr("Document is unloaded."), PlaceholderButton::None);
    emit stateChanged();
    return true;
}

bool TabPage::finishResume(const std::shared_ptr<OpenResult> &result)
{
    loading_ = false;
    loadStopped_ = false;
    stopSlowClock();
    if (!result->document) {
        showPlaceholder(result->error.isEmpty() ? tr("Could not load this document.") : result->error,
                        PlaceholderButton::Retry);
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
    const auto result = readDocument(*engine_->opener(),
                                     recoveryPath_.isEmpty() ? path_ : recoveryPath_, password_);
    if (error)
        *error = result->error;
    if (needsPassword)
        *needsPassword = result->needsPassword;
    return finishResume(result);
}

void TabPage::resumeAsync()
{
    if (doc_ || loading_ || loadStopped_)
        return;
    startLoad();
}

void TabPage::startLoad(std::shared_ptr<Job> after)
{
    // All new reads, including Retry, must first own their known path.
    if (!claimPath(canonicalPath_)) {
        const auto failure = std::make_shared<OpenResult>();
        failure->error = pathBeingWrittenError();
        const QPointer<TabPage> self(this);
        finishResume(failure);
        if (self)
            emit resumeFinished(false, failure->error, false);
        return;
    }
    const auto job = std::make_shared<Job>(recoveryPath_.isEmpty() ? path_ : recoveryPath_);
    ++loadGeneration_;
    job_ = job;
    loading_ = true;
    //: Shown in place of the page while the document loads.
    showPlaceholder(tr("Loading"), PlaceholderButton::None);
    startSlowClock();
    background::start([tab = QPointer<QObject>(this), job, after = std::move(after),
                       opener = engine_->opener(), password = password_,
                       resolve = freshOpen_.has_value() && recoveryPath_.isEmpty(),
                       knownCanonical = canonicalPath_, writingError = pathBeingWrittenError()] {
        // A stopped load still closing what it read ends first, so a recovery copy left
        // for this job to remove never goes while that load has it open.
        if (after)
            after->waitUntilEnded();
        std::shared_ptr<OpenResult> result;
        {
            const job::Scope scope(job->state);
            // First, so a drive that does not answer the first file call is named.
            if (job::reading(job->source)) {
                // Resolve links only on this thread. Keep the public key unchanged
                // until the owner accepts the canonical identity through its queue.
                QString canonical = knownCanonical;
                std::unique_ptr<job::OpenPath> admission;
                if (resolve) {
                    const QFileInfo file(job->source);
                    canonical = file.canonicalFilePath();
                    if (canonical.isEmpty())
                        canonical = QDir::cleanPath(file.absoluteFilePath());
                    admission = std::make_unique<job::OpenPath>(normalizePathKey(canonical));
                }
                if (!job::stopped() && (!admission || admission->admitted()))
                    result = readDocument(*opener, job->source, password);
                else {
                    result = std::make_shared<OpenResult>();
                    if (!job::stopped())
                        result->error = writingError;
                }
                result->canonical = canonical;
                // Keep the resolved target admitted while the owner's queue waits.
                result->admission = std::move(admission);
            } else {
                result = std::make_shared<OpenResult>();
                result->error = job::notRunError();
            }
        }
        std::unique_lock<std::mutex> lock(job->mutex);
        if (job->abandoned) {
            job->setPhase(Job::Phase::Closing);
            lock.unlock();
            // Closes here, before endClosed() removes the recovery copy, as Windows
            // cannot remove an open file.
            delete result->document.release();
            result.reset();
            job->endClosed();
            return;
        }
        job->result = std::move(result);
        job->setPhase(Job::Phase::Stored);
        lock.unlock();
        background::post(tab, [tab, job] { static_cast<TabPage *>(tab.data())->finishLoad(job); });
    });
    emit stateChanged();
}

void TabPage::finishLoad(const std::shared_ptr<Job> &job)
{
    if (job != job_)
        return;
    if (freshOpen_) {
        QString canonical;
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            if (job->phase != Job::Phase::Stored || job->abandoned || !job->result)
                return;
            canonical = job->result->canonical;
        }
        // Reading has finished. Waiting for the owner's queue is not a drive wait.
        stopSlowClock();
        showPlaceholder(tr("Loading"), PlaceholderButton::None);
        emit freshLoadReady(canonical, loadGeneration_);
        return;
    }
    installLoad(job);
}

bool TabPage::acceptFreshLoad(quint64 generation)
{
    if (!freshOpen_ || generation != loadGeneration_ || !job_ || !loading_)
        return false;
    return installLoad(job_);
}

void TabPage::rejectFreshLoad(quint64 generation)
{
    if (!freshOpen_ || generation != loadGeneration_ || !job_ || !loading_)
        return;
    abandon(job_, {});
    job_.reset();
    const auto failure = std::make_shared<OpenResult>();
    failure->error = pathBeingWrittenError();
    const QPointer<TabPage> self(this);
    finishResume(failure);
    if (self)
        emit resumeFinished(false, failure->error, false);
}

bool TabPage::installLoad(const std::shared_ptr<Job> &job)
{
    if (job != job_)
        return false;
    std::shared_ptr<OpenResult> result;
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        if (job->phase != Job::Phase::Stored || job->abandoned)
            return false;
        result = std::move(job->result);
        job->setPhase(Job::Phase::Ended);
    }
    job_.reset();
    if (freshOpen_ && result->document) {
        setPath(result->canonical, /*resolve=*/false);
        if (result->admission)
            openPath_ = std::move(result->admission);
        freshOpen_.reset();
    }
    const QPointer<TabPage> self(this);
    const bool success = finishResume(result);
    if (self)
        emit resumeFinished(success, result->error, result->needsPassword);
    return success;
}

void TabPage::stopWaiting()
{
    if (busy_) {
        // The job gives up at its next step, and a commit it already began completes.
        // Either way the tab treats the operation as not done.
        job_->state.stop();
        abandon(job_, {});
        job_.reset();
        finished_ = nullptr;
        const QPointer<TabPage> self(this);
        endOperation();
        if (self)
            emit operationFinished(false);
        return;
    }
    if (!loading_)
        return;
    loading_ = false;
    loadStopped_ = true;
    stopSlowClock();
    if (freshOpen_ && recoveryPath_.isEmpty()) {
        job_->state.stop();
        abandon(job_, {});
        job_.reset();
        stopButton_->hide();
        emit freshOpenStopped(loadGeneration_);
        return;
    }
    // The job stays in the slot, so Retry can take back its read while it runs. A
    // result it already stored closes in the background. The recovery copy stays.
    abandon(job_, {});
    showPlaceholder(tr("Document is unloaded."), PlaceholderButton::Retry);
    emit stateChanged();
}

void TabPage::retryLoad()
{
    loadStopped_ = false;
    if (doc_ || loading_)
        return;
    std::shared_ptr<Job> closing; // a stopped load that still closes what it read
    if (job_) {
        std::lock_guard<std::mutex> lock(job_->mutex);
        if (job_->phase == Job::Phase::Reading) {
            // The stopped read still runs. Wait for it again rather than read twice.
            job_->abandoned = false;
            loading_ = true;
        } else if (job_->phase == Job::Phase::Closing) {
            closing = job_;
        }
    }
    if (!loading_) {
        startLoad(std::move(closing));
        return;
    }
    showPlaceholder(tr("Loading"), PlaceholderButton::None);
    startSlowClock();
    emit stateChanged();
}

void TabPage::startOperation(Operation operation)
{
    Q_ASSERT(isUsable() && !job_);
    if (!isUsable() || job_)
        return;
    // Edits still being typed go into the document while nothing else reads it, and
    // pauseForOperation() removes the editors, so a later focus change cannot commit.
    viewer_->commitActiveFormEditor();
    viewer_->commitActiveAnnotEditor();
    // One ordinary paint while the document is idle. A tab out of view shows the plain
    // canvas.
    still_->setPicture(isVisible() ? viewer_->grab() : QPixmap());
    scrollColumn_ = std::max(0, viewer_->width() - viewer_->viewport()->width());
    viewer_->pauseForOperation();
    viewer_->hide();
    still_->show();
    busy_ = true;
    destination_ = operation.destination;
    finished_ = std::move(operation.finished);
    barName_.clear();
    barFolder_.clear();
    const auto job = std::make_shared<Job>(QString());
    job_ = job;
    background::start([tab = QPointer<QObject>(this), job, work = std::move(operation.work)] {
        {
            const job::Scope scope(job->state);
            work();
        }
        std::unique_lock<std::mutex> lock(job->mutex);
        if (job->abandoned) {
            job->setPhase(Job::Phase::Ended);
            return;
        }
        job->setPhase(Job::Phase::Stored);
        lock.unlock();
        background::post(tab, [tab, job] {
            static_cast<TabPage *>(tab.data())->finishOperation(job);
        });
    });
    startSlowClock();
    emit stateChanged();
}

void TabPage::finishOperation(const std::shared_ptr<Job> &job)
{
    if (job != job_)
        return; // the user stopped waiting for it
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        if (job->phase != Job::Phase::Stored || job->abandoned)
            return;
        job->setPhase(Job::Phase::Ended);
    }
    job_.reset();
    // Copied first, as what the signals start may close the tab.
    const std::function<void()> finished = std::exchange(finished_, nullptr);
    const QPointer<TabPage> self(this);
    endOperation();
    if (!self)
        return;
    emit operationFinished(true);
    if (self && finished)
        finished();
}

void TabPage::endOperation()
{
    busy_ = false;
    stopSlowClock();
    waitBar_->hide();
    still_->hide();
    still_->setPicture({});
    viewer_->show();
    viewer_->resumeAfterOperation();
    emit stateChanged();
}

void TabPage::setSlowDelayForTesting(int ms)
{
    slowDelayMs = ms;
}

std::shared_ptr<DocumentLifetime> TabPage::testing::storedDocument(const TabPage &tab)
{
    if (!tab.job_)
        return {};
    std::lock_guard<std::mutex> lock(tab.job_->mutex);
    const auto &result = tab.job_->result;
    return result && result->document ? result->document->lifetime() : nullptr;
}

QString TabPage::testing::slowNotice(const QString &name, const QString &folder,
                                     const QFontMetrics &metrics, int width)
{
    return mervin::slowNotice(name, folder, metrics, width);
}

void TabPage::startSlowClock()
{
    slowFile_.clear();
    seen_ = {};
    quiet_.start();
    slowPoll_->start();
}

void TabPage::stopSlowClock()
{
    slowPoll_->stop();
    slowFile_.clear();
}

void TabPage::pollJob()
{
    if (!(loading_ || busy_) || !job_) {
        stopSlowClock();
        return;
    }
    const job::State::Snapshot now = job_->state.snapshot();
    // The first report starts the job rather than shows progress. A later step or a
    // piece written does.
    if ((seen_.step != job::Step::None && (now.step != seen_.step || now.path != seen_.path))
        || now.ticks != seen_.ticks)
        quiet_.restart();
    seen_ = now;
    if (busy_) {
        // Once shown, the bar stays until the operation ends, and names the latest step.
        if (!waitBar_->isHidden() || quiet_.elapsed() >= slowDelayMs)
            showWaitBar(now);
        return;
    }
    if (slowFile_.isEmpty() && quiet_.elapsed() < slowDelayMs)
        return;
    // Once shown, the notice stays until the load ends, and names the latest file.
    const QString file = now.path.isEmpty() ? job_->source : now.path;
    if (file == slowFile_)
        return;
    slowFile_ = file;
    showPlaceholder({}, PlaceholderButton::StopWaiting);
    updateSlowNotice();
}

void TabPage::updateSlowNotice()
{
    if (slowFile_.isEmpty())
        return;
    // The tab's own name, also while it reads the recovery copy, with the folder of
    // the file it waits for, so the notice names the drive that does not answer.
    const QString folder = QDir::toNativeSeparators(QFileInfo(slowFile_).absolutePath());
    const QString text = slowNotice(tabTitle(), folder, placeholderLabel_->fontMetrics(),
                                    placeholderLabel_->contentsRect().width());
    if (text != placeholderLabel_->text())
        placeholderLabel_->setText(text);
}

void TabPage::showWaitBar(const job::State::Snapshot &now)
{
    // A write names its folder, and a commit the file it replaces. A read of a file in
    // the destination counts as saving there. Before its first step, and while it
    // prints, the operation waits for its destination.
    QString name;
    QString folder = destination_;
    switch (now.step) {
    case job::Step::Reading:
        if (normalizePathKey(QFileInfo(now.path).absolutePath()) != normalizePathKey(destination_)) {
            name = QFileInfo(now.path).fileName();
            folder = QFileInfo(now.path).absolutePath();
        }
        break;
    case job::Step::Writing:
        folder = now.path;
        break;
    case job::Step::Commit:
        folder = QFileInfo(now.path).absolutePath();
        break;
    case job::Step::None:
    case job::Step::Printing:
        break;
    }
    if (!waitBar_->isHidden() && name == barName_ && folder == barFolder_)
        return;
    barName_ = name;
    barFolder_ = folder;
    waitBar_->setNotice(waitSentence(name), name, QDir::toNativeSeparators(folder));
    if (waitBar_->isHidden()) {
        waitBar_->show();
        waitBar_->raise();
        placeWaitBar();
    }
}

void TabPage::placeWaitBar()
{
    // The scroll bar's column is on the left in a right-to-left layout.
    waitBar_->place(layoutDirection() == Qt::RightToLeft ? scrollColumn_ : 0,
                    std::max(0, width() - scrollColumn_));
}

void TabPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (!waitBar_->isHidden())
        placeWaitBar();
}

bool TabPage::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == placeholderLabel_ && event->type() == QEvent::Resize)
        updateSlowNotice();
    if (watched == waitBar_
        && (event->type() == QEvent::Resize || event->type() == QEvent::Show
            || event->type() == QEvent::Hide))
        still_->setVeil(waitBar_->isHidden() ? QRect() : waitBar_->geometry());
    return QWidget::eventFilter(watched, event);
}

void TabPage::detachDocument()
{
    releaseDocument(); // closes the file handle here
}

std::unique_ptr<Document> TabPage::releaseDocument()
{
    stopLoading();
    if (doc_) {
        saved_->viewer = viewer_->captureResumeState();
        saved_->restoreTools = true;
    }
    viewer_->setDocument(nullptr); // clear the viewer's raw Document* first
    return std::move(doc_);
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
