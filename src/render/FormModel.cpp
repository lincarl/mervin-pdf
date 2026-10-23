#include "render/FormModel.h"

#include "render/Document.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QByteArray>
#include <QObject>

#include <array>
#include <functional>

namespace mervin {

namespace {

FormFieldType mapType(int wt)
{
    switch (wt) {
    case PDF_WIDGET_TYPE_TEXT:        return FormFieldType::Text;
    case PDF_WIDGET_TYPE_CHECKBOX:    return FormFieldType::CheckBox;
    case PDF_WIDGET_TYPE_RADIOBUTTON: return FormFieldType::RadioButton;
    case PDF_WIDGET_TYPE_COMBOBOX:    return FormFieldType::ComboBox;
    case PDF_WIDGET_TYPE_LISTBOX:     return FormFieldType::ListBox;
    case PDF_WIDGET_TYPE_SIGNATURE:   return FormFieldType::Signature;
    default:                          return FormFieldType::PushButton; // BUTTON / unknown
    }
}

// MuPDF writes /V at the nearest ancestor with /T. Object identity, rather than
// the displayed name, keeps independent fields with duplicate names separate.
pdf_obj *fieldGroup(fz_context *ctx, pdf_obj *widget)
{
    // pdf_cycle imposes MuPDF's own depth bound. Stack storage also avoids
    // leaking C++ allocations if a PDF lookup throws through fz_try.
    std::array<pdf_cycle_list, 256> ancestors;
    pdf_cycle_list *previous = nullptr;
    size_t depth = 0;
    for (pdf_obj *obj = widget; obj; obj = pdf_dict_get(ctx, obj, PDF_NAME(Parent))) {
        if (depth == ancestors.size() || pdf_cycle(ctx, &ancestors[depth], previous, obj))
            fz_throw(ctx, FZ_ERROR_FORMAT, "cyclic or excessive form field parent chain");
        previous = &ancestors[depth++];
        if (pdf_dict_get(ctx, obj, PDF_NAME(T)))
            return obj;
    }
    return widget;
}

// Keep every sibling page open during the edit. MuPDF resynthesises open pages
// together and then clears its document flag, so opening siblings afterwards can
// leave their saved /AP stale. Page dictionaries also cover widgets without /P.
bool applyToWidget(const Document &doc, int page, int idx,
                   std::vector<std::pair<int, int>> &affected,
                   const QString *value,
                   const std::function<bool(fz_context *, pdf_annot *)> &op)
{
    affected.clear();
    if (page < 0 || page >= doc.pageCount() || idx < 0)
        return false;
    bool changed = false;
    const bool ran = doc.withPdfDocument([&](fz_context *ctx, pdf_document *pdf) {
        fz_page *fzpage = nullptr;
        std::vector<fz_page *> siblings;
        fz_var(fzpage);
        fz_var(siblings);
        fz_try(ctx) {
            fzpage = fz_load_page(ctx, doc.handle(), page);
            pdf_page *ppage = pdf_page_from_fz_page(ctx, fzpage);
            int i = 0;
            for (pdf_annot *w = pdf_first_widget(ctx, ppage); w;
                 w = pdf_next_widget(ctx, w), ++i) {
                if (i != idx)
                    continue;
                if (value && *value == QString::fromUtf8(pdf_annot_field_value(ctx, w)))
                    break;
                pdf_obj *group = fieldGroup(ctx, pdf_annot_obj(ctx, w));
                for (int p = 0; p < doc.pageCount(); ++p) {
                    pdf_obj *annots = pdf_dict_get(ctx, pdf_lookup_page_obj(ctx, pdf, p),
                                                  PDF_NAME(Annots));
                    bool sharesField = false;
                    for (int a = 0; a < pdf_array_len(ctx, annots); ++a) {
                        pdf_obj *annot = pdf_array_get(ctx, annots, a);
                        if (pdf_name_eq(ctx, pdf_dict_get(ctx, annot, PDF_NAME(Subtype)),
                                        PDF_NAME(Widget))
                            && pdf_objcmp(ctx, fieldGroup(ctx, annot), group) == 0) {
                            sharesField = true;
                            break;
                        }
                    }
                    if (!sharesField)
                        continue;
                    fz_page *loaded = p == page ? fzpage : fz_load_page(ctx, doc.handle(), p);
                    if (p != page)
                        siblings.push_back(loaded);
                    int field = 0;
                    for (pdf_annot *s = pdf_first_widget(ctx, pdf_page_from_fz_page(ctx, loaded));
                         s; s = pdf_next_widget(ctx, s), ++field)
                        if (pdf_objcmp(ctx, fieldGroup(ctx, pdf_annot_obj(ctx, s)), group) == 0)
                            affected.emplace_back(p, field);
                }
                changed = op(ctx, w);
                if (changed)
                    pdf_update_page(ctx, ppage);
                break;
            }
        }
        fz_always(ctx) {
            for (fz_page *sibling : siblings)
                fz_drop_page(ctx, sibling);
            if (fzpage)
                fz_drop_page(ctx, fzpage);
        }
        fz_catch(ctx) {
            fz_rethrow(ctx); // propagate so withPdfDocument reports failure
        }
    });
    if (!ran || !changed)
        affected.clear();
    return ran && changed;
}

} // namespace

FormModel::FormModel(const Document &doc) : doc_(doc)
{
    cache_.resize(static_cast<size_t>(doc_.pageCount()));
}

const std::vector<FormField> &FormModel::pageFields(int pageNo) const
{
    static const std::vector<FormField> kEmpty;
    if (pageNo < 0 || pageNo >= static_cast<int>(cache_.size()))
        return kEmpty;
    if (cache_[pageNo])
        return *cache_[pageNo];

    std::vector<FormField> fields;
    bool refused = false;
    doc_.withPdfDocument([&](fz_context *ctx, pdf_document *) {
        fz_page *fzpage = nullptr;
        fz_var(fzpage);
        fz_try(ctx) {
            fzpage = fz_load_page(ctx, doc_.handle(), pageNo);
            pdf_page *ppage = pdf_page_from_fz_page(ctx, fzpage);
            // pdf_bound_widget already folds in the page CTM (/Rotate, MediaBox
            // offset, y-flip) - it returns the rect in the same space as
            // fz_bound_page - so mapping to app page-point space (top-left origin,
            // [0,w]x[0,h], the space pageSize() and the viewer use) is just a
            // shift by the page bound's origin. No matrix inversion needed.
            const fz_rect bound = fz_bound_page(ctx, fzpage);
            for (pdf_annot *w = pdf_first_widget(ctx, ppage); w;
                 w = pdf_next_widget(ctx, w)) {
                FormField f;
                f.page = pageNo;
                f.type = mapType(pdf_widget_type(ctx, w));
                const fz_rect r = pdf_bound_widget(ctx, w);
                f.rect = QRectF(r.x0 - bound.x0, r.y0 - bound.y0, r.x1 - r.x0,
                                r.y1 - r.y0);
                if (const char *v = pdf_annot_field_value(ctx, w))
                    f.value = QString::fromUtf8(v);
                if (char *nm = pdf_load_field_name(ctx, pdf_annot_obj(ctx, w))) {
                    f.name = QString::fromUtf8(nm);
                    fz_free(ctx, nm); // pdf_load_field_name returns a freeable copy
                }
                f.flags = static_cast<unsigned>(pdf_annot_field_flags(ctx, w));

                // Resolve the field's text size from its /DA (the official API
                // folds in AcroForm-level /DA inheritance). daSize is in page
                // points; 0 is the auto-size sentinel. daFont points into
                // MuPDF-owned memory - copy it immediately, never store the ptr.
                const char *daFont = nullptr;
                float daSize = 0.0f;
                int daN = 0;
                float daColor[4] = {0, 0, 0, 0};
                pdf_annot_default_appearance(ctx, w, &daFont, &daSize, &daN, daColor);

                // base14 tag -> Qt family (glyph shape only; the size match is
                // independent of this). Unknown/absent -> Helvetica, matching
                // MuPDF's full_font_name fallback.
                const QString daTag = QString::fromUtf8(daFont ? daFont : "");
                if (daTag == QLatin1String("Cour"))
                    f.fontFamily = QStringLiteral("Courier New");
                else if (daTag == QLatin1String("TiRo"))
                    f.fontFamily = QStringLiteral("Times New Roman");
                else if (daTag == QLatin1String("Symb"))
                    f.fontFamily = QStringLiteral("Symbol");
                else if (daTag == QLatin1String("ZaDb"))
                    f.fontFamily = QStringLiteral("ZapfDingbats");
                else
                    f.fontFamily = QStringLiteral("Helvetica"); // "Helv" + unknown

                if (daSize > 0.0f) {
                    // Explicit /DA size, used verbatim for every field type. This
                    // is also the common case: a field with NO /DA at all comes
                    // back as 12 pt (MuPDF's hard-coded default), not as auto-size.
                    f.fontSizePt = daSize;
                } else {
                    // For 0 Tf, mirror MuPDF: multiline/list use 12pt; single-line/comb/combo
                    // use the inner-height cap, appropriate for short/empty editing values.
                    if ((f.type == FormFieldType::Text && f.multiline())
                        || f.type == FormFieldType::ListBox) {
                        f.fontSizePt = 12.0f;
                    } else {
                        // Inner height = rect.height - 2*padding; padding is b*2 for
                        // a plain single-line field, while comb passes padding 0
                        // (so it keeps the full rect height). b = border width.
                        const float b = pdf_annot_border_width(ctx, w);
                        const bool isComb = (f.type == FormFieldType::Text && f.comb());
                        float innerH = isComb
                                           ? static_cast<float>(f.rect.height())
                                           : static_cast<float>(f.rect.height()) - 4.0f * b;
                        if (innerH < 4.0f) // empty / degenerate rect guard
                            innerH = 12.0f;
                        // Qt line height is about 1.16x point size; reduce height-derived auto-
                        // size to fit descenders. Preserve explicit /DA and fixed 12pt sizes.
                        f.fontSizePt = innerH * 0.85f;
                    }
                }

                if (f.type == FormFieldType::ComboBox
                    || f.type == FormFieldType::ListBox) {
                    const int n = pdf_choice_widget_options(ctx, w, 0, nullptr);
                    if (n > 0) {
                        std::vector<const char *> opts(static_cast<size_t>(n), nullptr);
                        pdf_choice_widget_options(ctx, w, 0, opts.data());
                        for (int k = 0; k < n; ++k)
                            f.options << QString::fromUtf8(opts[k] ? opts[k] : "");
                    }
                }
                fields.push_back(std::move(f));
            }
        }
        fz_always(ctx) {
            if (fzpage)
                fz_drop_page(ctx, fzpage);
        }
        fz_catch(ctx) {
            fields.clear();
        }
    }, &refused);

    // withPdfDocument refuses on the screen thread while a job holds the document or
    // waits for it. The page is then read on a later call. Every other answer is cached,
    // the empty one of a document that is not a PDF included.
    if (refused)
        return kEmpty;
    cache_[pageNo] = std::move(fields);
    return *cache_[pageNo];
}

bool FormModel::setTextValue(int page, int fieldIndex, const QString &value)
{
    const bool changed = applyToWidget(doc_, page, fieldIndex, changedFields_, &value,
                                      [&](fz_context *ctx, pdf_annot *w) {
        const QByteArray v = value.toUtf8();
        return pdf_set_text_field_value(ctx, w, v.constData()) != 0;
    });
    if (changed) {
        for (const auto &[p, field] : changedFields_)
            invalidatePage(p);
        dirty_ = true;
    }
    return changed;
}

bool FormModel::setChoiceValue(int page, int fieldIndex, const QString &value)
{
    const bool changed = applyToWidget(doc_, page, fieldIndex, changedFields_, &value,
                                      [&](fz_context *ctx, pdf_annot *w) {
        const QByteArray v = value.toUtf8();
        return pdf_set_choice_field_value(ctx, w, v.constData()) != 0;
    });
    if (changed) {
        for (const auto &[p, field] : changedFields_)
            invalidatePage(p);
        dirty_ = true;
    }
    return changed;
}

bool FormModel::toggle(int page, int fieldIndex)
{
    const bool changed = applyToWidget(doc_, page, fieldIndex, changedFields_, nullptr,
                                      [&](fz_context *ctx, pdf_annot *w) {
        const QString before = QString::fromUtf8(pdf_annot_field_value(ctx, w));
        const QString appearance = QString::fromUtf8(
            pdf_to_name(ctx, pdf_dict_get(ctx, pdf_annot_obj(ctx, w), PDF_NAME(AS))));
        return pdf_toggle_widget(ctx, w) != 0
               && (before != QString::fromUtf8(pdf_annot_field_value(ctx, w))
                   || appearance != QString::fromUtf8(
                       pdf_to_name(ctx, pdf_dict_get(ctx, pdf_annot_obj(ctx, w), PDF_NAME(AS)))));
    });
    if (changed) {
        for (const auto &[p, field] : changedFields_)
            invalidatePage(p);
        dirty_ = true;
    }
    return changed;
}

bool FormModel::saveTo(const QString &tmpPath, QString *error) const
{
    // Filled values live in the live pdf_document; saving is just writing that
    // document. Delegates to the shared Document::savePdfTo (full rewrite,
    // PDF_ENCRYPT_KEEP) - the same path AnnotModel edits ride, since both mutate
    // the one pdf_document, so a single save captures form values AND annotations.
    return doc_.savePdfTo(tmpPath, error);
}

void FormModel::reset()
{
    changedFields_.clear();
    for (auto &c : cache_)
        c.reset();
}

void FormModel::invalidatePage(int page)
{
    if (page >= 0 && page < static_cast<int>(cache_.size()))
        cache_[page].reset();
}

} // namespace mervin
