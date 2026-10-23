#include "render/Document.h"

#include "io/Background.h"
#include "io/JobContext.h"
#include "render/MeasureContent.h"
#include "render/RenderTypes.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QThread>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_map>

namespace mervin {
namespace {

thread_local std::function<void()> rasterObserver;
std::atomic<quint64> constructedDocuments{0};

// Poll the atomic job state on the rendering thread. MuPDF's cookie fields are
// plain integers, so neither Stop nor any other thread writes this cookie.
struct CancellableDrawDevice
{
    fz_device base;
    fz_cookie *cookie;
};

bool continueRaster(fz_device *device)
{
    if (rasterObserver)
        rasterObserver();
    auto *draw = reinterpret_cast<CancellableDrawDevice *>(device);
    if (job::stopped())
        draw->cookie->abort = 1;
    return !draw->cookie->abort;
}

// Forward painting unchanged until Stop. Container operations still balance on
// the real draw device; the parser sees abort at its next cookie checkpoint.
fz_device *cancellableDrawDevice(fz_context *ctx, fz_device *target, fz_cookie *cookie)
{
    auto *draw = fz_new_derived_passthrough_device(ctx, target, CancellableDrawDevice);
    draw->cookie = cookie;
    draw->base.fill_path = [](fz_context *c, fz_device *d, const fz_path *p, int evenOdd,
                              fz_matrix m, fz_colorspace *cs, const float *color,
                              float alpha, fz_color_params params) {
        if (continueRaster(d)) fz_fill_path(c, d->passthrough, p, evenOdd, m, cs, color, alpha, params);
    };
    draw->base.stroke_path = [](fz_context *c, fz_device *d, const fz_path *p,
                                const fz_stroke_state *stroke, fz_matrix m, fz_colorspace *cs,
                                const float *color, float alpha, fz_color_params params) {
        if (continueRaster(d)) fz_stroke_path(c, d->passthrough, p, stroke, m, cs, color, alpha, params);
    };
    draw->base.fill_text = [](fz_context *c, fz_device *d, const fz_text *text, fz_matrix m,
                              fz_colorspace *cs, const float *color, float alpha, fz_color_params params) {
        if (continueRaster(d)) fz_fill_text(c, d->passthrough, text, m, cs, color, alpha, params);
    };
    draw->base.stroke_text = [](fz_context *c, fz_device *d, const fz_text *text,
                                const fz_stroke_state *stroke, fz_matrix m, fz_colorspace *cs,
                                const float *color, float alpha, fz_color_params params) {
        if (continueRaster(d)) fz_stroke_text(c, d->passthrough, text, stroke, m, cs, color, alpha, params);
    };
    draw->base.fill_shade = [](fz_context *c, fz_device *d, fz_shade *shade, fz_matrix m,
                               float alpha, fz_color_params params) {
        if (continueRaster(d)) fz_fill_shade(c, d->passthrough, shade, m, alpha, params);
    };
    draw->base.fill_image = [](fz_context *c, fz_device *d, fz_image *image, fz_matrix m,
                               float alpha, fz_color_params params) {
        if (continueRaster(d)) fz_fill_image(c, d->passthrough, image, m, alpha, params);
    };
    draw->base.fill_image_mask = [](fz_context *c, fz_device *d, fz_image *image, fz_matrix m,
                                    fz_colorspace *cs, const float *color, float alpha, fz_color_params params) {
        if (continueRaster(d)) fz_fill_image_mask(c, d->passthrough, image, m, cs, color, alpha, params);
    };
    return &draw->base;
}

} // namespace

void Document::testing::setRasterObserver(std::function<void()> observer)
{
    rasterObserver = std::move(observer);
}

Document::Document(fz_context *ctx, fz_document *doc, bool ownsContext,
                   std::shared_ptr<fz_context> base)
    : base_(std::move(base)), ctx_(ctx), ownsContext_(ownsContext), doc_(doc)
{
    lifetime_->document = this;
    int n = 0;
    fz_try(ctx_)
        n = fz_count_pages(ctx_, doc_);
    fz_catch(ctx_)
        n = 0;

    // Read PDF sizes with pdf_page_obj_transform to avoid loading annotations/links for every
    // page. It applies the same inherited boxes, rotation and UserUnit as fz_bound_page. Non-
    // PDF documents use the generic page-loading path.
    pdf_document *pdoc = nullptr;
    fz_try(ctx_)
        pdoc = pdf_specifics(ctx_, doc_);
    fz_catch(ctx_)
        pdoc = nullptr;

    pageSizes_.reserve(n);
    for (int i = 0; i < n; ++i) {
        // 0 means "not resolved yet": MuPDF clamps a real page box to at least
        // 1x1 (see pdf_page_obj_transform_box), so a zero can only be our own.
        double w = 0.0;
        double h = 0.0;
        fz_var(w);
        fz_var(h);

        if (pdoc) {
            fz_try(ctx_) {
                fz_rect box;
                fz_matrix ctm;
                pdf_page_obj_transform(ctx_, pdf_lookup_page_obj(ctx_, pdoc, i), &box, &ctm);
                const fz_rect r = fz_transform_rect(box, ctm);
                w = r.x1 - r.x0;
                h = r.y1 - r.y0;
            }
            fz_catch(ctx_) {
                w = h = 0.0; // malformed page dict - try the generic path
            }
        }

        if (!(w > 0.0 && h > 0.0 && std::isfinite(w) && std::isfinite(h))) {
            fz_page *page = nullptr;
            fz_var(page);
            fz_try(ctx_) {
                page = fz_load_page(ctx_, doc_, i);
                const fz_rect r = fz_bound_page(ctx_, page);
                w = r.x1 - r.x0;
                h = r.y1 - r.y0;
            }
            fz_always(ctx_) {
                if (page)
                    fz_drop_page(ctx_, page);
            }
            fz_catch(ctx_) {
                w = h = 0.0;
            }
        }

        if (!(w > 0.0 && h > 0.0 && std::isfinite(w) && std::isfinite(h))) {
            // US Letter fallback if the page has no usable finite bounds.
            w = 612.0;
            h = 792.0;
        }
        pageSizes_.push_back(QSizeF(w, h));
    }

    char buf[1024];
    if (fz_lookup_metadata(ctx_, doc_, FZ_META_INFO_TITLE, buf, sizeof(buf)) > 0)
        title_ = QString::fromUtf8(buf);

    measureCache_.resize(pageSizes_.size());
    geomCache_.resize(pageSizes_.size());
    ++constructedDocuments;
}

Document::~Document()
{
    std::lock_guard guard(lifetime_->mutex);
    lifetime_->document = nullptr;
    if (doc_)
        fz_drop_document(ctx_, doc_);
    if (ownsContext_)
        fz_drop_context(ctx_);
}

QImage Document::renderPage(int pageNo, double scale, int rotation, const QRect &clip,
                            bool antialias) const
{
    const ScreenQuery query(*lifetime_, "Document::renderPage");
    if (query.refused())
        return {};
    if (job::stopped())
        return {};
    std::lock_guard<std::mutex> lock(access_);
    fz_page *page = nullptr;
    fz_pixmap *pixmap = nullptr;
    fz_device *device = nullptr;
    fz_device *drawDevice = nullptr;
    fz_cookie cookie{};
    fz_var(page);
    fz_var(pixmap);
    fz_var(device);
    fz_var(drawDevice);
    fz_var(cookie);
    const fz_matrix transform = fz_pre_rotate(
        fz_scale(static_cast<float>(scale), static_cast<float>(scale)),
        static_cast<float>(rotation));
    fz_irect full{};
    fz_try(ctx_) {
        page = fz_load_page(ctx_, doc_, pageNo);
        full = fz_round_rect(fz_transform_rect(fz_bound_page(ctx_, page), transform));
    }
    fz_catch(ctx_) {
        fz_drop_page(ctx_, page);
        (void)fz_convert_error(ctx_, nullptr);
        return {};
    }
    // Beyond MuPDF's edge clamp the result would be blank or distorted, not an error.
    if (std::max({std::abs(full.x0), std::abs(full.y0), std::abs(full.x1), std::abs(full.y1)})
        > kMaxDevicePixel) {
        fz_drop_page(ctx_, page);
        return {};
    }
    // As in RenderEngine's deep-zoom tiles, the clip is relative to the full
    // raster's top-left, which is the transformed page bounds' rounded origin.
    fz_irect bbox = full;
    if (!clip.isEmpty()) {
        fz_irect want;
        want.x0 = full.x0 + clip.left();
        want.y0 = full.y0 + clip.top();
        want.x1 = want.x0 + clip.width();
        want.y1 = want.y0 + clip.height();
        bbox = fz_intersect_irect(want, full);
    }
    // MuPDF draws straight into the image's rows, so the raster exists only once.
    QImage image;
    if (bbox.x1 > bbox.x0 && bbox.y1 > bbox.y0)
        image = QImage(bbox.x1 - bbox.x0, bbox.y1 - bbox.y0, QImage::Format_RGB888);
    if (image.isNull()) {
        fz_drop_page(ctx_, page);
        return {};
    }
    uchar *samples = image.bits();
    const int stride = static_cast<int>(image.bytesPerLine());
    // Level 0 also makes MuPDF widen thinner strokes to one pixel. The access lock
    // keeps every other use of this context out until the levels are restored.
    const int textLevel = fz_text_aa_level(ctx_);
    const int graphicsLevel = fz_graphics_aa_level(ctx_);
    if (!antialias)
        fz_set_aa_level(ctx_, 0);
    fz_try(ctx_) {
        pixmap = fz_new_pixmap_with_data(ctx_, fz_device_rgb(ctx_), bbox.x1 - bbox.x0,
                                         bbox.y1 - bbox.y0, nullptr, 0, stride, samples);
        pixmap->x = bbox.x0;
        pixmap->y = bbox.y0;
        fz_clear_pixmap_with_value(ctx_, pixmap, 0xFF);
        drawDevice = fz_new_draw_device(ctx_, transform, pixmap);
        device = cancellableDrawDevice(ctx_, drawDevice, &cookie);
        fz_run_page(ctx_, page, device, fz_identity, &cookie);
        fz_close_device(ctx_, device);
    }
    fz_always(ctx_) {
        fz_drop_device(ctx_, device);
        fz_drop_device(ctx_, drawDevice);
        fz_drop_pixmap(ctx_, pixmap);
        fz_drop_page(ctx_, page);
        fz_set_text_aa_level(ctx_, textLevel);
        fz_set_graphics_aa_level(ctx_, graphicsLevel);
    }
    fz_catch(ctx_) {
        image = {};
        (void)fz_convert_error(ctx_, nullptr);
    }
    // A cookie may stop normally and leave a partially painted pixmap. Never
    // return it to printing, and honor Stop that arrived during the last call.
    if (cookie.abort || job::stopped())
        image = {};
    return image;
}

namespace {
std::atomic<int> closesInFlight{0};
std::atomic<quint64> completedBackgroundCloses{0};
std::mutex followUpsMutex;
std::condition_variable followUpsDone;
int followUps = 0; // guarded by followUpsMutex: closes with a `then` not finished
} // namespace

void Document::closeInBackground(std::unique_ptr<Document> document, std::function<void()> then)
{
    if (!document && !then)
        return;
    ++closesInFlight;
    if (then) {
        std::lock_guard<std::mutex> lock(followUpsMutex);
        ++followUps;
    }
    background::start([document = std::shared_ptr<Document>(std::move(document)),
                       then = std::move(then)]() mutable {
        const bool hadDocument = bool(document);
        document.reset();
        if (hadDocument)
            ++completedBackgroundCloses;
        --closesInFlight;
        if (!then)
            return;
        then();
        std::lock_guard<std::mutex> lock(followUpsMutex);
        if (--followUps == 0)
            followUpsDone.notify_all();
    });
}

void Document::CloseInBackground::operator()(Document *document) const
{
    closeInBackground(std::unique_ptr<Document>(document));
}

int Document::closing()
{
    return closesInFlight.load();
}

quint64 Document::completedCloses()
{
    return completedBackgroundCloses.load();
}

bool Document::waitForCloses(int ms)
{
    std::unique_lock<std::mutex> lock(followUpsMutex);
    return followUpsDone.wait_for(lock, std::chrono::milliseconds(ms),
                                  [] { return followUps == 0; });
}

namespace {
std::atomic<int> screenRefusals{0};
// The documents whose queries the screen thread is inside, innermost last. A query
// nested in another one of the same document is let through (see ScreenQuery).
thread_local std::vector<const DocumentLifetime *> screenQueriesInside;
} // namespace

// A screen-thread query counts itself in screenQueries before it reads holds, and a
// hold counts itself in holds before it reads screenQueries. Both are sequentially
// consistent, so at least one of them sees the other. Either the query refuses, or
// the hold waits for the query to end. A query that found no hold therefore never
// waits for the job behind the hold, however the two threads interleave. The hold
// waits before it takes the gate, not after, because TextIndex queries take the gate
// on the screen thread.
Document::Hold::Hold(std::shared_ptr<DocumentLifetime> lifetime, const std::function<bool()> &stop)
    : lifetime_(std::move(lifetime))
{
    // The screen thread never waits for a job, and inside one of its own queries a hold
    // would wait forever.
    Q_ASSERT(!QThread::isMainThread());
    if (!lifetime_)
        return;
    lifetime_->holds.fetch_add(1);
    std::unique_lock gate(lifetime_->mutex, std::defer_lock);
    while (!(stop && stop())) {
        if (lifetime_->screenQueries.load() > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        else if (gate.try_lock_for(DocumentLifetime::kStep))
            break;
    }
    if (gate.owns_lock() && lifetime_->document) {
        document_ = lifetime_->document;
        gate_ = std::move(gate);
        return;
    }
    if (gate.owns_lock())
        gate.unlock();
    lifetime_->holds.fetch_sub(1);
}

Document::Hold::~Hold()
{
    if (!document_)
        return;
    gate_.unlock();
    lifetime_->holds.fetch_sub(1);
}

Document::ScreenQuery::ScreenQuery(DocumentLifetime &lifetime, const char *method)
{
    if (!QThread::isMainThread())
        return;
    // Inside another query of this document, such as withPdfDocument inside
    // savePdfTo, the outer query already counts, and a hold that came after it waits
    // for it. Refusing here would fail a call that was let through.
    if (std::find(screenQueriesInside.begin(), screenQueriesInside.end(), &lifetime)
        != screenQueriesInside.end()) {
        screenQueriesInside.push_back(&lifetime);
        entered_ = true;
        return;
    }
    lifetime.screenQueries.fetch_add(1);
    if (lifetime.holds.load() == 0) {
        counted_ = &lifetime;
        screenQueriesInside.push_back(&lifetime);
        entered_ = true;
        return;
    }
    lifetime.screenQueries.fetch_sub(1);
    refused_ = true;
    ++screenRefusals;
    qWarning("Mervin: %s refused on the screen thread while a job holds the document.", method);
}

Document::ScreenQuery::~ScreenQuery()
{
    if (entered_)
        screenQueriesInside.pop_back();
    if (counted_)
        counted_->screenQueries.fetch_sub(1);
}

// Counts completed construction even when a canceled delivery discards its document.
quint64 Document::testing::constructions()
{
    return constructedDocuments.load();
}

int Document::testing::refusals()
{
    return screenRefusals.load();
}

QSizeF Document::pageSize(int pageNo) const
{
    if (pageNo >= 0 && pageNo < static_cast<int>(pageSizes_.size()))
        return pageSizes_[pageNo];
    return QSizeF(612.0, 792.0);
}

namespace {

// Recursively convert an fz_outline tree into OutlineItems.
std::vector<OutlineItem> convertOutline(fz_context *ctx, fz_document *doc, fz_outline *node)
{
    std::vector<OutlineItem> items;
    for (; node; node = node->next) {
        OutlineItem item;
        item.title = node->title ? QString::fromUtf8(node->title) : QString();
        item.page = -1;
        if (node->page.page >= 0) {
            const int p = fz_page_number_from_location(ctx, doc, node->page);
            if (p >= 0)
                item.page = p;
        }
        if (node->down)
            item.children = convertOutline(ctx, doc, node->down);
        items.push_back(std::move(item));
    }
    return items;
}

// Decode a PDF text string (/U unit label, /R ratio) to a trimmed QString.
// pdf_to_text_string is null-tolerant and never throws; AutoCAD's blank " "
// labels trim down to "".
QString readPdfString(fz_context *ctx, pdf_obj *obj)
{
    if (!obj)
        return {};
    const char *t = pdf_to_text_string(ctx, obj);
    return t ? QString::fromUtf8(t).trimmed() : QString();
}

int hexNibble(QChar ch)
{
    const ushort u = ch.unicode();
    if (u >= '0' && u <= '9')
        return int(u - '0');
    if (u >= 'a' && u <= 'f')
        return int(u - 'a' + 10);
    if (u >= 'A' && u <= 'F')
        return int(u - 'A' + 10);
    return -1;
}

bool readHexCode(const QString &text, qsizetype pos, int count, uint &code)
{
    if (pos + count > text.size())
        return false;
    code = 0;
    for (int i = 0; i < count; ++i) {
        const int h = hexNibble(text.at(pos + i));
        if (h < 0)
            return false;
        code = (code << 4) | uint(h);
    }
    return true;
}

QString parseJsStringLiteral(const QString &script, qsizetype &i)
{
    const QChar quote = script.at(i++);
    QString out;
    while (i < script.size()) {
        const QChar ch = script.at(i++);
        if (ch == quote)
            break;
        if (ch != QLatin1Char('\\')) {
            out.append(ch);
            continue;
        }
        if (i >= script.size())
            break;

        const QChar esc = script.at(i++);
        switch (esc.unicode()) {
        case 'b': out.append(QLatin1Char('\b')); break;
        case 'f': out.append(QLatin1Char('\f')); break;
        case 'n': out.append(QLatin1Char('\n')); break;
        case 'r': out.append(QLatin1Char('\r')); break;
        case 't': out.append(QLatin1Char('\t')); break;
        case 'v': out.append(QChar(0x000b)); break;
        case '\r':
            if (i < script.size() && script.at(i) == QLatin1Char('\n'))
                ++i;
            break; // JavaScript line continuation
        case '\n':
            break;
        case 'u': {
            uint code = 0;
            if (readHexCode(script, i, 4, code)) {
                out.append(QChar(static_cast<ushort>(code)));
                i += 4;
            } else {
                out.append(esc);
            }
            break;
        }
        case 'x': {
            uint code = 0;
            if (readHexCode(script, i, 2, code)) {
                out.append(QChar(static_cast<ushort>(code)));
                i += 2;
            } else {
                out.append(esc);
            }
            break;
        }
        default:
            out.append(esc);
            break;
        }
    }
    return out;
}

QStringList parseKiCadPropertyScript(const QString &script)
{
    QStringList values;
    const qsizetype call = script.indexOf(QStringLiteral("ShM"));
    if (call < 0)
        return values;

    qsizetype i = script.indexOf(QLatin1Char('['), call);
    if (i < 0)
        return values;

    int depth = 0;
    bool wantRowFirstString = false;
    while (i < script.size()) {
        const QChar ch = script.at(i);
        if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
            QString value = parseJsStringLiteral(script, i).trimmed();
            if (wantRowFirstString) {
                if (!value.isEmpty())
                    values << value;
                wantRowFirstString = false;
            }
            continue;
        }
        if (ch == QLatin1Char('[')) {
            ++depth;
            if (depth == 2)
                wantRowFirstString = true;
            ++i;
            continue;
        }
        if (ch == QLatin1Char(']')) {
            if (depth == 2)
                wantRowFirstString = false;
            --depth;
            ++i;
            if (depth <= 0)
                break;
            continue;
        }
        ++i;
    }
    return values;
}

bool rectContains(const fz_rect &r, float x, float y)
{
    return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1;
}

} // namespace

std::vector<OutlineItem> Document::outline() const
{
    const ScreenQuery query(*lifetime_, "Document::outline");
    if (query.refused())
        return {};
    std::lock_guard<std::mutex> lk(access_);
    std::vector<OutlineItem> result;
    fz_outline *outline = nullptr;
    fz_var(outline);
    fz_try(ctx_) {
        outline = fz_load_outline(ctx_, doc_);
        if (outline)
            result = convertOutline(ctx_, doc_, outline);
    }
    fz_always(ctx_) {
        if (outline)
            fz_drop_outline(ctx_, outline);
    }
    fz_catch(ctx_) {
        result.clear();
    }
    return result;
}

PageMeasurement Document::pageMeasurement(int pageNo) const
{
    const ScreenQuery query(*lifetime_, "Document::pageMeasurement");
    if (query.refused())
        return {};
    std::lock_guard<std::mutex> lk(access_);
    if (pageNo < 0 || pageNo >= static_cast<int>(measureCache_.size()))
        return {};
    if (measureCache_[pageNo])
        return *measureCache_[pageNo];

    PageMeasurement pm;
    fz_page *fzpage = nullptr;
    fz_var(fzpage);
    fz_try(ctx_) {
        // pdf_specifics returns null for non-PDF documents; the names VP, X, Y,
        // Measure and GEO are NOT in MuPDF's PDF_NAME() table, so they use the
        // string accessor pdf_dict_gets / a strcmp. All pdf_* accessors below are
        // null-tolerant, so missing entries simply yield empty/zero.
        pdf_document *pdoc = pdf_specifics(ctx_, doc_);
        if (pdoc) {
            pdf_obj *page = pdf_lookup_page_obj(ctx_, pdoc, pageNo);
            pm.userUnit = pdf_dict_get_real_default(ctx_, page, PDF_NAME(UserUnit), 1.0f);
            if (pm.userUnit <= 0.0)
                pm.userUnit = 1.0;

            // Transform PDF-user-space /VP bounds into zero-origin, y-down page points,
            // matching cursor/snap geometry. Otherwise rotation or shifted boxes select the
            // wrong viewport scale.
            fzpage = fz_load_page(ctx_, doc_, pageNo);
            const fz_rect bound = fz_bound_page(ctx_, fzpage);
            fz_rect mbox;
            fz_matrix pageCtm;
            pdf_page_transform(ctx_, pdf_page_from_fz_page(ctx_, fzpage), &mbox, &pageCtm);
            (void)mbox;
            const fz_matrix toApp = fz_concat(pageCtm, fz_translate(-bound.x0, -bound.y0));

            pdf_obj *vp = pdf_resolve_indirect(ctx_, pdf_dict_gets(ctx_, page, "VP"));
            const int n = pdf_array_len(ctx_, vp);
            for (int i = 0; i < n; ++i) {
                pdf_obj *vpd = pdf_resolve_indirect(ctx_, pdf_array_get(ctx_, vp, i));
                if (!pdf_is_dict(ctx_, vpd))
                    continue;
                pdf_obj *meas = pdf_resolve_indirect(ctx_, pdf_dict_gets(ctx_, vpd, "Measure"));
                if (!pdf_is_dict(ctx_, meas))
                    continue;

                const char *sub = pdf_to_name(ctx_, pdf_dict_get(ctx_, meas, PDF_NAME(Subtype)));
                if (sub && std::strcmp(sub, "GEO") == 0)
                    continue; // geospatial measure: out of scope

                MeasureViewport mv;
                const fz_rect bbPdf = pdf_to_rect(ctx_, pdf_dict_get(ctx_, vpd, PDF_NAME(BBox)));
                const fz_rect bb = fz_transform_rect(bbPdf, toApp);
                mv.bbox = QRectF(QPointF(bb.x0, bb.y0), QPointF(bb.x1, bb.y1)).normalized();

                pdf_obj *x0 = pdf_array_get(ctx_, pdf_dict_gets(ctx_, meas, "X"), 0);
                mv.cx = pdf_to_real(ctx_, pdf_dict_get(ctx_, x0, PDF_NAME(C)));
                mv.unit = readPdfString(ctx_, pdf_dict_get(ctx_, x0, PDF_NAME(U)));

                pdf_obj *y0 = pdf_array_get(ctx_, pdf_dict_gets(ctx_, meas, "Y"), 0);
                mv.cy = y0 ? pdf_to_real(ctx_, pdf_dict_get(ctx_, y0, PDF_NAME(C))) : mv.cx;
                if (mv.cy <= 0.0)
                    mv.cy = mv.cx;

                mv.ratio = readPdfString(ctx_, pdf_dict_get(ctx_, meas, PDF_NAME(R)));
                mv.rectilinear = true;

                if (mv.cx > 0.0) {
                    pm.viewports.push_back(mv);
                    pm.hasEmbedded = true;
                }
            }
        }
    }
    fz_always(ctx_) {
        if (fzpage)
            fz_drop_page(ctx_, fzpage);
    }
    fz_catch(ctx_) {
        pm = PageMeasurement{};
    }

    measureCache_[pageNo] = pm;
    return pm;
}

std::array<double, 6> Document::pagePointToPdfMatrix(int pageNo) const
{
    const ScreenQuery query(*lifetime_, "Document::pagePointToPdfMatrix");
    if (query.refused())
        return {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    std::lock_guard<std::mutex> lk(access_);
    std::array<double, 6> out{1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    fz_page *fzpage = nullptr;
    fz_var(fzpage);
    fz_try(ctx_) {
        // Same content->app transform pageMeasurement() builds, inverted: app
        // page-point space -> PDF user space. pdf_page_transform already folds in
        // /Rotate (about the box centre), the MediaBox offset, and the y-flip.
        pdf_document *pdoc = pdf_specifics(ctx_, doc_);
        if (pdoc) {
            fzpage = fz_load_page(ctx_, doc_, pageNo);
            const fz_rect bound = fz_bound_page(ctx_, fzpage);
            fz_rect mbox;
            fz_matrix pageCtm;
            pdf_page_transform(ctx_, pdf_page_from_fz_page(ctx_, fzpage), &mbox, &pageCtm);
            (void)mbox;
            const fz_matrix toApp = fz_concat(pageCtm, fz_translate(-bound.x0, -bound.y0));
            const fz_matrix toPdf = fz_invert_matrix(toApp);
            out = {toPdf.a, toPdf.b, toPdf.c, toPdf.d, toPdf.e, toPdf.f};
        }
    }
    fz_always(ctx_) {
        if (fzpage)
            fz_drop_page(ctx_, fzpage);
    }
    fz_catch(ctx_) {
        out = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    }
    return out;
}

std::optional<PdfLinkTarget> Document::linkTargetAt(int pageNo, QPointF pagePoint) const
{
    const ScreenQuery query(*lifetime_, "Document::linkTargetAt");
    if (query.refused())
        return std::nullopt;
    std::lock_guard<std::mutex> lk(access_);
    if (pageNo < 0 || pageNo >= static_cast<int>(pageSizes_.size()))
        return std::nullopt;

    std::optional<PdfLinkTarget> target;
    fz_page *page = nullptr;
    fz_link *links = nullptr;
    fz_var(page);
    fz_var(links);
    fz_try(ctx_) {
        page = fz_load_page(ctx_, doc_, pageNo);
        const fz_rect bound = fz_bound_page(ctx_, page);
        links = fz_load_links(ctx_, page);

        const float px = static_cast<float>(pagePoint.x() + bound.x0);
        const float py = static_cast<float>(pagePoint.y() + bound.y0);
        for (fz_link *link = links; link; link = link->next) {
            if (!link->uri)
                continue;
            if (!rectContains(link->rect, px, py))
                continue;

            PdfLinkTarget hit;
            hit.uri = QString::fromUtf8(link->uri).trimmed();
            if (hit.uri.startsWith(QLatin1Char('#'))) {
                const fz_link_dest dest = fz_resolve_link_dest(ctx_, doc_, link->uri);
                const int p = fz_page_number_from_location(ctx_, doc_, dest.loc);
                if (p >= 0)
                    hit.page = p;
            }
            if (hit.valid()) {
                target = hit;
                break;
            }
        }
    }
    fz_always(ctx_) {
        if (links)
            fz_drop_link(ctx_, links);
        if (page)
            fz_drop_page(ctx_, page);
    }
    fz_catch(ctx_) {
        target.reset();
    }
    return target;
}

QString Document::linkAt(int pageNo, QPointF pagePoint) const
{
    const ScreenQuery query(*lifetime_, "Document::linkAt");
    if (query.refused())
        return {};
    const std::optional<PdfLinkTarget> target = linkTargetAt(pageNo, pagePoint);
    return target ? target->uri : QString();
}

std::optional<PdfItemProperties> Document::itemPropertiesAt(int pageNo, QPointF pagePoint) const
{
    const ScreenQuery query(*lifetime_, "Document::itemPropertiesAt");
    if (query.refused())
        return std::nullopt;
    std::lock_guard<std::mutex> lk(access_);
    if (pageNo < 0 || pageNo >= static_cast<int>(pageSizes_.size()))
        return std::nullopt;

    std::optional<PdfItemProperties> found;
    fz_page *fzpage = nullptr;
    fz_var(fzpage);
    fz_try(ctx_) {
        pdf_document *pdoc = pdf_specifics(ctx_, doc_);
        if (pdoc) {
            fzpage = fz_load_page(ctx_, doc_, pageNo);
            pdf_page *ppage = pdf_page_from_fz_page(ctx_, fzpage);
            const fz_rect bound = fz_bound_page(ctx_, fzpage);
            fz_rect crop;
            fz_matrix pageCtm;
            pdf_page_transform(ctx_, ppage, &crop, &pageCtm);
            (void)crop;

            pdf_obj *pageObj = pdf_lookup_page_obj(ctx_, pdoc, pageNo);
            pdf_obj *annots = pdf_resolve_indirect(ctx_, pdf_dict_get(ctx_, pageObj, PDF_NAME(Annots)));
            const float px = static_cast<float>(pagePoint.x() + bound.x0);
            const float py = static_cast<float>(pagePoint.y() + bound.y0);

            for (int i = pdf_array_len(ctx_, annots) - 1; i >= 0; --i) {
                pdf_obj *annot = pdf_resolve_indirect(ctx_, pdf_array_get(ctx_, annots, i));
                if (!pdf_is_dict(ctx_, annot))
                    continue;
                if (!pdf_name_eq(ctx_, pdf_dict_get(ctx_, annot, PDF_NAME(Subtype)), PDF_NAME(Link)))
                    continue;

                const fz_rect rect = fz_transform_rect(
                    pdf_to_rect(ctx_, pdf_dict_get(ctx_, annot, PDF_NAME(Rect))), pageCtm);
                if (!rectContains(rect, px, py))
                    continue;

                pdf_obj *action = pdf_resolve_indirect(ctx_, pdf_dict_get(ctx_, annot, PDF_NAME(A)));
                if (!pdf_name_eq(ctx_, pdf_dict_get(ctx_, action, PDF_NAME(S)), PDF_NAME(JavaScript)))
                    continue;
                pdf_obj *jsObj = pdf_dict_get(ctx_, action, PDF_NAME(JS));
                const char *js = pdf_to_text_string(ctx_, jsObj);
                if (!js)
                    continue;

                const QStringList values = parseKiCadPropertyScript(QString::fromUtf8(js));
                if (values.isEmpty())
                    continue;

                PdfItemProperties item;
                item.page = pageNo;
                item.rect = QRectF(rect.x0 - bound.x0, rect.y0 - bound.y0, rect.x1 - rect.x0,
                                   rect.y1 - rect.y0)
                                .normalized();
                item.values = values;
                found = item;
                break;
            }
        }
    }
    fz_always(ctx_) {
        if (fzpage)
            fz_drop_page(ctx_, fzpage);
    }
    fz_catch(ctx_) {
        found.reset();
    }
    return found;
}

namespace {

constexpr int kMaxSnapSegments = 200000;   // hard cap for pathological CAD pages
constexpr int kCurveFlattenSteps = 8;      // cubic Bézier -> this many line segments
constexpr double kVertexQuantum = 100.0;   // vertex-dedup grid: round to 0.01 pt

// A minimal fz_device that harvests path geometry. fz_device MUST be the first
// member so (fz_device*)gd and (GeomDevice*)dev alias. It is allocated with
// fz_calloc (via fz_new_derived_device), which zero-initializes and does NOT run
// C++ constructors, so it holds only trivially-zeroable members and every field
// is set explicitly after allocation / reset per path before use.
struct GeomDevice
{
    fz_device base;
    std::vector<QPointF> *verts;
    std::vector<std::pair<int, int>> *segs;
    std::unordered_map<long long, int> *index;
    bool *truncated;
    // Per-path walk state (reset in geomCollect before each fz_walk_path).
    fz_matrix ctm;
    int startIdx;
    int prevIdx;
    bool haveStart;
};

// Insert an already-transformed (app page-point space) point, deduplicated on a
// quantized grid. Returns the vertex index, or -1 once the segment cap is hit.
int geomVertexPt(GeomDevice *d, fz_point p)
{
    if (*d->truncated)
        return -1;
    const long long kx = std::llround(static_cast<double>(p.x) * kVertexQuantum);
    const long long ky = std::llround(static_cast<double>(p.y) * kVertexQuantum);
    const long long key = (kx << 32) ^ (ky & 0xffffffffLL);
    auto it = d->index->find(key);
    if (it != d->index->end())
        return it->second;
    const int id = static_cast<int>(d->verts->size());
    d->verts->push_back(QPointF(p.x, p.y));
    (*d->index)[key] = id;
    return id;
}

int geomVertex(GeomDevice *d, float x, float y)
{
    return geomVertexPt(d, fz_transform_point_xy(x, y, d->ctm));
}

void geomAddSeg(GeomDevice *d, int a, int b)
{
    if (a < 0 || b < 0 || a == b)
        return;
    if (static_cast<int>(d->segs->size()) >= kMaxSnapSegments) {
        *d->truncated = true;
        return;
    }
    d->segs->emplace_back(a, b);
}

void wMoveto(fz_context *, void *arg, float x, float y)
{
    auto *d = static_cast<GeomDevice *>(arg);
    d->startIdx = d->prevIdx = geomVertex(d, x, y);
    d->haveStart = (d->startIdx >= 0);
}

void wLineto(fz_context *, void *arg, float x, float y)
{
    auto *d = static_cast<GeomDevice *>(arg);
    const int id = geomVertex(d, x, y);
    geomAddSeg(d, d->prevIdx, id);
    if (id >= 0)
        d->prevIdx = id;
}

void wCurveto(fz_context *, void *arg, float x1, float y1, float x2, float y2, float x3, float y3)
{
    auto *d = static_cast<GeomDevice *>(arg);
    if (d->prevIdx < 0)
        return;
    const QPointF p0 = (*d->verts)[d->prevIdx]; // current point, already app-space
    const fz_point c1 = fz_transform_point_xy(x1, y1, d->ctm);
    const fz_point c2 = fz_transform_point_xy(x2, y2, d->ctm);
    const fz_point e = fz_transform_point_xy(x3, y3, d->ctm);
    for (int i = 1; i <= kCurveFlattenSteps; ++i) {
        const double t = static_cast<double>(i) / kCurveFlattenSteps;
        const double u = 1.0 - t;
        const double b0 = u * u * u;
        const double b1 = 3 * u * u * t;
        const double b2 = 3 * u * t * t;
        const double b3 = t * t * t;
        fz_point s;
        s.x = static_cast<float>(b0 * p0.x() + b1 * c1.x + b2 * c2.x + b3 * e.x);
        s.y = static_cast<float>(b0 * p0.y() + b1 * c1.y + b2 * c2.y + b3 * e.y);
        const int id = geomVertexPt(d, s);
        geomAddSeg(d, d->prevIdx, id);
        if (id >= 0)
            d->prevIdx = id;
    }
}

void wClosepath(fz_context *, void *arg)
{
    auto *d = static_cast<GeomDevice *>(arg);
    if (d->haveStart)
        geomAddSeg(d, d->prevIdx, d->startIdx);
    d->prevIdx = d->startIdx;
}

// Only the first four callbacks are supplied; MuPDF auto-simulates quadto /
// curvetov / curvetoy / rectto via these (so rectangles arrive as moveto +
// linetos + closepath, exactly what we want for corner/edge snapping).
const fz_path_walker kGeomWalker = {
    wMoveto, wLineto, wCurveto, wClosepath, nullptr, nullptr, nullptr, nullptr,
};

void geomCollect(fz_context *ctx, GeomDevice *d, const fz_path *path, fz_matrix ctm)
{
    if (*d->truncated)
        return;
    d->ctm = ctm;
    d->haveStart = false;
    d->startIdx = d->prevIdx = -1;
    fz_walk_path(ctx, path, &kGeomWalker, d);
}

void devFillPath(fz_context *ctx, fz_device *dev, const fz_path *path, int, fz_matrix ctm,
                 fz_colorspace *, const float *, float, fz_color_params)
{
    geomCollect(ctx, reinterpret_cast<GeomDevice *>(dev), path, ctm);
}

void devStrokePath(fz_context *ctx, fz_device *dev, const fz_path *path, const fz_stroke_state *,
                   fz_matrix ctm, fz_colorspace *, const float *, float, fz_color_params)
{
    geomCollect(ctx, reinterpret_cast<GeomDevice *>(dev), path, ctm);
}

void devClipPath(fz_context *ctx, fz_device *dev, const fz_path *path, int, fz_matrix ctm, fz_rect)
{
    geomCollect(ctx, reinterpret_cast<GeomDevice *>(dev), path, ctm);
}

void devClipStrokePath(fz_context *ctx, fz_device *dev, const fz_path *path, const fz_stroke_state *,
                       fz_matrix ctm, fz_rect)
{
    geomCollect(ctx, reinterpret_cast<GeomDevice *>(dev), path, ctm);
}

} // namespace

PageGeometry Document::pageGeometry(int pageNo) const
{
    const ScreenQuery query(*lifetime_, "Document::pageGeometry");
    if (query.refused())
        return {};
    std::lock_guard<std::mutex> lk(access_);
    if (pageNo < 0 || pageNo >= static_cast<int>(geomCache_.size()))
        return {};
    if (geomCache_[pageNo])
        return *geomCache_[pageNo];

    PageGeometry pg;
    std::unordered_map<long long, int> index;

    // The fz_new_derived_device macro hardcodes the token `ctx`, so alias it.
    fz_context *ctx = ctx_;
    fz_page *page = nullptr;
    fz_device *dev = nullptr;
    fz_var(page);
    fz_var(dev);
    fz_try(ctx) {
        page = fz_load_page(ctx, doc_, pageNo);
        const fz_rect bound = fz_bound_page(ctx, page);
        GeomDevice *gd = fz_new_derived_device(ctx, GeomDevice);
        dev = reinterpret_cast<fz_device *>(gd);
        gd->base.fill_path = devFillPath;
        gd->base.stroke_path = devStrokePath;
        gd->base.clip_path = devClipPath;
        gd->base.clip_stroke_path = devClipStrokePath;
        gd->verts = &pg.vertices;
        gd->segs = &pg.segments;
        gd->index = &index;
        gd->truncated = &pg.truncated;
        gd->ctm = fz_identity;
        gd->startIdx = gd->prevIdx = -1;
        gd->haveStart = false;
        // Shift so output lands in the app's (0,0)-based page-point space (matches
        // TextIndex's ox/oy and ViewerWidget::canvasToPagePoint).
        const fz_matrix run = fz_translate(-bound.x0, -bound.y0);
        fz_run_page(ctx, page, dev, run, nullptr);
        fz_close_device(ctx, dev); // may throw -> must be inside fz_try
    }
    fz_always(ctx) {
        if (dev)
            fz_drop_device(ctx, dev); // never throws -> always run
        if (page)
            fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        pg = PageGeometry{}; // best-effort: empty on failure
    }

    geomCache_[pageNo] = pg;
    return pg;
}

bool Document::hasForm() const
{
    const ScreenQuery query(*lifetime_, "Document::hasForm");
    if (query.refused())
        return false;
    std::lock_guard<std::mutex> lk(access_);
    if (formCache_)
        return *formCache_;
    bool has = false;
    fz_try(ctx_) {
        pdf_document *pdoc = pdf_specifics(ctx_, doc_);
        if (pdoc) {
            pdf_obj *root = pdf_dict_get(ctx_, pdf_trailer(ctx_, pdoc), PDF_NAME(Root));
            pdf_obj *acro = pdf_dict_get(ctx_, root, PDF_NAME(AcroForm));
            pdf_obj *fields = pdf_dict_get(ctx_, acro, PDF_NAME(Fields));
            has = pdf_is_array(ctx_, fields) && pdf_array_len(ctx_, fields) > 0;
        }
    }
    fz_catch(ctx_) {
        has = false;
    }
    formCache_ = has;
    return has;
}

namespace {

// Whether the measurement blob `stream` names only the encoding Mervin writes: no filter,
// or a single /FlateDecode as a name or a one-element array, and no /DecodeParms. MuPDF
// also reads /F for /Filter and /DP for /DecodeParms, so those keys are refused too. A
// predictor's parameters would size its buffers before any data is read, and chained
// Flate stages can spend time without producing output.
bool plainFlateBlob(fz_context *ctx, pdf_obj *stream)
{
    for (pdf_obj *key : {PDF_NAME(DecodeParms), PDF_NAME(DP), PDF_NAME(F)}) {
        if (!pdf_is_null(ctx, pdf_dict_get(ctx, stream, key)))
            return false;
    }
    pdf_obj *filter = pdf_dict_get(ctx, stream, PDF_NAME(Filter));
    if (pdf_is_array(ctx, filter) && pdf_array_len(ctx, filter) == 1)
        filter = pdf_array_get(ctx, filter, 0);
    return pdf_is_null(ctx, filter) || pdf_name_eq(ctx, filter, PDF_NAME(FlateDecode));
}

// Records that MuPDF warned while the measurement blob was read.
void noteBlobWarning(void *warned, const char *)
{
    *static_cast<bool *>(warned) = true;
}

} // namespace

std::optional<QByteArray> Document::mervinMeasurements() const
{
    const ScreenQuery query(*lifetime_, "Document::mervinMeasurements");
    if (query.refused())
        return std::nullopt;
    std::lock_guard<std::mutex> lk(access_);
    constexpr size_t limit = size_t(kMaxMeasurementBlobBytes);
    fz_stream *stream = nullptr;
    fz_buffer *data = nullptr;
    bool present = false;
    bool warned = false;
    fz_var(stream);
    fz_var(data);
    fz_var(present);
    fz_var(warned);
    // MuPDF ends a Flate stream with a bad checksum or missing data with only a warning
    // and returns what it decoded, so a warning during this read counts as a failure, as
    // in measurementPages::decodeBlob. Flushing first keeps an earlier warning out, and lets
    // a repeat of it reach the callback instead of only being counted.
    fz_flush_warnings(ctx_);
    void *previousUser = nullptr;
    fz_warning_cb *previous = fz_warning_callback(ctx_, &previousUser);
    fz_set_warning_callback(ctx_, noteBlobWarning, &warned);
    fz_try(ctx_) {
        pdf_document *pdoc = pdf_specifics(ctx_, doc_);
        pdf_obj *root = pdoc ? pdf_dict_get(ctx_, pdf_trailer(ctx_, pdoc), PDF_NAME(Root)) : nullptr;
        pdf_obj *blob = pdf_dict_gets(ctx_, root, "Mervin_Measurements");
        present = blob && !pdf_is_null(ctx_, blob);
        // Mervin writes only streams. Any other object, such as a string, is refused as
        // page operations refuse it, so both readers see the same blobs.
        if (pdf_is_stream(ctx_, blob) && plainFlateBlob(ctx_, blob)) {
            // Empty Flate blocks decode to nothing, so the encoded data is counted first,
            // without keeping it, and a blob past kMaxMeasurementBlobRawBytes is refused.
            constexpr size_t rawLimit = size_t(kMaxMeasurementBlobRawBytes);
            unsigned char chunk[16 * 1024];
            stream = pdf_open_raw_stream(ctx_, blob);
            size_t raw = 0;
            while (raw <= rawLimit) {
                const size_t count = fz_read(ctx_, stream, chunk, sizeof chunk);
                if (!count)
                    break;
                raw += count;
            }
            fz_drop_stream(ctx_, stream);
            stream = nullptr;
            // MuPDF decodes as it reads, so the read ends as soon as the data passes the limit.
            if (raw <= rawLimit) {
                stream = pdf_open_stream(ctx_, blob);
                data = fz_new_buffer(ctx_, 4096);
            }
            size_t total = 0;
            while (stream) {
                const size_t count = fz_read(ctx_, stream, chunk, sizeof chunk);
                // MuPDF can turn a decoding error into the end of the stream.
                if (stream->error || count > limit - total) {
                    fz_drop_buffer(ctx_, data);
                    data = nullptr;
                    break;
                }
                if (!count)
                    break;
                fz_append_data(ctx_, data, chunk, count);
                total += count;
            }
        }
    }
    fz_always(ctx_) {
        fz_drop_stream(ctx_, stream); // closing the Flate filter can warn too
        fz_flush_warnings(ctx_);
        fz_set_warning_callback(ctx_, previous, previousUser);
    }
    fz_catch(ctx_) {
        fz_drop_buffer(ctx_, data);
        data = nullptr;
    }
    if (warned) {
        fz_drop_buffer(ctx_, data);
        data = nullptr;
    }
    if (!data) {
        if (present)
            measurementsUnreadable_.store(true);
        return std::nullopt;
    }
    unsigned char *bytes = nullptr;
    const size_t length = fz_buffer_storage(ctx_, data, &bytes);
    QByteArray out(reinterpret_cast<const char *>(bytes), qsizetype(length));
    fz_drop_buffer(ctx_, data);
    return out;
}

bool Document::isPdf() const
{
    const ScreenQuery query(*lifetime_, "Document::isPdf");
    if (query.refused())
        return false;
    std::lock_guard<std::mutex> lk(access_);
    bool pdf = false;
    fz_try(ctx_)
        pdf = pdf_specifics(ctx_, doc_) != nullptr;
    fz_catch(ctx_)
        pdf = false;
    return pdf;
}

bool Document::withPdfDocument(const std::function<void(fz_context *, pdf_document *)> &fn,
                               bool *refused) const
{
    const ScreenQuery query(*lifetime_, "Document::withPdfDocument");
    if (refused)
        *refused = query.refused();
    if (query.refused())
        return false;
    std::lock_guard<std::mutex> lk(access_);
    pdf_document *pdoc = pdf_specifics(ctx_, doc_);
    if (!pdoc)
        return false;
    bool ok = true;
    fz_try(ctx_) {
        fn(ctx_, pdoc);
    }
    fz_catch(ctx_) {
        ok = false;
    }
    return ok;
}

bool Document::savePdfTo(const QString &tmpPath, QString *error) const
{
    const ScreenQuery query(*lifetime_, "Document::savePdfTo");
    // MuPDF treats filenames as UTF-8 and converts to wide chars internally on
    // Windows, so pass UTF-8 bytes (not the local 8-bit encoding).
    const QByteArray utf8 = tmpPath.toUtf8();
    bool wrote = false;
    const bool ran = !query.refused() && withPdfDocument([&](fz_context *ctx, pdf_document *pdoc) {
        pdf_write_options opts = {}; // value-init: every field 0
        // Full rewrite, not incremental: MuPDF's repaired (slightly-non-conformant)
        // documents cannot be saved incrementally, and a full rewrite is what the
        // detach -> replaceFileAtomic swap expects (a brand-new file).
        opts.do_incremental = 0;
        opts.do_compress = 1;
        opts.do_compress_images = 1;
        opts.do_compress_fonts = 1;
        opts.do_encrypt = PDF_ENCRYPT_KEEP; // keep the source's encryption verbatim
        pdf_save_document(ctx, pdoc, utf8.constData(), &opts);
        wrote = true;
    });
    if (!ran || !wrote) {
        if (error)
            *error = QCoreApplication::translate("mervin::Document", // not a QObject, so no tr()
                                                 "Could not write the PDF.");
        return false;
    }
    return true;
}

} // namespace mervin
