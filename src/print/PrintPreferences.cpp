#include "PrintPreferences.h"

#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>

namespace mervin::printing {
namespace {

// Resolves an enum from its stable JSON name, retaining the default for unknown names.
template <typename Enum>
Enum readEnum(const QJsonValue &value, Enum fallback,
              std::initializer_list<std::pair<Enum, const char *>> names)
{
    const QString name = value.toString();
    for (const auto &[candidate, text] : names) {
        if (name == QLatin1String(text))
            return candidate;
    }
    return fallback;
}

// Writes semantic enum names so adding a UI choice does not change stored meanings.
template <typename Enum>
QString enumName(Enum value, std::initializer_list<std::pair<Enum, const char *>> names)
{
    for (const auto &[candidate, text] : names) {
        if (value == candidate)
            return QString::fromLatin1(text);
    }
    return {};
}

// Rejects fractional, out-of-range or wrongly typed input instead of coercing it.
int readInteger(const QJsonValue &value, int fallback, int minimum, int maximum)
{
    if (!value.isDouble())
        return fallback;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < minimum || number > maximum
        || std::floor(number) != number)
        return fallback;
    return static_cast<int>(number);
}

// Bounds saved identifiers and custom page input before passing them to widgets or drivers.
QString readText(const QJsonValue &value, qsizetype maximum = 4096)
{
    const QString text = value.toString();
    return text.size() <= maximum && !text.contains(QChar::Null) ? text : QString();
}

constexpr std::initializer_list<std::pair<ScaleMode, const char *>> scaleNames{
    {ScaleMode::FitToPage, "fit"}, {ScaleMode::ActualSize, "actual"},
    {ScaleMode::Custom, "custom"}};
constexpr std::initializer_list<std::pair<Alignment, const char *>> alignmentNames{
    {Alignment::Centered, "centered"}, {Alignment::CenteredVertically, "centered-vertically"},
    {Alignment::CenteredHorizontally, "centered-horizontally"}, {Alignment::TopLeft, "top-left"}};
constexpr std::initializer_list<std::pair<ColourMode, const char *>> colourNames{
    {ColourMode::Colour, "colour"}, {ColourMode::Grayscale, "grayscale"},
    {ColourMode::BlackAndWhite, "black-and-white"}};
constexpr std::initializer_list<std::pair<PageSelection, const char *>> selectionNames{
    {PageSelection::All, "all"}, {PageSelection::Current, "current"},
    {PageSelection::Range, "range"}, {PageSelection::Custom, "custom"}};
constexpr std::initializer_list<std::pair<QPageLayout::Orientation, const char *>> orientationNames{
    {QPageLayout::Portrait, "portrait"}, {QPageLayout::Landscape, "landscape"}};
constexpr std::initializer_list<std::pair<DuplexMode, const char *>> duplexNames{
    {DuplexMode::None, "none"}, {DuplexMode::Auto, "auto"},
    {DuplexMode::LongSide, "long-side"}, {DuplexMode::ShortSide, "short-side"}};

} // namespace

QJsonObject Preferences::toJson() const
{
    QJsonObject object{
        {QStringLiteral("printerName"), printerName},
        {QStringLiteral("printToFile"), printToFile},
        {QStringLiteral("orientation"), enumName(orientation, orientationNames)},
        {QStringLiteral("pageSelection"), enumName(pageSelection, selectionNames)},
        {QStringLiteral("fromPage"), fromPage},
        {QStringLiteral("toPage"), toPage},
        {QStringLiteral("customPages"), customPages},
        {QStringLiteral("copies"), copies},
        {QStringLiteral("scaleMode"), enumName(settings.scaleMode, scaleNames)},
        {QStringLiteral("scalePercent"), settings.scalePercent},
        {QStringLiteral("alignment"), enumName(settings.alignment, alignmentNames)},
        {QStringLiteral("colourMode"), enumName(settings.colourMode, colourNames)},
        {QStringLiteral("qualityDpi"), settings.qualityDpi},
        {QStringLiteral("duplex"), enumName(duplex, duplexNames)},
    };
    if (!paperPoints.isEmpty()) {
        object.insert(QStringLiteral("paper"), QJsonObject{
            {QStringLiteral("key"), paperKey},
            {QStringLiteral("widthPoints"), paperPoints.width()},
            {QStringLiteral("heightPoints"), paperPoints.height()},
        });
    }
    return object;
}

Preferences Preferences::fromJson(const QJsonObject &object)
{
    Preferences preferences;
    preferences.printerName = readText(object.value(QStringLiteral("printerName")));
    preferences.printToFile = object.value(QStringLiteral("printToFile")).toBool(false);
    preferences.orientation = readEnum(object.value(QStringLiteral("orientation")),
                                       preferences.orientation, orientationNames);
    preferences.pageSelection = readEnum(object.value(QStringLiteral("pageSelection")),
                                         preferences.pageSelection, selectionNames);
    preferences.fromPage = readInteger(object.value(QStringLiteral("fromPage")), 1, 1,
                                       std::numeric_limits<int>::max());
    preferences.toPage = readInteger(object.value(QStringLiteral("toPage")), 1, 1,
                                     std::numeric_limits<int>::max());
    preferences.toPage = qMax(preferences.fromPage, preferences.toPage);
    // Match QLineEdit's default maximum so every accepted custom specification survives.
    preferences.customPages = readText(object.value(QStringLiteral("customPages")), 32767);
    preferences.copies = readInteger(object.value(QStringLiteral("copies")), 1, 1, 999);
    preferences.settings.scaleMode = readEnum(object.value(QStringLiteral("scaleMode")),
                                              preferences.settings.scaleMode, scaleNames);
    preferences.settings.scalePercent = readInteger(object.value(QStringLiteral("scalePercent")),
                                                     100, 10, 400);
    preferences.settings.alignment = readEnum(object.value(QStringLiteral("alignment")),
                                              preferences.settings.alignment, alignmentNames);
    preferences.settings.colourMode = readEnum(object.value(QStringLiteral("colourMode")),
                                               preferences.settings.colourMode, colourNames);
    const int quality = readInteger(object.value(QStringLiteral("qualityDpi")), 300, 150, 600);
    if (quality == 150 || quality == 300 || quality == 600)
        preferences.settings.qualityDpi = quality;
    const DuplexMode fallbackDuplex = object.value(QStringLiteral("twoSided")).toBool(false)
        ? DuplexMode::LongSide : DuplexMode::None;
    preferences.duplex = readEnum(object.value(QStringLiteral("duplex")), fallbackDuplex,
                                  duplexNames);

    const QJsonObject paper = object.value(QStringLiteral("paper")).toObject();
    const double width = paper.value(QStringLiteral("widthPoints")).toDouble();
    const double height = paper.value(QStringLiteral("heightPoints")).toDouble();
    // Leave room for large-format printers while bounding untrusted dimensions.
    if (std::isfinite(width) && std::isfinite(height)
        && width > 0 && height > 0 && width <= 1000000 && height <= 1000000) {
        preferences.paperPoints = QSizeF(width, height);
        preferences.paperKey = readText(paper.value(QStringLiteral("key")));
    }
    return preferences;
}

} // namespace mervin::printing
