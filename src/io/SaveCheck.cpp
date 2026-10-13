#include "io/SaveCheck.h"

#include <condition_variable>
#include <mutex>

namespace mervin {

namespace {
std::mutex heldMutex;
std::condition_variable heldChanged;
bool held = false; // guarded by heldMutex
} // namespace

SaveCheck::SaveCheck(QObject *parent)
    : QObject(parent)
{
    slowTimer_.setSingleShot(true);
    slowTimer_.setInterval(kSlowMs);
    connect(&slowTimer_, &QTimer::timeout, this, [this] {
        slow_ = true;
        emit slow();
    });
}

void SaveCheck::finish()
{
    pending_ = false;
    slow_ = false;
    slowTimer_.stop();
}

void SaveCheck::holdForTesting(bool on)
{
    std::lock_guard<std::mutex> lock(heldMutex);
    held = on;
    heldChanged.notify_all();
}

void SaveCheck::waitWhileHeld()
{
    std::unique_lock<std::mutex> lock(heldMutex);
    heldChanged.wait(lock, [] { return !held; });
}

QString SaveCheck::checkingText()
{
    //: Shown under Save as while the app checks the folder the file would be saved
    //: in. On a network drive this can take a while; the button that saves stays
    //: disabled until the check is done.
    return tr("Checking the folder…");
}

} // namespace mervin
