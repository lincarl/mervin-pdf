#include "dialogs/MergeDialog.h"
#include "ui/TextLayout.h"

#include "dialogs/LayoutMinimum.h"
#include "dialogs/RowList.h"
#include "io/SaveCheck.h"
#include "io/JobContext.h"
#include "print/PageRange.h"
#include "recent/PathKey.h"
#include "recent/RecentStore.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/WaitBar.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace mervin {

namespace {

// Save as as the merge will write it: trimmed, with ".pdf" appended if missing.
QString withPdf(const QString &text)
{
    const QString out = text.trimmed();
    return out.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive) ? out
                                                                     : out + QStringLiteral(".pdf");
}

// Symlinks resolved when the file exists (the tabs hold canonical paths).
QString canonicalOrAbsolute(const QString &path)
{
    const QFileInfo fi(path);
    const QString canonical = fi.canonicalFilePath();
    return canonical.isEmpty() ? fi.absoluteFilePath() : canonical;
}

// Merge's own column widths; RowList owns the rest. Sized so the longest values
// ("Unreadable", "12 of 999") still fit at 125% Windows text scaling. A longer
// translation widens its column instead (see RowList::widthFor).
constexpr int kColSpec = 124;
constexpr int kColCount = 92;
constexpr int kColOutput = 74;
// A field's frame and padding around its text (Theme: 1px border, 8px padding,
// plus the line edit's own 2px margin, on each side).
constexpr int kFieldPadding = 24;

QString specPlaceholder()
{
    //: Placeholder of an empty page range field in the merge list.
    return MergeDialog::tr("e.g. 1-3, 5");
}

/// Shows a window-modal warning that tolerates deletion of its owner.
void warnInWindow(QWidget *parent, const QString &text)
{
    const QPointer<QMessageBox> box(new QMessageBox(QMessageBox::Warning,
        MergeDialog::tr("Merge PDFs"), text, QMessageBox::Ok, parent));
    box->setWindowModality(Qt::WindowModal);
    box->exec();
    delete box.data();
}
} // namespace

/// Collects one warning per surviving failed row after an Add Files batch settles.
struct MergeDialog::ProbeBatch
{
    int remaining = 0;
    QList<std::pair<quint64, QString>> warnings;
};

