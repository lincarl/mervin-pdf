#include "ui/RecentFilesPanel.h"

#include "ui/FileContextMenu.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/ThemeTokens.h"

#include <QContextMenuEvent>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QShowEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>

namespace mervin {

namespace {

constexpr int kPathRole      = Qt::UserRole;
constexpr int kMissingRole   = Qt::UserRole + 1;
constexpr int kEpochRole     = Qt::UserRole + 2; // qint64 lastOpened
constexpr int kPageCountRole = Qt::UserRole + 3; // int pageCount (0 = unknown)
constexpr int kFavoriteRole  = Qt::UserRole + 4; // bool favorite
constexpr int kQueryRole     = Qt::UserRole + 5; // QString search term to highlight
constexpr int kSnippetRole   = Qt::UserRole + 6; // QString content preview (content search)
constexpr int kCaptionRole   = Qt::UserRole + 7; // bool: a section caption, not a file

constexpr int kContentDebounceMs = 400;
constexpr int kRowHeight = 64;
constexpr int kContentRowHeight = 88; // taller: fits name + snippet + path
constexpr int kIconSize  = 34;
constexpr int kMetaW     = 180; // right-column minimum width
constexpr int kHPad      = 16;  // left/right outer margin
constexpr int kVPad      =  9;  // top/bottom inner padding
constexpr int kStarW     = 20;  // star icon hit area
constexpr int kStarGap   =  8;  // gap between right meta column and star
constexpr int kFieldWidth = 460; // the search field


// Search-match highlight - the same yellow find in document paints over the
// page, so a match looks consistent wherever it appears.
const QColor &kHighlightFill = theme::brand().searchMatch;

// Two details of a file row side by side, such as "3 days ago · 03/10/2026" or
// "12 pages · 1.4 MB".
QString joinDetails(const QString &first, const QString &second)
{
    //: Joins two details in a row of the Recent list, such as when the file was
    //: opened and the date ("3 days ago · 03/10/2026"), or its page count and size.
    return RecentFilesPanel::tr("%1 · %2").arg(first, second);
}

QString formatDate(qint64 epochMs)
{
    if (epochMs <= 0)
        return QString();
    const QDate date = QDateTime::fromMSecsSinceEpoch(epochMs).date();
    const int daysAgo = date.daysTo(QDate::currentDate());
    QString rel;
    // A date after today (the clock was set back) reads as today.
    if (daysAgo <= 0) {
        //: When a recent file was last opened.
        rel = RecentFilesPanel::tr("Today");
    } else if (daysAgo == 1) {
        //: When a recent file was last opened.
        rel = RecentFilesPanel::tr("Yesterday");
    } else {
        //: When a recent file was last opened. Used from 2 days on.
        rel = RecentFilesPanel::tr("%n day(s) ago", nullptr, daysAgo);
    }
    return joinDetails(rel, QLocale().toString(date, QLocale::ShortFormat));
}

// The OS regional format, as the other dialogs show sizes.
QString formatSize(qint64 bytes)
{
    if (bytes <= 0)
        return QString();
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

QRect starRegion(const QRect &row)
{
    return QRect(row.right() - kHPad - kStarW + 1,
                 row.top() + (row.height() - kStarW) / 2,
                 kStarW, kStarW);
}

// Lucide "star": filled in the favourite yellow when starred, an outline when not.
// Rendered at the device's pixel ratio so it stays sharp on scaled displays.
void drawStar(QPainter *p, const QRect &rect, bool filled)
{
    const qreal dpr = p->device()->devicePixelRatioF();
    const int px = qRound(rect.width() * dpr);
    QPixmap pm = filled ? icons::glyphPixmap(icons::Glyph::Star, theme::brand().starEdge, px, 2.0,
                                             theme::brand().starFill)
                        : icons::glyphPixmap(icons::Glyph::Star, theme::brand().starEmptyEdge,
                                             px, 2.0);
    pm.setDevicePixelRatio(dpr);
    p->drawPixmap(rect.topLeft(), pm);
}

// Draw `fullText` on a single line within `rect` (left-aligned, vertically
// centred), eliding to fit, and paint a highlight behind every case-insensitive
// occurrence of `needle`. With an empty/absent needle this is a plain elided
// drawText. Highlighting runs against the *elided* string, so a match hidden
// behind the ellipsis is correctly left unmarked.
void drawHighlighted(QPainter *p, const QRect &rect, Qt::TextElideMode elide,
                     const QString &fullText, const QString &needle,
                     const QColor &textCol)
{
    const QFontMetrics fm = p->fontMetrics();
    const QString shown = fm.elidedText(fullText, elide, rect.width());

    constexpr int kFlags = Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine;
    if (needle.isEmpty() || !shown.contains(needle, Qt::CaseInsensitive)) {
        p->setPen(textCol);
        p->drawText(rect, kFlags, shown);
        return;
    }

    // Walk the string, drawing unmatched runs plainly and matched runs over a
    // highlight fill; x advances by each run's measured width.
    const QString lower  = shown.toLower();
    const QString nlower = needle.toLower();
    const int nlen = nlower.size();
    int x = rect.left();
    int from = 0;
    while (from < shown.size()) {
        const int hit = lower.indexOf(nlower, from);
        const int runEnd = (hit < 0) ? shown.size() : hit;
        if (runEnd > from) {
            const QString run = shown.mid(from, runEnd - from);
            const int w = fm.horizontalAdvance(run);
            p->setPen(textCol);
            p->drawText(QRect(x, rect.top(), w, rect.height()), kFlags, run);
            x += w;
        }
        if (hit < 0)
            break;
        const QString match = shown.mid(hit, nlen);
        const int w = fm.horizontalAdvance(match);
        p->setPen(Qt::NoPen);
        p->setBrush(kHighlightFill);
        p->drawRoundedRect(QRect(x - 1, rect.top() + 2, w + 2, rect.height() - 4), 3, 3);
        p->setPen(textCol);
        p->drawText(QRect(x, rect.top(), w, rect.height()), kFlags, match);
        x += w;
        from = hit + nlen;
    }
}

// Delegate that paints the two-column recent-file row directly with QPainter.
class RecentItemDelegate : public QStyledItemDelegate
{
public:
    explicit RecentItemDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &idx) const override
    {
        // A caption hugs the rows it names: shorter at the top of the list, with
        // more air above it between sections.
        if (idx.data(kCaptionRole).toBool())
            return QSize(-1, idx.row() == 0 ? 26 : 36);
        if (idx.data(kPathRole).toString().isEmpty())
            return QSize(-1, 36);
        // Content-search rows carry a snippet and need a third line of height.
        return QSize(-1, idx.data(kSnippetRole).toString().isEmpty()
                             ? kRowHeight : kContentRowHeight);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &opt,
               const QModelIndex &idx) const override
    {
        const QString path = idx.data(kPathRole).toString();

        if (idx.data(kCaptionRole).toBool()) {
            // Small semibold soft ink at the icon column, baseline near the bottom.
            QFont f = opt.font;
            f.setPointSizeF(f.pointSizeF() * 0.85);
            f.setWeight(QFont::DemiBold);
            p->save();
            p->setFont(f);
            p->setPen(theme::chrome(opt.palette).inkSoft);
            p->drawText(opt.rect.adjusted(kHPad, 0, -kHPad, -5),
                        Qt::AlignLeft | Qt::AlignBottom | Qt::TextSingleLine,
                        idx.data(Qt::DisplayRole).toString());
            p->restore();
            return;
        }
        if (path.isEmpty()) {
            QStyledItemDelegate::paint(p, opt, idx);
            return;
        }

        QStyleOptionViewItem o = opt;
        initStyleOption(&o, idx);
        o.text.clear();
        o.icon = QIcon();
        opt.widget->style()->drawControl(QStyle::CE_ItemViewItem, &o, p, opt.widget);

        const bool missing   = idx.data(kMissingRole).toBool();
        const bool hasEntry  = idx.data(kEpochRole).isValid();
        const bool isFav     = idx.data(kFavoriteRole).toBool();
        const int  pages     = idx.data(kPageCountRole).toInt();
        const QFileInfo fi(path);
        const QString name   = fi.fileName().isEmpty() ? path : fi.fileName();
        // Full native path; the ElideLeft below trims it to the column width, so
        // it shrinks automatically as the window narrows (no fixed char cap).
        const QString pDisp  = QDir::toNativeSeparators(path);
        const QString date   = formatDate(idx.data(kEpochRole).toLongLong());
        const QString size   = (fi.exists() && fi.size() > 0) ? formatSize(fi.size()) : QString();
        const QString query  = idx.data(kQueryRole).toString();
        const QString snippet = idx.data(kSnippetRole).toString();
        const bool hasSnippet = !snippet.isEmpty();
        const QString pageText =
            pages > 0 ? RecentFilesPanel::tr("%n page(s)", nullptr, pages) : QString();
        const QString metaLine = pageText.isEmpty() || size.isEmpty()
                                     ? pageText + size
                                     : joinDetails(pageText, size);

        const QRect r = opt.rect;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setClipRect(r);

        // ── Icon ──────────────────────────────────────────────────────────
        const QRect iconR(r.left() + kHPad,
                          r.top()  + (r.height() - kIconSize) / 2,
                          kIconSize, kIconSize);
        const QPixmap documentIcon = icons::applicationIcon().pixmap(
            iconR.size(), p->device()->devicePixelRatioF());
        p->drawPixmap(iconR.topLeft(), documentIcon);

        // ── Star (right edge, only for regular entries) ───────────────────
        if (hasEntry) {
            drawStar(p, starRegion(r), isFav);
        }

        QFont smallF = opt.font;
        smallF.setPointSizeF(smallF.pointSizeF() * 0.85);

        // ── Right meta column ─────────────────────────────────────────────
        // At least kMetaW wide. A longer translation or date format, such as
        // "För 21 dagar sedan · 2026-09-15", widens it up to 40% of the row,
        // and the name and path columns, which elide, give up the space.
        const qreal metaText =
            std::max(QFontMetricsF(opt.font, p->device()).horizontalAdvance(date),
                     QFontMetricsF(smallF, p->device()).horizontalAdvance(metaLine));
        const int metaW = std::max(kMetaW, std::min(qCeil(metaText), r.width() * 2 / 5));
        const int starOff = hasEntry ? kStarW + kStarGap : 0;
        const int rightX  = r.right() - kHPad - starOff - metaW;
        const int midY    = r.top() + r.height() / 2;
        const QRect dateR(rightX, r.top() + kVPad,  metaW, midY - r.top() - kVPad);
        const QRect sizeR(rightX, midY,              metaW, r.bottom() - midY - kVPad);

        // ── Left column: name (+ snippet) + path ──────────────────────────
        const int leftX = iconR.right() + 12;
        const int leftW = rightX - 12 - leftX;

        // Two lines (name / path) normally; three (name / snippet / path) for a
        // content-search hit, split into equal bands inside the padded area.
        QRect nameR, snippetR, pathR;
        if (hasSnippet) {
            const int top  = r.top() + kVPad;
            const int avail = r.height() - 2 * kVPad;
            const int band = avail / 3;
            nameR    = QRect(leftX, top,            leftW, band);
            snippetR = QRect(leftX, top + band,     leftW, band);
            pathR    = QRect(leftX, top + 2 * band, leftW, avail - 2 * band);
        } else {
            nameR = QRect(leftX, r.top() + kVPad, leftW, midY - r.top() - kVPad);
            pathR = QRect(leftX, midY,             leftW, r.bottom() - midY - kVPad);
        }

        // The path subtitle is muted, not disabled: reading it out of the Disabled
        // colour group gave it exactly the ink a MISSING file's name gets, so the
        // two states were indistinguishable - and put the subtitle at 3.0:1 on the
        // list well. inkSoft is the vocabulary's muted ink, one step brighter.
        const QColor textCol = opt.palette.color(
            missing ? QPalette::Disabled : QPalette::Normal, QPalette::Text);
        const QColor subCol = theme::chrome(opt.palette).inkSoft;

        QFont boldF = opt.font;
        boldF.setWeight(QFont::DemiBold);
        p->setFont(boldF);
        drawHighlighted(p, nameR, Qt::ElideRight, name, query, textCol);

        if (hasSnippet) {
            p->setFont(smallF);
            drawHighlighted(p, snippetR, Qt::ElideRight, snippet, query, textCol);
        }

        p->setFont(smallF);
        drawHighlighted(p, pathR, Qt::ElideLeft, pDisp, QString(), subCol);

        // Text wider than the meta column's cap is elided.
        constexpr int kMetaFlags = Qt::AlignRight | Qt::AlignVCenter | Qt::TextSingleLine;
        p->setFont(opt.font);
        p->setPen(textCol);
        p->drawText(dateR, kMetaFlags,
                    p->fontMetrics().elidedText(date, Qt::ElideRight, dateR.width()));

        p->setFont(smallF);
        p->setPen(subCol);
        p->drawText(sizeR, kMetaFlags,
                    p->fontMetrics().elidedText(metaLine, Qt::ElideRight, sizeR.width()));

        p->restore();
    }
};

// The list's keyboard navigation steps over the section captions, which are
// disabled rows. Qt skips them for the arrow keys only; Home, Page Up and Page
// Down would otherwise stop on one and leave the current row where it was.
class RecentList final : public QListWidget
{
public:
    using QListWidget::QListWidget;

protected:
    QModelIndex moveCursor(CursorAction action, Qt::KeyboardModifiers modifiers) override
    {
        const QModelIndex index = QListWidget::moveCursor(action, modifiers);
        if (!index.isValid() || (model()->flags(index) & Qt::ItemIsEnabled))
            return index;
        // Page Up lands on the row above a caption; everything else, including a
        // caption at the very top, on the row below it.
        const int step = action == MovePageUp ? -1 : 1;
        for (const int dir : {step, -step}) {
            for (int row = index.row() + dir; row >= 0 && row < model()->rowCount(); row += dir) {
                const QModelIndex candidate = model()->index(row, 0);
                if (model()->flags(candidate) & Qt::ItemIsEnabled)
                    return candidate;
            }
        }
        return currentIndex();
    }
};

} // namespace

RecentFilesPanel::RecentFilesPanel(QWidget *parent)
    : QWidget(parent)
{
    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(kContentDebounceMs);
    connect(debounce_, &QTimer::timeout, this, &RecentFilesPanel::startContentSearch);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 16, 24, 12);
    layout->setSpacing(8);

