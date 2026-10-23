#include "config/Settings.h"

#include "config/ConfigPaths.h"
#include "io/AppFiles.h"

#include <QFile>
#include <QDebug>

#include <toml++/toml.hpp>

#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace mervin {

namespace {

// How long a save that reports its result waits for its write.
constexpr int kOrderedSaveWaitMs = 5000;

} // namespace

// The settings in the text of a config file. Keys it lacks keep their defaults.
static Settings parsedValues(const QByteArray &data)
{
    Settings s;
    try {
        const toml::table tbl =
            toml::parse(std::string_view(data.constData(), static_cast<size_t>(data.size())));

        auto str = [&](const char *key, const QString &def) -> QString {
            if (auto v = tbl[key].value<std::string>())
                return QString::fromStdString(*v);
            return def;
        };
        auto boolean = [&](const char *key, bool def) -> bool {
            return tbl[key].value<bool>().value_or(def);
        };
        auto integer = [&](const char *key, int def) -> int {
            return static_cast<int>(tbl[key].value<int64_t>().value_or(def));
        };
        auto real = [&](const char *key, double def) -> double {
            return tbl[key].value<double>().value_or(def);
        };

        s.defaultZoom = str("default_zoom", s.defaultZoom);
        // Migrate legacy two-page mode to the independent spread flag. Unrecognized scrolling
        // modes become continuous.
        s.twoPageSpread = boolean("two_page_spread", s.twoPageSpread);
        const QString pm = str("page_mode", s.pageMode);
        if (pm == QLatin1String("two-page"))
            s.twoPageSpread = true;
        else if (pm == QLatin1String("single"))
            s.pageMode = QStringLiteral("single");
        s.colorScheme = str("color_scheme", s.colorScheme);
        // Document theme. Prefer the new key; if absent, migrate from the old
        // boolean `invert_colors` (true -> dark, false -> light). A config with
        // neither key (fresh install) keeps the struct default of "light".
        if (tbl.contains("document_theme"))
            s.documentTheme = str("document_theme", s.documentTheme);
        else if (tbl.contains("invert_colors"))
            s.documentTheme = boolean("invert_colors", false) ? QStringLiteral("dark")
                                                              : QStringLiteral("light");
        s.accentColor = str("accent_color", s.accentColor);
        s.openBehavior = str("open_behavior", s.openBehavior);
        if (tbl["unload_inactive_minutes"].is_integer()) {
            const auto minutes = tbl["unload_inactive_minutes"].value<int64_t>();
            if (minutes && *minutes >= 0 && *minutes <= Settings::kMaxUnloadInactiveMinutes)
                s.unloadInactiveMinutes = static_cast<int>(*minutes);
        }
        s.closeToTray = boolean("close_to_tray", s.closeToTray);
        s.recentVisibleCount = integer("recent_visible_count", s.recentVisibleCount);
        s.recentRetention = integer("recent_retention", s.recentRetention);
        s.recentKeepMissing = boolean("recent_keep_removed", s.recentKeepMissing);
        const QString scope = str("recent_search_scope", s.recentSearchScope);
        if (scope == QLatin1String("names") || scope == QLatin1String("contents")
            || scope == QLatin1String("all"))
            s.recentSearchScope = scope;
        s.measurementUnit = str("measurement_unit", s.measurementUnit);
        s.measurementType = str("measurement_type", s.measurementType);
        s.measurementPrecision = integer("measurement_precision", s.measurementPrecision);
        s.measurementLineWidth = real("measurement_line_width", s.measurementLineWidth);
        s.measurementSnap = boolean("measurement_snap", s.measurementSnap);
        s.highlightFormFields = boolean("highlight_form_fields", s.highlightFormFields);
        s.autoFormFill = boolean("auto_form_fill", s.autoFormFill);
        s.extractOpenWhenDone = boolean("extract_open_when_done", s.extractOpenWhenDone);
        s.ocrDefaultLanguage = str("ocr_default_language", s.ocrDefaultLanguage);
        s.uiLanguage = str("ui_language", s.uiLanguage);
        s.annotationAuthor = str("annotation_author", s.annotationAuthor);
        s.annotationColor = str("annotation_color", s.annotationColor);
        s.annotationStyle = str("annotation_style", s.annotationStyle);
        s.restoreSession = boolean("restore_session", s.restoreSession);
        s.autoUpdate = boolean("auto_update", s.autoUpdate);
        s.promptedSetDefaultApp = boolean("prompted_set_default_app", s.promptedSetDefaultApp);
        s.windowGeometry = QByteArray::fromBase64(str("window_geometry", QString()).toLatin1());
        s.windowState = QByteArray::fromBase64(str("window_state", QString()).toLatin1());
    } catch (const toml::parse_error &) {
        return Settings{}; // corrupt file -> defaults
    }
    return s;
}

// The settings in the config file, read on the calling thread.
static Settings loadValues()
{
    QFile f(ConfigPaths::configFile());
    return parsedValues(f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray());
}

