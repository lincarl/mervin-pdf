#include "dialogs/RowList.h"

#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDrag>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QShortcut>
#include <QTimer>
#include <QToolButton>

namespace mervin {

namespace {

// Column geometry, shared by the header strip and every row so they line up.
// Sized with headroom for a UI font wider than Segoe UI 9pt: at 125% Windows
// text scaling the captions still fit, so nothing clips silently - QLabel
// hard-clips, it does not elide.
constexpr int kColGrip = 18;
constexpr int kColNum = 30;
constexpr int kColX = 22;
constexpr int kRowHeight = 30;
constexpr int kSpacing = 8;

// Rows carry their index in this, so a drop knows which row was picked up.
const char *const kRowMime = "application/x-mervin-row";
// Set on each row widget; its children find their row through their parent.
const char *const kRowProperty = "rowListRow";

QLabel *fixedLabel(QWidget *parent, int width, Qt::Alignment align, const char *name = nullptr)
{
    auto *l = new QLabel(parent);
    l->setFixedWidth(width);
    l->setAlignment(align | Qt::AlignVCenter);
    if (name)
        l->setObjectName(QLatin1String(name));
    return l;
}

// The dialog's own columns sit in one nested layout, spaced like the rest.
QHBoxLayout *columnsLayout()
{
    auto *h = new QHBoxLayout;
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(kSpacing);
    return h;
}

} // namespace

RowList::RowList(int countWidth, int outputWidth, QWidget *parent)
    : QListWidget(parent)
    , countWidth_(countWidth)
    , outputWidth_(outputWidth)
{
    setSelectionMode(QAbstractItemView::SingleSelection);
    setUniformItemSizes(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setAcceptDrops(true);
    // The rows are item widgets covering the viewport, so a drag over the list
    // is over a row, not the viewport; Qt walks up to the first ancestor that
    // accepts drops, which has to be the viewport. Setting it on the view alone
    // does not reach it.
    viewport()->setAcceptDrops(true);
    // Filter the viewport directly: QAbstractScrollArea does not forward these drag/drop events
    // to the overrides. Resize/show events also refresh header insets.
    viewport()->installEventFilter(this);
    setDropIndicatorShown(false); // we paint our own; see paintEvent

    // Reorder from the keyboard, scoped to the list so the shortcuts do not fight
    // the row editors for arrow keys. Ctrl+Delete (not plain Delete) frees Delete
    // for the QLineEdit the user is typing in.
    const auto shortcut = [this](const QKeySequence &keys, const std::function<void()> &run) {
        auto *s = new QShortcut(keys, this);
        s->setContext(Qt::WidgetWithChildrenShortcut);
        connect(s, &QShortcut::activated, this, run);
    };
    shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Up), [this] {
        if (onMove)
            onMove(-1);
    });
    shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Down), [this] {
        if (onMove)
            onMove(1);
    });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Delete), [this] {
        if (onRemove)
            onRemove();
    });
}

QWidget *RowList::makeHeader(const Fill &fill)
{
    // A plain widget rather than a QTableWidget header: nothing in the app uses
    // an item *view* with a header, so QHeaderView is entirely unstyled by
    // Theme::buildStyleSheet and a native header strip would sit on the slate
    // dialog surface looking imported.
    auto *header = new QWidget(parentWidget());
    header->setObjectName(QStringLiteral("rowListHeader"));
    headerRow_ = new QHBoxLayout(header);
    headerRow_->setContentsMargins(6, 0, 6, 0); // real insets set by syncHeaderInsets()
    headerRow_->setSpacing(kSpacing);
    headerRow_->addSpacing(kColGrip); // over the drag grips
    auto *num = fixedLabel(header, kColNum, Qt::AlignRight);
    num->setText(QStringLiteral("#"));
    headerRow_->addWidget(num);
    QHBoxLayout *columns = columnsLayout();
    fill(header, columns);
    headerRow_->addLayout(columns, 1);
    auto *count = fixedLabel(header, countWidth_, Qt::AlignRight);
    count->setText(tr("Count"));
    headerRow_->addWidget(count);
    auto *output = fixedLabel(header, outputWidth_, Qt::AlignRight);
    output->setText(tr("Output"));
    headerRow_->addWidget(output);
    headerRow_->addSpacing(kColX + kSpacing);
    return header;
}

void RowList::addRow(const Fill &fill)
{
    const int i = count();
    auto *row = new QWidget(this);
    row->setProperty(kRowProperty, i);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(6, 1, 6, 1);
    h->setSpacing(kSpacing);

    // The grip. Rows are item widgets, so a press anywhere else in the row lands
    // on a child widget and the list never sees it: the drag has to start from
    // something that exists for exactly that purpose.
    auto *grip = new QLabel(row);
    grip->setObjectName(QStringLiteral("rowListGrip"));
    grip->setFixedWidth(kColGrip);
    grip->setAlignment(Qt::AlignCenter);
    grip->setPixmap(icons::glyphPixmap(icons::Glyph::DragHandle, Theme::iconInk(palette()), 16));
    grip->setCursor(Qt::OpenHandCursor);
    grip->setToolTip(tr("Drag to reorder"));
    grip->installEventFilter(this);
    h->addWidget(grip);

    auto *num = fixedLabel(row, kColNum, Qt::AlignRight, "rowListNum");
    num->setText(QString::number(i + 1));
    h->addWidget(num);

    QHBoxLayout *columns = columnsLayout();
    fill(row, columns);
    h->addLayout(columns, 1);
    for (QWidget *w : row->findChildren<QWidget *>())
        if (w->focusPolicy() & Qt::TabFocus)
            w->installEventFilter(this); // FocusIn makes the row current

    h->addWidget(fixedLabel(row, countWidth_, Qt::AlignRight, "rowListCount"));
    h->addWidget(fixedLabel(row, outputWidth_, Qt::AlignRight, "rowListOutput"));

    auto *x = new QToolButton(row);
    x->setObjectName(QStringLiteral("rowListX"));
    icons::setButtonGlyph(x, icons::Glyph::Close, 16);
    x->setAutoRaise(true);
    x->setFixedSize(kColX, kColX);
    x->setFocusPolicy(Qt::NoFocus); // 20 rows must not add 20 extra tab stops
    x->setToolTip(tr("Remove this row"));
    // Select, then remove on the next event-loop turn. Rebuilding here would
    // delete this very button from inside its own clicked() emission; going
    // through the current row also keeps the right row if anything else moved
    // the list in between.
    connect(x, &QToolButton::clicked, this, [this, i] {
        setCurrentRow(i);
        QTimer::singleShot(0, this, [this] {
            if (onRemove)
                onRemove();
        });
    });
    h->addWidget(x);

    auto *item = new QListWidgetItem(this);
    item->setSizeHint(QSize(0, kRowHeight));
    setItemWidget(item, row);
}

