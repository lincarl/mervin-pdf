#include "dialogs/MergeDialog.h"

#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QShortcut>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

namespace mervin {

namespace {

// Column geometry, shared by the header strip and every row so they line up.
// Sized with headroom for a UI font wider than Segoe UI 9pt: at 125% Windows
// text scaling the captions and the longest cell values ("Unreadable", "12 of
// 999") still fit, so nothing clips silently - QLabel hard-clips, it does not
// elide.
constexpr int kColHandle = 18;
constexpr int kColNum = 30;
constexpr int kColSpec = 124;
constexpr int kColCount = 92;
constexpr int kColOutput = 74;
constexpr int kColX = 22;
constexpr int kRowHeight = 30;
constexpr int kRowSpacing = 8;

QLabel *fixedLabel(QWidget *parent, int width, Qt::Alignment align)
{
    auto *l = new QLabel(parent);
    l->setFixedWidth(width);
    l->setAlignment(align | Qt::AlignVCenter);
    return l;
}

// Rows carry their index in this, so a drop knows which row was picked up.
const char *const kRowMime = "application/x-mervin-merge-row";

// The row list. QListWidget's own InternalMove cannot be used here: it moves the
// QListWidgetItem while the visible row is a separate item widget, so the two
// come apart. Instead the drop is turned into a move on the MergePlan and the
// whole list is rebuilt from it - the same path every other mutation takes.
// Drops are accepted anywhere in the viewport, which also lets a row be dragged
// past the last one into the empty space below.
class RowList : public QListWidget
{
public:
    explicit RowList(QWidget *parent) : QListWidget(parent)
    {
        setAcceptDrops(true);
        // The rows are item widgets covering the viewport, so a drag over the
        // list is over a row, not the viewport; Qt walks up to the first ancestor
        // that accepts drops, which has to be the viewport. Setting it on the view
        // alone does not reach it.
        viewport()->setAcceptDrops(true);
        // ...and the drag events are handled here rather than through the
        // dragMoveEvent/dropEvent overrides, because QAbstractScrollArea's
        // viewport forwarding does not deliver them to the view (verified: the
        // overrides never ran). Filtering the viewport directly is unambiguous.
        viewport()->installEventFilter(this);
        setDropIndicatorShown(false); // we paint our own; see paintEvent
    }

    // Called with (sourceRow, insertionGap) when a row is dropped. Turning the
    // gap into a destination index is MergePlan's job - that off-by-one is the
    // part with teeth, and it is unit-tested there.
    std::function<void(int, int)> onRowDropped;

protected:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (o == viewport()) {
            switch (e->type()) {
            case QEvent::DragEnter:
            case QEvent::DragMove:
                takeIfOurs(static_cast<QDragMoveEvent *>(e));
                return true;
            case QEvent::DragLeave:
                dropAt_ = -1;
                viewport()->update();
                return true;
            case QEvent::Drop:
                handleDrop(static_cast<QDropEvent *>(e));
                return true;
            default:
                break;
            }
        }
        return QListWidget::eventFilter(o, e);
    }

    void handleDrop(QDropEvent *e)
    {
        const int gap = dropAt_;
        dropAt_ = -1;
        viewport()->update();
        if (!e->mimeData()->hasFormat(QLatin1String(kRowMime)) || gap < 0) {
            e->ignore();
            return;
        }
        const int from = e->mimeData()->data(QLatin1String(kRowMime)).toInt();
        e->setDropAction(Qt::MoveAction);
        e->accept();
        if (onRowDropped)
            onRowDropped(from, gap);
    }

    void paintEvent(QPaintEvent *e) override
    {
        QListWidget::paintEvent(e);
        if (dropAt_ < 0)
            return;
        QPainter p(viewport());
        QPen pen(palette().color(QPalette::Accent));
        pen.setWidth(2);
        p.setPen(pen);
        p.drawLine(0, gapY(dropAt_), viewport()->width(), gapY(dropAt_));
    }

private:
    void takeIfOurs(QDragMoveEvent *e)
    {
        if (!e->mimeData()->hasFormat(QLatin1String(kRowMime))) {
            e->ignore();
            return;
        }
        const int gap = gapAt(e->position().toPoint());
        if (gap != dropAt_) {
            dropAt_ = gap;
            viewport()->update();
        }
        e->setDropAction(Qt::MoveAction);
        e->accept();
    }

