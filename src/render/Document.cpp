#include "render/Document.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QByteArray>
#include <QObject>

#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace mervin {

Document::Document(fz_context *baseCtx, fz_document *doc)
    : ctx_(baseCtx), doc_(doc)
{
    int n = 0;
    fz_try(ctx_)
        n = fz_count_pages(ctx_, doc_);
    fz_catch(ctx_)
        n = 0;

    // A PDF's page sizes are read from the page OBJECT, not from a loaded page.
    // fz_load_page builds a pdf_page and, for any page carrying /Annots, eagerly
    // resolves every annotation and link on it (pdf_load_page_imp) - all of which
    // is dropped again one line later here. On link-heavy exports (a KiCad
    // schematic carries a Link annotation per component) that preload dominated
    // document open, and on a cold file cache it turned every open into thousands
    // of scattered reads. pdf_page_obj_transform reads the same inheritable
    // /MediaBox + /CropBox + /Rotate + /UserUnit that fz_bound_page ends up
    // using - it requests the crop box too - so the sizes are identical, just
    // without building the page. Non-PDF documents (XPS, CBZ, image files) have
    // no page objects and keep the generic path below.
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

        if (w <= 0.0 || h <= 0.0) {
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

        if (w <= 0.0 || h <= 0.0) { // US Letter fallback if a page won't load at all
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
}

Document::~Document()
{
    if (doc_)
        fz_drop_document(ctx_, doc_);
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

            // /VP BBoxes are in PDF user space (y-up). The cursor and the snap
            // geometry live in the app's page-point space (y-down, shifted to a
            // 0-based origin - see pageGeometry). Build the same content->app
            // transform and apply it to each BBox, otherwise resolveScale's
            // bbox-containment test is wrong off-centre: a y-flip is invisible at
            // the page centre (so an unflipped bbox appeared to work there) but
            // mismatches everywhere else, which flipped the readout to 1:1.
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
    const std::optional<PdfLinkTarget> target = linkTargetAt(pageNo, pagePoint);
    return target ? target->uri : QString();
}

std::optional<PdfItemProperties> Document::itemPropertiesAt(int pageNo, QPointF pagePoint) const
{
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

bool Document::hasMervinMeasurements() const
{
    std::lock_guard<std::mutex> lk(access_);
    if (mervinBlobCache_)
        return *mervinBlobCache_;
    bool has = false;
    fz_try(ctx_) {
        pdf_document *pdoc = pdf_specifics(ctx_, doc_);
        if (pdoc) {
            pdf_obj *root = pdf_dict_get(ctx_, pdf_trailer(ctx_, pdoc), PDF_NAME(Root));
            pdf_obj *blob = pdf_dict_gets(ctx_, root, "Mervin_Measurements");
            has = blob != nullptr && !pdf_is_null(ctx_, blob);
        }
    }
    fz_catch(ctx_) {
        has = false;
    }
    mervinBlobCache_ = has;
    return has;
}

bool Document::isPdf() const
{
    std::lock_guard<std::mutex> lk(access_);
    bool pdf = false;
    fz_try(ctx_)
        pdf = pdf_specifics(ctx_, doc_) != nullptr;
    fz_catch(ctx_)
        pdf = false;
    return pdf;
}

bool Document::withPdfDocument(const std::function<void(fz_context *, pdf_document *)> &fn) const
{
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
    // MuPDF treats filenames as UTF-8 and converts to wide chars internally on
    // Windows, so pass UTF-8 bytes (not the local 8-bit encoding).
    const QByteArray utf8 = tmpPath.toUtf8();
    bool wrote = false;
    const bool ran = withPdfDocument([&](fz_context *ctx, pdf_document *pdoc) {
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
            *error = QObject::tr("Could not write the PDF.");
        return false;
    }
    return true;
}

} // namespace mervin
