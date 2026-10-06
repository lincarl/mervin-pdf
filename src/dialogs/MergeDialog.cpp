#include "dialogs/MergeDialog.h"

#include "dialogs/RowList.h"
#include "print/PageRange.h"
#include "recent/PathKey.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

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
#include <QPushButton>
#include <QResizeEvent>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>

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
} // namespace

MergeDialog::MergeDialog(const QString &initialPath, int initialPageCount,
                         const QString &initialPassword, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Merge PDFs"));
    resize(760, 520);
    setMinimumSize(660, 440);

    auto *layout = new QVBoxLayout(this);

    auto *hint = new QLabel(tr("Files are merged top to bottom, in the order shown."), this);
    hint->setObjectName(QStringLiteral("mergeHint"));
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

    if (!initialPath.isEmpty()) {
        // Probe the current document with qpdf like any input: MuPDF accepts files qpdf may
        // reject. Use the tab's password; probing never prompts.
        MergePlan::Entry e = probeEntry(initialPath, initialPassword);
        if (e.load == MergePlan::Load::Ok && e.pageCount <= 0 && initialPageCount > 0)
            e.pageCount = initialPageCount; // qpdf read it but counted nothing
        plan_.append(e);
    }
    rebuild(plan_.isEmpty() ? -1 : 0);
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

void MergeDialog::addFiles()
{
    //: File type filter. Keep "(*.pdf)" as it is.
    const QString filter = tr("PDF documents (*.pdf)");
    const QStringList picked = QFileDialog::getOpenFileNames(this, tr("Add PDFs to Merge"),
                                                             startDirectory(), filter);
    addPaths(picked);
}

void MergeDialog::addPaths(const QStringList &paths)
{
    if (paths.isEmpty())
        return;

    const int firstNew = plan_.count();
    QStringList problems;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (const QString &p : paths) {
        const MergePlan::Entry e = probeEntry(p);
        if (e.load != MergePlan::Load::Ok) {
            //: One line of the "cannot be merged" list: %1 is a file name, %2 why.
            problems << tr("%1 - %2").arg(QFileInfo(p).fileName(), e.loadError);
        }
        plan_.append(e);
    }
    QApplication::restoreOverrideCursor();
    rebuild(firstNew);

    // One report for the batch, not one box per bad file. The rows are added
    // either way, marked and blocking, so the user can see and remove them.
    if (!problems.isEmpty())
        QMessageBox::warning(this, tr("Merge PDFs"),
                             //: %1 is a list of files, one per line. The number of
                             //: files picks the singular or plural form.
                             tr("These files cannot be merged:\n\n%1", nullptr,
                                int(problems.size()))
                                 .arg(problems.join(QStringLiteral("\n"))));
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

void MergeDialog::browseForOutput()
{
    const QString suggested =
        outputEdit_->text().isEmpty() ? plan_.defaultOutputPath() : outputEdit_->text();
    const QString out = QFileDialog::getSaveFileName(this, tr("Save Merged PDF"), suggested,
                                                     tr("PDF documents (*.pdf)"));
    if (out.isEmpty())
        return;
    outputEdit_->setText(QDir::toNativeSeparators(out));
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
    const QColor ink = Theme::iconInk(palette());
    nameLabels_.clear();
    nameTexts_.clear();
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
            if (e.load != MergePlan::Load::Ok) {
                auto *badge = new QLabel(row);
                badge->setFixedSize(18, 18);
                badge->setPixmap(icons::glyphPixmap(e.load == MergePlan::Load::Locked
                                                        ? icons::Glyph::Security
                                                        : icons::Glyph::Close,
                                                    ink, 16));
                badge->setToolTip(name->toolTip());
                h->addWidget(badge, 0);
            }
            h->addWidget(name, 1);

            auto *spec = new QLineEdit(e.spec, row);
            spec->setObjectName(QStringLiteral("mergeRowSpec"));
            spec->setFixedWidth(specWidth_);
            // A format prompt, not "All": a greyed placeholder identical to the
            // default value made an emptied cell look like the default was in
            // force while it was actually blocking the merge.
            spec->setPlaceholderText(specPlaceholder());
            spec->setEnabled(e.load == MergePlan::Load::Ok);
            // The list is rebuilt wholesale on every mutation, so this build-time
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
        l->setText(l->fontMetrics().elidedText(nameTexts_.at(i), Qt::ElideMiddle, w));
    }
}

void MergeDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    reelideNames();
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
    // rather than somewhere on this machine.
    if (!outputEdited_)
        outputEdit_->setText(QDir::toNativeSeparators(plan_.defaultOutputPath()));

    // Set after the auto-refill so it sees the field's final text. A missing
    // destination has to reach this line: it disables Merge, and without a
    // reason here the button would grey out with nothing on screen saying why.
    QString problem = plan_.errorText();
    if (problem.isEmpty() && !plan_.isEmpty() && outputEdit_->text().trimmed().isEmpty())
        problem = tr("Choose where to save the merged PDF.");
    // The viewer keeps an open tab's file open, so the write would fail only once
    // Merge was pressed.
    if (problem.isEmpty() && !plan_.isEmpty()
        && openKeys_.contains(normalizePathKey(canonicalOrAbsolute(withPdf(outputEdit_->text())))))
        problem = tr("That file is open in a tab. Choose another name.");
    // A disabled Merge always has its reason here. A failed write shows here too
    // but leaves Merge enabled, so closing the file in the program that holds it
    // and pressing Merge again is enough.
    const bool showWrite = problem.isEmpty() && !writeError_.isEmpty();
    error_->setText(showWrite ? writeError_ : problem);
    error_->setToolTip(showWrite ? writeErrorDetail_ : QString());

    const bool has = !plan_.isEmpty();
    const int cur = currentRow();
    removeBtn_->setEnabled(cur >= 0);
    duplicateBtn_->setEnabled(cur >= 0);
    upBtn_->setEnabled(cur > 0);
    downBtn_->setEnabled(cur >= 0 && cur < plan_.count() - 1);
    mergeBtn_->setEnabled(has && plan_.isValid() && problem.isEmpty());
}

