#pragma once

#include <QDate>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace mervin {

// Persists the set of currently-open document paths so they can be reopened on
// the next start (crash recovery / session restore - on by default, toggleable
// in Settings). JSON-persisted in %APPDATA%/MervinPDF/session.json. The single
// UI process is the only writer. Plain value type, unit-testable.
class SessionStore
{
public:
    // A session document missing at startup on a detached drive, an offline network
    // share or an unmounted volume. It stays in the saved session, unopened, until a
    // later start finds it again or it expires (see prepareRestore). Persisted as
    // "unavailable" next to "open". Older versions ignore it.
    struct Unavailable
    {
        QString path;
        QDate since; // the date of the start that first found it missing
    };

    // Days an unavailable document is kept, counted from Unavailable::since.
    static constexpr int kKeepUnavailableDays = 30;

    // The open documents. A kept document among them is open again, so it leaves
    // unavailable() and a later close drops it like any other tab. Kept documents
    // are not checked on disk here: saves run on the UI thread, and a check on an
    // unreachable share can stall for seconds.
    void setPaths(const QStringList &paths);
    const QStringList &paths() const { return paths_; }

    // The document that was on screen when the session was last recorded (one of
    // paths(), or empty). Restore opens this one FIRST and makes it the current
    // tab, so the document the user was actually reading is the one that appears
    // while the rest of the session is still loading. Persisted as "active";
    // a session file written before this field existed simply has none.
    void setActivePath(const QString &path) { activePath_ = path; }
    const QString &activePath() const { return activePath_; }

    // Kept documents, never part of paths() or the active path. Saves write them
    // as they are, without checking the disk.
    void setUnavailable(const QList<Unavailable> &entries) { unavailable_ = entries; }
    const QList<Unavailable> &unavailable() const { return unavailable_; }

    // The mount point a document was opened from (RecentEntry::mount). The session
    // keeps its own copy, so a share that is away is still told from a deleted file
    // after Recent trims or clears the document. Kept while the document is open or
    // kept, and saved with it in "mounts". Older versions ignore it. An empty mount
    // forgets the document's.
    void setMount(const QString &path, const QString &mount);
    QString mount(const QString &path) const;

    // The startup decision for a loaded session. A missing document whose folder is
    // still there on an attached volume was deleted, moved or renamed, and is
    // dropped. Any other missing document, on a volume that is away or in a missing
    // folder, is kept in unavailable(), dated `today` when first found. Kept
    // documents found again are restored after the open ones, and those kept for
    // kKeepUnavailableDays are dropped without checking the disk. Replaces paths()
    // with the documents to reopen, in order, and returns them. Each document is
    // judged by its mount(), or else by `recordedMount`, which WindowManager looks
    // up in Recent for a session saved before sessions recorded mounts. A mount
    // found that way is kept from then on. `volumeAttached` defaults to
    // RecentStore::isVolumeAttached with that mount, and tests pass their own.
    QStringList prepareRestore(const QDate &today,
                               const std::function<bool(const QString &)> &volumeAttached = {},
                               const std::function<QString(const QString &)> &recordedMount = {});

    void clear()
    {
        paths_.clear();
        activePath_.clear();
        unavailable_.clear();
        mounts_.clear();
    }

    bool load(const QString &file = defaultFile());
    bool save(const QString &file = defaultFile()) const;

    static QString defaultFile(); // %APPDATA%/MervinPDF/session.json

private:
    // Forgets the mounts of documents that are neither open nor kept.
    void keepListedMounts();

    QStringList paths_;
    QString activePath_;
    QList<Unavailable> unavailable_;
    QHash<QString, QString> mounts_; // by normalizePathKey
};

} // namespace mervin
