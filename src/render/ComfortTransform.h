#pragma once

#include <QImage>
#include <QRect>
#include <QVector>

namespace mervin {

// Comfort tones RGB888 page renders (converting other formats as needed).
// Outside images, neutral pixels map white/black to #181A1E/#D6D9DE. Dark coloured inks are
// recomposited over the dark page without white halos; pale fills retain their colour.
// Embedded-image modes are chosen from source pixels, independent of zoom or clipping:
// PhotoOnWhite removes a white/transparent backdrop; KeepAuthored preserves images without one;
// Split inverts scans, line art and flat artwork that would disappear on dark paper.
// Ink and Split agree exactly on neutral pixels, avoiding seams at rectangle boundaries.

// Channel endpoints for Ink and neutral Split: 255 maps to kRampBg, 0 to kRampFg. The viewer
// uses the same backdrop via theme::doc().paperComfort; tests pin both values.
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

// Device-pixel image rectangle relative to the render origin; may extend outside it.
// PhotoOnWhite alpha uses smoothstep(rampLo, rampHi, distance-from-white), selected by
// comfortBackdropRamp.
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

// Embedded-image classification features, measured after alpha/soft-mask compositing over
// white. Non-white content has 255-min(channel) > 8. Normalize features to content rather than
// full image area so white margins do not change classification.
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

// Default to authored pictures; use Split when page coverage indicates a scan, or a white
// backdrop surrounds thin strokes/flat artwork. Corpus strokeFrac: scans 0.49-0.99, line art
// 0.33-0.64, photos <=0.15; gradFrac: flat art <=0.0005, photos 0.024-0.80. Stroke proximity
// catches dithered scans; hard-edge counts also catch crisp photos and are unsuitable.
ComfortImageMode comfortImageMode(const ComfortImageFeatures &f);

// Shared early-out thresholds: scan coverage forces Split without probing; full-resolution
// ink/ringing analysis is only needed for possible ink-on-white or removable backdrops.
bool comfortScannedPageCoverage(float pageCoverage);
bool comfortNeedsInkFeatures(float whiteFrac);
bool comfortHasWhiteBackdrop(float ringWhiteFrac);

// Choose a per-image backdrop ramp without erasing white subjects. The
// ringingFrac/nearWhiteFrac ratio distinguishes JPEG edge noise (corpus: 0.30-0.84) from white
// housings (0.002-0.088). Ringing-dominated images with <=1.5% near-white content use an
// aggressive dead zone. Otherwise use border-noise p99 + 1, width 4: clean backdrops get (1,
// 5), preserving faces 2-5 values from white.
void comfortBackdropRamp(float nearWhiteFrac, float ringingFrac, int ringNoiseP99, quint8 *rampLo,
                         quint8 *rampHi);

} // namespace mervin
