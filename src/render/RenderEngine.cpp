#include "render/RenderEngine.h"

#include "io/Background.h"
#include "io/JobContext.h"
#include "render/ComfortTransform.h"
#include "render/Document.h"
#include "render/MuPdfLocks.h"

#include <mupdf/fitz.h>

#include <QByteArray>
#include <QPointer>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace mervin {

static_assert(FZ_LOCK_MAX <= 4, "the lock array is too small for MuPDF's FZ_LOCK_MAX");

namespace {
std::atomic<int> attemptedOpens{0};

// Sized to MuPDF's FZ_LOCK_MAX (currently 3); 4 is a safe upper bound.
using LockArray = std::array<std::mutex, 4>;

void lockCallback(void *user, int lock)
{
    (*static_cast<LockArray *>(user))[lock].lock();
}

void unlockCallback(void *user, int lock)
{
    (*static_cast<LockArray *>(user))[lock].unlock();
}

void warningCallback(void *, const char *message)
{
    qDebug("MuPDF: %s", message);
}

// Harvest embedded-image bounds and source-pixel classifications for Comfort. fz_device must be
// first for pointer aliasing; fz_new_derived_device zero-allocates, so members must be
// trivially zeroable. Exclude stencil masks: their signatures and bilevel line art need PageInk
// treatment.
struct ImageDecision
{
    uint8_t mode;
    uint8_t rampLo;
    uint8_t rampHi;
};

struct ImageRectDevice
{
    fz_device base;
    std::vector<fz_rect> *rects;
    std::vector<fz_image *> *images; // parallel to rects, kept references
    std::vector<int> *imageSeq;     // parallel: paint-order index of the image
    std::vector<fz_rect> *overlays; // TEXT bboxes, for the PageInk regions
    std::vector<int> *overlaySeq;   // parallel: paint-order index of the text
    int seq;                        // running paint-order counter
};

// Sample decoded RGB over white, applying the separate MuPDF soft mask. Otherwise transparent
// image surrounds can be misclassified from their hidden base colour.
struct SampledImage
{
    int w = 0, h = 0;
    const unsigned char *samples = nullptr;
    int n = 0, comps = 0, stride = 0, alpha = 0;
    bool isGray = false; // otherwise RGB - anything else is converted first
    // Soft mask, at its own resolution (nearest-neighbour sampled).
    const unsigned char *mask = nullptr;
    int mw = 0, mh = 0, mn = 0, mstride = 0;

    bool usable() const { return samples && w >= 8 && h >= 8 && (isGray || comps == 3); }

    // Coverage (0-255) of the pixel at (x, y): the soft mask plus any alpha
    // channel the pixmap itself carries (colour-key masking, JPX alpha).
    int coverageAt(int x, int y) const
    {
        int cov = 255;
        if (mask) {
            const int mx = mw == w ? x : static_cast<int>(static_cast<int64_t>(x) * mw / w);
            const int my = mh == h ? y : static_cast<int>(static_cast<int64_t>(y) * mh / h);
            cov = mask[static_cast<ptrdiff_t>(mstride) * std::min(my, mh - 1)
                       + static_cast<ptrdiff_t>(mn) * std::min(mx, mw - 1)];
        }
        if (alpha) {
            const unsigned char *p = samples + static_cast<ptrdiff_t>(stride) * y
                + static_cast<ptrdiff_t>(n) * x;
            cov = cov * p[comps] / 255;
        }
        return cov;
    }

