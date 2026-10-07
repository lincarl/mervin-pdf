#include "ui/CommentsSidebar.h"

#include "render/AnnotTypes.h"
#include "ui/ThemeTokens.h"

#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QStackedLayout>
#include <QVBoxLayout>

namespace mervin {

namespace {
constexpr int kPageRole = Qt::UserRole;
constexpr int kIdRole = Qt::UserRole + 1;

// The kind of an annotation, shown for one without comment text.
QString kindLabel(AnnotType t)
{
    switch (t) {
    //: Noun naming an annotation kind, highlighted text.
    case AnnotType::Highlight: return CommentsSidebar::tr("Highlight");
    //: Noun naming an annotation kind, underlined text.
    case AnnotType::Underline: return CommentsSidebar::tr("Underline");
    //: Noun naming an annotation kind, struck-out text.
    case AnnotType::StrikeOut: return CommentsSidebar::tr("Strike-out");
    //: Noun naming an annotation kind, a sticky note.
    case AnnotType::Text:      return CommentsSidebar::tr("Note");
    default:                   return CommentsSidebar::tr("Annotation");
    }
}

// A small filled square in the annotation's colour, as the row icon.
QIcon colorChip(const QColor &c)
{
    QPixmap pm(12, 12);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(c.isValid() ? c : annot::defaultColor());
    p.setPen(theme::doc().chipBorder);
    p.drawRoundedRect(0, 0, 11, 11, 2, 2);
    return QIcon(pm);
}
} // namespace

CommentsSidebar::CommentsSidebar(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    stack_ = new QStackedLayout;
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("commentsList"));
    list_->setWordWrap(true);
    list_->setUniformItemSizes(false);
    // Single-click navigation. (itemClicked alone - wiring itemActivated too would
    // double-fire on a double-click / single-click-activation styles.)
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        if (it)
            emit annotationActivated(it->data(kPageRole).toInt(), it->data(kIdRole).toInt());
    });
    stack_->addWidget(list_);

    //: "Comment" names the toolbar tool for highlights and sticky notes, and
    //: "Highlight" a style in it. Use their translations here.
    empty_ = new QLabel(tr("No annotations yet.\n\nUse the Highlight or Comment tool to add some."),
                        this);
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    empty_->setEnabled(false);
    stack_->addWidget(empty_);

    layout->addLayout(stack_);
    stack_->setCurrentWidget(empty_);
}

void CommentsSidebar::setAnnotations(const std::vector<Annotation> &annots)
{
    list_->clear();
    for (const Annotation &a : annots) {
        QString text = a.contents.simplified();
        if (text.isEmpty())
            text = QStringLiteral("[%1]").arg(kindLabel(a.type));
        const QString page = QString::number(a.page + 1);
        QString label;
        if (a.author.isEmpty()) {
            //: A row in the comments list. "p." is short for page. %1 is the page
            //: number, %2 the comment text.
            label = tr("p.%1  %2").arg(page, text);
        } else {
            //: A row in the comments list. "p." is short for page. %1 is the page
            //: number, %2 the comment's author, %3 the comment text.
            label = tr("p.%1  %2 · %3").arg(page, a.author, text);
        }
        auto *it = new QListWidgetItem(colorChip(a.color), label);
        it->setData(kPageRole, a.page);
        it->setData(kIdRole, a.id);
        it->setToolTip(a.contents.isEmpty() ? kindLabel(a.type) : a.contents);
        list_->addItem(it);
    }
    stack_->setCurrentWidget(annots.empty() ? static_cast<QWidget *>(empty_) : list_);
}

} // namespace mervin