void RowList::setRowTexts(int row, const QString &count, const QString &output)
{
    QListWidgetItem *it = item(row);
    QWidget *w = it ? itemWidget(it) : nullptr;
    if (!w)
        return;
    if (auto *c = w->findChild<QLabel *>(QStringLiteral("rowListCount")))
        c->setText(count);
    if (auto *o = w->findChild<QLabel *>(QStringLiteral("rowListOutput")))
        o->setText(output);
}

void RowList::syncHeaderInsets()
{
    if (!headerRow_)
        return;
    // Left: the list frame plus its QSS padding, i.e. wherever the viewport
    // actually starts. Right: the same, plus the vertical scrollbar when it is
    // showing, which is what used to shove the Count and Output captions 12px off
    // their columns as soon as the list got long enough to scroll.
    const int left = viewport()->x();
    const int right = width() - (viewport()->x() + viewport()->width());
    headerRow_->setContentsMargins(6 + left, 0, 6 + qMax(0, right), 0);
}

bool RowList::eventFilter(QObject *o, QEvent *e)
{
    if (o == viewport()) {
        switch (e->type()) {
        case QEvent::Resize:
            syncHeaderInsets();
            break;
        case QEvent::Show:
            // While the dialog is being shown the vertical scrollbar can hide
            // after the viewport's last Resize, widening it with no Resize of its
            // own. The header and the row widgets would stay 12 px short of the
            // columns until the next resize, so both are re-fitted here.
            syncHeaderInsets();
            updateGeometries();
            break;
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
        return QListWidget::eventFilter(o, e);
    }

    auto *w = qobject_cast<QWidget *>(o);
    const QVariant row = w && w->parentWidget() ? w->parentWidget()->property(kRowProperty)
                                                : QVariant();
    if (!row.isValid())
        return QListWidget::eventFilter(o, e);
    if (e->type() == QEvent::FocusIn)
        setCurrentRow(row.toInt());
    if (w->objectName() != QLatin1String("rowListGrip"))
        return QListWidget::eventFilter(o, e);

    // Drag a row by its grip. Arming on press and only starting once the pointer
    // has travelled the platform's drag distance keeps a plain click on the grip
    // from turning into a drag: it just selects the row.
    switch (e->type()) {
    case QEvent::MouseButtonPress: {
        auto *me = static_cast<QMouseEvent *>(e);
        if (me->button() == Qt::LeftButton) {
            dragRow_ = row.toInt();
            dragOrigin_ = me->globalPosition().toPoint();
            setCurrentRow(dragRow_);
        }
        return true;
    }
    case QEvent::MouseButtonRelease:
        dragRow_ = -1;
        return true;
    case QEvent::MouseMove: {
        if (dragRow_ < 0)
            break;
        auto *me = static_cast<QMouseEvent *>(e);
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
    default:
        break;
    }
    return QListWidget::eventFilter(o, e);
}

void RowList::startRowDrag(int row)
{
    QListWidgetItem *it = item(row);
    QWidget *rowWidget = it ? itemWidget(it) : nullptr;
    if (!rowWidget)
        return;

    auto *mime = new QMimeData;
    mime->setData(QLatin1String(kRowMime), QByteArray::number(row));

    QDrag drag(this);
    drag.setMimeData(mime);
    // The row itself, dimmed, rides with the cursor, so what is being moved is
    // never in doubt in a list where several rows can look alike.
    const QPixmap shot = rowWidget->grab();
    QPixmap ghost(shot.size());
    ghost.fill(Qt::transparent);
    {
        QPainter p(&ghost);
        p.setOpacity(0.75);
        p.drawPixmap(0, 0, shot);
    }
    drag.setPixmap(ghost);
    drag.setHotSpot(QPoint(kColGrip / 2, ghost.height() / 2));
    drag.exec(Qt::MoveAction);
}

void RowList::handleDrop(QDropEvent *e)
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

void RowList::paintEvent(QPaintEvent *e)
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

void RowList::takeIfOurs(QDragMoveEvent *e)
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
// last. Anywhere past the final row counts as the end, so the empty space under a
// short list is a valid target.
int RowList::gapAt(const QPoint &pos) const
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

int RowList::gapY(int gap) const
{
    if (count() == 0)
        return 0;
    if (gap >= count())
        return qMin(visualItemRect(item(count() - 1)).bottom(), viewport()->height() - 1);
    return qMax(visualItemRect(item(gap)).top(), 1);
}

} // namespace mervin
