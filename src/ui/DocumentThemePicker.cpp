#include "ui/DocumentThemePicker.h"

#include "render/ComfortTransform.h"
#include "ui/ThemeCard.h"
#include "ui/ThemeTokens.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QPainter>
#include <QRadioButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>

namespace mervin {
namespace {

enum class PreviewTheme { Traditional, Comfort, Inverted };

const std::array<QString, 3> kThemeValues = {
    QStringLiteral("light"), QStringLiteral("comfort"), QStringLiteral("dark")};

QColor sampleColor(PreviewTheme preview, const QColor &color)
{
    if (preview == PreviewTheme::Inverted)
        return QColor(255 - color.red(), 255 - color.green(), 255 - color.blue());
    return color;
}

// The same sample page in each theme: a heading, text lines and a landscape photo.
void paintPage(QPainter &painter, const QRectF &well, PreviewTheme preview)
{
    const auto &documentColors = theme::doc();
    const QColor ink = preview == PreviewTheme::Comfort
        ? QColor(comfort::kRampFg[0], comfort::kRampFg[1], comfort::kRampFg[2])
        : sampleColor(preview, QColor(0x45, 0x52, 0x60));
    const QColor paper = preview == PreviewTheme::Traditional
        ? documentColors.paperNormal
        : preview == PreviewTheme::Comfort ? documentColors.paperComfort
                                         : documentColors.paperInverted;

    const qreal scale = std::min({1.0, (well.width() - 16) / 72.0,
                                 (well.height() - 18) / 101.0});
    painter.translate(well.center() - QPointF(36 * scale, 50.5 * scale));
    painter.scale(scale, scale);
    painter.setPen(QPen(preview == PreviewTheme::Comfort ? QColor(0x39, 0x41, 0x4c)
                                                         : documentColors.pageBorder,
                        0.7));
    painter.setBrush(paper);
    painter.drawRect(QRectF(0, 0, 72, 101));

    painter.fillRect(QRectF(9, 10, 34, 4), ink);
    QColor lineInk = ink;
    lineInk.setAlphaF(0.72);
    for (const QRectF &line : {QRectF(9, 20, 54, 2), QRectF(9, 26, 35, 2),
                              QRectF(9, 76, 54, 2), QRectF(9, 82, 44, 2)})
        painter.fillRect(line, lineInk);

    // The sample photo keeps its authored colours in Comfort, while Inverted
    // reverses them along with the rest of the page.
    painter.setClipRect(QRectF(9, 35, 54, 32));
    painter.fillRect(QRectF(9, 35, 54, 32), sampleColor(preview, QColor(0x8c, 0xc5, 0xdf)));
    painter.setPen(Qt::NoPen);
    painter.setBrush(sampleColor(preview, QColor(0xff, 0xdc, 0x91)));
    painter.drawEllipse(QRectF(48, 40, 7, 7));
    painter.setBrush(sampleColor(preview, QColor(0x67, 0x97, 0x8f)));
    painter.drawPolygon(QPolygonF{QPointF(24, 67), QPointF(49, 44), QPointF(74, 67)});
    painter.setBrush(sampleColor(preview, QColor(0x39, 0x71, 0x6a)));
    painter.drawPolygon(QPolygonF{QPointF(0, 67), QPointF(26, 46), QPointF(52, 67)});
}

} // namespace

DocumentThemePicker::DocumentThemePicker(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("documentThemePicker"));
    setAccessibleName(tr("Document theme"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    auto *row = new QHBoxLayout;
    row->setSpacing(10);
    layout->addLayout(row);
    choices_ = new QButtonGroup(this);
    choices_->setExclusive(true);

    const std::array<QString, 3> labels = {
        //: Document theme name: pages as authored, on white paper.
        tr("Traditional"),
        //: Document theme name: dark grey pages that are easy on the eyes, with readable photos.
        tr("Comfort"),
        //: Document theme name: black pages with all colours inverted.
        tr("Inverted")};
    const std::array<QString, 3> descriptions = {
        tr("White paper, dark text and pictures in their original colours"),
        tr("Dark grey paper, light text and readable photos"),
        tr("Black paper, light text and inverted picture colours")};
    const std::array<QString, 3> names = {QStringLiteral("documentThemeLight"),
                                        QStringLiteral("documentThemeComfort"),
                                        QStringLiteral("documentThemeDark")};
    for (int i = 0; i < 3; ++i) {
        const auto preview = static_cast<PreviewTheme>(i);
        auto *choice = new ThemeCard(
            labels[i],
            [preview](QPainter &painter, const QRectF &well) { paintPage(painter, well, preview); },
            this);
        choice->setObjectName(names[i]);
        choice->setAccessibleDescription(descriptions[i]);
        choices_->addButton(choice, i);
        row->addWidget(choice, 1);
    }

    //: Document theme choice: dark pages with a dark UI theme, light pages with a light one.
    followUi_ = new QRadioButton(tr("Follow UI theme"), this);
    followUi_->setObjectName(QStringLiteral("documentThemeFollowUi"));
    choices_->addButton(followUi_, 3);
    layout->addWidget(followUi_);
    setTheme(QStringLiteral("light"));
}

void DocumentThemePicker::setTheme(const QString &value)
{
    for (int i = 0; i < static_cast<int>(kThemeValues.size()); ++i) {
        if (value == kThemeValues[i]) {
            choices_->button(i)->setChecked(true);
            followUi_->hide();
            return;
        }
    }

    // Preserve hand-edited values, including follow-ui, on an unchanged OK.
    followUiTheme_ = value;
    followUi_->show();
    followUi_->setChecked(true);
}

QString DocumentThemePicker::theme() const
{
    const int id = choices_->checkedId();
    return id >= 0 && id < static_cast<int>(kThemeValues.size()) ? kThemeValues[id]
                                                               : followUiTheme_;
}

} // namespace mervin
