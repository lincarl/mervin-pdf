#include "dialogs/ExtractDialog.h"

#include "dialogs/ExtractStrip.h"
#include "dialogs/RowList.h"
#include "security/PageOps.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace mervin {

namespace {
constexpr int kWidth = 820;
constexpr int kHeight = 560;
constexpr int kMinWidth = 660;
constexpr int kMinHeight = 480;
constexpr int kColCount = 56;  // "999" and the caption, at 125% text scaling
constexpr int kColOutput = 74; // "999-999", as in Merge
constexpr int kPasswordWidth = 260;
constexpr int kBrowseMinWidth = 84;

QHBoxLayout *row()
{
    auto *h = new QHBoxLayout;
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);
    return h;
}
} // namespace

ExtractDialog::ExtractDialog(const Source &source, QWidget *parent)
    : QDialog(parent)
    , source_(source)
    , plan_(source.path, source.viewerPageCount)
{
    setWindowTitle(tr("Extract Pages"));

    // qpdf is what will write, so qpdf is asked: whatever it can open, the write
    // can read. It never prompts. An encrypted file is tried with the tab's
    // password, and gets the inline password row only when that does not open it.
    int count = 0;
    bool needsPassword = false;
    PageOps::Status st = PageOps::probe(source.path, &count, QString(), &readErrorDetail_);
    if (st == PageOps::Status::NeedsPassword && !source.password.isEmpty()) {
        st = PageOps::probe(source.path, &count, source.password, &readErrorDetail_);
        if (st == PageOps::Status::Ok)
            password_ = source.password;
    }
    switch (st) {
    case PageOps::Status::Ok:
        if (count > 0)
            plan_.setPageCount(count);
        break;
    case PageOps::Status::NeedsPassword:
        needsPassword = true;
        break;
    case PageOps::Status::Failed:
        readError_ = true;
        break;
    }

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(0); // the gaps between rows are set one by one below

    // ── Rows + button column (Merge's arrangement, without its hint lines) ───
    auto *middle = row();
    auto *listSide = new QVBoxLayout;
    listSide->setContentsMargins(0, 0, 0, 0);
    listSide->setSpacing(4);
    list_ = new RowList(kColCount, kColOutput, this);
    list_->setObjectName(QStringLiteral("extractList"));
    listSide->addWidget(list_->makeHeader([this](QWidget *header, QHBoxLayout *columns) {
        columns->addWidget(new QLabel(tr("Pages"), header));
        of_ = new QLabel(header);
        of_->setObjectName(QStringLiteral("extractOf"));
        columns->addWidget(of_, 1);
    }));
    list_->onRowDropped = [this](int from, int gap) {
        // The drop arrives inside QDrag::exec(), whose grip is still on the stack:
        // move the plan now, rebuild the widgets on the next turn.
        const int landed = plan_.moveToGap(from, gap);
        if (landed >= 0)
            QTimer::singleShot(0, this, [this, landed] { rebuild(landed, Focus::Keep); });
    };
    list_->onMove = [this](int delta) { moveCurrent(delta); };
    list_->onRemove = [this] { removeCurrent(); };
    connect(list_, &QListWidget::currentRowChanged, this, [this] { syncCurrentRow(true); });
    listSide->addWidget(list_, 1);
    middle->addLayout(listSide, 1);

    // The side buttons take no focus from a click, so the caret stays in the row
    // being edited and Enter still extracts.
    auto *side = new QVBoxLayout;
    side->setContentsMargins(0, 0, 0, 0);
    side->setSpacing(6);
    const auto sideButton = [this, side](const QString &text, const QIcon &icon = {}) {
        auto *b = new QPushButton(icon, text, this);
        b->setFocusPolicy(Qt::TabFocus);
        side->addWidget(b);
        return b;
    };
    connect(sideButton(tr("&Add Range")), &QPushButton::clicked, this, &ExtractDialog::addRange);
    upBtn_ = sideButton(tr("Move &Up"));
    connect(upBtn_, &QPushButton::clicked, this, [this] { moveCurrent(-1); });
    downBtn_ = sideButton(tr("Move &Down"));
    connect(downBtn_, &QPushButton::clicked, this, [this] { moveCurrent(1); });
    side->addSpacing(6);
    removeBtn_ = sideButton(tr("&Remove"), icons::glyph(icons::Glyph::Delete, Theme::iconInk(palette())));
    connect(removeBtn_, &QPushButton::clicked, this, &ExtractDialog::removeCurrent);
    side->addStretch(1);
    middle->addLayout(side, 0);
    layout->addLayout(middle, 1); // extra height goes to the rows
    layout->addSpacing(8);

    // ── The result strip ─────────────────────────────────────────────────────
    strip_ = new ExtractStrip(this);
    strip_->setObjectName(QStringLiteral("extractStrip"));
    strip_->setSource(source.doc, source.engine);
    // Resizing refolds from scratch: it is one of the two ways an expanded fold
    // closes (editing its run is the other, see refresh).
    connect(strip_, &ExtractStrip::viewportResized, this, [this] {
        expanded_.clear();
        refresh();
    });
    connect(strip_, &ExtractStrip::foldActivated, this, &ExtractDialog::expand);
    connect(strip_, &ExtractStrip::removeClicked, this, &ExtractDialog::removeCells);
    connect(strip_, &ExtractStrip::currentCellChanged, this, &ExtractDialog::updateActions);
    connect(strip_, &ExtractStrip::rowPicked, this, [this](int row) { list_->setCurrentRow(row); });
    layout->addWidget(strip_);
    layout->addSpacing(6);

    // Strip actions: the context menu, and their keys while the strip has focus.
    const auto stripAction = [this](const QString &text) {
        auto *a = new QAction(text, strip_);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        a->setShortcutVisibleInContextMenu(true);
        strip_->addAction(a);
        return a;
    };
    removeAct_ = stripAction(tr("Remove"));
    removeAct_->setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)});
    connect(removeAct_, &QAction::triggered, this, [this] {
        if (const ExtractPlan::Cell *c = strip_->currentCell())
            removeCells(*c);
    });
    expandAct_ = stripAction(QString());
    expandAct_->setShortcut(QKeySequence(Qt::Key_Space));
    connect(expandAct_, &QAction::triggered, this, [this] {
        if (const ExtractPlan::Cell *c = strip_->currentCell())
            expand(c->run);
    });

    // ── Summary and the reserved error line ──────────────────────────────────
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("extractSummary"));
    // Ignored: a long line clips instead of widening the dialog. At the minimum
    // width even the longest summary fits.
    summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(summary_);
    layout->addSpacing(2);
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("extractError"));
    error_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    // Reserved even when empty, so nothing below moves as the rows go in and out
    // of validity.
    error_->setMinimumHeight(error_->fontMetrics().lineSpacing());
    layout->addWidget(error_);
    layout->addSpacing(8);

    // ── Password (only for an encrypted file, so the layout never shifts) ────
    QLabel *passwordLabel = nullptr;
    if (needsPassword) {
        auto *pwRow = row();
        passwordLabel = new QLabel(tr("Pass&word:"), this);
        passwordEdit_ = new QLineEdit(this);
        passwordEdit_->setObjectName(QStringLiteral("extractPassword"));
        passwordEdit_->setEchoMode(QLineEdit::Password);
        passwordEdit_->setPlaceholderText(tr("This PDF is encrypted"));
        passwordEdit_->setFixedWidth(kPasswordWidth);
        passwordLabel->setBuddy(passwordEdit_);
        connect(passwordEdit_, &QLineEdit::textEdited, this, [this] {
            passwordRejected_ = false;
            refresh();
        });
        pwRow->addWidget(passwordLabel);
        pwRow->addWidget(passwordEdit_);
        pwRow->addStretch(1);
        layout->addLayout(pwRow);
        layout->addSpacing(8);
    }

    // ── Save as ──────────────────────────────────────────────────────────────
    auto *saveRow = row();
    auto *saveLabel = new QLabel(tr("&Save as:"), this);
    output_ = new QLineEdit(this);
    output_->setObjectName(QStringLiteral("extractOutput"));
    saveLabel->setBuddy(output_);
    connect(output_, &QLineEdit::textEdited, this, [this] {
        // Latch on any edit, including the one that empties the field (Merge's
        // rule: tracking emptiness instead refilled a field the user had just
        // cleared). setText() does not emit textEdited, so refresh cannot trip it.
        outputEdited_ = true;
        refresh();
    });
    auto *browse = new QPushButton(tr("&Browse…"), this);
    browse->setObjectName(QStringLiteral("extractBrowse"));
    browse->setMinimumWidth(kBrowseMinWidth);
    browse->setFocusPolicy(Qt::TabFocus); // as the side buttons: a click keeps Enter on Extract
    connect(browse, &QPushButton::clicked, this, &ExtractDialog::browseForOutput);
    saveRow->addWidget(saveLabel);
    saveRow->addWidget(output_, 1);
    saveRow->addWidget(browse);
    layout->addLayout(saveRow);
    layout->addSpacing(10);
    // The field labels share the wider one's width, so the fields line up.
    if (passwordLabel) {
        const int w = qMax(passwordLabel->sizeHint().width(), saveLabel->sizeHint().width());
        passwordLabel->setFixedWidth(w);
        saveLabel->setFixedWidth(w);
    }

    // ── Buttons ──────────────────────────────────────────────────────────────
    auto *buttonRow = row();
    openWhenDone_ = new QCheckBox(tr("&Open when done"), this);
    openWhenDone_->setObjectName(QStringLiteral("extractOpenWhenDone"));
    openWhenDone_->setChecked(source.openWhenDone);
    buttonRow->addWidget(openWhenDone_);
    buttonRow->addStretch(1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    acceptBtn_ = buttons->addButton(tr("Extract"), QDialogButtonBox::AcceptRole);
    acceptBtn_->setObjectName(QStringLiteral("extractAccept"));
    acceptBtn_->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &ExtractDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &ExtractDialog::reject);
    buttonRow->addWidget(buttons);
    layout->addLayout(buttonRow);

    // One row holding the page being viewed, selected, so typing replaces it and
    // Enter extracts it.
    const int seed = qBound(0, source.currentPage, qMax(0, plan_.pageCount() - 1));
    plan_.append(QString::number(seed + 1));
    rebuild(0, Focus::SelectAll);

    resize(kWidth, kHeight);
    setMinimumSize(kMinWidth, qMax(kMinHeight, minimumSizeHint().height()));
}

