#pragma once

#include <QRadioButton>

#include <functional>

class QPainter;

namespace mervin {

// One choice in a theme picker: a radio button drawn as a card with a preview
// above its label and a check mark when selected. Keeping the native radio
// button preserves its accessible role, checked state, Space activation and
// arrow-key navigation. Only its painting and hit area change.
class ThemeCard : public QRadioButton
{
public:
    // Draws the preview inside `well`, in card coordinates.
    using PaintPreview = std::function<void(QPainter &painter, const QRectF &well)>;

    ThemeCard(const QString &label, PaintPreview paintPreview, QWidget *parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    bool hitButton(const QPoint &point) const override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    PaintPreview paintPreview_;
};

} // namespace mervin