    // The pixel at (x, y) as RGB over white. Hot: the ink features walk every
    // pixel of up to 64 full-width rows, so this stays branch-light and does no
    // colour conversion - the pixmap is converted to RGB or grey up front.
    void rgbAt(int x, int y, uint8_t out[3]) const
    {
        const unsigned char *p = samples + static_cast<ptrdiff_t>(stride) * y
            + static_cast<ptrdiff_t>(n) * x;
        if (isGray) {
            out[0] = out[1] = out[2] = p[0];
        } else {
            out[0] = p[0];
            out[1] = p[1];
            out[2] = p[2];
        }
        const int cov = coverageAt(x, y);
        if (cov < 255)
            for (int c = 0; c < 3; ++c)
                out[c] = static_cast<uint8_t>((out[c] * cov + 255 * (255 - cov) + 127) / 255);
    }
};

// Distance from white, the quantity both the classifier's "content" test and
// the backdrop ramp are expressed in.
int distFromWhite(const uint8_t rgb[3])
{
    return 255 - std::min({rgb[0], rgb[1], rgb[2]});
}

// The features comfortImageMode needs, measured on the embedded image (never
// on the render, so a deep-zoom tile reaches the same verdict as a fit-width
// page - the v1.31.0 lesson), plus the backdrop ramp bounds. pageCoverage is
// filled in by the caller, which is the only place that knows the whole page.
struct ImageProbe
{
    ComfortImageFeatures f;
    uint8_t rampLo = 8;
    uint8_t rampHi = 24;
};

// Pixels this far from white count as backdrop rather than content.
constexpr int kWhiteSlack = 8;
// Half-width of the horizontal window strokeFrac and the ringing test look in.
constexpr int kStrokeReach = 3;
// The band the backdrop ramp has to decide about: near-white pixels that are
// either a white subject's own faces or compression ringing.
constexpr int kNearWhiteLo = 6;
constexpr int kNearWhiteHi = 40;
// Ringing is near-white sandwiched between pure white and clearly darker
// content; these bound the two neighbours.
constexpr int kPureWhite = 5;
constexpr int kRingingDark = 60;

ImageProbe embeddedImageProbe(fz_context *ctx, fz_image *img)
{
    ImageProbe out;
    // Heap-owning locals declared before fz_try: a longjmp out of the try
    // block does not run C++ destructors (cleanup happens at scope end).
    std::vector<uint8_t> dist;    // one row: distance from white per pixel
    std::vector<uint8_t> lum;     // one row: luminance per pixel
    std::vector<int32_t> whitePs; // one row: prefix sum of backdrop pixels
    std::vector<int32_t> purePs;  // one row: prefix sum of pure-white pixels
    std::vector<int32_t> darkPs;  // one row: prefix sum of clearly dark pixels
    // Inputs to the backdrop ramp, measured in pass 3 (0 when it is skipped -
    // an image with no white backdrop gets no ramp worth calibrating).
    float nearWhiteFrac = 0.0f;
    float ringingFrac = 0.0f;
    fz_pixmap *pix = nullptr;
    fz_pixmap *maskPix = nullptr;
    fz_var(pix);
    fz_var(maskPix);
    fz_try(ctx) {
        pix = fz_get_pixmap_from_image(ctx, img, nullptr, nullptr, nullptr, nullptr);
        // Convert non-RGB/grey colourspaces once. Check the colourspace type, not pointer
        // identity: ICC-based RGB is not fz_device_rgb and would otherwise incur per-pixel
        // conversions.
        fz_colorspace *cs = fz_pixmap_colorspace(ctx, pix);
        if (cs && !fz_colorspace_is_rgb(ctx, cs) && !fz_colorspace_is_gray(ctx, cs)) {
            fz_pixmap *conv = fz_convert_pixmap(ctx, pix, fz_device_rgb(ctx), nullptr, nullptr,
                                                fz_default_color_params, 1);
            fz_drop_pixmap(ctx, pix);
            pix = conv;
            cs = fz_pixmap_colorspace(ctx, pix);
        }
        SampledImage si;
        si.w = fz_pixmap_width(ctx, pix);
        si.h = fz_pixmap_height(ctx, pix);
        si.n = fz_pixmap_components(ctx, pix);
        si.alpha = fz_pixmap_alpha(ctx, pix);
        si.comps = si.n - si.alpha;
        si.stride = fz_pixmap_stride(ctx, pix);
        si.samples = cs ? fz_pixmap_samples(ctx, pix) : nullptr; // no colourspace: a stencil
        si.isGray = si.comps == 1;
        if (img->mask) {
            maskPix = fz_get_pixmap_from_image(ctx, img->mask, nullptr, nullptr, nullptr, nullptr);
            // A mask pixmap is single-component (grey or alpha-only) with
            // 255 = show the image. Anything else is not a mask we can read.
            if (fz_pixmap_components(ctx, maskPix) == 1) {
                si.mask = fz_pixmap_samples(ctx, maskPix);
                si.mw = fz_pixmap_width(ctx, maskPix);
                si.mh = fz_pixmap_height(ctx, maskPix);
                si.mn = 1;
                si.mstride = fz_pixmap_stride(ctx, maskPix);
            }
        }

        if (si.usable()) {
            const int w = si.w, h = si.h;
            uint8_t rgb[3];

            // Pass 1 - grid subsample (<= 64x64): the backdrop fraction.
            const int stepX = std::max(1, w / 64);
            const int stepY = std::max(1, h / 64);
            long long white = 0, count = 0;
            for (int y = 0; y < h; y += stepY)
                for (int x = 0; x < w; x += stepX) {
                    si.rgbAt(x, y, rgb);
                    white += std::min({rgb[0], rgb[1], rgb[2]}) >= 250;
                    ++count;
                }
            if (count > 0)
                out.f.whiteFrac = float(double(white) / count);

            // Pass 2 - the border ring: is there a white backdrop to remove,
            // and how noisy is it (the ramp calibration of v1.38.2).
            int histo[256] = {};
            long long ring = 0, ringWhite = 0;
            auto ringSample = [&](int x, int y) {
                si.rgbAt(x, y, rgb);
                const int d = distFromWhite(rgb);
                ++histo[d];
                ringWhite += d <= kWhiteSlack;
                ++ring;
            };
            for (int x = 0; x < w; ++x) {
                ringSample(x, 0);
                ringSample(x, h - 1);
            }
            for (int y = 1; y < h - 1; ++y) {
                ringSample(0, y);
                ringSample(w - 1, y);
            }
            if (ring > 0)
                out.f.ringWhiteFrac = float(double(ringWhite) / ring);

            // Pass 3 - up to 64 full-width rows: thin marks and smooth shading
            // for the ink test, plus how much of the near-white is compression
            // ringing for the backdrop ramp. This is the only full-resolution
            // walk in the probe; an image that can be neither ink on white nor
            // a picture with a backdrop to remove skips it entirely.
            if (comfortNeedsInkFeatures(out.f.whiteFrac)
                || comfortHasWhiteBackdrop(out.f.ringWhiteFrac)) {
                dist.resize(w);
                lum.resize(w);
                whitePs.resize(w + 1);
                purePs.resize(w + 1);
                darkPs.resize(w + 1);
                long long strokePx = 0, contentPx = 0;
                long long gradPairs = 0, contentPairs = 0;
                long long nearWhitePx = 0, ringingPx = 0, allPx = 0;
                const int rowStep = std::max(1, h / 64);
                for (int y = 0; y < h; y += rowStep) {
                    for (int x = 0; x < w; ++x) {
                        si.rgbAt(x, y, rgb);
                        dist[x] = static_cast<uint8_t>(distFromWhite(rgb));
                        lum[x] = static_cast<uint8_t>(
                            (77 * rgb[0] + 151 * rgb[1] + 28 * rgb[2]) >> 8);
                    }
                    whitePs[0] = purePs[0] = darkPs[0] = 0;
                    for (int x = 0; x < w; ++x) {
                        whitePs[x + 1] = whitePs[x] + (dist[x] <= kWhiteSlack ? 1 : 0);
                        purePs[x + 1] = purePs[x] + (dist[x] <= kPureWhite ? 1 : 0);
                        darkPs[x + 1] = darkPs[x] + (dist[x] > kRingingDark ? 1 : 0);
                    }
                    // Near-white pixels, and how many of them are ringing:
                    // sandwiched between pure white and dark content within a
                    // few pixels. A white subject face has neither neighbour.
                    allPx += w;
                    for (int x = 0; x < w; ++x) {
                        if (dist[x] < kNearWhiteLo || dist[x] > kNearWhiteHi)
                            continue;
                        ++nearWhitePx;
                        const int lo = std::max(0, x - kStrokeReach);
                        const int hi = std::min(w, x + kStrokeReach + 1);
                        if (purePs[hi] - purePs[lo] > 0 && darkPs[hi] - darkPs[lo] > 0)
                            ++ringingPx;
                    }
                    int run = 0, prevSign = 0;
                    for (int x = 0; x < w; ++x) {
                        if (dist[x] <= kWhiteSlack) {
                            run = 0;
                            prevSign = 0;
                            continue;
                        }
                        ++contentPx;
                        const int lo = std::max(0, x - kStrokeReach);
                        const int hi = std::min(w, x + kStrokeReach + 1);
                        strokePx += whitePs[hi] - whitePs[lo] > 0;
                        if (x == 0 || dist[x - 1] <= kWhiteSlack) {
                            run = 0;
                            prevSign = 0;
                            continue; // no content-content pair ending here
                        }
                        const int d = int(lum[x]) - int(lum[x - 1]);
                        const int ad = std::abs(d);
                        ++contentPairs;
                        if (ad >= 1 && ad <= 24) {
                            const int sign = d > 0 ? 1 : -1;
                            if (run == 0 || sign == prevSign) {
                                ++run;
                                if (run == 4)
                                    gradPairs += 4;
                                else if (run > 4)
                                    ++gradPairs;
                            } else {
                                run = 1;
                            }
                            prevSign = sign;
                        } else {
                            run = 0;
                            prevSign = 0;
                        }
                    }
                }
                if (contentPx > 0)
                    out.f.strokeFrac = float(double(strokePx) / contentPx);
                if (contentPairs > 0)
                    out.f.gradFrac = float(double(gradPairs) / contentPairs);
                if (allPx > 0) {
                    nearWhiteFrac = float(double(nearWhitePx) / allPx);
                    ringingFrac = float(double(ringingPx) / allPx);
                }
            }

            // The backdrop ramp, from the ringing share and the border ring's
            // 99th-percentile noise (see comfortBackdropRamp).
            int acc = 0, ringP99 = 255;
            for (int d = 0; d < 256 && ring > 0; ++d) {
                acc += histo[d];
                if (acc * 100 >= ring * 99) {
                    ringP99 = d;
                    break;
                }
            }
            comfortBackdropRamp(nearWhiteFrac, ringingFrac, ringP99, &out.rampLo, &out.rampHi);
        }
    }
    fz_always(ctx) {
        if (maskPix)
            fz_drop_pixmap(ctx, maskPix);
        if (pix)
            fz_drop_pixmap(ctx, pix);
    }
    fz_catch(ctx) {
        // Undecodable: no features. Zeroed features mean "no white backdrop,
        // no ink evidence", i.e. leave the picture alone - and a scanned page
        // still inverts, because pageCoverage does not depend on decoding.
        out = ImageProbe{};
    }
    return out;
}


void imageRectFillImage(fz_context *ctx, fz_device *dev, fz_image *img, fz_matrix ctm,
                        float alpha, fz_color_params)
{
    auto *d = reinterpret_cast<ImageRectDevice *>(dev);
    ++d->seq;
    if (alpha == 0.0f) // fully transparent placement: nothing visible to treat
        return;
    // Clip to the device's live scissor: cropped image frames (image drawn
    // under a rectangular clip path) must contribute their visible part only,
    // not the full uncropped placement - which can overlap body text. The
    // base device tracks the scissor stack even though this device implements
    // no clip callbacks.
    const fz_rect r = fz_intersect_rect(fz_transform_rect(fz_unit_rect, ctm),
                                        fz_device_current_scissor(ctx, dev));
    if (fz_is_empty_rect(r))
        return;
    // Record only; classifying here would decode every image on the page even
    // when a deep-zoom tile shows one of them, and the page-coverage feature
    // is not known until the whole list has been walked.
    d->rects->push_back(r);
    d->images->push_back(fz_keep_image(ctx, img));
    d->imageSeq->push_back(d->seq);
}

// Record text bounds and paint sequence to distinguish labels above images from hidden text
// below. Exclude paths: their bounds can be much larger than their ink.
void recordOverlay(fz_context *ctx, fz_device *dev, fz_rect bounds, float alpha)
{
    auto *d = reinterpret_cast<ImageRectDevice *>(dev);
    ++d->seq;
    if (alpha == 0.0f)
        return;
    const fz_rect r = fz_intersect_rect(bounds, fz_device_current_scissor(ctx, dev));
    if (fz_is_empty_rect(r))
        return;
    d->overlays->push_back(r);
    d->overlaySeq->push_back(d->seq);
}

void imageRectFillText(fz_context *ctx, fz_device *dev, const fz_text *text, fz_matrix ctm,
                       fz_colorspace *, const float *, float alpha, fz_color_params)
{
    recordOverlay(ctx, dev, fz_bound_text(ctx, text, nullptr, ctm), alpha);
}

void imageRectStrokeText(fz_context *ctx, fz_device *dev, const fz_text *text,
                         const fz_stroke_state *stroke, fz_matrix ctm, fz_colorspace *,
                         const float *, float alpha, fz_color_params)
{
    recordOverlay(ctx, dev, fz_bound_text(ctx, text, stroke, ctm), alpha);
}

} // namespace