static toml::table settingsTable(const Settings &s)
{
    toml::table tbl;
    tbl.insert("default_zoom", s.defaultZoom.toStdString());
    tbl.insert("page_mode", s.pageMode.toStdString());
    tbl.insert("two_page_spread", s.twoPageSpread);
    tbl.insert("color_scheme", s.colorScheme.toStdString());
    tbl.insert("document_theme", s.documentTheme.toStdString());
    tbl.insert("accent_color", s.accentColor.toStdString());
    tbl.insert("open_behavior", s.openBehavior.toStdString());
    tbl.insert("unload_inactive_minutes", static_cast<int64_t>(s.unloadInactiveMinutes));
    tbl.insert("close_to_tray", s.closeToTray);
    tbl.insert("recent_visible_count", static_cast<int64_t>(s.recentVisibleCount));
    tbl.insert("recent_retention", static_cast<int64_t>(s.recentRetention));
    tbl.insert("recent_keep_removed", s.recentKeepMissing);
    tbl.insert("recent_search_scope", s.recentSearchScope.toStdString());
    tbl.insert("measurement_unit", s.measurementUnit.toStdString());
    tbl.insert("measurement_type", s.measurementType.toStdString());
    tbl.insert("measurement_precision", static_cast<int64_t>(s.measurementPrecision));
    tbl.insert("measurement_line_width", s.measurementLineWidth);
    tbl.insert("measurement_snap", s.measurementSnap);
    tbl.insert("highlight_form_fields", s.highlightFormFields);
    tbl.insert("auto_form_fill", s.autoFormFill);
    tbl.insert("extract_open_when_done", s.extractOpenWhenDone);
    tbl.insert("ocr_default_language", s.ocrDefaultLanguage.toStdString());
    tbl.insert("ui_language", s.uiLanguage.toStdString());
    tbl.insert("annotation_author", s.annotationAuthor.toStdString());
    tbl.insert("annotation_color", s.annotationColor.toStdString());
    tbl.insert("annotation_style", s.annotationStyle.toStdString());
    tbl.insert("restore_session", s.restoreSession);
    tbl.insert("auto_update", s.autoUpdate);
    tbl.insert("prompted_set_default_app", s.promptedSetDefaultApp);
    tbl.insert("window_geometry", std::string(s.windowGeometry.toBase64().constData()));
    tbl.insert("window_state", std::string(s.windowState.toBase64().constData()));

    return tbl;
}

static QByteArray serializeTable(const toml::table &tbl)
{
    std::stringstream stream;
    stream << tbl;
    return QByteArray::fromStdString(stream.str());
}

bool Settings::operator==(const Settings &other) const
{
    return settingsTable(*this) == settingsTable(other);
}

void Settings::assignValues(const Settings &other)
{
    const QByteArray baseline = baseline_;
    *this = other;
    baseline_ = baseline;
}

namespace {

// The values in effect, by the config file they belong to, so a test or a
// --profile run with another folder reads its own file.
std::mutex cacheMutex;
QString cachedFile;              // guarded by cacheMutex
std::optional<Settings> cached;  // guarded by cacheMutex
bool persistenceEnabled = true; // guarded by cacheMutex

// The values in effect for `file`, read from it the first time. Call with
// cacheMutex held.
Settings &current(const QString &file)
{
    if (!cached || cachedFile != file) {
        cached = loadValues();
        cachedFile = file;
    }
    return *cached;
}

} // namespace

Settings Settings::load()
{
    Settings result;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        result = current(ConfigPaths::configFile());
    }
    result.baseline_ = serializeTable(settingsTable(result));
    return result;
}

void Settings::setPersistenceEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    persistenceEnabled = enabled;
}

bool Settings::save(QString *error) const
{
    if (unloadInactiveMinutes < 0 || unloadInactiveMinutes > kMaxUnloadInactiveMinutes) {
        // Untranslated because the Settings dialog checks the timeout before it
        // saves, so its save error message never shows this text. Translate it if
        // another caller that shows save errors can pass an unchecked value.
        if (error)
            *error = QStringLiteral("The document inactivity timeout must be from 0 to %1 minutes.")
                         .arg(kMaxUnloadInactiveMinutes);
        return false;
    }
    const QString path = ConfigPaths::configFile();
    std::unique_lock<std::mutex> lock(cacheMutex);
    const auto desired = settingsTable(*this);
    auto merged = settingsTable(current(path));
    if (baseline_.isEmpty()) {
        merged = desired;
    } else {
        const auto baseline = toml::parse(std::string_view(baseline_.constData(), baseline_.size()));
        for (const auto &[key, value] : desired) {
            toml::table before, after;
            if (const auto *old = baseline.get(key))
                before.insert(key, *old);
            after.insert(key, value);
            if (before != after)
                merged.insert_or_assign(key, value);
        }
    }
    const QByteArray bytes = serializeTable(merged);
    if (!persistenceEnabled) {
        current(path) = parsedValues(bytes);
        baseline_ = serializeTable(desired);
        return true;
    }
    if (!error) {
        current(path) = parsedValues(bytes);
        lock.unlock();
        appfiles::write(path, bytes);
        baseline_ = serializeTable(desired);
        return true;
    }
    lock.unlock();
    // In order with the writes asked for before, so none of them can land later and
    // undo it. A write still waiting after that lands in its turn and counts as
    // saved; only one that failed is reported.
    if (appfiles::writeAndWait(path, bytes, kOrderedSaveWaitMs, error) == appfiles::Wait::Failed)
        return false;
    {
        std::lock_guard<std::mutex> relock(cacheMutex);
        current(path) = parsedValues(bytes);
    }
    baseline_ = serializeTable(desired);
    return true;
}

} // namespace mervin
