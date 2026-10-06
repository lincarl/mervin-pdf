#pragma once

#include <QImage>
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

// Colour keeps the source unchanged. Grayscale and black and white composite
// transparency over white first. Black and white uses a 128 luminance threshold.
QImage convertColour(const QImage &source, ColourMode mode);

// Draws within the existing painter clip and printableRect without filling the
// paper. The caller supplies the paper origin and any points-to-device transform.
// smoothPreview allows display antialiasing of binary output. Leave it false for
// printing so black and white source samples remain binary when scaled.
void paintPage(QPainter &painter, const QImage &image, const QSizeF &pagePoints,
               const QRectF &printableRect, const Settings &settings, bool smoothPreview = false);

} // namespace mervin::printing
