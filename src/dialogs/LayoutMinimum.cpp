#include "dialogs/LayoutMinimum.h"

#include <QEvent>
#include <QLayout>
#include <QWidget>

namespace mervin {

namespace {

// Refits the window's minimum whenever its layout changes. The window's layout
// handles a layout request before event filters see it, so the sizes read here
// are current. Show covers changes made before the window first opens, when
// Qt activates the layout without a request.
class MinimumFollower final : public QObject
{
public:
    MinimumFollower(QWidget &window, QSize floor)
        : QObject(&window), window_(window), floor_(floor)
    {
        window.installEventFilter(this);
        fit();
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Show)
            fit();
        return QObject::eventFilter(watched, event);
    }

private:
    void fit()
    {
        window_.ensurePolished(); // stylesheet padding and fonts count
        QSize minimum = floor_.expandedTo(window_.minimumSizeHint());
        if (const QLayout *layout = window_.layout())
            minimum.setHeight(qMax(minimum.height(), layout->totalHeightForWidth(minimum.width())));
        // A larger minimum also enlarges a window that is now smaller.
        window_.setMinimumSize(minimum);
    }

    QWidget &window_;
    const QSize floor_;
};

} // namespace

void followLayoutMinimum(QWidget &window, QSize floor)
{
    new MinimumFollower(window, floor); // owned by the window
}

} // namespace mervin
