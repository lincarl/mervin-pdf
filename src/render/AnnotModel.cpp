#include "render/AnnotModel.h"

#include "render/Document.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <ctime>
#include <functional>

namespace mervin {

namespace {

AnnotType mapType(int at)
{
    switch (at) {
    case PDF_ANNOT_HIGHLIGHT:  return AnnotType::Highlight;
    case PDF_ANNOT_UNDERLINE:  return AnnotType::Underline;
    case PDF_ANNOT_STRIKE_OUT: return AnnotType::StrikeOut;
    case PDF_ANNOT_TEXT:       return AnnotType::Text;
    default:                   return AnnotType::Other;
    }
}

enum pdf_annot_type pdfTypeOf(AnnotType t)
{
    switch (t) {
    case AnnotType::Highlight: return PDF_ANNOT_HIGHLIGHT;
    case AnnotType::Underline: return PDF_ANNOT_UNDERLINE;
    case AnnotType::StrikeOut: return PDF_ANNOT_STRIKE_OUT;
    case AnnotType::Text:      return PDF_ANNOT_TEXT;
    default:                   return PDF_ANNOT_HIGHLIGHT;
    }
}

// Read an annotation's stroke colour (n=1 grey / 3 rgb / 4 cmyk) into a QColor.
// Falls back to the default highlight yellow when the annotation carries none.
QColor readColor(fz_context *ctx, pdf_annot *a)
{
    int n = 0;
    float c[4] = {0, 0, 0, 0};
    pdf_annot_color(ctx, a, &n, c);
    if (n == 1)
        return QColor::fromRgbF(c[0], c[0], c[0]);
    if (n == 3)
        return QColor::fromRgbF(c[0], c[1], c[2]);
    if (n == 4) { // CMYK -> RGB
        return QColor::fromRgbF((1 - c[0]) * (1 - c[3]), (1 - c[1]) * (1 - c[3]),
                                (1 - c[2]) * (1 - c[3]));
    }
    return annot::defaultColor(); // the markup preset yellow
}

// Build one Annotation value from a live pdf_annot. `boundOrigin` is the page's
// fz_bound_page top-left. pdf_bound_annot already folds in the page CTM (/Rotate,
// MediaBox offset, y-flip) - it returns the rect in the same fz "doc space" as
// fz_bound_page - so mapping to app page-point space (top-left origin, y-down,
// the space rangeRects / FormField::rect / Measurement::pts use) is just a shift
// by the page bound's origin, exactly as FormModel maps widget rects. The
// creation helpers below place coordinates in the inverse of this (app + origin),
// so a created annotation re-enumerates back onto the same page location.
Annotation readAnnot(fz_context *ctx, pdf_annot *a, int pageNo, fz_point boundOrigin)
{
    Annotation an;
    an.page = pageNo;
    an.id = pdf_to_num(ctx, pdf_annot_obj(ctx, a));
    an.type = mapType(pdf_annot_type(ctx, a));
    const fz_rect r = pdf_bound_annot(ctx, a);
    an.rect = QRectF(r.x0 - boundOrigin.x, r.y0 - boundOrigin.y, r.x1 - r.x0, r.y1 - r.y0);
    an.color = readColor(ctx, a);
    if (const char *cs = pdf_annot_contents(ctx, a))
        an.contents = QString::fromUtf8(cs);
    if (const char *au = pdf_annot_author(ctx, a))
        an.author = QString::fromUtf8(au);
    const int64_t m = pdf_annot_modification_date(ctx, a);
    an.modifiedMs = m > 0 ? static_cast<qint64>(m) * 1000 : 0;
    return an;
}

// Walk to the annotation with object number `id` on `page` under the document
// lock and apply `op` (which returns whether it changed the annotation). On a
// real change, resynthesise the annotation + page appearances. Returns true iff
// something changed.
bool applyToAnnot(const Document &doc, int page, int id,
                  const std::function<bool(fz_context *, pdf_annot *)> &op)
{
    bool changed = false;
    const bool ran = doc.withPdfDocument([&](fz_context *ctx, pdf_document *) {
        fz_page *fzpage = nullptr;
        fz_var(fzpage);
        fz_try(ctx) {
            fzpage = fz_load_page(ctx, doc.handle(), page);
            pdf_page *ppage = pdf_page_from_fz_page(ctx, fzpage);
            for (pdf_annot *a = pdf_first_annot(ctx, ppage); a; a = pdf_next_annot(ctx, a)) {
                if (pdf_to_num(ctx, pdf_annot_obj(ctx, a)) != id)
                    continue;
                changed = op(ctx, a);
                if (changed) {
                    pdf_update_annot(ctx, a);
                    pdf_update_page(ctx, ppage);
                }
                break;
            }
        }
        fz_always(ctx) {
            if (fzpage)
                fz_drop_page(ctx, fzpage);
        }
        fz_catch(ctx) {
            fz_rethrow(ctx); // propagate so withPdfDocument reports failure
        }
    });
    return ran && changed;
}

} // namespace

AnnotModel::AnnotModel(const Document &doc) : doc_(doc)
{
    cache_.resize(static_cast<size_t>(doc_.pageCount()));
}

const std::vector<Annotation> &AnnotModel::pageAnnots(int pageNo) const
{
    static const std::vector<Annotation> kEmpty;
    if (pageNo < 0 || pageNo >= static_cast<int>(cache_.size()))
        return kEmpty;
    if (cache_[pageNo])
        return *cache_[pageNo];

    std::vector<Annotation> annots;
    const bool ran = doc_.withPdfDocument([&](fz_context *ctx, pdf_document *) {
        fz_page *fzpage = nullptr;
        fz_var(fzpage);
        fz_try(ctx) {
            fzpage = fz_load_page(ctx, doc_.handle(), pageNo);
            pdf_page *ppage = pdf_page_from_fz_page(ctx, fzpage);
            const fz_rect bound = fz_bound_page(ctx, fzpage);
            const fz_point origin = {bound.x0, bound.y0};
            for (pdf_annot *a = pdf_first_annot(ctx, ppage); a; a = pdf_next_annot(ctx, a)) {
                // Widget (form-field) annotations are owned by FormModel; never
                // list or touch them here. Popup annotations are the editor boxes
                // for markup and carry no standalone content - skip them too.
                const int raw = pdf_annot_type(ctx, a);
                if (raw == PDF_ANNOT_WIDGET || raw == PDF_ANNOT_POPUP)
                    continue;
                annots.push_back(readAnnot(ctx, a, pageNo, origin));
            }
        }
        fz_always(ctx) {
            if (fzpage)
                fz_drop_page(ctx, fzpage);
        }
        fz_catch(ctx) {
            annots.clear();
        }
    });

    // Cache only on success: a transient enumeration failure (withPdfDocument
    // returned false) leaves the slot empty so the next access retries, rather
    // than permanently presenting the page as annotation-free.
    if (!ran)
        return kEmpty;
    cache_[pageNo] = std::move(annots);
    return *cache_[pageNo];
}

std::vector<Annotation> AnnotModel::allAnnots() const
{
    std::vector<Annotation> out;
    for (int p = 0; p < static_cast<int>(cache_.size()); ++p) {
        const std::vector<Annotation> &pa = pageAnnots(p);
        out.insert(out.end(), pa.begin(), pa.end());
    }
    return out;
}

std::optional<Annotation> AnnotModel::annot(int page, int id) const
{
    for (const Annotation &a : pageAnnots(page))
        if (a.id == id)
            return a;
    return std::nullopt;
}

int AnnotModel::addTextMarkup(int page, AnnotType type, const std::vector<QRectF> &lineRects,
                              const QColor &color, const QString &author, const QString &contents)
{
    if (!isTextMarkup(type) || lineRects.empty())
        return -1;
    const QByteArray authorUtf8 = author.toUtf8();
    const QByteArray contentsUtf8 = contents.toUtf8();
    const float rgb[3] = {static_cast<float>(color.redF()), static_cast<float>(color.greenF()),
                          static_cast<float>(color.blueF())};
    int newId = -1;
    const bool ran = doc_.withPdfDocument([&](fz_context *ctx, pdf_document *) {
        fz_page *fzpage = nullptr;
        pdf_annot *a = nullptr;
        fz_var(fzpage);
        fz_var(a);
        fz_try(ctx) {
            fzpage = fz_load_page(ctx, doc_.handle(), page);
            pdf_page *ppage = pdf_page_from_fz_page(ctx, fzpage);
            // MuPDF's annotation setters take coordinates in fz "doc space"
            // (fz_bound_page space, y-down) and apply the inverse page CTM
            // internally when writing the PDF object - so app page-point coords
            // map to doc space by adding the page bound origin (the inverse of the
            // enumeration shift). No y-flip / matrix here: doc space is y-down.
            const fz_rect bound = fz_bound_page(ctx, fzpage);
            const float ox = bound.x0, oy = bound.y0;
            a = pdf_create_annot(ctx, ppage, pdfTypeOf(type));
            for (const QRectF &r : lineRects) {
                if (r.isEmpty())
                    continue;
                // Doc space is y-down, so r.top is the visual top (UL/UR) and
                // r.bottom the visual bottom (LL/LR). After MuPDF's internal flip
                // to user space, LL/LR become the lower edge - the convention its
                // underline/strike-out appearance synthesis expects.
                const fz_point ul = {static_cast<float>(r.left()) + ox,
                                     static_cast<float>(r.top()) + oy};
                const fz_point ur = {static_cast<float>(r.right()) + ox,
                                     static_cast<float>(r.top()) + oy};
                const fz_point ll = {static_cast<float>(r.left()) + ox,
                                     static_cast<float>(r.bottom()) + oy};
                const fz_point lr = {static_cast<float>(r.right()) + ox,
                                     static_cast<float>(r.bottom()) + oy};
                const fz_quad q = {ul, ur, ll, lr};
                pdf_add_annot_quad_point(ctx, a, q);
            }
            pdf_set_annot_color(ctx, a, 3, rgb);
            if (!author.isEmpty())
                pdf_set_annot_author(ctx, a, authorUtf8.constData());
            if (!contents.isEmpty())
                pdf_set_annot_contents(ctx, a, contentsUtf8.constData());
            const int64_t now = static_cast<int64_t>(::time(nullptr));
            pdf_set_annot_creation_date(ctx, a, now);
            pdf_set_annot_modification_date(ctx, a, now);
            pdf_update_annot(ctx, a);
            pdf_update_page(ctx, ppage);
            newId = pdf_to_num(ctx, pdf_annot_obj(ctx, a));
        }
        fz_always(ctx) {
            if (a)
                pdf_drop_annot(ctx, a); // pdf_create_annot returns an owned ref
            if (fzpage)
                fz_drop_page(ctx, fzpage);
        }
        fz_catch(ctx) {
            newId = -1;
            fz_rethrow(ctx);
        }
    });
    if (ran && newId >= 0) {
        invalidatePage(page);
        dirty_ = true;
        return newId;
    }
    return -1;
}

int AnnotModel::addTextNote(int page, QPointF at, const QColor &color, const QString &author,
                            const QString &contents)
{
    const QByteArray authorUtf8 = author.toUtf8();
    const QByteArray contentsUtf8 = contents.toUtf8();
    const float rgb[3] = {static_cast<float>(color.redF()), static_cast<float>(color.greenF()),
                          static_cast<float>(color.blueF())};
    int newId = -1;
    const bool ran = doc_.withPdfDocument([&](fz_context *ctx, pdf_document *) {
        fz_page *fzpage = nullptr;
        pdf_annot *a = nullptr;
        fz_var(fzpage);
        fz_var(a);
        fz_try(ctx) {
            fzpage = fz_load_page(ctx, doc_.handle(), page);
            pdf_page *ppage = pdf_page_from_fz_page(ctx, fzpage);
            // Doc space (y-down) = app page-point + page bound origin (see
            // addTextMarkup). MuPDF applies the inverse page CTM internally.
            const fz_rect bound = fz_bound_page(ctx, fzpage);
            const float px = static_cast<float>(at.x()) + bound.x0;
            const float py = static_cast<float>(at.y()) + bound.y0;
            a = pdf_create_annot(ctx, ppage, PDF_ANNOT_TEXT);
            // A Text annotation's rect sizes the note icon; MuPDF synthesises a
            // standard ~20pt comment icon anchored at the rect.
            const fz_rect rect = {px, py, px + 20.f, py + 20.f};
            pdf_set_annot_rect(ctx, a, rect);
            pdf_set_annot_icon_name(ctx, a, "Comment");
            pdf_set_annot_color(ctx, a, 3, rgb);
            if (!author.isEmpty())
                pdf_set_annot_author(ctx, a, authorUtf8.constData());
            if (!contents.isEmpty())
                pdf_set_annot_contents(ctx, a, contentsUtf8.constData());
            const int64_t now = static_cast<int64_t>(::time(nullptr));
            pdf_set_annot_creation_date(ctx, a, now);
            pdf_set_annot_modification_date(ctx, a, now);
            pdf_update_annot(ctx, a);
            pdf_update_page(ctx, ppage);
            newId = pdf_to_num(ctx, pdf_annot_obj(ctx, a));
        }
        fz_always(ctx) {
            if (a)
                pdf_drop_annot(ctx, a);
            if (fzpage)
                fz_drop_page(ctx, fzpage);
        }
        fz_catch(ctx) {
            newId = -1;
            fz_rethrow(ctx);
        }
    });
    if (ran && newId >= 0) {
        invalidatePage(page);
        dirty_ = true;
        return newId;
    }
    return -1;
}

bool AnnotModel::setContents(int page, int id, const QString &contents)
{
    const QByteArray utf8 = contents.toUtf8();
    const bool changed = applyToAnnot(doc_, page, id, [&](fz_context *ctx, pdf_annot *a) {
        const char *cur = pdf_annot_contents(ctx, a);
        if (contents == QString::fromUtf8(cur ? cur : ""))
            return false;
        pdf_set_annot_contents(ctx, a, utf8.constData());
        pdf_set_annot_modification_date(ctx, a, static_cast<int64_t>(::time(nullptr)));
        return true;
    });
    if (changed) {
        invalidatePage(page);
        dirty_ = true;
    }
    return changed;
}

bool AnnotModel::setColor(int page, int id, const QColor &color)
{
    const float rgb[3] = {static_cast<float>(color.redF()), static_cast<float>(color.greenF()),
                          static_cast<float>(color.blueF())};
    const bool changed = applyToAnnot(doc_, page, id, [&](fz_context *ctx, pdf_annot *a) {
        int n = 0;
        float cur[4] = {0, 0, 0, 0};
        pdf_annot_color(ctx, a, &n, cur);
        if (n == 3 && qFuzzyCompare(cur[0] + 1, rgb[0] + 1) && qFuzzyCompare(cur[1] + 1, rgb[1] + 1)
            && qFuzzyCompare(cur[2] + 1, rgb[2] + 1))
            return false;
        pdf_set_annot_color(ctx, a, 3, rgb);
        pdf_set_annot_modification_date(ctx, a, static_cast<int64_t>(::time(nullptr)));
        return true;
    });
    if (changed) {
        invalidatePage(page);
        dirty_ = true;
    }
    return changed;
}

bool AnnotModel::remove(int page, int id)
{
    bool removed = false;
    const bool ran = doc_.withPdfDocument([&](fz_context *ctx, pdf_document *) {
        fz_page *fzpage = nullptr;
        fz_var(fzpage);
        fz_try(ctx) {
            fzpage = fz_load_page(ctx, doc_.handle(), page);
            pdf_page *ppage = pdf_page_from_fz_page(ctx, fzpage);
            for (pdf_annot *a = pdf_first_annot(ctx, ppage); a; a = pdf_next_annot(ctx, a)) {
                if (pdf_to_num(ctx, pdf_annot_obj(ctx, a)) != id)
                    continue;
                pdf_delete_annot(ctx, ppage, a);
                pdf_update_page(ctx, ppage);
                removed = true;
                break;
            }
        }
        fz_always(ctx) {
            if (fzpage)
                fz_drop_page(ctx, fzpage);
        }
        fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });
    if (ran && removed) {
        invalidatePage(page);
        dirty_ = true;
    }
    return ran && removed;
}

void AnnotModel::reset()
{
    for (auto &c : cache_)
        c.reset();
}

void AnnotModel::invalidatePage(int page)
{
    if (page >= 0 && page < static_cast<int>(cache_.size()))
        cache_[page].reset();
}

} // namespace mervin