struct RenderEngine::Shared
{
    struct Job
    {
        RenderRequest request;
        std::shared_ptr<DocumentLifetime> lifetime;
        std::atomic<bool> canceled{false};
    };

    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<std::shared_ptr<Job>> queue; // guarded by queueMutex
    std::vector<std::weak_ptr<Job>> jobs;   // includes active jobs; guarded by queueMutex
    std::atomic<bool> stop{false};
    QPointer<QObject> owner; // the engine, which results go to on its thread
};

const fz_locks_context *sharedMuPdfLocks()
{
    // Never destroyed, for contexts that outlive their owner (see the header).
    static const fz_locks_context *locks = [] {
        auto *context = new fz_locks_context;
        context->user = new LockArray;
        context->lock = lockCallback;
        context->unlock = unlockCallback;
        return context;
    }();
    return locks;
}

RenderEngine::RenderEngine(QObject *parent)
    : QObject(parent)
    , shared_(std::make_shared<Shared>())
{
    qRegisterMetaType<mervin::RenderResult>("mervin::RenderResult");

    fz_context *base = fz_new_context(nullptr, sharedMuPdfLocks(), FZ_STORE_DEFAULT);
    if (!base)
        qFatal("Mervin: failed to create MuPDF context");
    fz_set_warning_callback(base, warningCallback, nullptr);
    fz_register_document_handlers(base);
    base_.reset(base, fz_drop_context);

    shared_->owner = this;
    const unsigned hw = std::thread::hardware_concurrency();
    const int nworkers = static_cast<int>(std::clamp(hw / 2u, 2u, 4u));
    for (int i = 0; i < nworkers; ++i) {
        fz_context *ctx = fz_clone_context(base);
        background::start([shared = shared_, base = base_, ctx]() mutable {
            workerLoop(shared, ctx);
            base.reset(); // after the clone (see baseContext())
        });
    }
}