    // ── Search row: the field, then the content-search status ─────────────
    // The status sits beside the field so the list never moves when a scan starts.
    auto *searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(0, 0, 0, 4);
    searchRow->setSpacing(16);
    search_ = new RecentSearchField(this);
    search_->setFixedWidth(kFieldWidth);
    search_->installEventFilter(this);
    searchRow->addWidget(search_);
    // The status, and Stop while a scan runs, sit a little closer together.
    auto *statusRow = new QHBoxLayout;
    statusRow->setSpacing(10);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("recentSearchStatus"));
    status_->setVisible(false);
    statusRow->addWidget(status_);
    stopBtn_ = new QToolButton(this);
    stopBtn_->setObjectName(QStringLiteral("recentSearchStop"));
    //: Button that stops the search inside the documents.
    stopBtn_->setText(tr("Stop"));
    stopBtn_->setFocusPolicy(Qt::TabFocus); // a click leaves the caret in the field
    stopBtn_->setVisible(false);
    connect(stopBtn_, &QToolButton::clicked, this, &RecentFilesPanel::stopSearch);
    statusRow->addWidget(stopBtn_);
    searchRow->addLayout(statusRow);
    searchRow->addStretch();
    layout->addLayout(searchRow);
    connect(search_, &QLineEdit::textChanged, this, &RecentFilesPanel::onSearchChanged);
    connect(search_, &RecentSearchField::scopeChanged, this, &RecentFilesPanel::onSearchChanged);

    // ── List ──────────────────────────────────────────────────────────────
    list_ = new RecentList(this);
    list_->setItemDelegate(new RecentItemDelegate(list_));
    list_->setAlternatingRowColors(false);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setFrameShape(QFrame::NoFrame);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->viewport()->installEventFilter(this);
    connect(list_, &QListWidget::itemActivated, this, &RecentFilesPanel::onItemActivated);
    layout->addWidget(list_, 1);

    rebuild();
}

