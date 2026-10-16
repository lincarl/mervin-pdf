#pragma once

#include <QImage>
#include <QList>
#include <QRect>
#include <QRectF>
#include <QSizeF>

class QPainter;

namespace mervin::printing {

enum class ScaleMode { FitToPage, ActualSize, Custom };
enum class Alignment { Centered, CenteredVertically, CenteredHorizontally, TopLeft };
enum class ColourMode { Colour, Grayscale, BlackAndWhite };

struct Settings
{
    ScaleMode scaleMode = ScaleMode::FitToPage;
    int scalePercent = 100;
    Alignment alignment = Alignment::Centered;
    ColourMode colourMode = ColourMode::Colour;
    int qualityDpi = 300;
};

// pagePoints includes the PDF's existing margins. Page dimensions, printableRect,
// and painter coordinates must use the same units, normally physical points.
// Actual size and custom scale can extend beyond printableRect for clipping.
QRectF destinationRect(const QSizeF &pagePoints, const QRectF &printableRect,
                      const Settings &settings);

// The raster that prints one page with `dpi` raster pixels per inch of paper.
struct RasterPlan
{
    double scale = 0;    // raster pixels per page point, as Document::renderPage takes it
    QRect source;        // the visible pixels of the page's full raster at that scale
    QRectF destination;  // the whole page on the sheet, as destinationRect places it

    bool isEmpty() const { return source.isEmpty(); }
};

// Plans a page raster whose resolution follows the page's printed size, so a
// reduced page needs fewer pixels and an enlarged one keeps `dpi`. The source
// covers only the part of the page inside printableRect, rounded out to whole
// pixels. Empty for invalid sizes or a non-positive dpi.
RasterPlan rasterPlan(const QSizeF &pagePoints, const QRectF &printableRect,
                      const Settings &settings, double dpi);

// Where `pixels` of a page raster rendered at `scale` land on the sheet, for the
// page placed at `destination`.
QRectF rasterTarget(const QRectF &destination, const QSizeF &pagePoints, double scale,
                    const QRect &pixels);

// Splits `source` into the fewest horizontal bands of whole rows that hold at most
// `maxPixels` each, with heights that differ by at most one row. A band always has
// at least one row, so a row wider than `maxPixels` becomes its own band.
QList<QRect> rasterBands(const QRect &source, qint64 maxPixels);

// Painting a raster has a prepare step, convertColour and for previews
// reduceToDevice, and a draw step, drawImage. Printing prepares each raster before
// it starts the sheet, and the preview keeps prepared rasters between paints.

// Colour keeps the source unchanged. Grayscale and black and white composite
// transparency over white first. Black and white uses a 128 luminance threshold.
// A null result means the conversion failed, normally for lack of memory.
QImage convertColour(const QImage &source, ColourMode mode);

// Reduces a raster to deviceSize with area sampling when it is larger in both
// dimensions, which keeps thin lines that QPainter's bilinear sampling can skip
// when it reduces by several times. Returns the raster itself otherwise, and null
// if the reduction fails.
QImage reduceToDevice(const QImage &image, const QSize &deviceSize);

// Draws a prepared raster into `target` within the existing painter clip and
// printableRect, without filling the paper. The caller supplies the paper origin
// and any points-to-device transform. smoothPreview allows display antialiasing of
// binary output. Leave it false for printing so black and white source samples
// remain binary when scaled.
void drawImage(QPainter &painter, const QImage &prepared, const QRectF &target,
               const QRectF &printableRect, ColourMode mode, bool smoothPreview = false);

} // namespace mervin::printing