RenderEngine::~RenderEngine()
{
    // Workers and documents can outlive the engine; they keep the base context.
    shutdown();
}

void RenderEngine::shutdown()
{
    shared_->stop.store(true);
    {
        std::lock_guard<std::mutex> lk(shared_->queueMutex);
        for (auto &weak : shared_->jobs)
            if (auto job = weak.lock())
                job->canceled.store(true);
        shared_->queue.clear();
    }
    shared_->queueCv.notify_all();
}

// Opens through an independent context so the request does not retain this engine.
std::unique_ptr<Document> RenderEngine::openDocument(const QString &path, const QString &password,
                                                     QString *error, bool *needsPassword)
{
    return opener()->open(path, password, error, needsPassword);
}

// Captures an independent opener whose context can outlive this render engine.
std::shared_ptr<DocumentOpener> RenderEngine::opener()
{
    return std::make_shared<DocumentOpener>(base_);
}

// Clones a retained base context before one background open begins.
DocumentOpener::DocumentOpener(std::shared_ptr<fz_context> base)
    : base_(std::move(base))
    , ctx_(fz_clone_context(base_.get()))
{
}

// Releases the cloned context unless its single open already transferred ownership.
DocumentOpener::~DocumentOpener()
{
    if (ctx_)
        fz_drop_context(ctx_);
}

// Returns attempted document opens for checks that unloaded Save never parses a PDF.
int RenderEngine::testing::opens()
{
    return attemptedOpens.load();
}

// Transfers the captured context into one document open on the calling worker.
std::unique_ptr<Document> DocumentOpener::open(const QString &path, const QString &password,
                                               QString *error, bool *needsPassword)
{
    return RenderEngine::open(std::exchange(ctx_, nullptr), base_, path, password, error,
                              needsPassword);
}