    // The insertion point nearest `pos`: 0 above the first row, count() below the
    // last. Anywhere past the final row counts as the end, so the empty space
    // under a short list is a valid target.
    int gapAt(const QPoint &pos) const
    {
        for (int i = 0; i < count(); ++i) {
            const QRect r = visualItemRect(item(i));
            if (pos.y() < r.center().y())
                return i;
            if (pos.y() <= r.bottom())
                return i + 1;
        }
        return count();
    }

    int gapY(int gap) const
    {
        if (count() == 0)
            return 0;
        if (gap >= count())
            return qMin(visualItemRect(item(count() - 1)).bottom(), viewport()->height() - 1);
        return qMax(visualItemRect(item(gap)).top(), 1);
    }

    int dropAt_ = -1; // insertion point under the cursor, -1 when not dragging
};

} // namespace

MergeDialog::MergeDialog(const QString &initialPath, int initialPageCount, QWidget *parent)
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

    // Column captions. A QListWidget of row widgets rather than a QTableWidget:
    // nothing in the app uses an item *view* with a header, so QHeaderView is
    // entirely unstyled by Theme::buildStyleSheet and a native header strip would
    // sit on the slate dialog surface looking imported. The row-widget idiom is
    // the one MeasurePanel already ships.
    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("mergeListHeader"));
    auto *hh = new QHBoxLayout(header);
    headerRow_ = hh;
    hh->setContentsMargins(6, 0, 6, 0); // real insets set by syncHeaderInsets()
    hh->setSpacing(kRowSpacing);
    hh->addSpacing(kColHandle); // over the drag grips
    auto *hNum = fixedLabel(header, kColNum, Qt::AlignRight);
    hNum->setText(QStringLiteral("#"));
    hh->addWidget(hNum);
    auto *hFile = new QLabel(tr("File"), header);
    hh->addWidget(hFile, 1);
    auto *hSpec = fixedLabel(header, kColSpec, Qt::AlignLeft);
    hSpec->setText(tr("Pages, in order"));
    hh->addWidget(hSpec);
    auto *hCount = fixedLabel(header, kColCount, Qt::AlignRight);
    hCount->setText(tr("Count"));
    hh->addWidget(hCount);
    auto *hOut = fixedLabel(header, kColOutput, Qt::AlignRight);
    hOut->setText(tr("Output"));
    hh->addWidget(hOut);
    hh->addSpacing(kColX + kRowSpacing);
    listSide->addWidget(header);

    auto *rows = new RowList(this);
    rows->onRowDropped = [this](int from, int gap) {
        // Move the plan now, rebuild the widgets after the drag machinery has
        // unwound: the drop arrives inside QDrag::exec()'s nested event loop, and
        // rebuilding here would delete the very grip widget whose event filter is
        // still on the stack.
        const int landed = plan_.moveToGap(from, gap);
        if (landed < 0)
            return; // dropped back where it already was
        QTimer::singleShot(0, this, [this, landed] { rebuild(landed); });
    };
    list_ = rows;
    list_->setObjectName(QStringLiteral("mergeList"));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setUniformItemSizes(true);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(list_, &QListWidget::currentRowChanged, this, [this] { refreshFooter(); });
    // The captions are a sibling of the list, so they have to track the list's
    // frame, padding and - the case that actually bites - the vertical scrollbar
    // appearing once the plan gets long.
    list_->viewport()->installEventFilter(this);
    listSide->addWidget(list_, 1);

    auto *specHint = new QLabel(tr("Type a page range in the Pages column, for example "
                                   "1-3, 5, 8-10 - or All. Pages are taken in the order "
                                   "you type them."),
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

    duplicateBtn_ = new QPushButton(icons::glyph(icons::Glyph::Copy, ink), tr("Duplicate"), this);
    connect(duplicateBtn_, &QPushButton::clicked, this, &MergeDialog::duplicateCurrent);
    side->addWidget(duplicateBtn_);

    removeBtn_ = new QPushButton(icons::glyph(icons::Glyph::Delete, ink), tr("Remove"), this);
    connect(removeBtn_, &QPushButton::clicked, this, &MergeDialog::removeCurrent);
    side->addWidget(removeBtn_);

    side->addStretch(1);
    middle->addLayout(side, 0);
    layout->addLayout(middle, 1);

    // Reorder from the keyboard, scoped to the list so the shortcuts do not fight
    // the page-range editors for arrow keys. Ctrl+Delete (not plain Delete) frees
    // Delete for the QLineEdit the user is typing in.
    auto *up = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Up), list_);
    up->setContext(Qt::WidgetWithChildrenShortcut);
    connect(up, &QShortcut::activated, this, [this] { moveCurrent(-1); });
    auto *down = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Down), list_);
    down->setContext(Qt::WidgetWithChildrenShortcut);
    connect(down, &QShortcut::activated, this, [this] { moveCurrent(1); });
    auto *dup = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_D), list_);
    dup->setContext(Qt::WidgetWithChildrenShortcut);
    connect(dup, &QShortcut::activated, this, &MergeDialog::duplicateCurrent);
    auto *del = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Delete), list_);
    del->setContext(Qt::WidgetWithChildrenShortcut);
    connect(del, &QShortcut::activated, this, &MergeDialog::removeCurrent);

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
        refreshFooter();
    });
    outRow->addWidget(outputEdit_, 1);
    auto *browse = new QPushButton(tr("Browse…"), this);
    connect(browse, &QPushButton::clicked, this, &MergeDialog::browseForOutput);
    outRow->addWidget(browse, 0);
    layout->addLayout(outRow);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    mergeBtn_ = buttons->addButton(tr("Merge"), QDialogButtonBox::AcceptRole);
    mergeBtn_->setObjectName(QStringLiteral("mergeAccept"));
    mergeBtn_->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &MergeDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &MergeDialog::reject);
    layout->addWidget(buttons);

    if (!initialPath.isEmpty()) {
        // The open document goes in as an ordinary row - and is probed like any
        // other. It has to be: the viewer opened it with MuPDF, and MuPDF opens
        // things this merge cannot. A document the user unlocked with a password
        // at open time reads perfectly in the tab, but the password is not kept
        // anywhere, so qpdf would meet it locked. Trusting the viewer's page count
        // here would show a healthy row and dead-end after Merge, which is the
        // exact failure this dialog exists to prevent. probe() is qpdf-only and
        // never prompts, so this costs one parse and asks the user nothing.
        MergePlan::Entry e = probeEntry(initialPath);
        if (e.load == MergePlan::Load::Ok && e.pageCount <= 0 && initialPageCount > 0)
            e.pageCount = initialPageCount; // qpdf read it but counted nothing
        plan_.append(e);
    }
    rebuild(plan_.isEmpty() ? -1 : 0);
}