MergeDialog::MergeDialog(const QString &initialPath, int initialPageCount,
                         const QString &initialPassword, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Merge PDFs"));
    resize(760, 520);

    auto *layout = new QVBoxLayout(this);

    auto *hint = new QLabel(tr("Files are merged top to bottom, in the order shown."), this);
    hint->setObjectName(QStringLiteral("mergeHint"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    // ── List + button column ─────────────────────────────────────────────────
    auto *middle = new QHBoxLayout;
    middle->setContentsMargins(0, 0, 0, 0);
    middle->setSpacing(8);

    auto *listSide = new QVBoxLayout;
    listSide->setContentsMargins(0, 0, 0, 0);
    listSide->setSpacing(4);

    // Column captions over a QListWidget of row widgets, rather than a
    // QTableWidget; RowList explains why.
    //: Column caption over the page range fields.
    const QString specCaption = tr("Pages, in order");
    specWidth_ = qMax(
        RowList::widthFor(kColSpec, RowList::captionFont(font()), {specCaption}, 4),
        RowList::widthFor(kColSpec, font(), {specPlaceholder(), PageRange::allKeyword()},
                          kFieldPadding));
    list_ = new RowList(RowList::widthFor(kColCount, font(), MergePlan::widestCountTexts(), 4),
                        kColOutput, this);
    list_->setObjectName(QStringLiteral("mergeList"));
    listSide->addWidget(list_->makeHeader([this, &specCaption](QWidget *header,
                                                               QHBoxLayout *columns) {
        //: Column caption over the file names.
        columns->addWidget(new QLabel(tr("File"), header), 1);
        auto *spec = new QLabel(specCaption, header);
        spec->setFixedWidth(specWidth_);
        columns->addWidget(spec);
    }));
    list_->onRowDropped = [this](int from, int gap) {
        // Move the plan now, rebuild the widgets after the drag machinery has
        // unwound: the drop arrives inside QDrag::exec()'s nested event loop, and
        // rebuilding here would delete the very grip widget whose event filter is
        // still on the stack.
        const int landed = plan_.moveToGap(from, gap);
        if (landed < 0)
            return; // dropped back where it already was
        QTimer::singleShot(0, this, [this, landed] { rebuild(landed); });
    };
    list_->onMove = [this](int delta) { moveCurrent(delta); };
    list_->onRemove = [this] { removeCurrent(); };
    connect(list_, &QListWidget::currentRowChanged, this, [this] { refreshFooter(); });
    listSide->addWidget(list_, 1);

    //: %1 is the word for every page (PageRange "All"), which can be typed
    //: instead of a range. "The Pages column" is the column captioned
    //: "Pages, in order".
    auto *specHint = new QLabel(tr("Type a page range in the Pages column, for example "
                                   "1-3, 5, 8-10 - or %1. Pages are taken in the order "
                                   "you type them.")
                                    .arg(PageRange::allKeyword()),
                                this);
    specHint->setWordWrap(true);
    specHint->setObjectName(QStringLiteral("mergeHint"));
    listSide->addWidget(specHint);
    middle->addLayout(listSide, 1);

    auto *side = new QVBoxLayout;
    side->setContentsMargins(0, 0, 0, 0);
    side->setSpacing(6);
    const QColor ink = Theme::iconInk(palette());

    auto *addBtn = new QPushButton(icons::glyph(icons::Glyph::Open, ink), tr("Add Files…"), this);
    addBtn->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_O));
    connect(addBtn, &QPushButton::clicked, this, &MergeDialog::addFiles);
    side->addWidget(addBtn);

    upBtn_ = new QPushButton(tr("Move Up"), this);
    connect(upBtn_, &QPushButton::clicked, this, [this] { moveCurrent(-1); });
    side->addWidget(upBtn_);

    downBtn_ = new QPushButton(tr("Move Down"), this);
    connect(downBtn_, &QPushButton::clicked, this, [this] { moveCurrent(1); });
    side->addWidget(downBtn_);

    side->addSpacing(6);

    //: Button: adds a copy of the selected row below it.
    duplicateBtn_ = new QPushButton(icons::glyph(icons::Glyph::Copy, ink), tr("Duplicate"), this);
    connect(duplicateBtn_, &QPushButton::clicked, this, &MergeDialog::duplicateCurrent);
    side->addWidget(duplicateBtn_);

    //: Button: removes the selected row from the list.
    removeBtn_ = new QPushButton(icons::glyph(icons::Glyph::Delete, ink), tr("Remove"), this);
    connect(removeBtn_, &QPushButton::clicked, this, &MergeDialog::removeCurrent);
    side->addWidget(removeBtn_);

    side->addStretch(1);
    middle->addLayout(side, 0);
    layout->addLayout(middle, 1);

    // RowList has the move and remove shortcuts; Duplicate joins them, scoped
    // the same way.
    auto *dup = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_D), list_);
    dup->setContext(Qt::WidgetWithChildrenShortcut);
    connect(dup, &QShortcut::activated, this, &MergeDialog::duplicateCurrent);

    // ── Summary, error, output ───────────────────────────────────────────────
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("mergeSummary"));
    layout->addWidget(summary_);

    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("mergeError"));
    error_->setTextFormat(Qt::PlainText);
    error_->setWordWrap(true);
    error_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    error_->installEventFilter(this);
    // Reserve the line even when empty so the buttons below never jump as the
    // user types a range in and out of validity.
    error_->setMinimumHeight(error_->fontMetrics().lineSpacing());
    layout->addWidget(error_);

    auto *outRow = new QHBoxLayout;
    outRow->setContentsMargins(0, 0, 0, 0);
    outRow->setSpacing(8);
    outRow->addWidget(new QLabel(tr("Save as:"), this));
    outputEdit_ = new QLineEdit(this);
    outputEdit_->setObjectName(QStringLiteral("mergeOutput"));
    outputEdit_->setPlaceholderText(tr("Choose where to write the merged PDF"));
    fitPlaceholder(*outputEdit_);
    connect(outputEdit_, &QLineEdit::textEdited, this, [this](const QString &) {
        // Latch on any edit, including the one that empties the field. Tracking
        // emptiness instead would refill the field the instant the user cleared
        // it, so select-all-delete-retype silently appended to the old path.
        // textEdited fires only for real input - setText() emits textChanged
        // alone - so this cannot be tripped by our own refresh.
        outputEdited_ = true;
        writeError_.clear(); // a new destination deserves a fresh attempt
        refreshFooter();
    });
    outRow->addWidget(outputEdit_, 1);
    auto *browse = new QPushButton(tr("Browse…"), this);
    connect(browse, &QPushButton::clicked, this, &MergeDialog::browseForOutput);
    outRow->addWidget(browse, 0);
    layout->addLayout(outRow);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    //: Button: writes the merged PDF.
    mergeBtn_ = buttons->addButton(tr("Merge"), QDialogButtonBox::AcceptRole);
    mergeBtn_->setObjectName(QStringLiteral("mergeAccept"));
    mergeBtn_->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &MergeDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &MergeDialog::reject);
    layout->addWidget(buttons);

    saveCheck_ = new SaveCheck(this);
    connect(saveCheck_, &SaveCheck::slow, this, &MergeDialog::showProblem);

    Q_UNUSED(initialPageCount);
    if (!initialPath.isEmpty())
        queueProbe(initialPath, initialPassword);
    rebuild(plan_.isEmpty() ? -1 : 0);
    followLayoutMinimum(*this, QSize(660, 440));
}