// Consumes an opener context and opens with the job's read and Stop checks.
std::unique_ptr<Document> RenderEngine::open(fz_context *ctx, std::shared_ptr<fz_context> base,
                                             const QString &path, const QString &password,
                                             QString *error, bool *needsPassword)
{
    ++attemptedOpens;
    if (needsPassword)
        *needsPassword = false;

    if (!ctx) {
        if (error)
            *error = tr("Could not allocate a document context.");
        return nullptr;
    }
    if (!job::reading(path)) {
        if (error)
            *error = job::notRunError();
        fz_drop_context(ctx);
        return nullptr;
    }
    const QByteArray utf8 = path.toUtf8();
    //: Shown when the complete source file cannot be read. A partially read file
    //: is not opened, even if the PDF reader could repair it.
    const QByteArray readError = tr("Could not read the document.").toUtf8();
    const QByteArray stoppedError = job::notRunError().toUtf8();
    fz_document *doc = nullptr;
    fz_stream *file = nullptr;
    fz_buffer *buffer = nullptr;
    fz_var(doc);
    fz_var(file);
    fz_var(buffer);
    fz_try(ctx) {
        const fz_document_handler *handler = fz_recognize_document_content(ctx, utf8.constData());
        if (job::stopped())
            fz_throw(ctx, FZ_ERROR_ABORT, "%s", stoppedError.constData());
        if (handler && (handler->wants_dir || handler->wants_file)) {
            // Directory-dependent formats keep their relative resource lookup.
            doc = fz_open_document(ctx, utf8.constData());
        } else {
            file = fz_open_file(ctx, utf8.constData());
            buffer = fz_new_buffer(ctx, 64 * 1024);
            unsigned char chunk[64 * 1024];
            for (;;) {
                if (job::stopped())
                    fz_throw(ctx, FZ_ERROR_ABORT, "%s", stoppedError.constData());
                const size_t count = fz_read(ctx, file, chunk, sizeof(chunk));
                if (job::stopped())
                    fz_throw(ctx, FZ_ERROR_ABORT, "%s", stoppedError.constData());
                // MuPDF can turn a stream read error into EOF. Do not accept it
                // as a complete, repairable document.
                if (file->error) {
                    // fz_throw records errno for a system exception.
                    errno = EIO;
                    fz_throw(ctx, FZ_ERROR_SYSTEM, "%s", readError.constData());
                }
                if (!count)
                    break;
                fz_append_data(ctx, buffer, chunk, count);
                job::progressed();
            }
            fz_trim_buffer(ctx, buffer);
            // The document keeps the buffer's bytes through its own stream reference.
            // Later viewer queries can no longer read the original file.
            doc = fz_open_document_with_buffer(ctx, utf8.constData(), buffer);
        }
    }
    fz_always(ctx) {
        fz_drop_stream(ctx, file);
        fz_drop_buffer(ctx, buffer);
    }
    fz_catch(ctx) {
        if (error) {
            const int kind = fz_caught(ctx);
            if (kind == FZ_ERROR_SYSTEM
                && (fz_caught_errno(ctx) == ENOENT || fz_caught_errno(ctx) == ENOTDIR)) {
                //: Reason in a failed document's placeholder. The reader could not
                //: find the source file or one of the folders leading to it.
                *error = tr("The file was not found.");
            } else if (kind == FZ_ERROR_FORMAT) {
                //: Reason in a failed document's placeholder. Its data has an
                //: unrecoverable format error, rather than a drive or password error.
                *error = tr("The file is damaged.");
            } else {
                *error = QString::fromUtf8(fz_caught_message(ctx));
            }
        }
        (void)fz_convert_error(ctx, nullptr); // the failure is now the returned error
        fz_drop_context(ctx);
        return nullptr;
    }
    if (!doc) {
        fz_drop_context(ctx);
        return nullptr;
    }
    if (job::stopped()) {
        fz_drop_document(ctx, doc);
        fz_drop_context(ctx);
        if (error)
            *error = job::notRunError();
        return nullptr;
    }

    // Encrypted with a user password: authenticate, or report that one is needed.
    if (fz_needs_password(ctx, doc)) {
        const QByteArray pw = password.toUtf8();
        const bool ok = !password.isEmpty() && fz_authenticate_password(ctx, doc, pw.constData());
        if (!ok) {
            fz_drop_document(ctx, doc);
            fz_drop_context(ctx);
            if (needsPassword)
                *needsPassword = true;
            if (error) {
                if (password.isEmpty())
                    *error = tr("This document is password-protected.");
                else {
                    //: Reason in a failed document's placeholder after its supplied
                    //: open password was rejected. Retry can ask for another password.
                    *error = tr("The password was not accepted.");
                }
            }
            return nullptr;
        }
    }
    return std::make_unique<Document>(ctx, doc, true, std::move(base));
}

QImage RenderEngine::renderPageImage(Document *doc, int pageNo, double scale, int rotation)
{
    fz_context *base = base_.get();
    if (!doc)
        return {};
    const Document::ScreenQuery query(*doc->lifetime(), "RenderEngine::renderPageImage");
    if (query.refused())
        return {};

    // Serialize against the worker pool / TextIndex: a single fz_document's
    // object cache is not thread-safe (see Document). We render on the base
    // context, which is only ever touched from this (the UI) thread.
    std::lock_guard<std::mutex> docLk(doc->accessMutex());

    fz_document *fdoc = doc->handle();
    fz_page *page = nullptr;
    fz_pixmap *pix = nullptr;
    fz_var(page);
    fz_var(pix);
    QImage out;
    fz_try(base) {
        page = fz_load_page(base, fdoc, pageNo);
        const fz_matrix ctm = fz_pre_rotate(
            fz_scale(static_cast<float>(scale), static_cast<float>(scale)),
            static_cast<float>(rotation));
        pix = fz_new_pixmap_from_page(base, page, ctm, fz_device_rgb(base), 0);
        const int w = fz_pixmap_width(base, pix);
        const int h = fz_pixmap_height(base, pix);
        const int stride = fz_pixmap_stride(base, pix);
        unsigned char *samples = fz_pixmap_samples(base, pix);
        out = QImage(samples, w, h, stride, QImage::Format_RGB888).copy();
    }
    fz_always(base) {
        if (pix)
            fz_drop_pixmap(base, pix);
        if (page)
            fz_drop_page(base, page);
    }
    fz_catch(base) {
        out = QImage();
    }
    return out;
}