bool ExtractDialog::openWhenDone() const
{
    return openWhenDone_->isChecked();
}

void ExtractDialog::rebuild(int selectRow, Focus focus)
{
    if (focus == Focus::Keep) {
        const QWidget *f = focusWidget();
        focus = f && list_->isAncestorOf(f) ? Focus::Field : Focus::Keep;
    }
    // The old fields outlive clear() until the next event-loop turn (deleteLater),
    // still connected: the new generation makes them ignore their own edits.
    ++fieldsGen_;
    split_.reset(); // its rows, if still pending, are gone with the fields
    specs_.clear();
    list_->clear();
    for (int i = 0; i < plan_.count(); ++i) {
        list_->addRow([this, i](QWidget *rowWidget, QHBoxLayout *columns) {
            auto *spec = new QLineEdit(plan_.spec(i), rowWidget);
            spec->setObjectName(QStringLiteral("extractRowSpec"));
            spec->setPlaceholderText(tr("e.g. 5-7"));
            spec->setToolTip(tr("One page or range, like 5 or 5-7."));
            // Rebuilt wholesale on every change of rows, so this build-time index
            // is valid while the field's generation is current.
            connect(spec, &QLineEdit::textChanged, this,
                    [this, i, gen = fieldsGen_](const QString &text) {
                        if (gen == fieldsGen_)
                            specChanged(i, text);
                        else if (split_ && split_->gen == gen && split_->row == i)
                            split_->text = text; // typed before the split ran: split this
                    });
            columns->addWidget(spec, 1);
            specs_ << spec;
        });
    }
    // The strip gets the new cells first, so revealing the current row in it
    // scrolls to the right ones.
    refresh();
    const int current = qMin(selectRow, plan_.count() - 1);
    if (current >= 0) {
        list_->setCurrentRow(current);
        list_->scrollToItem(list_->item(current));
        syncCurrentRow(true); // also when the row was already current
    }
    if (current < 0 || focus == Focus::Keep)
        return;
    QLineEdit *field = specs_.at(current);
    field->setFocus();
    if (focus == Focus::SelectAll)
        field->selectAll();
    else
        field->end(false);
}