MergeDialog::~MergeDialog()
{
    for (const auto &[ticket, state] : probeStates_)
        state->stop();
}

MergeDialog::MergeDialog(const MergePlan &plan, const QString &output, const QString &error,
                         int failedIndex, Conflict conflict, QWidget *parent)
    : MergeDialog(QString(), 0, QString(), parent)
{
    plan_ = plan;
    for (const auto &entry : plan_.entries())
        nextProbe_ = qMax(nextProbe_, entry.probeTicket);
    outputEdited_ = true;
    outputEdit_->setText(QDir::toNativeSeparators(output));
    rebuild(plan_.isEmpty() ? -1 : 0);
    writeError_ = failureSummary(plan_, output, failedIndex, conflict);
    writeErrorDetail_ = QDir::toNativeSeparators(error);
    refreshFooter();
}

QString MergeDialog::failureSummary(const MergePlan &plan, const QString &output,
                                    int failedIndex, Conflict conflict)
{
    if (conflict == Conflict::Input) {
        return tr("The merged file cannot replace one of the files being "
                  "merged. Choose a different name.");
    } else if (conflict == Conflict::OpenTab) {
        return tr("That file is open in a tab. Choose another name.");
    } else if (failedIndex >= 0 && failedIndex < plan.count()) {
        //: %1 is the name of the file being merged when it failed.
        return tr("The merge failed on \"%1\".").arg(plan.displayName(failedIndex));
    } else {
        //: %1 is the merged file's name.
        return tr("Could not write \"%1\". If another program has it open, close "
                  "it and press Merge again, or choose another name.")
                   .arg(QFileInfo(output).fileName());
    }
}

void MergeDialog::queueProbe(const QString &path, const QString &password,
                             const std::shared_ptr<ProbeBatch> &batch, int index)
{
    MergePlan::Entry entry;
    entry.path = path;
    entry.load = MergePlan::Load::Reading;
    entry.probeTicket = ++nextProbe_;
    plan_.append(entry);
    const auto state = std::make_shared<job::State>();
    probeStates_.emplace(entry.probeTicket, state);
    auto &queue = probes_[RecentStore::volumeKey(path)];
    if (!queue)
        queue = std::make_unique<background::SerialQueue>();
    queue->add([guard = QPointer<QObject>(this), path, password, state,
                ticket = entry.probeTicket, batch, index] {
        MergePlan::Entry result;
        {
            const job::Scope scope(*state);
            result = probeEntry(path, password);
        }
        background::post(guard, [guard, ticket, result, batch, index] {
            static_cast<MergeDialog *>(guard.data())->finishProbe(ticket, result, batch, index);
        });
    });
}

