#pragma once

#include "io/FileFacts.h"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include <memory>
#include <optional>

namespace mervin {

// Checks files on background threads for the UI thread, which must never touch
// the disk itself. Each drive, share or mount gets one thread at a time, which
// checks its files in order, so a share that stopped answering holds up only its
// own files and never collects more threads. Within one run of a thread, files in
// the same folder with the same recorded mount share a volume check that found the
// volume away, as an offline share can stall every check. Answers arrive through checked() and stay
// available from facts(). A file on a volume that never answers gets no answer.
//
// A file is checked with the mount recorded for it (FileCheck::mount), which the
// volume check depends on, so the same path with another mount is another check.
class FileProbe : public QObject
{
    Q_OBJECT

public:
    using File = FileCheck;

    explicit FileProbe(QObject *parent = nullptr);
    // Leaves running checks behind without waiting for them.
    ~FileProbe() override;

    // Check `files`. A file already waiting or being checked is not checked twice.
    // One answered within the last `maxAgeMs` milliseconds is not checked again:
    // its answer is announced again instead, on a later turn of the event loop,
    // like a fresh one. `first` puts the files ahead of those already waiting on
    // their volumes, for a decision the user is waiting for.
    void check(const QList<File> &files, int maxAgeMs = 0, bool first = false);

    // The latest cached answer for `file`, if one has arrived. One-shot generation
    // requests deliver only their checked() signal and retain no cached answer.
    std::optional<FileFacts> facts(const File &file) const;

    // Whether a check of `file` is waiting or running.
    bool isChecking(const File &file) const;

    // What a check finds for one file, on the calling thread. It can stall on an
    // unreachable drive, so call it only off the UI thread or from tests.
    static FileFacts examine(const File &file);

    // A comparison key from normalized path, recorded mount and request generation.
    static QString keyOf(const File &file);

signals:
    void checked(const mervin::FileCheck &file, const mervin::FileFacts &facts);

private:
    struct Answer
    {
        FileFacts facts;
        QElapsedTimer age;
    };
    struct Shared; // the queues, shared with the threads that drain them

    // examine(), taking a volume in `away` (by folder and mount) as away without
    // checking it again, and adding one it finds away.
    static FileFacts examine(const File &file, QSet<QString> &away);
    // Checks one volume's files in order until none are left.
    static void drain(const std::shared_ptr<Shared> &shared, const QString &volume);
    void answer(const File &file, const FileFacts &facts);

    std::shared_ptr<Shared> shared_;
    QSet<QString> checking_;        // by keyOf()
    QHash<QString, Answer> answers_; // by keyOf()
};

} // namespace mervin
