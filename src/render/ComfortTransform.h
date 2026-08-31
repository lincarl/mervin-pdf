#pragma once

#include <QImage>
#include <QRect>
#include <QVector>

namespace mervin {

// The "Comfort" document theme's pixel transform (final form chosen in the
// comfort-image-variants exploration, 2026-07). Three per-pixel treatments,
// selected by where the pixel sits:
//
// - Page content (outside embedded-image rectangles): the "ink" treatment.
//   Neutral pixels take the offset negative - each channel inverted onto
//   [#181A1E, #D6D9DE], so white paper lands on the dark background and
//   black ink on the off-white text colour, bit-identical to the previous
//   uniform Comfort on greys. Colourful pixels are unmixed into ink + paper
//   coverage: dark inks (coloured text/lines) are re-composited over the
//   dark background, which kills the white anti-alias halo around coloured
//   glyphs; light inks (pale fills) keep their authored colour.
//
// - Inside embedded-image rectangles, one of three per-image modes decided
//   by the render worker from the EMBEDDED image's pixels (so every zoom
//   level and deep-zoom tile agrees). A picture must not change colour, so
//   the default is to leave it alone and only sink its white/transparent
//   backdrop into the page:
//   - PhotoOnWhite: the image is shown authored and its white backdrop is
//     ramped into the dark page (a dead-zone whiteness ramp that also
//     swallows JPEG ringing at object edges). A transparent backdrop lands
//     here too - over white paper it renders as white.
//   - KeepAuthored: an image with no white backdrop to remove (a full-bleed
//     photo or render) is left completely untouched.
//   - Split: a soft Oklab chroma gate between the offset negative and the
//     authored pixel, i.e. neutral content inverts like the page. Reserved
//     for content that would go INVISIBLE if kept authored on a dark page:
//     scanned pages, line art, plots and flat dark artwork on white (see
//     comfortImageMode). This is the v1.36.0 lesson - a page that IS a scan
//     must still invert - scoped to exactly the images that need it.
//
// The page treatment and Split are bit-identical on neutral pixels, so
// those rectangle boundaries are invisible on paper/greys, and nothing
// outside a KeepAuthored/PhotoOnWhite photo is ever un-inverted.
//
// (History worth keeping: v1.36.0 deleted image handling altogether because a
// whole-page scan was being un-inverted into invisibility, and v1.37-v1.38
// brought it back photo-first - only images that positively looked like a
// product photo were spared. That made every image the user cared about but
// the classifier had not seen - wide crops, dark subjects, flat renders - come
// out as a negative. The polarity below is the fix: keep the picture, and
// invert only what would otherwise disappear.)
//
// `image` is the page render (Format_RGB888 from the worker; converted if
// not). Safe for whole-page renders and clipped deep-zoom tiles alike.

// Endpoints of the offset invert that the ink treatment (and Split's neutral
// half) maps every channel into: an input channel at 255 lands on kRampBg, an
// input at 0 on kRampFg. Exposed here so the viewer can back a not-yet-rendered
// page with the exact backdrop the pixel pass produces (surfaced as
// theme::doc().paperComfort) instead of re-typing the literal. Both values are
// pinned by tst_comfort_transform.
namespace comfort {
inline constexpr int kRampBg[3] = {24, 26, 30};    // #181A1E - white paper lands here
inline constexpr int kRampFg[3] = {214, 217, 222}; // #D6D9DE - black ink lands here
} // namespace comfort

// PageInk is not an image treatment but an override: it marks the region of an
// image rect that page TEXT was drawn over - a callout label, a caption, a
// title set on a banner image. That text belongs to the page, so the region
// takes the page treatment and reads like every other line of text in the
// document. It outranks the three image modes.
enum class ComfortImageMode : quint8 { Split, KeepAuthored, PhotoOnWhite, PageInk };

// One embedded raster image's placement: its device-pixel rectangle
// (relative to the render's top-left, may extend outside it) and the
// per-image treatment decision made by the render worker. For PhotoOnWhite,
// rampLo/rampHi parameterize the backdrop ramp
// alpha = smoothstep(rampLo, rampHi, distance-from-white); the two are chosen
// per image by comfortBackdropRamp.
struct ComfortImageRect
{
    QRect rect;
    ComfortImageMode mode = ComfortImageMode::Split;
    quint8 rampLo = 8;
    quint8 rampHi = 24;
};

// Applies the transform without image rectangles: every pixel gets the ink
// treatment. Used where no display list is available (and by tests that pin
// the pixel rule).
void applyComfortTransform(QImage &image);

// The full transform: ink outside `imageRects`, the per-mode treatment
// inside them.
void applyComfortTransform(QImage &image, const QVector<ComfortImageRect> &imageRects);

// What the render worker measures about one embedded image to choose its
// mode. Everything is measured on the image composited over WHITE (its
// soft mask / alpha applied), because that is what the page render shows:
// a transparent backdrop over white paper IS a white backdrop.
//
// The "non-white" pixels referred to below are the image's content: those
// further than 8 from white (255 - min channel > 8). Normalising to content
// rather than to the whole image is what makes the features comparable
// between a tightly cropped photo and one floating in white space.
struct ComfortImageFeatures
{
    // Backdrop: fraction of grid samples with min channel >= 250.
    float whiteFrac = 0.0f;
    // Thin-mark fraction: content pixels with a near-white pixel within 3
    // px horizontally. Strokes, glyphs, plot lines, hatching and scanner
    // grain are nearly all edge; a photo or render subject has an interior.
    float strokeFrac = 0.0f;
    // Smooth shading: content pixel pairs inside MONOTONE runs (>= 4 steps
    // in one direction, each of luminance 1..24). Lit surfaces shade;
    // flat vector artwork does not, and grain/dither alternates direction.
    float gradFrac = 0.0f;
    // Border ring: fraction of the outermost pixel ring within 8 of white.
    // This is the direct test for "does this image have a white backdrop".
    float ringWhiteFrac = 0.0f;
    // Union of every image rect on the PAGE (not this render - tiles must
    // agree with whole-page renders) over the page area. A scanned page is
    // wall-to-wall image; a document with pictures in it is not.
    float pageCoverage = 0.0f;
};

// Picks the treatment for one embedded image. Keeping a picture authored is
// the default; Split (inversion) is chosen only where authored content would
// vanish into the dark page:
//   - the images cover the page => it is a scanned page, invert it;
//   - content on a white backdrop that reads as ink rather than as a
//     picture: thin marks (strokeFrac) or no shading at all (gradFrac).
// Corpus separation, measured over examples/: scan strips stroke 0.49-0.99
// (and coverage 1.00), a line-art plot stroke 0.38, a logo on white stroke
// 0.33-0.64, flat logo artwork grad 0.0000-0.0005 - against photos and
// product renders at stroke <= 0.15 and grad 0.024-0.80. Every image the
// "photo first" classifier used to miss (wide crops, black subjects, flat
// CAD renders, anything with a soft mask) now lands on PhotoOnWhite or
// KeepAuthored.
//
// A dithered or grainy scan is caught by strokeFrac, not by a hard-edge
// count: its marks are isolated pixels, so nearly all of them have white
// within reach. A separate hard-step feature was tried and dropped - real
// photographs with crisp silhouettes score as high as line art does.
ComfortImageMode comfortImageMode(const ComfortImageFeatures &f);

// Cheap gates that let the render worker skip measurement it cannot use, so
// the thresholds still live in exactly one place:
// - with this much of the page covered by images the verdict is Split whatever
//   the pixels say, so an image needs no probe at all;
// - the full-resolution walk (strokeFrac/gradFrac for the ink test, and the
//   ringing measurements the backdrop ramp needs) is only worth doing for an
//   image that could be ink on white, or that has a backdrop to remove.
bool comfortScannedPageCoverage(float pageCoverage);
bool comfortNeedsInkFeatures(float whiteFrac);
bool comfortHasWhiteBackdrop(float ringWhiteFrac);

// Chooses the PhotoOnWhite backdrop ramp for one image. The hard part is that
// lossy compression leaves a band of near-white pixels hugging every dark
// silhouette (JPEG ringing), and a photograph of a WHITE object has near-white
// pixels of its own - the two are the same values, so one fixed dead zone
// cannot serve both. Sinking too little leaves a ragged bright halo around the
// subject; sinking too much eats the subject's white faces (the "bleed" of
// v1.38.x).
//
// The way out is that the choice only matters when there is white subject
// content to lose, and then the image has plenty of near-white pixels to
// measure. So:
// - ringingFrac / nearWhiteFrac - what share of the near-white pixels are
//   RINGING, i.e. sandwiched between pure white and dark content within a few
//   pixels - decides. Corpus: a CMYK photo of a grey part scores 0.30, tinted
//   product shots 0.66-0.84, while photos of white housings score 0.002-0.088.
// - Ringing-dominated images get an aggressive dead zone: they have almost no
//   near-white content of their own (<= 1.5% of pixels), so nothing is lost.
// - Everything else gets a ramp calibrated to the border ring's own noise
//   (99th percentile + 1, width 4), which is (1, 5) for a clean backdrop and
//   keeps subject faces sitting 2-5 values from white.
void comfortBackdropRamp(float nearWhiteFrac, float ringingFrac, int ringNoiseP99, quint8 *rampLo,
                         quint8 *rampHi);

} // namespace mervin