void MergeDialog::finishProbe(quint64 ticket, const MergePlan::Entry &entry,
                              const std::shared_ptr<ProbeBatch> &batch, int index)
{
    probeStates_.erase(ticket);
    if (closed_)
        return;
    const bool remains = plan_.resolveProbe(ticket, entry);
    const QPointer<MergeDialog> self(this);
    refreshRows();
    refreshFooter();
    if (!self || !batch)
        return;
    if (remains && entry.load != MergePlan::Load::Ok) {
        //: One line of the "cannot be merged" list: %1 is a file name, %2 why.
        batch->warnings[index] = {ticket,
            tr("%1 - %2").arg(QFileInfo(entry.path).fileName(), entry.loadError)};
    }
    if (--batch->remaining != 0)
        return;
    QStringList warnings;
    for (const auto &[id, warning] : batch->warnings)
        if (id && std::any_of(plan_.entries().cbegin(), plan_.entries().cend(),
                             [id](const MergePlan::Entry &row) { return row.probeTicket == id; }))
            warnings.append(warning);
    if (warnings.isEmpty())
        return;
    const QPointer<QMessageBox> box(new QMessageBox(QMessageBox::Warning, tr("Merge PDFs"),
        //: %1 is a list of files, one per line. The number of files picks the
        //: singular or plural form.
        tr("These files cannot be merged:\n\n%1", nullptr, int(warnings.size()))
            .arg(warnings.join(QStringLiteral("\n"))), QMessageBox::Ok, this));
    box->setWindowModality(Qt::WindowModal);
    box->exec();
    delete box.data();
}

MergePlan::Entry MergeDialog::probeEntry(const QString &path, const QString &password)
{
    MergePlan::Entry e;
    e.path = path;
    int n = 0;
    QString err;
    PageOps::Status st = PageOps::probe(path, &n, QString(), &err);
    if (st == PageOps::Status::NeedsPassword && !password.isEmpty()) {
        st = PageOps::probe(path, &n, password, &err);
        if (st == PageOps::Status::Ok)
            e.password = password; // MergePlan::inputs() hands it to the merge
    }
    switch (st) {
    case PageOps::Status::Ok:
        e.load = n > 0 ? MergePlan::Load::Ok : MergePlan::Load::Unreadable;
        e.pageCount = n;
        if (n <= 0)
            e.loadError = tr("The file contains no pages.");
        break;
    case PageOps::Status::NeedsPassword:
        e.load = MergePlan::Load::Locked;
        // Security works on the document in the tab and writes an unencrypted
        // copy under a new name, so both of those steps have to be spelled out -
        // "use Document > Security" alone sends the user to a menu that does not
        // act on the file they just picked here.
        //: "Document > Security" is the Security item in the Document menu. Use
        //: the same translated names as the menu.
        e.loadError = tr("This PDF is encrypted. Open it in Mervin, use "
                         "Document > Security to save an unlocked copy, then add "
                         "that copy instead.");
        break;
    case PageOps::Status::Failed:
        e.load = MergePlan::Load::Unreadable;
        e.loadError = err;
        break;
    }
    return e;
}

QString MergeDialog::startDirectory() const
{
    for (int i = plan_.count() - 1; i >= 0; --i) {
        const QString dir = QFileInfo(plan_.at(i).path).absolutePath();
        if (!dir.isEmpty())
            return dir;
    }
    return QString();
}

