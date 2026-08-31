#include "config/Settings.h"

#include "config/ConfigPaths.h"

#include <QFile>

#include <toml++/toml.hpp>

#include <sstream>
#include <string>
#include <string_view>

namespace mervin {

Settings Settings::load()
{
    Settings s;
    QFile f(ConfigPaths::configFile());
    if (!f.open(QIODevice::ReadOnly))
        return s;
    const QByteArray data = f.readAll();
    f.close();

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
        // Page layout, on two independent axes. `page_mode` was once a single
        // three-valued key, so its retired "two-page" value migrates to the
        // spread flag and leaves the scrolling choice at its default. Any
        // unrecognised value falls back to continuous rather than being kept:
        // pageMode is compared against literals downstream, so an odd string
        // would silently read as continuous anyway.
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
        s.recentVisibleCount = integer("recent_visible_count", s.recentVisibleCount);
        s.recentRetention = integer("recent_retention", s.recentRetention);
        s.measurementUnit = str("measurement_unit", s.measurementUnit);
        s.measurementType = str("measurement_type", s.measurementType);
        s.measurementPrecision = integer("measurement_precision", s.measurementPrecision);
        s.measurementLineWidth = real("measurement_line_width", s.measurementLineWidth);
        s.measurementSnap = boolean("measurement_snap", s.measurementSnap);
        s.highlightFormFields = boolean("highlight_form_fields", s.highlightFormFields);
        s.autoFormFill = boolean("auto_form_fill", s.autoFormFill);
        s.ocrDefaultLanguage = str("ocr_default_language", s.ocrDefaultLanguage);
        s.annotationAuthor = str("annotation_author", s.annotationAuthor);
        s.annotationColor = str("annotation_color", s.annotationColor);
        s.annotationStyle = str("annotation_style", s.annotationStyle);
        s.restoreSession = boolean("restore_session", s.restoreSession);
        s.checkUpdatesOnStartup = boolean("check_updates_on_startup", s.checkUpdatesOnStartup);
        s.promptedSetDefaultApp = boolean("prompted_set_default_app", s.promptedSetDefaultApp);
        s.windowGeometry = QByteArray::fromBase64(str("window_geometry", QString()).toLatin1());
        s.windowState = QByteArray::fromBase64(str("window_state", QString()).toLatin1());
    } catch (const toml::parse_error &) {
        return Settings{}; // corrupt file -> defaults
    }
    return s;
}

void Settings::save() const
{
    toml::table tbl;
    tbl.insert("default_zoom", defaultZoom.toStdString());
    tbl.insert("page_mode", pageMode.toStdString());
    tbl.insert("two_page_spread", twoPageSpread);
    tbl.insert("color_scheme", colorScheme.toStdString());
    tbl.insert("document_theme", documentTheme.toStdString());
    tbl.insert("accent_color", accentColor.toStdString());
    tbl.insert("open_behavior", openBehavior.toStdString());
    tbl.insert("recent_visible_count", static_cast<int64_t>(recentVisibleCount));
    tbl.insert("recent_retention", static_cast<int64_t>(recentRetention));
    tbl.insert("measurement_unit", measurementUnit.toStdString());
    tbl.insert("measurement_type", measurementType.toStdString());
    tbl.insert("measurement_precision", static_cast<int64_t>(measurementPrecision));
    tbl.insert("measurement_line_width", measurementLineWidth);
    tbl.insert("measurement_snap", measurementSnap);
    tbl.insert("highlight_form_fields", highlightFormFields);
    tbl.insert("auto_form_fill", autoFormFill);
    tbl.insert("ocr_default_language", ocrDefaultLanguage.toStdString());
    tbl.insert("annotation_author", annotationAuthor.toStdString());
    tbl.insert("annotation_color", annotationColor.toStdString());
    tbl.insert("annotation_style", annotationStyle.toStdString());
    tbl.insert("restore_session", restoreSession);
    tbl.insert("check_updates_on_startup", checkUpdatesOnStartup);
    tbl.insert("prompted_set_default_app", promptedSetDefaultApp);
    tbl.insert("window_geometry", std::string(windowGeometry.toBase64().constData()));
    tbl.insert("window_state", std::string(windowState.toBase64().constData()));

    std::stringstream ss;
    ss << tbl;
    const std::string out = ss.str();

    QFile f(ConfigPaths::configFile());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(out.data(), static_cast<qint64>(out.size()));
}

} // namespace mervin
