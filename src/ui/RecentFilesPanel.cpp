#include "ui/RecentFilesPanel.h"

#include "ui/FileContextMenu.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/ThemeTokens.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace mervin {

namespace {

constexpr int kPathRole      = Qt::UserRole;
constexpr int kMissingRole   = Qt::UserRole + 1;
constexpr int kEpochRole     = Qt::UserRole + 2; // qint64 lastOpened
constexpr int kPageCountRole = Qt::UserRole + 3; // int pageCount (0 = unknown)
constexpr int kFavoriteRole  = Qt::UserRole + 4; // bool favorite
constexpr int kQueryRole     = Qt::UserRole + 5; // QString search term to highlight
constexpr int kSnippetRole   = Qt::UserRole + 6; // QString content preview (content search)

constexpr int kContentDebounceMs = 400;
constexpr int kRowHeight = 64;
constexpr int kContentRowHeight = 88; // taller: fits name + snippet + path
constexpr int kIconSize  = 34;
constexpr int kMetaW     = 180; // right-column fixed width
constexpr int kHPad      = 16;  // left/right outer margin
constexpr int kVPad      =  9;  // top/bottom inner padding
constexpr int kStarW     = 20;  // star icon hit area
constexpr int kStarGap   =  8;  // gap between right meta column and star

constexpr double kPi = 3.141592653589793;

// Search-match highlight - the same yellow the in-page find bar paints over the
// page, so a match looks consistent wherever it appears.
const QColor &kHighlightFill = theme::brand().searchMatch;

QString formatDate(qint64 epochMs)
{
    if (epochMs <= 0)
        return QString();
    const QDate date = QDateTime::fromMSecsSinceEpoch(epochMs).date();
    const int daysAgo = date.daysTo(QDate::currentDate());
    QString rel;
    if (daysAgo == 0)      rel = QObject::tr("Today");
    else if (daysAgo == 1) rel = QObject::tr("Yesterday");
    else                   rel = QObject::tr("%1 days ago").arg(daysAgo);
    return rel + QStringLiteral(" · ") + QLocale().toString(date, QLocale::ShortFormat);
}

QString formatSize(qint64 bytes)
{
    if (bytes <= 0)          return QString();
    if (bytes < 1024)        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024*1024)   return QStringLiteral("%1 KB").arg(bytes / 1024);
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
}

// Blue gradient rounded-square with a white page glyph.
QPixmap makeDocIcon(int size)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient g(0, 0, size, size);
    g.setColorAt(0.0, theme::brand().gradientStart);
    g.setColorAt(1.0, theme::brand().gradientEnd);
    p.setBrush(g);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(pm.rect(), size / 6, size / 6);

    const int m = size / 5, fold = size / 5;
    p.setPen(QPen(theme::brand().onBrand, qMax(1.0, size * 0.06), Qt::SolidLine, Qt::RoundCap,
                  Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    QPolygonF doc;
    doc << QPointF(m, m) << QPointF(size-m-fold, m) << QPointF(size-m, m+fold)
        << QPointF(size-m, size-m) << QPointF(m, size-m) << QPointF(m, m);
    p.drawPolyline(doc);
    p.drawLine(QPointF(size-m-fold, m),      QPointF(size-m-fold, m+fold));
    p.drawLine(QPointF(size-m-fold, m+fold), QPointF(size-m, m+fold));

    p.setPen(QPen(theme::brand().onBrandSoft, qMax(1.0, size*0.05)));
    int ly = m + fold + (size - 2*m - fold) * 2 / 5;
    p.drawLine(m+3, ly, size-m-3, ly);
    ly += (size - 2*m - fold) / 5;
    p.drawLine(m+3, ly, size-m-7, ly);
    return pm;
}

QRect starRegion(const QRect &row)
{
    return QRect(row.right() - kHPad - kStarW + 1,
                 row.top() + (row.height() - kStarW) / 2,
                 kStarW, kStarW);
}

void drawStar(QPainter *p, const QRect &rect, bool filled)
{
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QPointF center(rect.center().x(), rect.center().y());
    const qreal r1 = rect.width() / 2.0 - 1.0;
    const qreal r2 = r1 * 0.4;
    QPolygonF star;
    for (int i = 0; i < 10; ++i) {
        const qreal angle = -kPi / 2.0 + i * kPi / 5.0;
        const qreal r = (i % 2 == 0) ? r1 : r2;
        star << center + QPointF(r * std::cos(angle), r * std::sin(angle));
    }
    if (filled) {
        p->setBrush(theme::brand().starFill);
        p->setPen(QPen(theme::brand().starEdge, 1.0));
    } else {
        p->setBrush(Qt::NoBrush);
        p->setPen(QPen(theme::brand().starEmptyEdge, 1.5));
    }
    p->drawPolygon(star);
    p->restore();
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
        QStringList meta;
        if (pages > 0)        meta << (pages == 1 ? QObject::tr("1 page")
                                                   : QObject::tr("%1 pages").arg(pages));
        if (!size.isEmpty())  meta << size;
        const QString metaLine = meta.join(QStringLiteral(" · "));

        const QRect r = opt.rect;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setClipRect(r);

        // ── Icon ──────────────────────────────────────────────────────────
        const QRect iconR(r.left() + kHPad,
                          r.top()  + (r.height() - kIconSize) / 2,
                          kIconSize, kIconSize);
        p->drawPixmap(iconR, makeDocIcon(kIconSize));

        // ── Star (right edge, only for regular entries) ───────────────────
        if (hasEntry) {
            drawStar(p, starRegion(r), isFav);
        }

        // ── Right meta column ─────────────────────────────────────────────
        const int starOff = hasEntry ? kStarW + kStarGap : 0;
        const int rightX  = r.right() - kHPad - starOff - kMetaW;
        const int midY    = r.top() + r.height() / 2;
        const QRect dateR(rightX, r.top() + kVPad,  kMetaW, midY - r.top() - kVPad);
        const QRect sizeR(rightX, midY,              kMetaW, r.bottom() - midY - kVPad);

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

        QFont smallF = opt.font;
        smallF.setPointSizeF(smallF.pointSizeF() * 0.85);

        if (hasSnippet) {
            p->setFont(smallF);
            drawHighlighted(p, snippetR, Qt::ElideRight, snippet, query, textCol);
        }

        p->setFont(smallF);
        drawHighlighted(p, pathR, Qt::ElideLeft, pDisp, QString(), subCol);

        p->setFont(opt.font);
        p->setPen(textCol);
        p->drawText(dateR, Qt::AlignRight | Qt::AlignVCenter | Qt::TextSingleLine, date);

        p->setFont(smallF);
        p->setPen(subCol);
        p->drawText(sizeR, Qt::AlignRight | Qt::AlignVCenter | Qt::TextSingleLine, metaLine);

        p->restore();
    }
};

} // namespace