/// Appends files chosen in an owner-guarded, window-modal picker.
void MergeDialog::addFiles()
{
    //: File type filter. Keep "(*.pdf)" as it is.
    const QString filter = tr("PDF documents (*.pdf)");
    const QPointer<MergeDialog> self(this);
    const QPointer<QFileDialog> picker(new QFileDialog(this, tr("Add PDFs to Merge"),
                                                       startDirectory(), filter));
    picker->setFileMode(QFileDialog::ExistingFiles);
    picker->setWindowModality(Qt::WindowModal);
    const bool accepted = picker->exec() == QDialog::Accepted;
    const QStringList picked = picker && accepted ? picker->selectedFiles() : QStringList();
    delete picker.data();
    if (self)
        addPaths(picked);
}

void MergeDialog::addPaths(const QStringList &paths)
{
    if (paths.isEmpty())
        return;

    const int firstNew = plan_.count();
    const auto batch = std::make_shared<ProbeBatch>();
    batch->remaining = paths.size();
    batch->warnings.resize(paths.size());
    for (int index = 0; index < paths.size(); ++index)
        queueProbe(paths.at(index), {}, batch, index);
    rebuild(firstNew);
}

void MergeDialog::removeCurrent()
{
    const int i = currentRow();
    if (i < 0)
        return;
    plan_.remove(i);
    rebuild(qMin(i, plan_.count() - 1));
}

void MergeDialog::duplicateCurrent()
{
    const int i = currentRow();
    if (i < 0)
        return;
    rebuild(plan_.duplicate(i));
}

void MergeDialog::moveCurrent(int delta)
{
    const int i = currentRow();
    if (i < 0)
        return;
    const int to = plan_.move(i, delta);
    if (to >= 0)
        rebuild(to);
}

/// Updates the output choice only while the dialog survives its save picker.
void MergeDialog::browseForOutput()
{
    // The file dialog asks before replacing a file, so the plain default will do.
    const QString suggested =
        outputEdit_->text().isEmpty() ? plan_.plainOutputPath() : outputEdit_->text();
    const QPointer<MergeDialog> self(this);
    const QPointer<QFileDialog> picker(new QFileDialog(this, tr("Save Merged PDF"), suggested,
                                                       tr("PDF documents (*.pdf)")));
    picker->setAcceptMode(QFileDialog::AcceptSave);
    picker->setWindowModality(Qt::WindowModal);
    const bool accepted = picker->exec() == QDialog::Accepted;
    const QStringList picked = picker && accepted ? picker->selectedFiles() : QStringList();
    delete picker.data();
    if (!self || picked.isEmpty())
        return;
    outputEdit_->setText(QDir::toNativeSeparators(picked.front()));
    outputEdited_ = true;
    writeError_.clear();
    refreshFooter();
}

int MergeDialog::currentRow() const
{
    return list_ ? list_->currentRow() : -1;
}

void MergeDialog::rebuild(int selectRow)
{
    // A structural change (a row added, removed, moved) may be the fix for an
    // input that stopped the last merge, so its verdict no longer stands.
    writeError_.clear();
    nameLabels_.clear();
    nameTexts_.clear();
    badges_.clear();
    specEditors_.clear();
    list_->clear();

    for (int i = 0; i < plan_.count(); ++i) {
        const MergePlan::Entry &e = plan_.at(i);
        list_->addRow([&](QWidget *row, QHBoxLayout *h) {
            auto *name = new QLabel(row);
            name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            nameLabels_.append(name);
            nameTexts_.append(plan_.displayName(i));
            name->setToolTip(e.loadError.isEmpty()
                                 ? QDir::toNativeSeparators(e.path)
                                 : QStringLiteral("%1\n\n%2").arg(
                                       QDir::toNativeSeparators(e.path), e.loadError));
            auto *badge = new QLabel(row);
            badge->setFixedSize(18, 18);
            badges_.append(badge);
            h->addWidget(badge, 0);
            h->addWidget(name, 1);

            auto *spec = new QLineEdit(e.spec, row);
            specEditors_.append(spec);
            spec->setObjectName(QStringLiteral("mergeRowSpec"));
            spec->setFixedWidth(specWidth_);
            // A format prompt, not "All": a greyed placeholder identical to the
            // default value made an emptied cell look like the default was in
            // force while it was actually blocking the merge.
            spec->setPlaceholderText(specPlaceholder());
            spec->setEnabled(e.load == MergePlan::Load::Ok);
            // The list is rebuilt on every structural change, so this build-time
            // index stays valid for as long as the widget it is captured in exists.
            connect(spec, &QLineEdit::textChanged, this, [this, i](const QString &t) {
                plan_.setSpec(i, t);
                // Only the derived numbers change, so refresh them in place: a
                // full rebuild here would destroy the QLineEdit being typed into.
                refreshFooter();
            });
            h->addWidget(spec);
        });
    }

    refreshRows();

    if (selectRow >= 0 && selectRow < plan_.count()) {
        list_->setCurrentRow(selectRow);
        list_->scrollToItem(list_->item(selectRow));
    }
    refreshFooter();
    // Once now, and once after the layout has settled and the labels have their
    // real widths - the same two-step MeasurePanel uses for its row text.
    reelideNames();
    QTimer::singleShot(0, this, [this] { reelideNames(); });
}