void RenderEngine::submit(const RenderRequest &req)
{
    if (!req.document)
        return;
    auto job = std::make_shared<Shared::Job>();
    job->request = req;
    job->lifetime = req.document->lifetime();
    job->request.document = nullptr; // workers resolve through the lifetime gate
    {
        std::lock_guard<std::mutex> lk(shared_->queueMutex);
        if (shared_->stop.load())
            return;
        std::erase_if(shared_->jobs, [&](const auto &weak) {
            auto old = weak.lock();
            if (!old)
                return true;
            if (old->request.requester == req.requester
                && old->request.pageNo == req.pageNo)
                old->canceled.store(true);
            return false;
        });
        std::erase_if(shared_->queue, [](const auto &old) { return old->canceled.load(); });
        shared_->jobs.push_back(job);
        shared_->queue.push_back(std::move(job));
    }
    shared_->queueCv.notify_one();
}

void RenderEngine::cancelRequests(quint64 requester)
{
    std::lock_guard<std::mutex> lk(shared_->queueMutex);
    for (auto &weak : shared_->jobs)
        if (auto job = weak.lock(); job && job->request.requester == requester)
            job->canceled.store(true);
    std::erase_if(shared_->queue, [](const auto &job) { return job->canceled.load(); });
}

void RenderEngine::trimCache()
{
    fz_empty_store(base_.get());
    fz_purge_glyph_cache(base_.get());
}

