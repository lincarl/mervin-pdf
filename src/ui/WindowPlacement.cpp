#include "ui/WindowPlacement.h"

#include <QApplication>
#include <QEvent>
#include <QLayout>
#include <QResizeEvent>
#include <QScreen>
#include <QTimer>
#include <QWidget>

#include <algorithm>

namespace mervin {
namespace {

// Fit new windows and actual growth, including later translated text or native
// frame metrics. Routine layout requests and shrinking must preserve a user's
// deliberate placement across screens.
class WindowPlacement final : public QObject
{
public:
    explicit WindowPlacement(QApplication &application) : QObject(&application)
    {
        setObjectName(QStringLiteral("mervinWindowPlacement"));
        application.installEventFilter(this);
    }

    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() != QEvent::Show && event->type() != QEvent::Resize)
            return false;
        if (event->type() == QEvent::Resize) {
            const auto *resize = static_cast<QResizeEvent *>(event);
            if (resize->size().width() <= resize->oldSize().width()
                && resize->size().height() <= resize->oldSize().height())
                return false;
        }
        auto *window = qobject_cast<QWidget *>(object);
        if (!window || !window->isWindow() || !window->isVisible())
            return false;
        const Qt::WindowType type = window->windowType();
        if (type != Qt::Window && type != Qt::Dialog && type != Qt::Tool)
            return false;
        if (window->property("_mervinFitPending").toBool())
            return false;
        window->setProperty("_mervinFitPending", true);
        // Show precedes the platform's final frame/placement updates. The widget
        // context cancels this callback if a short-lived dialog closes and dies.
        scheduleFit(*window, 2);
        return false;
    }

private:
    // A native resize can deliver an older position after move() returns. Give
    // that configure event a turn before checking the resulting frame again.
    static void scheduleFit(QWidget &window, int remaining)
    {
        QTimer::singleShot(0, &window, [&window, remaining] {
            if (fit(window) && remaining > 0)
                scheduleFit(window, remaining - 1);
            else
                window.setProperty("_mervinFitPending", false);
        });
    }

    // Resizes the client area before clamping the complete frame's position.
    static bool fit(QWidget &window)
    {
        if (!window.isVisible() || window.isMaximized() || window.isFullScreen()
            || window.isMinimized() || !window.screen())
            return false;
        if (window.layout())
            window.layout()->activate();
        const QRect available = window.screen()->availableGeometry();
        const QSize frame = window.frameGeometry().size() - window.size();
        const QSize limit = (available.size() - frame).expandedTo(QSize(1, 1));
        const QSize size = window.size().boundedTo(limit);
        const QSize previous = window.size();
        if (size != window.size())
            window.resize(size);
        const QRect bounds = window.frameGeometry();
        const QPoint position(
            std::clamp(bounds.left(), available.left(),
                       std::max(available.left(), available.right() - bounds.width() + 1)),
            std::clamp(bounds.top(), available.top(),
                       std::max(available.top(), available.bottom() - bounds.height() + 1)));
        if (position != bounds.topLeft())
            window.move(window.pos() + position - bounds.topLeft());
        return window.size() != previous || position != bounds.topLeft();
    }
};

} // namespace

void keepWindowsOnScreen(QApplication &application)
{
    if (!application.findChild<QObject *>(QStringLiteral("mervinWindowPlacement"),
                                         Qt::FindDirectChildrenOnly))
        new WindowPlacement(application);
}

} // namespace mervin