void MergeDialog::refreshRows()
{
    const QColor ink = Theme::iconInk(palette());
    for (int row = 0; row < plan_.count() && row < nameLabels_.size(); ++row) {
        const auto &entry = plan_.at(row);
        nameTexts_[row] = plan_.displayName(row);
        nameLabels_.at(row)->setToolTip(entry.loadError.isEmpty()
            ? QDir::toNativeSeparators(entry.path)
            : QStringLiteral("%1\n\n%2").arg(QDir::toNativeSeparators(entry.path), entry.loadError));
        auto *badge = badges_.at(row);
        badge->setPixmap(icons::glyphPixmap(entry.load == MergePlan::Load::Locked
            ? icons::Glyph::Security : icons::Glyph::Close, ink, 16));
        badge->setToolTip(nameLabels_.at(row)->toolTip());
        badge->setVisible(entry.load == MergePlan::Load::Locked
                          || entry.load == MergePlan::Load::Unreadable);
        specEditors_.at(row)->setEnabled(entry.load == MergePlan::Load::Ok);
    }
    reelideNames();
}

void MergeDialog::reelideNames()
{
    for (int i = 0; i < nameLabels_.size() && i < nameTexts_.size(); ++i) {
        QLabel *l = nameLabels_.at(i);
        if (!l)
            continue;
        const int w = l->width();
        if (w <= 0)
            continue;
        // ElideMiddle, not ElideRight: the disambiguating " (folder)" suffix
        // displayName() appends is the whole point of the label when two rows
        // share a file name, and eliding from the right would eat it first.
        l->setText(elidedLabelText(l->fontMetrics(), nameTexts_.at(i), Qt::ElideMiddle, w));
    }
}

void MergeDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    reelideNames();
}

bool MergeDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == error_ && plan_.hasReading()
        && (event->type() == QEvent::Resize || event->type() == QEvent::FontChange))
        showProblem();
    return QDialog::eventFilter(watched, event);
}

void MergeDialog::refreshFooter()
{
    // The per-row Count and Output cells are derived from the whole plan (Output
    // is a running sum), so they are refreshed together in one pass, not per row.
    const QList<MergePlan::RowText> texts = plan_.rowTexts();
    for (int i = 0; i < list_->count() && i < texts.size(); ++i)
        list_->setRowTexts(i, texts.at(i).count, texts.at(i).output);

    summary_->setText(plan_.summaryText());

    // Native separators: QDir hands back '/' on Windows too, and a path with
    // forward slashes in a Save-as field reads as something the app generated
    // rather than somewhere on this machine. The check steps the default past a
    // name in use when it answers.
    if (!outputEdited_ && plan_.plainOutputPath() != defaultPath_) {
        defaultPath_ = plan_.plainOutputPath();
        outputEdit_->setText(QDir::toNativeSeparators(defaultPath_));
    }
    if (outputEdit_->text() != checking_ || files() != checkedFiles_)
        checkOutput();

    const int cur = currentRow();
    removeBtn_->setEnabled(cur >= 0);
    duplicateBtn_->setEnabled(cur >= 0);
    upBtn_->setEnabled(cur > 0);
    downBtn_->setEnabled(cur >= 0 && cur < plan_.count() - 1);
    showProblem();
}