void MergeDialog::setOpenFiles(const QStringList &paths)
{
    openKeys_.clear();
    for (const QString &p : paths)
        openKeys_ << normalizePathKey(canonicalOrAbsolute(p));
    refreshFooter();
}

void MergeDialog::accept()
{
    if (!plan_.isValid()) {
        QMessageBox::warning(this, tr("Merge PDFs"), plan_.errorText());
        return;
    }

    if (outputEdit_->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("Merge PDFs"), tr("Choose where to save the merged PDF."));
        outputEdit_->setFocus();
        return;
    }
    const QString out = withPdf(outputEdit_->text());

    // The merge reads every source while it writes the output, so naming an input
    // as the output destroys the file it is copying from. canonicalFilePath()
    // resolves symlinks and case, and is empty for a file that does not exist yet
    // - which is the normal case and correctly matches nothing.
    const QString outCanonical = QFileInfo(out).canonicalFilePath();
    if (!outCanonical.isEmpty()) {
        for (const MergePlan::Entry &e : plan_.entries()) {
            if (QFileInfo(e.path).canonicalFilePath() == outCanonical) {
                QMessageBox::warning(this, tr("Merge PDFs"),
                                     tr("The merged file cannot replace one of the files being "
                                        "merged. Choose a different name."));
                outputEdit_->setFocus();
                outputEdit_->selectAll();
                return;
            }
        }
        if (QMessageBox::question(this, tr("Merge PDFs"),
                                  //: %1 is a file path.
                                  tr("\"%1\" already exists. Replace it?")
                                      .arg(QDir::toNativeSeparators(out)),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
            return;
    }

    // Written here rather than by the caller after the dialog closes: a file
    // another program holds open, an input that went bad, or a full disk then
    // costs a message on the error line instead of the whole plan.
    const QList<PageOps::MergeInput> inputs = plan_.inputs();
    QString err;
    int failed = -1;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const PageOps::Status st = PageOps::merge(inputs, out, &err, &failed);
    QApplication::restoreOverrideCursor();
    if (st != PageOps::Status::Ok) {
        // Name the input that stopped it when the backend can tell; otherwise
        // it was the write.
        writeError_ = failed >= 0 && failed < inputs.size()
                          //: %1 is the name of the file being merged when it failed.
                          ? tr("The merge failed on \"%1\".")
                                .arg(QFileInfo(inputs.at(failed).path).fileName())
                          //: %1 is the merged file's name.
                          : tr("Could not write \"%1\". If another program has it open, close "
                               "it and press Merge again, or choose another name.")
                                .arg(QFileInfo(out).fileName());
        writeErrorDetail_ = QDir::toNativeSeparators(err);
        refreshFooter();
        return;
    }
    outputPath_ = out;
    QDialog::accept();
}

} // namespace mervin
