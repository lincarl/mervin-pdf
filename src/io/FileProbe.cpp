#include "io/FileProbe.h"

#include "io/Background.h"
#include "recent/MountTable.h"
#include "recent/PathKey.h"
#include "recent/RecentStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <mutex>
#include <utility>

namespace mervin {

struct FileProbe::Shared
{
    std::mutex mutex;
    // Set on the UI thread and only copied elsewhere: making a QPointer from the
    // object, or converting one, reads the object, which may be going.
    QPointer<QObject> owner;
    bool stopped = false;
    QList<File> incoming;      // waiting to be sorted by volume
    QList<File> incomingFirst; // ...and to go ahead of the files already waiting
    bool sorting = false;
    struct Volume
    {
        QList<File> files;
        bool draining = false;
        QString examining; // keyOf() the file being checked now, if any
    };
    QHash<QString, Volume> volumes; // by answering folder
};

namespace {

// The file systems Linux lists now, read without touching any volume. Empty
// elsewhere, where a volume is a drive or a share named in the path itself.
QList<mounts::Mount> listedMounts()
{
#ifdef Q_OS_WIN
    return {};
#else
    QFile mountInfo(QStringLiteral("/proc/self/mountinfo"));
    return mountInfo.open(QIODevice::ReadOnly) ? mounts::parseMountInfo(mountInfo.readAll())
                                               : QList<mounts::Mount>();
#endif
}

// The volume a file is checked with. That is its drive or share on Windows, and
// elsewhere the folder whose file system answers for it.
QString volumeOf(const QString &path, const QList<mounts::Mount> &mounted)
{
#ifdef Q_OS_WIN
    Q_UNUSED(mounted);
    return RecentStore::volumeKey(path);
#else
    const QString p = QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()));
    return mounted.isEmpty() ? RecentStore::volumeKey(p) : mounts::answeringFolder(p, mounted);
#endif
}

} // namespace

QString FileProbe::keyOf(const File &file)
{
    return normalizePathKey(file.path) + QLatin1Char('\n') + file.mount;
}

FileProbe::FileProbe(QObject *parent)
    : QObject(parent)
    , shared_(std::make_shared<Shared>())
{
    shared_->owner = this;
}

FileProbe::~FileProbe()
{
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->stopped = true;
    shared_->incoming.clear();
    shared_->incomingFirst.clear();
    shared_->volumes.clear();
}

FileFacts FileProbe::examine(const File &file)
{
    QSet<QString> away;
    return examine(file, away);
}

FileFacts FileProbe::examine(const File &file, QSet<QString> &away)
{
    const QFileInfo info(file.path);
    FileFacts facts;
    facts.exists = info.exists();
    if (facts.exists) {
        facts.size = info.size();
        facts.mount = RecentStore::mountOf(file.path);
        return facts;
    }
    // Files in one folder with one recorded mount get the same answer (see
    // RecentStore::volumeKey), so on an offline share each folder stalls once. Only
    // an away answer is reused: it only ever keeps a file, while an attached one
    // reused after the volume went away would have files forgotten as removed.
    const QString volume = RecentStore::volumeKey(file.path) + QLatin1Char('\n') + file.mount;
    if (away.contains(volume)) {
        facts.volumeAttached = false;
    } else {
        facts.volumeAttached = RecentStore::isVolumeAttached(file.path, file.mount);
        if (!facts.volumeAttached)
            away.insert(volume);
    }
    facts.folderExists = facts.volumeAttached && info.absoluteDir().exists();
    return facts;
}