std::optional<QString> MergeDialog::problem() const
{
    const QString planError = plan_.errorText();
    if (!planError.isEmpty() || plan_.isEmpty())
        return planError;
    return saveAsProblem();
}

std::optional<QString> MergeDialog::saveAsProblem() const
{
    // Read after the auto-refill so it sees the field's final text. A missing
    // destination has to reach the error line: it disables Merge, and without a
    // reason there the button would grey out with nothing on screen saying why.
    const QString text = outputEdit_->text();
    if (text.trimmed().isEmpty())
        return tr("Choose where to save the merged PDF.");
    if (!checked_ || checked_->text != text)
        return std::nullopt;
    // The viewer keeps an open tab's file open, so the write would fail only once
    // Merge was pressed.
    if (openKeys_.contains(checked_->key))
        return tr("That file is open in a tab. Choose another name.");
    return QString();
}

void MergeDialog::showProblem()
{
    const QString planReason = fitPendingReason(plan_.errorText());
    const bool planOk = planReason.isEmpty() && !plan_.isEmpty();
    const std::optional<QString> saveAsReason =
        planOk ? saveAsProblem() : std::optional<QString>(QString());
    // A slow check of the text, or the one Merge asked for when pressed, says so and
    // holds Merge.
    if (planOk && saveCheck_->isSlow() && (!saveAsReason || pressed_)) {
        error_->setText(SaveCheck::checkingText());
        error_->setToolTip({});
        mergeBtn_->setEnabled(false);
        saveAsReasonShown_ = true;
        return;
    }
    // Until the Save as check answers, a reason about Save as stays up, so typing
    // in it does not make the line flicker. Any other reason no longer applies, and
    // Merge waits for the check when pressed.
    if (!saveAsReason && saveAsReasonShown_)
        return;
    const QString why = planReason.isEmpty() ? saveAsReason.value_or(QString()) : planReason;
    saveAsReasonShown_ = planReason.isEmpty() && !why.isEmpty();
    // A disabled Merge always has its reason here. A failed write shows here too
    // but leaves Merge enabled, so closing the file in the program that holds it
    // and pressing Merge again is enough.
    const bool showWrite = why.isEmpty() && !writeError_.isEmpty();
    const QString text = showWrite ? writeError_ : why;
    if (error_->text() != text)
        error_->setText(text);
    error_->setToolTip(showWrite ? writeErrorDetail_ : QString());
    mergeBtn_->setEnabled(!plan_.isEmpty() && plan_.isValid() && why.isEmpty());
}

QString MergeDialog::fitPendingReason(const QString &reason) const
{
    for (int i = 0; i < plan_.count(); ++i) {
        if (plan_.at(i).load != MergePlan::Load::Reading)
            continue;
        const QString name = plan_.displayName(i);
        const QString isolated = QChar(0x2068) + name + QChar(0x2069);
        // Keep the translated row and reason intact, shortening only the named
        // file when an unbroken part cannot fit at the current label width.
        return fitFileSentence([&reason, &isolated](const QString &shown, const QString &) {
            return QString(reason).replace(isolated, shown);
        }, name, {}, error_->fontMetrics(), error_->contentsRect().width());
    }
    return reason;
}

QStringList MergeDialog::files() const
{
    QStringList files;
    for (const MergePlan::Entry &e : plan_.entries())
        files << e.path + QLatin1Char('\n') + e.spec;
    return files;
}

