#pragma once

#include "io/Background.h"

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <utility>

namespace mervin {

// Checks where a dialog will save, on a background thread: the folder can be on a
// drive or share that stopped answering. A newer check drops one that has not
// started, and only the newest answer arrives. The dialog keeps showing the last
// answer meanwhile, so a quick check changes nothing on screen, and slow() tells it
// when a check is taking long enough to say so.
class SaveCheck : public QObject
{
    Q_OBJECT

public:
    // How long a check runs before slow().
    static constexpr int kSlowMs = 300;

    explicit SaveCheck(QObject *parent = nullptr);

    // Runs `work` on a background thread, then `done` with its result on this
    // object's thread, unless another check was asked for by then or this is gone.
    // `work` gets its own copies of what it needs.
    template <class Work, class Done>
    void run(Work work, Done done)
    {
        const quint64 serial = ++serial_;
        pending_ = true;
        slow_ = false;
        slowTimer_.start();
        queue_.clear();
        queue_.add([guard = QPointer<QObject>(this), serial, work = std::move(work),
                    done = std::move(done)] {
            waitWhileHeld();
            background::post(guard, [guard, serial, result = work(), done] {
                auto *self = static_cast<SaveCheck *>(guard.data());
                if (serial != self->serial_)
                    return;
                self->finish();
                done(result);
            });
        });
    }

    // A check asked for has not answered yet.
    bool pending() const { return pending_; }
    // ...and has taken kSlowMs so far: slow() has been emitted for it.
    bool isSlow() const { return slow_; }

    // The error line's text while a check is slow.
    static QString checkingText();

    // For tests only: while held, checks wait before they run, as on a drive that
    // stopped answering.
    static void holdForTesting(bool on);

signals:
    // The check asked for last has been running for kSlowMs.
    void slow();

private:
    void finish();
    static void waitWhileHeld();

    background::SerialQueue queue_;
    quint64 serial_ = 0;
    bool pending_ = false;
    bool slow_ = false;
    QTimer slowTimer_;
};

} // namespace mervin