void ExtractDialog::specChanged(int row, const QString &text)
{
    if (!text.contains(QLatin1Char(',')) && !text.contains(QLatin1Char(';'))) {
        plan_.setSpec(row, text);
        refresh(); // only the derived columns change: the field keeps its caret
        strip_->ensureRowVisible(row);
        return;
    }
    // A comma or semicolon: the split rebuilds the rows, deleting the field being
    // typed in, so it waits for the next event-loop turn. Keys already queued for
    // this field only change the text it will split: the field is now stale.
    split_ = PendingSplit{row, fieldsGen_, text};
    ++fieldsGen_;
    QTimer::singleShot(0, this, &ExtractDialog::applySplit);
}

void ExtractDialog::applySplit()
{
    if (!split_)
        return; // already applied, or dropped by a rebuild in between
    const PendingSplit split = *split_;
    const QStringList pieces = ExtractPlan::splitPieces(split.text);
    plan_.setSpec(split.row, pieces.first());
    for (int k = 1; k < pieces.size(); ++k)
        plan_.insert(split.row + k, pieces.at(k));
    rebuild(split.row + int(pieces.size()) - 1, Focus::Field);
}

void ExtractDialog::addRange()
{
    const int at = list_->currentRow() < 0 ? plan_.count() : list_->currentRow() + 1;
    plan_.insert(at, QString());
    rebuild(at, Focus::Field);
}