MergePlan::Entry MergeDialog::probeEntry(const QString &path)
{
    MergePlan::Entry e;
    e.path = path;
    int n = 0;
    QString err;
    switch (PageOps::probe(path, &n, QString(), &err)) {
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
    const QStringList picked = QFileDialog::getOpenFileNames(
        this, tr("Add PDFs to Merge"), startDirectory(), tr("PDF documents (*.pdf)"));
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
        if (e.load != MergePlan::Load::Ok)
            problems << tr("%1 - %2").arg(QFileInfo(p).fileName(), e.loadError);
        plan_.append(e);
    }
    QApplication::restoreOverrideCursor();
    rebuild(firstNew);

    // One report for the batch, not one box per bad file. The rows are added
    // either way, marked and blocking, so the user can see and remove them.
    if (!problems.isEmpty())
        QMessageBox::warning(this, tr("Merge PDFs"),
                             tr("These files cannot be merged:\n\n%1").arg(problems.join(
                                 QStringLiteral("\n"))));
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
    refreshFooter();
}

int MergeDialog::currentRow() const
{
    return list_ ? list_->currentRow() : -1;
}

void MergeDialog::rebuild(int selectRow)
{
    const QColor ink = Theme::iconInk(palette());
    nameLabels_.clear();
    nameTexts_.clear();
    list_->clear();

    for (int i = 0; i < plan_.count(); ++i) {
        const MergePlan::Entry &e = plan_.at(i);
        auto *row = new QWidget(list_);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(6, 1, 6, 1);
        h->setSpacing(kRowSpacing);

        // The grip. Rows are item widgets, so a press anywhere else in the row
        // lands on a child widget and the list never sees it - the drag has to
        // start from something that exists for exactly that purpose.
        auto *grip = new QLabel(row);
        grip->setObjectName(QStringLiteral("mergeRowGrip"));
        grip->setFixedWidth(kColHandle);
        grip->setAlignment(Qt::AlignCenter);
        grip->setPixmap(icons::glyphPixmap(icons::Glyph::DragHandle, ink, 16));
        grip->setCursor(Qt::OpenHandCursor);
        grip->setToolTip(tr("Drag to reorder"));
        grip->setProperty("mergeRow", i);
        grip->installEventFilter(this);
        h->addWidget(grip);

        auto *num = fixedLabel(row, kColNum, Qt::AlignRight);
        num->setObjectName(QStringLiteral("mergeRowNum"));
        num->setText(QString::number(i + 1));
        h->addWidget(num);

        auto *name = new QLabel(row);
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        nameLabels_.append(name);
        nameTexts_.append(plan_.displayName(i));
        name->setToolTip(e.loadError.isEmpty()
                             ? QDir::toNativeSeparators(e.path)
                             : QStringLiteral("%1\n\n%2").arg(QDir::toNativeSeparators(e.path),
                                                              e.loadError));
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
        spec->setFixedWidth(kColSpec);
        // A format prompt, not "All": a greyed placeholder identical to the
        // default value made an emptied cell look like the default was in force
        // while it was actually blocking the merge.
        spec->setPlaceholderText(tr("e.g. 1-3, 5"));
        spec->setEnabled(e.load == MergePlan::Load::Ok);
        // Typing in a row makes it the current row, so Remove / Duplicate /
        // Move Up / Move Down act on the row the user is looking at. The editor
        // is the row's only focusable child, so this covers click and Tab alike.
        spec->installEventFilter(this);
        spec->setProperty("mergeRow", i);
        // The list is rebuilt wholesale on every mutation, so this build-time
        // index stays valid for as long as the widget it is captured in exists.
        connect(spec, &QLineEdit::textChanged, this, [this, i](const QString &t) {
            plan_.setSpec(i, t);
            // Only the derived numbers change, so refresh them in place: a full
            // rebuild here would destroy the QLineEdit being typed into.
            refreshFooter();
        });
        h->addWidget(spec);

        auto *count = fixedLabel(row, kColCount, Qt::AlignRight);
        count->setObjectName(QStringLiteral("mergeRowCount"));
        h->addWidget(count);

        auto *outp = fixedLabel(row, kColOutput, Qt::AlignRight);
        outp->setObjectName(QStringLiteral("mergeRowOutput"));
        h->addWidget(outp);

        auto *x = new QToolButton(row);
        x->setObjectName(QStringLiteral("mergeRowX"));
        x->setToolButtonStyle(Qt::ToolButtonTextOnly);
        x->setText(QStringLiteral("✕"));
        x->setAutoRaise(true);
        x->setFixedSize(kColX, kColX);
        x->setFocusPolicy(Qt::NoFocus); // 20 rows must not add 20 extra tab stops
        x->setToolTip(tr("Remove this row"));
        // Select, then remove on the next event-loop turn. Rebuilding here would
        // delete this very button from inside its own clicked() emission; going
        // through the current row also keeps the right row if anything else moved
        // the list in between.
        connect(x, &QToolButton::clicked, this, [this, i] {
            list_->setCurrentRow(i);
            QTimer::singleShot(0, this, [this] { removeCurrent(); });
        });
        h->addWidget(x);

        auto *item = new QListWidgetItem(list_);
        item->setSizeHint(QSize(0, kRowHeight));
        list_->addItem(item);
        list_->setItemWidget(item, row);
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

void MergeDialog::syncHeaderInsets()
{
    if (!headerRow_ || !list_)
        return;
    // Left: the list frame plus its QSS padding, i.e. wherever the viewport
    // actually starts. Right: the same, plus the vertical scrollbar when it is
    // showing - which is what used to shove the Count and Output captions 12px
    // off their columns as soon as the plan got long enough to scroll.
    const int left = list_->viewport()->x();
    const int right = list_->width() - (list_->viewport()->x() + list_->viewport()->width());
    headerRow_->setContentsMargins(6 + left, 0, 6 + qMax(0, right), 0);
}

bool MergeDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (list_ && watched == list_->viewport() && event->type() == QEvent::Resize)
        syncHeaderInsets();

    if (event->type() == QEvent::FocusIn) {
        const QVariant row = watched->property("mergeRow");
        if (row.isValid())
            list_->setCurrentRow(row.toInt());
    }

    // Drag a row by its grip. Arming on press and only starting once the pointer
    // has travelled the platform's drag distance keeps a plain click on the grip
    // from turning into a drag - it just selects the row.
    auto *grip = qobject_cast<QLabel *>(watched);
    if (grip && grip->objectName() == QLatin1String("mergeRowGrip")) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                dragRow_ = grip->property("mergeRow").toInt();
                dragOrigin_ = me->globalPosition().toPoint();
                list_->setCurrentRow(dragRow_);
            }
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease) {
            dragRow_ = -1;
            return true;
        }
        if (event->type() == QEvent::MouseMove && dragRow_ >= 0) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (!(me->buttons() & Qt::LeftButton)) {
                dragRow_ = -1;
                return true;
            }
            if ((me->globalPosition().toPoint() - dragOrigin_).manhattanLength()
                < QApplication::startDragDistance())
                return true;
            startRowDrag(dragRow_);
            dragRow_ = -1;
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void MergeDialog::startRowDrag(int row)
{
    QListWidgetItem *item = list_->item(row);
    QWidget *rowWidget = item ? list_->itemWidget(item) : nullptr;
    if (!rowWidget)
        return;

    auto *mime = new QMimeData;
    mime->setData(QLatin1String(kRowMime), QByteArray::number(row));

    QDrag drag(this);
    drag.setMimeData(mime);
    // The row itself, dimmed, rides with the cursor - so what is being moved is
    // never in doubt in a list where several rows can be the same file.
    QPixmap shot = rowWidget->grab();
    QPixmap ghost(shot.size());
    ghost.fill(Qt::transparent);
    {
        QPainter p(&ghost);
        p.setOpacity(0.75);
        p.drawPixmap(0, 0, shot);
    }
    drag.setPixmap(ghost);
    drag.setHotSpot(QPoint(kColHandle / 2, ghost.height() / 2));
    drag.exec(Qt::MoveAction);
}

void MergeDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    syncHeaderInsets();
    reelideNames();
}

void MergeDialog::refreshFooter()
{
    // The per-row Count and Output cells are derived from the whole plan (Output
    // is a running sum), so they are refreshed together in one pass, not per row.
    const QList<MergePlan::RowText> texts = plan_.rowTexts();
    for (int i = 0; i < list_->count() && i < texts.size(); ++i) {
        QWidget *row = list_->itemWidget(list_->item(i));
        if (!row)
            continue;
        if (auto *c = row->findChild<QLabel *>(QStringLiteral("mergeRowCount")))
            c->setText(texts.at(i).count);
        if (auto *o = row->findChild<QLabel *>(QStringLiteral("mergeRowOutput")))
            o->setText(texts.at(i).output);
    }

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
    error_->setText(problem);

    const bool has = !plan_.isEmpty();
    const int cur = currentRow();
    removeBtn_->setEnabled(cur >= 0);
    duplicateBtn_->setEnabled(cur >= 0);
    upBtn_->setEnabled(cur > 0);
    downBtn_->setEnabled(cur >= 0 && cur < plan_.count() - 1);
    mergeBtn_->setEnabled(has && plan_.isValid() && !outputEdit_->text().trimmed().isEmpty());
}

void MergeDialog::accept()
{
    if (!plan_.isValid()) {
        QMessageBox::warning(this, tr("Merge PDFs"), plan_.errorText());
        return;
    }

    QString out = outputEdit_->text().trimmed();
    if (out.isEmpty()) {
        QMessageBox::warning(this, tr("Merge PDFs"), tr("Choose where to save the merged PDF."));
        outputEdit_->setFocus();
        return;
    }
    if (!out.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive))
        out += QStringLiteral(".pdf");

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
                                  tr("\"%1\" already exists. Replace it?")
                                      .arg(QDir::toNativeSeparators(out)),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
            return;
    }

    outputPath_ = out;
    QDialog::accept();
}

} // namespace mervin