void FileProbe::check(const QList<File> &files, int maxAgeMs, bool first)
{
    QList<File> wanted;
    QList<File> fresh;
    for (const File &file : files) {
        if (file.path.isEmpty())
            continue;
        const QString k = keyOf(file);
        if (checking_.contains(k)) {
            // Moved ahead if it is still waiting behind others.
            if (first)
                wanted.append(file);
            continue;
        }
        if (maxAgeMs > 0) {
            const auto it = answers_.constFind(k);
            if (it != answers_.cend() && !it->age.hasExpired(maxAgeMs)) {
                fresh.append(file);
                continue;
            }
        }
        checking_.insert(k);
        wanted.append(file);
    }
    if (!fresh.isEmpty()) {
        // Announced again on a later turn, as a checked answer would be.
        QTimer::singleShot(0, this, [this, fresh] {
            for (const File &file : fresh) {
                // Checked afresh meanwhile: that answer reaches every listener.
                if (checking_.contains(keyOf(file)))
                    continue;
                if (const std::optional<FileFacts> f = facts(file))
                    emit checked(file, *f);
            }
        });
    }
    if (wanted.isEmpty())
        return;

    bool startSorting = false;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        (first ? shared_->incomingFirst : shared_->incoming).append(wanted);
        startSorting = !std::exchange(shared_->sorting, true);
    }
    if (!startSorting)
        return;
    // Sorting reads the mount list, never a volume, so it cannot stall.
    background::start([shared = shared_] {
        for (;;) {
            QList<File> files;
            QList<File> filesFirst;
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                if (shared->stopped || (shared->incoming.isEmpty() && shared->incomingFirst.isEmpty())) {
                    shared->sorting = false;
                    return;
                }
                files.swap(shared->incoming);
                filesFirst.swap(shared->incomingFirst);
            }
            const QList<mounts::Mount> mounted = listedMounts();
            QStringList idle;
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                if (shared->stopped)
                    return;
                // Urgent files go ahead of each volume's waiting files, in the order
                // given; `ahead` is where the next one goes. A file is never queued
                // twice: one already waiting moves ahead instead, and one being
                // checked now is left to that check.
                QHash<QString, int> ahead;
                const auto add = [&](const File &file, bool urgent) {
                    const QString volume = volumeOf(file.path, mounted);
                    Shared::Volume &v = shared->volumes[volume];
                    const QString k = keyOf(file);
                    const auto waiting = [&k](const File &f) { return keyOf(f) == k; };
                    if (v.examining == k)
                        return;
                    if (urgent) {
                        v.files.removeIf(waiting);
                        int &at = ahead[volume];
                        v.files.insert(qMin(at++, int(v.files.size())), file);
                    } else if (std::none_of(v.files.cbegin(), v.files.cend(), waiting)) {
                        v.files.append(file);
                    }
                    if (!std::exchange(v.draining, true))
                        idle.append(volume);
                };
                for (const File &file : std::as_const(filesFirst))
                    add(file, true);
                for (const File &file : std::as_const(files))
                    add(file, false);
            }
            for (const QString &volume : std::as_const(idle))
                background::start([shared, volume] { drain(shared, volume); });
        }
    });
}

void FileProbe::drain(const std::shared_ptr<Shared> &shared, const QString &volume)
{
    QSet<QString> away; // volumes found away in this run, by folder and mount
    for (;;) {
        File file;
        QPointer<QObject> owner;
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            const auto it = shared->volumes.find(volume);
            if (shared->stopped || it == shared->volumes.end())
                return;
            if (it->files.isEmpty()) {
                shared->volumes.erase(it);
                return;
            }
            file = it->files.takeFirst();
            it->examining = keyOf(file);
            owner = shared->owner;
        }
        const FileFacts facts = examine(file, away);
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            const auto it = shared->volumes.find(volume);
            if (it != shared->volumes.end())
                it->examining.clear();
        }
        background::post(owner, [owner, file, facts] {
            static_cast<FileProbe *>(owner.data())->answer(file, facts);
        });
    }
}

void FileProbe::answer(const File &file, const FileFacts &facts)
{
    const QString k = keyOf(file);
    checking_.remove(k);
    Answer &a = answers_[k];
    a.facts = facts;
    a.age.start();
    emit checked(file, facts);
}

std::optional<FileFacts> FileProbe::facts(const File &file) const
{
    const auto it = answers_.constFind(keyOf(file));
    if (it == answers_.cend())
        return std::nullopt;
    return it->facts;
}

bool FileProbe::isChecking(const File &file) const
{
    return checking_.contains(keyOf(file));
}

} // namespace mervin