void ExtractDialog::moveCurrent(int delta)
{
    const int to = plan_.move(list_->currentRow(), delta);
    if (to >= 0)
        rebuild(to, Focus::Keep);
}

void ExtractDialog::removeCurrent()
{
    const int i = list_->currentRow();
    if (i < 0)
        return;
    plan_.remove(i);
    rebuild(i, Focus::Keep);
}

void ExtractDialog::removeCells(const ExtractPlan::Cell &cell)
{
    const int next = plan_.removeFromRow(cell.row, cell.first, cell.last);
    pendingFlat_ = cell.firstFlat; // the cell that slides into its place
    rebuild(next, Focus::Keep);
}

void ExtractDialog::expand(const ExtractPlan::RunKey &run)
{
    if (!expanded_.contains(run))
        expanded_.append(run);
    if (const ExtractPlan::Cell *c = strip_->currentCell())
        pendingFlat_ = c->firstFlat;
    refresh();
}

void ExtractDialog::updateActions()
{
    const ExtractPlan::Cell *c = strip_->currentCell();
    removeAct_->setEnabled(c != nullptr);
    const bool fold = c && c->kind == ExtractPlan::Cell::Kind::Fold;
    expandAct_->setVisible(fold);
    expandAct_->setEnabled(fold);
    if (fold)
        expandAct_->setText(tr("Show Pages %1").arg(plan_.caption(*c)));
}

void ExtractDialog::syncCurrentRow(bool reveal)
{
    const int cur = list_->currentRow();
    upBtn_->setEnabled(cur > 0);
    downBtn_->setEnabled(cur >= 0 && cur < plan_.count() - 1);
    removeBtn_->setEnabled(cur >= 0);
    // With one row every cell is that row's, so marking it would say nothing.
    strip_->setHighlightedRow(plan_.count() > 1 ? cur : -1);
    if (reveal)
        strip_->ensureRowVisible(cur);
}

