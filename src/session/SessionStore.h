#pragma once

#include <QString>
#include <QStringList>

namespace mervin {

// Persists the set of currently-open document paths so they can be reopened on
// the next start (crash recovery / session restore - on by default, toggleable
// in Settings). JSON-persisted in %APPDATA%/MervinPDF/session.json. The single
// UI process is the only writer. Plain value type, unit-testable.
class SessionStore
{
public:
    void setPaths(const QStringList &paths) { paths_ = paths; }
    const QStringList &paths() const { return paths_; }

    // The document that was on screen when the session was last recorded (one of
    // paths(), or empty). Restore opens this one FIRST and makes it the current
    // tab, so the document the user was actually reading is the one that appears
    // while the rest of the session is still loading. Persisted as "active";
    // a session file written before this field existed simply has none.
    void setActivePath(const QString &path) { activePath_ = path; }
    const QString &activePath() const { return activePath_; }

    void clear()
    {
        paths_.clear();
        activePath_.clear();
    }

    bool load(const QString &file = defaultFile());
    bool save(const QString &file = defaultFile()) const;

    static QString defaultFile(); // %APPDATA%/MervinPDF/session.json

private:
    QStringList paths_;
    QString activePath_;
};

} // namespace mervin