bool RecentFilesPanel::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == search_ && event->type() == QEvent::KeyPress) {
        // Escape and Down move to the list and keep the search.
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Escape || key == Qt::Key_Down) {
            focusList();
            return true;
        }
    } else if (obj == list_->viewport() && event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            QListWidgetItem *item = list_->itemAt(me->pos());
            if (item && !item->data(kPathRole).toString().isEmpty()
                     &&  item->data(kEpochRole).isValid()) {
                if (starRegion(list_->visualItemRect(item)).contains(me->pos())) {
                    toggleItemFavorite(item);
                    return true;
                }
            }
        }
    } else if (obj == list_->viewport() && event->type() == QEvent::ContextMenu) {
        auto *ce = static_cast<QContextMenuEvent *>(event);
        if (QListWidgetItem *item = list_->itemAt(ce->pos())) {
            const QString path = item->data(kPathRole).toString();
            if (!path.isEmpty()) {
                const bool missing = item->data(kMissingRole).toBool();
                const bool regularEntry = item->data(kEpochRole).isValid();
                const QStringList missingPaths = missingFilesInCurrentFilter();
                const QColor ink = Theme::iconInk(palette());
                QList<FileMenuItem> surfaceItems{
                    {tr("Open in new window"),
                     [this, path] { emit openInNewWindowRequested(path); },
                     !missing,
                     icons::glyph(icons::Glyph::OpenInNewWindow, ink)}
                };
                if (regularEntry) {
                    surfaceItems.append({tr("Clear missing files"),
                                         [this, missingPaths] {
                                             emit clearMissingRequested(missingPaths);
                                         },
                                         !missingPaths.isEmpty(),
                                         icons::glyph(icons::Glyph::Broom, ink)});
                }
                showFileContextMenu(this, path, ce->globalPos(), surfaceItems);
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void RecentFilesPanel::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    if (event->spontaneous())
        return;
    // Leaving Recent stops a pending or running scan; it starts again when Recent
    // is shown, so the list never presents a cut-short scan as complete.
    if (debounce_->isActive() || searching_) {
        debounce_->stop();
        if (searching_) {
            searching_ = false;
            emit contentSearchCanceled();
        }
        rescanOnShow_ = true;
    }
    // It also settles starred rows into their sections for the next visit.
    if (!stickySection_.isEmpty()) {
        stickySection_.clear();
        rebuild();
    }
}

void RecentFilesPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (rescanOnShow_ && !event->spontaneous()) {
        rescanOnShow_ = false;
        onSearchChanged();
    }
}

void RecentFilesPanel::toggleItemFavorite(QListWidgetItem *item)
{
    const QString path = item->data(kPathRole).toString();
    const bool wasFav  = item->data(kFavoriteRole).toBool();

    // The row stays in the section it is listed in until the search changes or
    // Recent is left; rebuilds in between (the store echoes the change back
    // through setEntries) keep it there.
    if (!stickySection_.contains(path))
        stickySection_.insert(path, wasFav);

    // Keep in-memory entries_ consistent so rebuild() doesn't undo the change
    // before the store round-trip completes.
    for (RecentEntry &e : entries_) {
        if (e.path == path) { e.favorite = !wasFav; break; }
    }
    item->setData(kFavoriteRole, !wasFav);
    list_->update(list_->indexFromItem(item));
    emit favoriteToggled(path, !wasFav);
}

QString RecentFilesPanel::needle() const
{
    return search_->text().trimmed();
}

bool RecentFilesPanel::nameMatches(const RecentEntry &entry) const
{
    return QFileInfo(entry.path).fileName().contains(needle(), Qt::CaseInsensitive);
}

bool RecentFilesPanel::listedAsFavourite(const RecentEntry &entry) const
{
    return stickySection_.value(entry.path, entry.favorite);
}

const RecentEntry *RecentFilesPanel::entryFor(const QString &path) const
{
    for (const RecentEntry &e : entries_)
        if (e.path == path)
            return &e;
    return nullptr;
}

QStringList RecentFilesPanel::missingFilesInCurrentFilter() const
{
    QStringList paths;
    for (int i = 0; i < list_->count(); ++i) {
        const QListWidgetItem *item = list_->item(i);
        if (item->data(kMissingRole).toBool())
            paths.append(item->data(kPathRole).toString());
    }
    return paths;
}

void RecentFilesPanel::setEntries(const QList<RecentEntry> &entries)
{
    entries_ = entries;
    rebuild();
}

void RecentFilesPanel::setVisibleCount(int count)
{
    visibleCount_ = count > 0 ? count : 100;
    rebuild();
}

void RecentFilesPanel::setDefaultScope(Scope scope)
{
    search_->setScope(scope);
}

void RecentFilesPanel::focusSearch()
{
    search_->setFocus(Qt::ShortcutFocusReason);
    search_->selectAll();
}

void RecentFilesPanel::focusList()
{
    const QListWidgetItem *current = list_->currentItem();
    if (!current || !(current->flags() & Qt::ItemIsSelectable)) {
        for (int i = 0; i < list_->count(); ++i) {
            if (list_->item(i)->flags() & Qt::ItemIsSelectable) {
                list_->setCurrentRow(i);
                break;
            }
        }
    }
    list_->setFocus(Qt::ShortcutFocusReason);
}

bool RecentFilesPanel::contentMode() const
{
    return search_->scope() != Scope::Names && !needle().isEmpty();
}

void RecentFilesPanel::onSearchChanged()
{
    // A new search settles starred rows into their sections.
    stickySection_.clear();
    rescanOnShow_ = false;
    debounce_->stop();
    if (searching_) {
        searching_ = false;
        emit contentSearchCanceled();
    }
    hitOrder_.clear();
    hits_.clear();
    scanDone_ = false;
    // Names (and All's name matches) are listed at once; contents after the pause.
    rebuild();
    if (!contentMode()) {
        setRunning(false);
        status_->setVisible(false);
        return;
    }
    // The status and Stop show at once, so the search is visibly on its way during
    // the pause too.
    status_->setText(tr("Searching…"));
    status_->setVisible(true);
    setRunning(true);
    debounce_->start();
}

void RecentFilesPanel::startContentSearch()
{
    if (!contentMode())
        return;
    // Never scan for a hidden page (a Settings change, or the pause running out
    // after Recent was left); showEvent starts it when Recent comes back.
    if (!isVisible()) {
        rescanOnShow_ = true;
        return;
    }
    const bool all = search_->scope() == Scope::All;
    QStringList paths;
    paths.reserve(entries_.size());
    for (const RecentEntry &e : std::as_const(entries_)) {
        if (all && nameMatches(e))
            continue; // already listed by name
        paths.append(e.path);
    }
    searching_ = true;
    scanned_ = 0;
    scanTotal_ = paths.size();
    status_->setVisible(true);
    setRunning(true);
    showScanProgress();
    emit contentSearchRequested(needle(), paths);
}

void RecentFilesPanel::setRunning(bool running)
{
    if (status_->property("running").toBool() != running) {
        status_->setProperty("running", running);
        status_->style()->unpolish(status_);
        status_->style()->polish(status_);
    }
    // While Stop shows, the status keeps the width of its longest text for this
    // history, so Stop never moves as "Searching…" turns into "Searching file 9
    // of 40" and the counts gain digits.
    if (running) {
        // A space of slack: QLabel sizes text by its bounding box, which can be a
        // pixel wider than the advance.
        const int n = std::max(1, int(entries_.size()));
        const QFontMetrics fm = status_->fontMetrics();
        //: %1 is the file being read, %2 how many files the search reads.
        const QString widest = tr("Searching file %1 of %2").arg(n).arg(n);
        status_->setMinimumWidth(std::max(fm.horizontalAdvance(widest), fm.boundingRect(widest).width())
                                 + fm.horizontalAdvance(QLatin1Char(' ')));
    } else {
        status_->setMinimumWidth(0);
    }
    stopBtn_->setVisible(running);
}

// Shows "Searching file 3 of 40", the file being read out of the files to read.
void RecentFilesPanel::showScanProgress()
{
    if (scanTotal_ <= 0)
        return;
    //: %1 is the file being read, %2 how many files the search reads.
    status_->setText(tr("Searching file %1 of %2")
                         .arg(std::min(scanned_ + 1, scanTotal_))
                         .arg(scanTotal_));
}

void RecentFilesPanel::stopSearch()
{
    if (!debounce_->isActive() && !searching_)
        return;
    debounce_->stop();
    if (searching_) {
        searching_ = false;
        emit contentSearchCanceled();
    }
    rescanOnShow_ = false; // stopped on purpose: no restart on return
    setRunning(false);
    const int listed = favRows_ + recentRows_;
    //: The search inside the documents was stopped before it finished.
    status_->setText(listed == 0 ? tr("Stopped") : tr("Stopped, %n file(s) found", nullptr, listed));
    status_->setVisible(true);
    updateSummary(listed);
}

void RecentFilesPanel::addContentHit(const QString &path, int page, const QString &snippet)
{
    if (!searching_ || hits_.contains(path))
        return;
    const RecentEntry *entry = entryFor(path);
    if (!entry)
        return; // left the history while the scan ran
    const ContentHit &hit = *hits_.insert(path, {page, snippet});
    hitOrder_.append(path);
    insertResult(makeHitItem(*entry, hit), listedAsFavourite(*entry));
}

void RecentFilesPanel::setContentProgress(int scanned, int total)
{
    if (!searching_)
        return;
    scanned_ = scanned;
    scanTotal_ = total;
    showScanProgress();
}

void RecentFilesPanel::endContentSearch(bool canceled, int matched)
{
    Q_UNUSED(matched); // the rows listed are the count: hits for files that left
                       // the history are not shown
    if (!searching_)
        return;
    searching_ = false;
    setRunning(false);
    if (canceled) {
        // Stopped from outside (the window left Recent): start again on return.
        status_->setVisible(false);
        rescanOnShow_ = contentMode();
        return;
    }
    scanDone_ = true;
    int listed = favRows_ + recentRows_;
    if (listed == 0)
        rebuild(); // shows the "no documents contain" note
    listed = favRows_ + recentRows_;
    if (listed == 0)
        status_->setVisible(false);
    else
        status_->setText(tr("%n file(s) found", nullptr, listed));
    updateSummary(listed);
}

void RecentFilesPanel::clearList()
{
    list_->clear();
    favCaption_ = nullptr;
    recentCaption_ = nullptr;
    emptyNote_ = nullptr;
    favRows_ = 0;
    recentRows_ = 0;
}

// Sections show captions only while Favourites has a row: with nothing starred
// the list is the plain history it always was.
void RecentFilesPanel::insertResult(QListWidgetItem *item, bool favourite)
{
    if (emptyNote_) {
        delete list_->takeItem(list_->row(emptyNote_));
        emptyNote_ = nullptr;
    }
    const auto caption = [](const QString &text) {
        auto *c = new QListWidgetItem(text);
        c->setData(kCaptionRole, true);
        c->setFlags(Qt::NoItemFlags);
        return c;
    };
    if (favourite) {
        if (!favCaption_) {
            //: Caption above the starred files in the Recent list.
            favCaption_ = caption(tr("Favourites"));
            list_->insertItem(0, favCaption_);
            if (recentRows_ > 0 && !recentCaption_) {
                //: Caption above the files that are not starred in the Recent list.
                recentCaption_ = caption(tr("Recent"));
                list_->insertItem(1, recentCaption_);
            }
        }
        list_->insertItem(list_->row(favCaption_) + 1 + favRows_, item);
        ++favRows_;
    } else {
        if (favRows_ > 0 && !recentCaption_) {
            //: Caption above the files that are not starred in the Recent list.
            recentCaption_ = caption(tr("Recent"));
            list_->addItem(recentCaption_);
        }
        list_->addItem(item);
        ++recentRows_;
    }
}

void RecentFilesPanel::showEmptyNote(const QString &text)
{
    emptyNote_ = new QListWidgetItem(text, list_);
    emptyNote_->setFlags(Qt::NoItemFlags);
    emptyNote_->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
}

QListWidgetItem *RecentFilesPanel::makeHitItem(const RecentEntry &entry, const ContentHit &hit)
{
    QListWidgetItem *item = makeFileItem(entry);
    item->setData(kSnippetRole, hit.snippet);
    const QString native = QDir::toNativeSeparators(entry.path);
    //: Tooltip of a file found by the content search. %1 is the file's path, %2
    //: the page with the first match.
    item->setToolTip(hit.page > 0 ? tr("%1\nMatch on page %2").arg(native, QString::number(hit.page))
                                  : native);
    return item;
}

QListWidgetItem *RecentFilesPanel::makeFileItem(const RecentEntry &entry)
{
    const QFileInfo fi(entry.path);
    const bool missing = !fi.exists();
    auto *item = new QListWidgetItem(fi.fileName());
    item->setData(kPathRole,      entry.path);
    item->setData(kMissingRole,   missing);
    item->setData(kEpochRole,     entry.lastOpened);
    item->setData(kPageCountRole, entry.pageCount);
    item->setData(kFavoriteRole,  entry.favorite);
    // While searching, record the term so the delegate highlights it.
    if (!needle().isEmpty())
        item->setData(kQueryRole, needle());
    const QString native = QDir::toNativeSeparators(entry.path);
    //: Tooltip of a recent file that was moved or deleted. %1 is its path.
    item->setToolTip(missing ? tr("%1\nThis file is no longer on disk.").arg(native) : native);
    return item;
}

// Every file for an empty search (starred ones all, the rest up to the visible
// count). With a search: the files whose name contains it (Names, and All
// first), then the content hits so far (Contents, and All after the names).
void RecentFilesPanel::rebuild()
{
    clearList();
    const QString q = needle();
    const bool filtering = !q.isEmpty();
    const bool contentsOnly = filtering && search_->scope() == Scope::Contents;

    if (!contentsOnly) {
        QList<const RecentEntry *> favourites;
        QList<const RecentEntry *> rest;
        for (const RecentEntry &e : std::as_const(entries_)) {
            if (filtering && !nameMatches(e))
                continue;
            (listedAsFavourite(e) ? favourites : rest).append(&e);
        }
        if (!filtering && rest.size() > visibleCount_)
            rest.resize(visibleCount_);
        for (const RecentEntry *e : std::as_const(favourites))
            insertResult(makeFileItem(*e), true);
        for (const RecentEntry *e : std::as_const(rest))
            insertResult(makeFileItem(*e), false);
    }
    nameRows_ = favRows_ + recentRows_;
    for (const QString &path : std::as_const(hitOrder_)) {
        if (const RecentEntry *e = entryFor(path))
            insertResult(makeHitItem(*e, hits_.value(path)), listedAsFavourite(*e));
    }

    const int listed = favRows_ + recentRows_;
    if (listed == 0) {
        if (!filtering)
            showEmptyNote(tr("No recent files yet"));
        else if (!contentMode())
            showEmptyNote(tr("No file names contain \"%1\"").arg(q));
        else if (scanDone_)
            showEmptyNote(contentsOnly ? tr("No documents contain \"%1\"").arg(q)
                                       : tr("No file names or documents contain \"%1\"").arg(q));
        // Otherwise a scan is pending or running, and the status says so.
    }
    updateSummary(listed);
}

// The window's status bar line for the list as it stands.
void RecentFilesPanel::updateSummary(int listed)
{
    if (!needle().isEmpty()) {
        statusSummary_ = tr("%n result(s)", nullptr, listed);
    } else if (favRows_ > 0) {
        //: Status bar summary of the Recent list. %1 is the number of starred
        //: files, %2 the number of other recent files. If the words must agree
        //: with the numbers, a form such as "Starred: %1, recent: %2" works.
        statusSummary_ = tr("%1 starred, %2 recent").arg(favRows_).arg(recentRows_);
    } else if (recentRows_ > 0) {
        //: Status bar summary of the Recent list.
        statusSummary_ = tr("Your last %n opened document(s)", nullptr, recentRows_);
    } else {
        statusSummary_ = tr("No recent files yet");
    }
    emit statusSummaryChanged(statusSummary_);
    emit countChanged(listed);
}

void RecentFilesPanel::onItemActivated(QListWidgetItem *item)
{
    if (!item) return;
    const QString path = item->data(kPathRole).toString();
    if (path.isEmpty()) return;
    if (item->data(kMissingRole).toBool())
        handleMissingFile(path);
    else
        emit openRequested(path);
}

void RecentFilesPanel::handleMissingFile(const QString &path)
{
    const QString native = QDir::toNativeSeparators(path);
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("File not found"));
    //: %1 is the file's path.
    box.setText(tr("\"%1\" could not be found.").arg(native));
    box.setInformativeText(tr("It may have been moved, renamed, or deleted."));
    //: Button that opens a file dialog to find where the missing file is now.
    QPushButton *locate = box.addButton(tr("Locate"), QMessageBox::AcceptRole);
    QPushButton *remove = box.addButton(tr("Remove from history"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(locate);
    box.exec();
    if (box.clickedButton() == locate) {
        const QFileInfo fi(path);
        const QString picked = QFileDialog::getOpenFileName(
            //: Title of the file dialog. %1 is the missing file's name.
            this, tr("Locate \"%1\"").arg(fi.fileName()), fi.absolutePath(),
            //: File type filter. Keep the patterns in parentheses and the ";;".
            tr("PDF documents (*.pdf);;All files (*)"));
        if (!picked.isEmpty()) { emit removeRequested(path); emit openRequested(picked); }
    } else if (box.clickedButton() == remove) {
        emit removeRequested(path);
    }
}

} // namespace mervin