void MergeDialog::checkOutput()
{
    checking_ = outputEdit_->text();
    checkedFiles_ = files();
    const bool stepDefault = !outputEdited_;
    // The inputs matter only to a write, and one on a share that stopped answering
    // would hold up checks of Save as.
    const bool forWrite = pressed_.has_value();
    saveCheck_->run(
        [plan = plan_, text = checking_, stepDefault, forWrite] {
            Checked checked;
            checked.text = stepDefault ? QDir::toNativeSeparators(plan.defaultOutputPath()) : text;
            const QString out = withPdf(checked.text);
            checked.canonical = QFileInfo(out).canonicalFilePath();
            checked.key = normalizePathKey(canonicalOrAbsolute(out));
            if (forWrite)
                for (const MergePlan::Entry &e : plan.entries())
                    checked.inputs << QFileInfo(e.path).canonicalFilePath();
            return checked;
        },
        [this](const Checked &checked) {
            if (!outputEdited_ && checked.text != outputEdit_->text()) {
                // The default, past a name in use. Text selected for typing over
                // stays selected.
                const bool selected = outputEdit_->selectedText() == outputEdit_->text();
                outputEdit_->setText(checked.text);
                if (selected)
                    outputEdit_->selectAll();
                checking_ = checked.text;
            }
            checked_ = checked;
            showProblem();
            // Merge writes what it was pressed for. The default Save as can have
            // stepped past a name in use meanwhile, which the user did not choose.
            if (const auto pressed = std::exchange(pressed_, std::nullopt);
                pressed && pressed->second == files()
                && (pressed->first == outputEdit_->text() || !outputEdited_))
                write();
        });
}

void MergeDialog::setOpenFiles(const QStringList &paths)
{
    // The tabs hold canonical paths.
    openKeys_.clear();
    for (const QString &p : paths)
        openKeys_ << normalizePathKey(p);
    refreshFooter();
}

void MergeDialog::accept()
{
    if (!plan_.isValid()) {
        warnInWindow(this, plan_.errorText());
        return;
    }

    if (outputEdit_->text().trimmed().isEmpty()) {
        const QPointer<MergeDialog> self(this);
        warnInWindow(this, tr("Choose where to save the merged PDF."));
        if (self)
            outputEdit_->setFocus();
        return;
    }
    pressed_ = std::pair(outputEdit_->text(), files());
    checkOutput();
}

void MergeDialog::write()
{
    // Pressed before the check answered: the answer can be a reason not to write,
    // such as a file open in a tab.
    if (const std::optional<QString> why = problem(); !why || !why->isEmpty())
        return;
    const QString out = withPdf(outputEdit_->text());

    // The merge reads every source while it writes the output, so naming an input
    // as the output destroys the file it is copying from. canonicalFilePath()
    // resolves symlinks and case, and is empty for a file that does not exist yet
    // - which is the normal case and correctly matches nothing.
    const QString &outCanonical = checked_->canonical;
    if (!outCanonical.isEmpty()) {
        if (checked_->inputs.contains(outCanonical)) {
            const QPointer<MergeDialog> self(this);
            warnInWindow(this, tr("The merged file cannot replace one of the files being "
                                 "merged. Choose a different name."));
            if (self) {
                outputEdit_->setFocus();
                outputEdit_->selectAll();
            }
            return;
        }
        const QStringList originalFiles = files();
        const QString originalText = outputEdit_->text();
        const QPointer<MergeDialog> self(this);
        const QPointer<QMessageBox> box(new QMessageBox(QMessageBox::Question, tr("Merge PDFs"),
            //: %1 is a file path.
            tr("\"%1\" already exists. Replace it?").arg(QDir::toNativeSeparators(out)),
            QMessageBox::Yes | QMessageBox::No, this));
        box->setDefaultButton(QMessageBox::No);
        box->setWindowModality(Qt::WindowModal);
        const int answer = box->exec();
        delete box.data();
        if (!self || answer != QMessageBox::Yes || files() != originalFiles
            || outputEdit_->text() != originalText)
            return;
    }

    outputPath_ = out;
    QDialog::accept();
}

void MergeDialog::done(int result)
{
    closed_ = true;
    for (const auto &[ticket, state] : probeStates_)
        state->stop();
    for (const auto &[volume, queue] : probes_)
        queue->clear();
    QDialog::done(result);
}

} // namespace mervin