void ExtractDialog::refresh()
{
    // Save as first: the error line and the button read it. Native separators, as
    // in Merge: a '/' path reads as generated, not local.
    if (!outputEdited_)
        output_->setText(QDir::toNativeSeparators(plan_.defaultOutputPath()));

    const QList<ExtractPlan::RowText> texts = plan_.rowTexts();
    for (int i = 0; i < specs_.size() && i < texts.size(); ++i) {
        list_->setRowTexts(i, texts.at(i).count, texts.at(i).output);
        // Red outline for text that does not parse, not for an empty row.
        QLineEdit *spec = specs_.at(i);
        const bool bad = plan_.isBadText(i);
        if (spec->property("invalid").toBool() != bad) {
            spec->setProperty("invalid", bad);
            spec->style()->unpolish(spec);
            spec->style()->polish(spec);
        }
    }

    // The strip: an expansion lasts until its run is edited away.
    expanded_.removeIf([this](const ExtractPlan::RunKey &run) { return !plan_.hasRun(run); });
    ExtractStrip::Content content;
    content.cells = plan_.cells(ExtractStrip::metrics(), strip_->viewport()->width(), expanded_);
    for (const ExtractPlan::Cell &c : content.cells) {
        content.captions << plan_.caption(c);
        content.toolTips << plan_.describe(c);
    }
    int current = 0;
    if (pendingFlat_)
        current = *pendingFlat_;
    else if (const ExtractPlan::Cell *c = strip_->currentCell())
        current = c->firstFlat;
    pendingFlat_.reset();
    strip_->setContent(content, current);
    syncCurrentRow(false);

    of_->setText(tr("of %1").arg(plan_.pageCount()));
    QString summary = plan_.summaryText();
    if (!summary.isEmpty() && source_.hasUnsavedEdits)
        summary += tr(" Unsaved changes are not included."); // qpdf writes from disk
    summary_->setText(summary);

    // The button is enabled exactly when the error line is empty, so a disabled
    // Extract always has its reason on screen.
    const QString why = problem();
    error_->setText(why);
    error_->setToolTip(readError_ ? readErrorDetail_ : QString());
    acceptBtn_->setEnabled(why.isEmpty());
}

QString ExtractDialog::problem() const
{
    if (readError_)
        return tr("This PDF cannot be read for extraction.");
    const QString planError = plan_.errorText();
    if (!planError.isEmpty())
        return planError;
    if (passwordEdit_ && passwordEdit_->text().isEmpty())
        return tr("Enter the password for this PDF.");
    if (passwordRejected_)
        return tr("That password is not correct.");
    return plan_.destinationError(output_->text());
}

void ExtractDialog::browseForOutput()
{
    const QString picked = QFileDialog::getSaveFileName(this, tr("Save Extracted Pages"),
                                                        plan_.resolvePath(output_->text()),
                                                        tr("PDF documents (*.pdf)"));
    if (picked.isEmpty())
        return;
    output_->setText(QDir::toNativeSeparators(picked));
    outputEdited_ = true;
    refresh();
}

void ExtractDialog::accept()
{
    applySplit(); // Enter queued behind a comma judges the rows the field shows
    if (!problem().isEmpty())
        return; // Enter with a disabled Extract does nothing

    // Verify the password before closing, so a wrong one is fixed here rather
    // than after the dialog has gone.
    if (passwordEdit_) {
        int count = 0;
        QString err;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const PageOps::Status st =
            PageOps::probe(source_.path, &count, passwordEdit_->text(), &err);
        QApplication::restoreOverrideCursor();
        if (st == PageOps::Status::NeedsPassword) {
            passwordRejected_ = true;
            refresh();
            passwordEdit_->setFocus();
            passwordEdit_->selectAll();
            return;
        }
        if (st == PageOps::Status::Failed) {
            readError_ = true;
            readErrorDetail_ = err;
            refresh();
            return;
        }
        // Until now the count was the viewer's; qpdf's is the one the write uses.
        if (count > 0 && count != plan_.pageCount()) {
            plan_.setPageCount(count);
            refresh();
            if (!problem().isEmpty())
                return;
        }
    }

    const ExtractPlan::Job job = plan_.job(output_->text());
    if (QFileInfo::exists(job.path)
        && QMessageBox::question(this, tr("Extract Pages"),
                                 tr("\"%1\" already exists. Replace it?")
                                     .arg(QDir::toNativeSeparators(job.path)),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
               != QMessageBox::Yes)
        return;

    job_ = job;
    if (passwordEdit_)
        password_ = passwordEdit_->text();
    QDialog::accept();
}

} // namespace mervin