void RenderEngine::workerLoop(const std::shared_ptr<Shared> &shared, fz_context *ctx)
{
    // Results go to the engine's thread, unless the engine has stopped by then.
    const auto deliver = [&shared](RenderResult result) {
        background::post(shared->owner, [shared, result = std::move(result)] {
            if (!shared->stop.load())
                emit static_cast<RenderEngine *>(shared->owner.data())->resultReady(result);
        });
    };
    for (;;) {
        std::shared_ptr<Shared::Job> job;
        {
            std::unique_lock<std::mutex> lk(shared->queueMutex);
            shared->queueCv.wait(lk, [&shared] { return shared->stop.load() || !shared->queue.empty(); });
            if (shared->stop.load())
                break;
            // Most-recently-requested first: keeps the visible viewport responsive.
            job = shared->queue.back();
            shared->queue.pop_back();
        }

        if (job->canceled.load())
            continue;
        const RenderRequest &req = job->request;

        RenderResult res;
        res.requester = req.requester;
        res.pageNo = req.pageNo;
        res.scale = req.scale;
        res.rotation = req.rotation;
        res.epoch = req.epoch;
        res.token = req.token;

        fz_document *doc = nullptr;
        fz_page *page = nullptr;
        fz_display_list *list = nullptr;
        fz_pixmap *pix = nullptr;
        fz_var(page);
        fz_var(list);
        fz_var(pix);

        // Phase 1 (serialized per document): load the page and capture its
        // drawing into a display list. This is the only part that touches the
        // document's object cache / lazy loading, which is not thread-safe, so
        // it runs under the document's access lock (see Document class note).
        {
            // A job's Document::Hold can keep the gate for as long as a save takes.
            // Waiting in steps lets a canceled request, such as one from the tab
            // whose job holds the document, free this worker for other tabs.
            std::unique_lock gate(job->lifetime->mutex, std::defer_lock);
            while (!gate.try_lock_for(DocumentLifetime::kStep))
                if (job->canceled.load())
                    break;
            Document *document = gate.owns_lock() ? job->lifetime->document : nullptr;
            if (!document || job->canceled.load())
                continue;
            std::lock_guard<std::mutex> docLk(document->accessMutex());
            doc = document->handle();
            fz_try(ctx) {
                page = fz_load_page(ctx, doc, req.pageNo);
                list = fz_new_display_list_from_page(ctx, page);
            }
            fz_always(ctx) {
                if (page) {
                    fz_drop_page(ctx, page);
                    page = nullptr;
                }
            }
            fz_catch(ctx) {
                res.ok = false;
                res.error = QString::fromUtf8(fz_caught_message(ctx));
            }
        }

        if (job->canceled.load()) {
            fz_drop_display_list(ctx, list);
            continue;
        }
        if (!list) {
            deliver(std::move(res)); // phase 1 failed; res carries the error
            continue;
        }

        // Phase 2 (parallel): rasterize the display list. A built list is
        // self-contained - it no longer reads the document - and the shared
        // store / glyph cache it does touch is covered by the lock callbacks,
        // so multiple workers may rasterize different pages simultaneously.
        fz_device *imgDev = nullptr;
        fz_var(imgDev);
        // Comfort only: raster-image bboxes + per-image treatment decisions.
        // Declared before fz_try - a longjmp out of the try block does not
        // run C++ destructors, so heap-owning locals must not be constructed
        // inside it (declared here they clean up at scope end on every path).
        std::vector<fz_rect> fzRects;
        std::vector<fz_image *> fzImages; // kept references, dropped in fz_always
        std::vector<ImageDecision> fzDecisions;
        std::vector<int> fzImageSeq;
        std::vector<fz_rect> fzOverlays;
        std::vector<int> fzOverlaySeq;
        QVector<ComfortImageRect> imageRects;
        // This worker's own context, so the level returns to it in fz_always.
        const int textLevel = fz_text_aa_level(ctx);
        const int graphicsLevel = fz_graphics_aa_level(ctx);
        if (!req.antialias)
            fz_set_aa_level(ctx, 0);
        fz_try(ctx) {
            fz_matrix ctm = fz_pre_rotate(
                fz_scale(static_cast<float>(req.scale), static_cast<float>(req.scale)),
                static_cast<float>(req.rotation));
            const fz_irect full =
                fz_round_rect(fz_transform_rect(fz_bound_display_list(ctx, list), ctm));
            // Beyond MuPDF's edge clamp a tile would come back blank or with shapes
            // moved, so fail instead. The viewer and the print preview stay within it.
            if (std::max({std::abs(full.x0), std::abs(full.y0), std::abs(full.x1),
                          std::abs(full.y1)}) > kMaxDevicePixel)
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "page raster beyond the rasteriser's range");

            if (req.clip.isEmpty()) {
                // Whole page (normal zoom).
                pix = fz_new_pixmap_from_display_list(ctx, list, ctm, fz_device_rgb(ctx), 0);
            } else {
                // Clipped tile (deep zoom): render only the requested band of the
                // page's full device image, so the pixmap stays small no matter
                // how high the scale. clip is in device px relative to the full
                // image's top-left, which maps onto the transformed bbox origin.
                fz_irect want;
                want.x0 = full.x0 + req.clip.left();
                want.y0 = full.y0 + req.clip.top();
                want.x1 = want.x0 + req.clip.width();
                want.y1 = want.y0 + req.clip.height();
                const fz_irect bbox = fz_intersect_irect(want, full);
                if (bbox.x1 > bbox.x0 && bbox.y1 > bbox.y0) {
                    pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), bbox, nullptr, 0);
                    fz_clear_pixmap_with_value(ctx, pix, 0xFF);
                    fz_fill_pixmap_from_display_list(ctx, list, ctm, pix);
                }
            }

            if (pix) {
                if (req.theme == PageTheme::Comfort) {
                    // Collect image bounds before constructing QImage: MuPDF longjmp would skip
                    // its destructor. Apply the Qt-only pixel transform after fz_always.
                    ImageRectDevice *ird = fz_new_derived_device(ctx, ImageRectDevice);
                    imgDev = reinterpret_cast<fz_device *>(ird);
                    ird->base.fill_image = imageRectFillImage;
                    ird->base.fill_text = imageRectFillText;
                    ird->base.stroke_text = imageRectStrokeText;
                    ird->rects = &fzRects;
                    ird->images = &fzImages;
                    ird->imageSeq = &fzImageSeq;
                    ird->overlays = &fzOverlays;
                    ird->overlaySeq = &fzOverlaySeq;
                    ird->seq = 0;
                    // Walk the whole page so scan coverage is independent of zoom/clipping.
                    // Bounds are cheap; decode/classify only images intersecting this render.
                    fz_run_display_list(ctx, list, imgDev, ctm, fz_infinite_rect, nullptr);
                    fz_close_device(ctx, imgDev);

                    const fz_irect pixBox = fz_pixmap_bbox(ctx, pix);
                    const fz_rect pixRect = fz_rect_from_irect(pixBox);
                    const fz_rect pageRect =
                        fz_transform_rect(fz_bound_display_list(ctx, list), ctm);
                    const float pageW = pageRect.x1 - pageRect.x0;
                    const float pageH = pageRect.y1 - pageRect.y0;

                    // Page coverage: the union of every image rect over the
                    // page area, counted on a coarse grid (exact enough for a
                    // 0.60 threshold, and independent of rect order).
                    float coverage = 0.0f;
                    if (pageW > 0 && pageH > 0 && !fzRects.empty()) {
                        constexpr int kGrid = 64;
                        std::array<bool, kGrid * kGrid> cell{};
                        for (const fz_rect &r : fzRects) {
                            const fz_rect c = fz_intersect_rect(r, pageRect);
                            if (fz_is_empty_rect(c))
                                continue;
                            const int gx0 = std::clamp(
                                int((c.x0 - pageRect.x0) / pageW * kGrid), 0, kGrid - 1);
                            const int gx1 = std::clamp(
                                int((c.x1 - pageRect.x0) / pageW * kGrid), gx0, kGrid - 1);
                            const int gy0 = std::clamp(
                                int((c.y0 - pageRect.y0) / pageH * kGrid), 0, kGrid - 1);
                            const int gy1 = std::clamp(
                                int((c.y1 - pageRect.y0) / pageH * kGrid), gy0, kGrid - 1);
                            for (int gy = gy0; gy <= gy1; ++gy)
                                for (int gx = gx0; gx <= gx1; ++gx)
                                    cell[gy * kGrid + gx] = true;
                        }
                        coverage = float(std::count(cell.begin(), cell.end(), true))
                            / float(kGrid * kGrid);
                    }

                    // Classify the images this render actually touches. A
                    // scanned page is decided by its geometry alone, so its
                    // images - the largest in any document - are never decoded
                    // or measured for this.
                    const bool scannedPage = comfortScannedPageCoverage(coverage);
                    fzDecisions.assign(
                        fzRects.size(),
                        ImageDecision{static_cast<uint8_t>(scannedPage
                                                               ? ComfortImageMode::Split
                                                               : ComfortImageMode::KeepAuthored),
                                      8, 24});
                    for (size_t ri = 0; ri < fzRects.size() && !scannedPage; ++ri) {
                        if (fz_is_empty_rect(fz_intersect_rect(fzRects[ri], pixRect)))
                            continue;
                        ImageProbe probe = embeddedImageProbe(ctx, fzImages[ri]);
                        probe.f.pageCoverage = coverage;
                        fzDecisions[ri] = ImageDecision{
                            static_cast<uint8_t>(comfortImageMode(probe.f)), probe.rampLo,
                            probe.rampHi};
                    }

                    // The pixmap origin includes clipping, so rect offsets also work for deep-
                    // zoom tiles. Text painted over an image uses PageInk: otherwise removing a
                    // white label backdrop leaves black text on dark paper. Exclude paths,
                    // whose loose bounding boxes can cover much of the picture.
                    std::vector<fz_rect> inkRegions;
                    for (size_t ri = 0; ri < fzRects.size(); ++ri) {
                        if (fzDecisions[ri].mode
                            != static_cast<uint8_t>(ComfortImageMode::PhotoOnWhite))
                            continue;
                        const fz_rect placed = fzRects[ri];
                        const float area = (placed.x1 - placed.x0) * (placed.y1 - placed.y0);
                        if (area <= 0)
                            continue;
                        const size_t first = inkRegions.size();
                        for (size_t oi = 0; oi < fzOverlays.size(); ++oi) {
                            if (fzOverlaySeq[oi] < fzImageSeq[ri])
                                continue;
                            const fz_rect ov = fz_intersect_rect(fzOverlays[oi], placed);
                            if (fz_is_empty_rect(ov))
                                continue;
                            const fz_rect &full = fzOverlays[oi];
                            const float ovArea = (full.x1 - full.x0) * (full.y1 - full.y0);
                            const float inArea = (ov.x1 - ov.x0) * (ov.y1 - ov.y0);
                            if (inArea < 4.0f || ovArea <= 0)
                                continue;
                            // Only text that LIVES on the picture: a line of
                            // body text that merely starts inside it keeps its
                            // meaning from the part outside.
                            if (inArea >= 0.90f * ovArea)
                                inkRegions.push_back(ov);
                        }
                        // Densely lettered picture (a table set over a
                        // background image): collapse to one region rather than
                        // making the row splitter walk hundreds of rects.
                        constexpr size_t kMaxRegionsPerImage = 16;
                        if (inkRegions.size() - first > kMaxRegionsPerImage) {
                            fz_rect all = fz_empty_rect;
                            for (size_t k = first; k < inkRegions.size(); ++k)
                                all = fz_union_rect(all, inkRegions[k]);
                            inkRegions.resize(first);
                            inkRegions.push_back(all);
                        }
                    }

                    const int ox = fz_pixmap_x(ctx, pix);
                    const int oy = fz_pixmap_y(ctx, pix);
                    imageRects.reserve(static_cast<int>(fzRects.size()));
                    for (size_t ri = 0; ri < fzRects.size(); ++ri) {
                        if (fz_is_empty_rect(fz_intersect_rect(fzRects[ri], pixRect)))
                            continue; // outside this render (unculled walk)
                        const fz_irect ir = fz_round_rect(fzRects[ri]);
                        imageRects.append(ComfortImageRect{
                            QRect(ir.x0 - ox, ir.y0 - oy, ir.x1 - ir.x0, ir.y1 - ir.y0),
                            static_cast<ComfortImageMode>(fzDecisions[ri].mode),
                            fzDecisions[ri].rampLo, fzDecisions[ri].rampHi});
                    }
                    for (const fz_rect &r : inkRegions) {
                        if (fz_is_empty_rect(fz_intersect_rect(r, pixRect)))
                            continue;
                        const fz_irect ir = fz_round_rect(r);
                        imageRects.append(ComfortImageRect{
                            QRect(ir.x0 - ox, ir.y0 - oy, ir.x1 - ir.x0, ir.y1 - ir.y0),
                            ComfortImageMode::PageInk, 0, 0});
                    }
                }

                const int w = fz_pixmap_width(ctx, pix);
                const int h = fz_pixmap_height(ctx, pix);
                const int stride = fz_pixmap_stride(ctx, pix);
                unsigned char *samples = fz_pixmap_samples(ctx, pix);

                // Deep-copy into a QImage so the fz_pixmap can be dropped
                // immediately. Nothing after this construction may fz_throw.
                QImage img(samples, w, h, stride, QImage::Format_RGB888);
                res.image = img.copy();
                res.ok = true;
            }
        }
        fz_always(ctx) {
            for (fz_image *img : fzImages)
                fz_drop_image(ctx, img);
            fzImages.clear();
            if (imgDev)
                fz_drop_device(ctx, imgDev);
            if (pix)
                fz_drop_pixmap(ctx, pix);
            if (list)
                fz_drop_display_list(ctx, list);
            fz_set_text_aa_level(ctx, textLevel);
            fz_set_graphics_aa_level(ctx, graphicsLevel);
        }
        fz_catch(ctx) {
            res.ok = false;
            res.error = QString::fromUtf8(fz_caught_message(ctx));
        }

        // The transform is per-pixel (switching treatment at image-rect
        // boundaries), so it applies to whole-page renders and clipped
        // deep-zoom tiles identically.
        if (!job->canceled.load() && res.ok && req.theme == PageTheme::Comfort)
            applyComfortTransform(res.image, imageRects);

        if (!job->canceled.load())
            deliver(std::move(res));
    }

    fz_drop_context(ctx);
}

} // namespace mervin
