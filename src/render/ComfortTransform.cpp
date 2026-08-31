#include "render/ComfortTransform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <thread>
#include <utility>
#include <vector>

namespace mervin {

namespace {

// Endpoints of the offset invert: an input channel at 255 maps to the comfort
// background value, an input at 0 to the soft text value, so white paper lands
// on kBg and black ink on kFg. Defined in the header (comfort::kRampBg/kRampFg)
// because the viewer needs the backdrop to paint unrendered page area.
constexpr const auto &kBg = comfort::kRampBg;
constexpr const auto &kFg = comfort::kRampFg;

// Tuning constants, chosen by visual A/B on the example corpus (see the
// comfort-image-variants exploration, 2026-07):
// - the chroma gate: pixels below kChromaC0 Oklab chroma take the neutral
//   negative, above kChromaC1 the colour treatment, smoothstep between.
//   Measured JPEG background noise on the corpus stays under 0.002 (p99),
//   a 10x margin below the gate.
constexpr float kChromaC0 = 0.02f;
constexpr float kChromaC1 = 0.06f;
// - the ink-lightness band: unmixed inks darker than kInkLightS0 Oklab L are
//   re-composited over the dark page (halo-free coloured text), lighter than
//   kInkLightS1 keep the authored pixel (pale fills, bright photo bodies).
//   Corpus landmarks: green wire ink 0.67, bright photo body 0.81.
constexpr float kInkLightS0 = 0.65f;
constexpr float kInkLightS1 = 0.82f;

// ---------------------------------------------------------------------------
// The neutral tone map: the plain colour negative (what the Inverted theme
// does), offset so it lands on the comfort greys instead of pure black/white.
// Each channel is inverted independently and scaled into [kBg, kFg]:
//   tone_c(v) = kBg_c + round((255 - v) * (kFg_c - kBg_c) / 255)
// White paper -> kBg, black ink -> kFg.
//
// For an exact grey (r = g = b = v) this is bit-identical to every previous
// Comfort map (their luminance invert kept greys on the grey axis and used
// the same ramp), so grey rendering has never changed across versions. The
// rounding is half-up and no half-way ties exist: a tie would need
// 2*(255-v)*(kFg_c-kBg_c), an even number, to be congruent to 255 (odd)
// mod 510 (even) - impossible.
// ---------------------------------------------------------------------------

struct ChannelLut
{
    uint8_t v[256];
};

constexpr ChannelLut makeLut(int c)
{
    ChannelLut lut{};
    for (int i = 0; i < 256; ++i)
        lut.v[i] = static_cast<uint8_t>(kBg[c] + ((255 - i) * (kFg[c] - kBg[c]) + 127) / 255);
    return lut;
}

constexpr ChannelLut kLut[3] = {makeLut(0), makeLut(1), makeLut(2)};

static_assert(kLut[0].v[255] == 24 && kLut[1].v[255] == 26 && kLut[2].v[255] == 30,
              "white paper must map to the comfort background #181A1E");
static_assert(kLut[0].v[0] == 214 && kLut[1].v[0] == 217 && kLut[2].v[0] == 222,
              "black ink must map to the comfort text colour #D6D9DE");

// --- sRGB -> Oklab helpers (float; used to build lookup tables once) --------

float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

struct Oklab
{
    float L, a, b;
};

Oklab srgbToOklab(float r, float g, float b)
{
    r = srgbToLinear(r);
    g = srgbToLinear(g);
    b = srgbToLinear(b);
    const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
            1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}

float smoothstepf(float e0, float e1, float x)
{
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// --- 3D lookup lattices over RGB (33 nodes/axis, trilinear interpolation) ---
// The transform needs per-pixel Oklab quantities; evaluating them exactly
// would cost 3 cbrt per pixel. Both are smooth scalar functions of RGB, so a
// 33^3 node lattice with trilinear interpolation reproduces them visually
// exactly at a tiny fraction of the cost - the same technique colour-grading
// LUTs use. Built lazily on first use (thread-safe magic statics).

constexpr int kGrid = 33; // nodes per axis, spaced 255/32 apart

struct WeightLut3
{
    float w[kGrid][kGrid][kGrid];
};

template <typename F> WeightLut3 *buildWeightLut(F nodeValue)
{
    auto *lut = new WeightLut3;
    for (int ri = 0; ri < kGrid; ++ri)
        for (int gi = 0; gi < kGrid; ++gi)
            for (int bi = 0; bi < kGrid; ++bi)
                lut->w[ri][gi][bi] = nodeValue(srgbToOklab(ri / 32.0f, gi / 32.0f, bi / 32.0f));
    return lut;
}

float sampleWeightF(const WeightLut3 &lut, float r, float g, float b)
{
    const float fr = r * (32.0f / 255.0f);
    const float fg = g * (32.0f / 255.0f);
    const float fb = b * (32.0f / 255.0f);
    const int r0 = std::min(31, static_cast<int>(fr));
    const int g0 = std::min(31, static_cast<int>(fg));
    const int b0 = std::min(31, static_cast<int>(fb));
    const float tr = fr - r0, tg = fg - g0, tb = fb - b0;
    float c[2][2];
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            c[i][j] = lut.w[r0 + i][g0 + j][b0] * (1 - tb) + lut.w[r0 + i][g0 + j][b0 + 1] * tb;
    const float c0 = c[0][0] * (1 - tg) + c[0][1] * tg;
    const float c1 = c[1][0] * (1 - tg) + c[1][1] * tg;
    return c0 * (1 - tr) + c1 * tr;
}

float sampleWeight(const WeightLut3 &lut, const uint8_t *px)
{
    return sampleWeightF(lut, px[0], px[1], px[2]);
}

// w = smoothstep(kChromaC0, kChromaC1, Oklab chroma): 0 = neutral (take the
// offset negative), 1 = colourful (take the colour treatment).
const WeightLut3 &chromaGateLut()
{
    static const WeightLut3 *lut = buildWeightLut(
        [](const Oklab &lab) { return smoothstepf(kChromaC0, kChromaC1, std::hypot(lab.a, lab.b)); });
    return *lut;
}

// s = smoothstep(kInkLightS0, kInkLightS1, Oklab L), sampled at the unmixed
// ink colour: 0 = dark ink (composite over the dark page), 1 = light ink
// (keep the authored pixel).
const WeightLut3 &inkLightnessLut()
{
    static const WeightLut3 *lut = buildWeightLut(
        [](const Oklab &lab) { return smoothstepf(kInkLightS0, kInkLightS1, lab.L); });
    return *lut;
}

// Pixels this close to the grey axis have Oklab chroma far below kChromaC0
// (max-min 2 is ~0.005), so they take the plain offset-negative LUTs -
// bit-exact with every previous Comfort's grey rendering, and much cheaper
// than a lattice lookup. This is the dominant path on text pages.
bool nearNeutral(const uint8_t *px)
{
    const int mx = std::max({px[0], px[1], px[2]});
    const int mn = std::min({px[0], px[1], px[2]});
    return mx - mn <= 2;
}

// --- the ink treatment (outside image rects) ---------------------------------
// Each pixel is unmixed into ink over white paper (alpha = (255-min)/255; the
// ink always has a zero channel, greys unmix to pure black ink). Neutral
// pixels take the offset negative. Colourful pixels: dark inks - coloured
// text and lines - are re-composited over the comfort background with their
// paper coverage, so the anti-alias fringe blends glyph -> dark page with no
// white halo; light inks - pale fills like schematic component boxes - keep
// the authored pixel and stay bright.
void inkRow(uint8_t *px, int width)
{
    const WeightLut3 &wlut = chromaGateLut();
    const WeightLut3 &slut = inkLightnessLut();
    for (int x = 0; x < width; ++x, px += 3) {
        if (nearNeutral(px)) {
            px[0] = kLut[0].v[px[0]];
            px[1] = kLut[1].v[px[1]];
            px[2] = kLut[2].v[px[2]];
            continue;
        }
        const float w = sampleWeight(wlut, px);
        const int mn = std::min({px[0], px[1], px[2]});
        const float alpha = (255 - mn) / 255.0f;
        float ink[3];
        for (int c = 0; c < 3; ++c)
            ink[c] = std::max(0.0f, 255.0f - (255.0f - px[c]) / alpha);
        const float s = sampleWeightF(slut, ink[0], ink[1], ink[2]);
        for (int c = 0; c < 3; ++c) {
            const float comfort = kLut[c].v[px[c]];
            const float composite = alpha * ink[c] + (1.0f - alpha) * kBg[c];
            const float keep = s * px[c] + (1.0f - s) * composite;
            px[c] = static_cast<uint8_t>(
                std::clamp(comfort + (keep - comfort) * w + 0.5f, 0.0f, 255.0f));
        }
    }
}

// --- the photo treatment (inside image rects) --------------------------------
// Soft chroma gate between the offset negative and the authored pixel:
// neutral image content (greyscale photos, scans, white backdrops) inverts
// exactly like the page, colourful content keeps its authored colours. No
// unmixing here - a photo's colours are not paper-diluted ink, and keeping
// them at full authored brightness is what reads as "the picture, on a dark
// page".
void photoRow(uint8_t *px, int width)
{
    const WeightLut3 &wlut = chromaGateLut();
    for (int x = 0; x < width; ++x, px += 3) {
        if (nearNeutral(px)) {
            px[0] = kLut[0].v[px[0]];
            px[1] = kLut[1].v[px[1]];
            px[2] = kLut[2].v[px[2]];
            continue;
        }
        const float w = sampleWeight(wlut, px);
        for (int c = 0; c < 3; ++c) {
            const float comfort = kLut[c].v[px[c]];
            px[c] = static_cast<uint8_t>(comfort + (px[c] - comfort) * w + 0.5f);
        }
    }
}

// --- the photo-on-white treatment (PhotoOnWhite rects) -----------------------
// The photo is shown fully authored; only its white backdrop is ramped into
// the comfort background: alpha = smoothstep(lo, hi, 255 - min(R,G,B)).
// The dead zone up to `lo` makes the backdrop's noise fully transparent;
// distance >= `hi` is the authored pixel. See ComfortImageRect for how
// lo/hi are chosen per image.
void photoOnWhiteRow(uint8_t *px, int width, int lo, int hi)
{
    for (int x = 0; x < width; ++x, px += 3) {
        const int mn = std::min({px[0], px[1], px[2]});
        const int dist = 255 - mn;
        if (dist >= hi)
            continue; // authored, untouched
        const int a = dist <= lo
            ? 0
            : static_cast<int>(std::lround(smoothstepf(float(lo), float(hi), float(dist)) * 256.0f));
        for (int c = 0; c < 3; ++c)
            px[c] = static_cast<uint8_t>((a * px[c] + (256 - a) * kBg[c] + 128) >> 8);
    }
}

// How many threads to tone `pixels` with. The pass runs on one of
// RenderEngine's clamp(hw/2, 2, 4) render workers, any of which may be toning
// a page at the same time; sizing the fan-out to hw/workers lets all workers
// together roughly fill the machine instead of oversubscribing it. Small
// images stay single-threaded (below ~1 MPx the spawn/join overhead is not
// worth it).
int comfortThreadCount(long long pixels)
{
    if (pixels < 1000000)
        return 1;
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned workers = std::clamp(hw / 2u, 2u, 4u);
    const long long n = std::max(1u, hw / workers);
    return static_cast<int>(std::min(n, pixels / 1000000));
}

// Runs proc(rowPtr, y) for every row, fanned out over comfortThreadCount
// threads. proc must be safe to call concurrently for distinct rows.
template <typename RowProc>
void runBanded(uint8_t *bits, qsizetype stride, int width, int height, const RowProc &proc)
{
    const int nthreads = comfortThreadCount(static_cast<long long>(width) * height);
    if (nthreads <= 1) {
        for (int y = 0; y < height; ++y)
            proc(bits + y * stride, y);
        return;
    }

    const int rowsPer = (height + nthreads - 1) / nthreads;
    std::vector<std::thread> helpers;
    int helperEnd = rowsPer; // rows [rowsPer, helperEnd) are covered by helpers
    try {
        helpers.reserve(nthreads - 1);
        for (int t = 1; t < nthreads; ++t) {
            const int y0 = t * rowsPer;
            const int y1 = std::min(height, y0 + rowsPer);
            if (y0 >= y1)
                break;
            helpers.emplace_back([bits, stride, y0, y1, &proc] {
                for (int y = y0; y < y1; ++y)
                    proc(bits + y * stride, y);
            });
            helperEnd = y1;
        }
    } catch (const std::exception &) {
        // Thread creation failed (resource limits). Rendering must not crash
        // for that: the bands that got no helper are toned on this thread
        // below.
    }
    // The calling render worker tones the first band itself, then any bands
    // left uncovered by a failed spawn.
    for (int y = 0; y < std::min(rowsPer, height); ++y)
        proc(bits + y * stride, y);
    for (int y = helperEnd; y < height; ++y)
        proc(bits + y * stride, y);
    for (std::thread &h : helpers)
        h.join();
}

// --- interval helpers for the row splitter -----------------------------------

using Interval = std::pair<int, int>; // [first, second)

void mergeIntervals(std::vector<Interval> &iv)
{
    if (iv.size() <= 1)
        return;
    std::sort(iv.begin(), iv.end());
    size_t out = 0;
    for (size_t i = 1; i < iv.size(); ++i) {
        if (iv[i].first <= iv[out].second)
            iv[out].second = std::max(iv[out].second, iv[i].second);
        else
            iv[++out] = iv[i];
    }
    iv.resize(out + 1);
}

// a minus b; both merged and sorted. Result stays merged and sorted.
std::vector<Interval> subtractIntervals(const std::vector<Interval> &a,
                                        const std::vector<Interval> &b)
{
    std::vector<Interval> out;
    size_t j = 0;
    for (const Interval &ai : a) {
        int cur = ai.first;
        while (j < b.size() && b[j].second <= cur)
            ++j;
        for (size_t k = j; k < b.size() && b[k].first < ai.second; ++k) {
            if (b[k].first > cur)
                out.emplace_back(cur, b[k].first);
            cur = std::max(cur, b[k].second);
        }
        if (cur < ai.second)
            out.emplace_back(cur, ai.second);
    }
    return out;
}

} // namespace

void applyComfortTransform(QImage &image)
{
    applyComfortTransform(image, {});
}

namespace {
// Thresholds sit in the measured corpus gaps (see ComfortTransform.h for the
// landmarks). Each is a "would this go invisible?" test, not a "does this look
// like a photograph?" test - the classifier only has to recognise ink, and
// everything it does not recognise keeps its colours.
constexpr float kScanCoverage = 0.60f;  // images cover the page: a scan
constexpr float kInkWhiteFrac = 0.30f;  // enough backdrop to be ink ON white
constexpr float kInkStrokeFrac = 0.30f; // thin marks: strokes, glyphs, grain
constexpr float kInkGradFrac = 0.004f;  // no shading at all: flat artwork
constexpr float kBackdropRingFrac = 0.40f; // a white backdrop to remove
// Share of an image's near-white pixels that must be compression ringing
// before its backdrop gets the aggressive dead zone (see comfortBackdropRamp).
constexpr float kRingingShare = 0.12f;
constexpr quint8 kRingingRampLo = 24; // measured: removes 97% of the halo on
constexpr quint8 kRingingRampHi = 40; // the corpus's worst case (a CMYK photo)
} // namespace

bool comfortScannedPageCoverage(float pageCoverage)
{
    return pageCoverage >= kScanCoverage;
}

bool comfortNeedsInkFeatures(float whiteFrac)
{
    return whiteFrac >= kInkWhiteFrac;
}

bool comfortHasWhiteBackdrop(float ringWhiteFrac)
{
    return ringWhiteFrac >= kBackdropRingFrac;
}

void comfortBackdropRamp(float nearWhiteFrac, float ringingFrac, int ringNoiseP99, quint8 *rampLo,
                         quint8 *rampHi)
{
    // Written as a product so an image with no near-white pixels at all - a
    // photo of a black part - takes the aggressive branch without dividing by
    // zero. It has nothing to lose either way.
    if (ringingFrac >= kRingingShare * nearWhiteFrac) {
        *rampLo = kRingingRampLo;
        *rampHi = kRingingRampHi;
        return;
    }
    *rampLo = static_cast<quint8>(std::min(ringNoiseP99 + 1, 8));
    *rampHi = static_cast<quint8>(*rampLo + 4);
}

ComfortImageMode comfortImageMode(const ComfortImageFeatures &f)
{
    if (f.pageCoverage >= kScanCoverage)
        return ComfortImageMode::Split;
    if (f.whiteFrac >= kInkWhiteFrac
        && (f.strokeFrac >= kInkStrokeFrac || f.gradFrac < kInkGradFrac))
        return ComfortImageMode::Split;
    if (f.ringWhiteFrac >= kBackdropRingFrac)
        return ComfortImageMode::PhotoOnWhite;
    return ComfortImageMode::KeepAuthored;
}

void applyComfortTransform(QImage &image, const QVector<ComfortImageRect> &imageRects)
{
    if (image.isNull())
        return;
    if (image.format() != QImage::Format_RGB888)
        image = image.convertToFormat(QImage::Format_RGB888);

    const int width = image.width();
    const int height = image.height();
    uint8_t *bits = image.bits(); // detach once; scanlines are bits + y*stride
    const qsizetype stride = image.bytesPerLine();

    if (imageRects.isEmpty()) {
        runBanded(bits, stride, width, height,
                  [width](uint8_t *row, int) { inkRow(row, width); });
        return;
    }

    // Rows are split into segments - ink outside the image rectangles, the
    // per-mode treatment inside them: Split (chroma-gated), PhotoOnWhite
    // (authored + backdrop ramp), KeepAuthored (nothing at all). Ink and
    // Split are bit-identical on neutral pixels, so those segment boundaries
    // cannot show on paper or grey content. Where rects overlap, the higher
    // mode wins: PageInk (page text drawn over a picture) > KeepAuthored >
    // PhotoOnWhite > Split.
    const std::vector<ComfortImageRect> rects(imageRects.begin(), imageRects.end());
    runBanded(bits, stride, width, height, [&rects, width](uint8_t *row, int y) {
        std::vector<Interval> keepIv, splitIv, inkIv; // x-intervals covering row y
        // PhotoOnWhite intervals grouped by ramp parameters (usually one or
        // two distinct ramps per page).
        struct PowGroup
        {
            int lo, hi;
            std::vector<Interval> iv;
        };
        std::vector<PowGroup> powGroups;
        for (const ComfortImageRect &cr : rects) {
            if (y < cr.rect.top() || y > cr.rect.bottom())
                continue;
            const int x0 = std::max(0, cr.rect.left());
            const int x1 = std::min(width, cr.rect.left() + cr.rect.width());
            if (x0 >= x1)
                continue;
            switch (cr.mode) {
            case ComfortImageMode::KeepAuthored: keepIv.emplace_back(x0, x1); break;
            case ComfortImageMode::PhotoOnWhite: {
                PowGroup *g = nullptr;
                for (PowGroup &cand : powGroups)
                    if (cand.lo == cr.rampLo && cand.hi == cr.rampHi)
                        g = &cand;
                if (!g) {
                    powGroups.push_back({cr.rampLo, cr.rampHi, {}});
                    g = &powGroups.back();
                }
                g->iv.emplace_back(x0, x1);
                break;
            }
            case ComfortImageMode::Split: splitIv.emplace_back(x0, x1); break;
            case ComfortImageMode::PageInk: inkIv.emplace_back(x0, x1); break;
            }
        }
        if (keepIv.empty() && powGroups.empty() && splitIv.empty()) {
            inkRow(row, width);
            return;
        }
        mergeIntervals(keepIv);
        for (PowGroup &g : powGroups)
            mergeIntervals(g.iv);
        mergeIntervals(splitIv);
        // Page text drawn over an image outranks every image treatment. No
        // segment is emitted for it: subtracting it from the image intervals
        // leaves a gap, and the loop below fills gaps with the ink treatment -
        // exactly what that text would get anywhere else on the page.
        if (!inkIv.empty()) {
            mergeIntervals(inkIv);
            keepIv = subtractIntervals(keepIv, inkIv);
            for (PowGroup &g : powGroups)
                g.iv = subtractIntervals(g.iv, inkIv);
            splitIv = subtractIntervals(splitIv, inkIv);
        }
        // Precedence where rects overlap: Keep > PhotoOnWhite (earlier ramp
        // group wins) > Split - each class is carved out of the lower ones.
        for (size_t gi = 0; gi < powGroups.size(); ++gi) {
            if (!keepIv.empty())
                powGroups[gi].iv = subtractIntervals(powGroups[gi].iv, keepIv);
            for (size_t gj = 0; gj < gi; ++gj)
                if (!powGroups[gj].iv.empty())
                    powGroups[gi].iv = subtractIntervals(powGroups[gi].iv, powGroups[gj].iv);
        }
        if (!keepIv.empty())
            splitIv = subtractIntervals(splitIv, keepIv);
        for (const PowGroup &g : powGroups)
            if (!g.iv.empty())
                splitIv = subtractIntervals(splitIv, g.iv);

        struct Seg
        {
            int s, e;
            ComfortImageMode mode;
            int lo, hi;
        };
        std::vector<Seg> segs;
        for (const Interval &v : keepIv)
            segs.push_back({v.first, v.second, ComfortImageMode::KeepAuthored, 0, 0});
        for (const PowGroup &g : powGroups)
            for (const Interval &v : g.iv)
                segs.push_back({v.first, v.second, ComfortImageMode::PhotoOnWhite, g.lo, g.hi});
        for (const Interval &v : splitIv)
            segs.push_back({v.first, v.second, ComfortImageMode::Split, 0, 0});
        std::sort(segs.begin(), segs.end(), [](const Seg &a, const Seg &b) { return a.s < b.s; });

        int cursor = 0;
        for (const Seg &seg : segs) {
            if (seg.s > cursor)
                inkRow(row + 3 * cursor, seg.s - cursor);
            if (seg.mode == ComfortImageMode::PhotoOnWhite)
                photoOnWhiteRow(row + 3 * seg.s, seg.e - seg.s, seg.lo, seg.hi);
            else if (seg.mode == ComfortImageMode::Split)
                photoRow(row + 3 * seg.s, seg.e - seg.s);
            // KeepAuthored: untouched
            cursor = std::max(cursor, seg.e);
        }
        if (cursor < width)
            inkRow(row + 3 * cursor, width - cursor);
    });
}

} // namespace mervin