RecentFilesPanel::RecentFilesPanel(QWidget *parent)
    : QWidget(parent)
{
    filter_ = new QLineEdit(this);
    filter_->hide();
    connect(filter_, &QLineEdit::textChanged, this, &RecentFilesPanel::onFilterOrModeChanged);

    contentCheck_ = new QCheckBox(this);
    contentCheck_->hide();
    connect(contentCheck_, &QCheckBox::toggled, this, [this](bool) { onFilterOrModeChanged(); });

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(kContentDebounceMs);
    connect(debounce_, &QTimer::timeout, this, &RecentFilesPanel::startContentSearch);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 12);
    layout->setSpacing(8);

    // ── Heading ───────────────────────────────────────────────────────────
    auto *heading = new QLabel(tr("Recent files"), this);
    heading->setObjectName(QStringLiteral("recentHeading"));
    QFont hf = heading->font();
    hf.setPointSizeF(hf.pointSizeF() * 1.4);
    hf.setBold(true);
    heading->setFont(hf);
    layout->addWidget(heading);

    // ── View-mode toggle (Recent | Favorites), below the heading ───────────
    // Styled via QWidget#viewModeBar CSS. Wrapped in a row with a trailing
    // stretch so the segmented control hugs the left edge instead of stretching.
    auto *viewModeBar = new QWidget(this);
    viewModeBar->setObjectName(QStringLiteral("viewModeBar"));
    auto *modeLayout = new QHBoxLayout(viewModeBar);
    modeLayout->setContentsMargins(0, 0, 0, 0);
    modeLayout->setSpacing(0);

    modeRecentBtn_ = new QToolButton(viewModeBar);
    modeRecentBtn_->setText(tr("All"));
    modeRecentBtn_->setCheckable(true);
    modeRecentBtn_->setChecked(true);
    modeRecentBtn_->setAutoRaise(false);

    modeFavoritesBtn_ = new QToolButton(viewModeBar);
    modeFavoritesBtn_->setText(tr("Favourites"));
    modeFavoritesBtn_->setCheckable(true);
    modeFavoritesBtn_->setAutoRaise(false);

    // Drives the segmented control's outer corner rounding (see mervin::Theme).
    modeRecentBtn_->setProperty("segpos", "first");
    modeFavoritesBtn_->setProperty("segpos", "last");

    auto *modeGroup = new QButtonGroup(this);
    modeGroup->setExclusive(true);
    modeGroup->addButton(modeRecentBtn_);
    modeGroup->addButton(modeFavoritesBtn_);

    modeLayout->addWidget(modeRecentBtn_);
    modeLayout->addWidget(modeFavoritesBtn_);

    auto *modeRow = new QHBoxLayout;
    modeRow->setContentsMargins(0, 0, 0, 4);
    modeRow->setSpacing(0);
    modeRow->addWidget(viewModeBar);
    modeRow->addStretch();
    layout->addLayout(modeRow);

    // Route through onFilterOrModeChanged() (not rebuild() directly) so that
    // switching the scope while a content search is active re-runs that search
    // with the new scope, rather than dropping back to the name-filtered list.
    connect(modeRecentBtn_, &QToolButton::toggled, this, [this](bool checked) {
        if (checked) { mode_ = Mode::Recent;     onFilterOrModeChanged(); }
    });
    connect(modeFavoritesBtn_, &QToolButton::toggled, this, [this](bool checked) {
        if (checked) { mode_ = Mode::Favorites;  onFilterOrModeChanged(); }
    });

    // ── Status label (content search progress) ────────────────────────────
    status_ = new QLabel(this);
    status_->setVisible(false);
    layout->addWidget(status_);

    // ── List ──────────────────────────────────────────────────────────────
    list_ = new QListWidget(this);
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
    if (obj == list_->viewport() && event->type() == QEvent::MouseButtonPress) {
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
                QList<FileMenuItem> leadingItems{
                    {tr("Open in new window"),
                     [this, path] { emit openInNewWindowRequested(path); },
                     !missing,
                     icons::glyph(icons::Glyph::OpenInNewWindow, ink)}
                };
                if (regularEntry) {
                    leadingItems.append({tr("Clear missing files"),
                                         [this, missingPaths] {
                                             emit clearMissingRequested(missingPaths);
                                         },
                                         !missingPaths.isEmpty(),
                                         icons::glyph(icons::Glyph::Broom, ink)});
                }
                showFileContextMenu(this, path, ce->globalPos(),
                                    leadingItems);
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void RecentFilesPanel::toggleItemFavorite(QListWidgetItem *item)
{
    const QString path  = item->data(kPathRole).toString();
    const bool newFav   = !item->data(kFavoriteRole).toBool();

    // Keep in-memory entries_ consistent so rebuild() doesn't undo the change
    // before the IPC round-trip completes.
    for (RecentEntry &e : entries_) {
        if (e.path == path) { e.favorite = newFav; break; }
    }

    item->setData(kFavoriteRole, newFav);
    list_->update(list_->indexFromItem(item));

    // In favorites mode, removing the star should hide the entry immediately.
    if (mode_ == Mode::Favorites && !newFav) {
        delete list_->takeItem(list_->row(item));
        if (list_->count() == 0) {
            auto *empty = new QListWidgetItem(tr("No favourites yet"), list_);
            empty->setFlags(Qt::NoItemFlags);
            empty->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
        }
        emit countChanged(list_->count());
    }

    emit favoriteToggled(path, newFav);
}

bool RecentFilesPanel::matchesCurrentFilter(const RecentEntry &entry) const
{
    if (mode_ == Mode::Favorites && !entry.favorite)
        return false;

    const QString needle = filter_->text().trimmed();
    if (needle.isEmpty())
        return true;

    if (contentCheck_->isChecked())
        return false;

    return QFileInfo(entry.path).fileName().contains(needle, Qt::CaseInsensitive);
}

QStringList RecentFilesPanel::missingFilesInCurrentFilter() const
{
    QStringList paths;
    for (const RecentEntry &e : entries_) {
        if (!matchesCurrentFilter(e))
            continue;
        if (!QFileInfo::exists(e.path))
            paths.append(e.path);
    }
    return paths;
}

void RecentFilesPanel::setEntries(const QList<RecentEntry> &entries)
{
    entries_ = entries;
    if (!contentMode())
        rebuild();
}

void RecentFilesPanel::setVisibleCount(int count)
{
    visibleCount_ = count > 0 ? count : 100;
    if (!contentMode())
        rebuild();
}

void RecentFilesPanel::setSearch(const QString &text, bool contentSearch)
{
    { QSignalBlocker b(filter_);       filter_->setText(text); }
    { QSignalBlocker b(contentCheck_); contentCheck_->setChecked(contentSearch); }
    onFilterOrModeChanged();
}

bool RecentFilesPanel::contentMode() const
{
    return contentCheck_->isChecked() && !filter_->text().trimmed().isEmpty();
}

void RecentFilesPanel::onFilterOrModeChanged()
{
    if (contentMode()) {
        status_->setText(tr("Type to search file contents…"));
        status_->setVisible(true);
        debounce_->start();
        return;
    }
    debounce_->stop();
    if (searching_) {
        searching_ = false;
        emit contentSearchCanceled();
    }
    status_->setVisible(false);
    rebuild();
}

void RecentFilesPanel::startContentSearch()
{
    if (!contentMode()) return;
    list_->clear();
    searching_ = true;
    status_->setText(tr("Searching file contents…"));
    status_->setVisible(true);
    emit contentSearchRequested(filter_->text().trimmed(), mode_ == Mode::Favorites);
}

void RecentFilesPanel::addContentHit(const QString &path, int page, const QString &snippet)
{
    if (!searching_) return;
    auto *item = new QListWidgetItem(QFileInfo(path).fileName(), list_);
    item->setData(kPathRole,    path);
    item->setData(kMissingRole, false);
    item->setData(kSnippetRole, snippet);
    item->setData(kQueryRole,   filter_->text().trimmed());
    item->setToolTip(QDir::toNativeSeparators(path)
                     + (page > 0 ? tr("\nMatch on page %1").arg(page) : QString()));
}

void RecentFilesPanel::setContentProgress(int scanned, int total)
{
    if (!searching_) return;
    status_->setText(tr("Searching file contents… %1 / %2").arg(scanned).arg(total));
}

void RecentFilesPanel::endContentSearch(bool canceled, int matched)
{
    if (!searching_) return;
    searching_ = false;
    if (canceled) { status_->setVisible(false); return; }
    status_->setText(matched == 0
                         ? tr("No files contain \"%1\"").arg(filter_->text().trimmed())
                         : (matched == 1 ? tr("1 file found")
                                         : tr("%1 files found").arg(matched)));
    // Keep the window status bar in step with the in-panel result count.
    statusSummary_ = (matched == 1) ? tr("1 result") : tr("%1 results").arg(matched);
    emit statusSummaryChanged(statusSummary_);
}

void RecentFilesPanel::rebuild()
{
    list_->clear();
    const QString needle   = filter_->text().trimmed();
    const bool filtering   = !needle.isEmpty();
    const bool favMode     = (mode_ == Mode::Favorites);
    int shown = 0;

    // In Favorites mode, present entries sorted by name; otherwise keep the
    // last-opened order supplied by the store.
    QList<RecentEntry> ordered = entries_;
    if (favMode) {
        std::sort(ordered.begin(), ordered.end(),
                  [](const RecentEntry &a, const RecentEntry &b) {
                      return QFileInfo(a.path).fileName().compare(
                                 QFileInfo(b.path).fileName(), Qt::CaseInsensitive) < 0;
                  });
    }

    for (const RecentEntry &e : std::as_const(ordered)) {
        if (favMode && !e.favorite)
            continue;
        const QFileInfo fi(e.path);
        const QString name = fi.fileName();
        if (filtering) {
            if (!name.contains(needle, Qt::CaseInsensitive)) continue;
        } else if (!favMode && shown >= visibleCount_) {
            break;
        }
        ++shown;

        const bool missing = !fi.exists();
        auto *item = new QListWidgetItem(list_);
        item->setData(kPathRole,      e.path);
        item->setData(kMissingRole,   missing);
        item->setData(kEpochRole,     e.lastOpened);
        item->setData(kPageCountRole, e.pageCount);
        item->setData(kFavoriteRole,  e.favorite);
        // When filtering by name, record the term so the delegate highlights
        // the matching span in the file name.
        if (filtering)
            item->setData(kQueryRole, needle);
        item->setToolTip(QDir::toNativeSeparators(e.path)
                         + (missing ? tr("\nThis file is no longer on disk.") : QString()));
    }

    if (list_->count() == 0) {
        const QString msg = favMode        ? tr("No favourites yet")
                          : filtering      ? tr("No recent files match the filter")
                                           : tr("No recent files yet");
        auto *empty = new QListWidgetItem(msg, list_);
        empty->setFlags(Qt::NoItemFlags);
        empty->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
    }

    if (filtering)
        statusSummary_ = (shown == 1) ? tr("1 result") : tr("%1 results").arg(shown);
    else if (favMode)
        statusSummary_ = tr("Your starred documents");
    else
        statusSummary_ = tr("Your last %1 opened documents").arg(shown);
    emit statusSummaryChanged(statusSummary_);

    emit countChanged(shown);
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
    box.setText(tr("\"%1\" could not be found.").arg(native));
    box.setInformativeText(tr("It may have been moved, renamed, or deleted."));
    QPushButton *locate = box.addButton(tr("Locate"), QMessageBox::AcceptRole);
    QPushButton *remove = box.addButton(tr("Remove from history"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(locate);
    box.exec();
    if (box.clickedButton() == locate) {
        const QFileInfo fi(path);
        const QString picked = QFileDialog::getOpenFileName(
            this, tr("Locate \"%1\"").arg(fi.fileName()), fi.absolutePath(),
            tr("PDF documents (*.pdf);;All files (*)"));
        if (!picked.isEmpty()) { emit removeRequested(path); emit openRequested(picked); }
    } else if (box.clickedButton() == remove) {
        emit removeRequested(path);
    }
}

} // namespace mervin
