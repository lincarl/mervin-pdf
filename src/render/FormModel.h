#pragma once

#include "render/FormTypes.h"

#include <QString>

#include <optional>
#include <vector>

namespace mervin {

class Document;

// The sole owner of all AcroForm widget mutation in the app, keeping MuPDF
// confined to render/ (see design §6). Bound to one open Document; every access
// runs through Document::withPdfDocument(), which holds the document's access
// mutex and yields the live pdf_document, so a field edit is serialised against
// the render workers' page loads exactly like any other object-model touch.
//
// Field values persist as standard AcroForm /V + /AP (written by saveTo via
// pdf_save_document), so there is no private blob to restore - re-opening a saved
// file just re-enumerates /V. GUI-free; lives in mervin_core.
//
// Lifetime: holds the Document by reference and must not outlive it. The viewer
// recreates the FormModel whenever it binds a new Document, so the reference is
// always valid for the model's lifetime.
class FormModel
{
public:
    explicit FormModel(const Document &doc);

    // The fillable widgets on a page, in stable MuPDF widget order (the index
    // space the mutation methods below use). Lazily enumerated and cached in app
    // page-point space. Returns a reference valid until the next mutation of that
    // page (which invalidates its cache) or reset().
    const std::vector<FormField> &pageFields(int pageNo) const;

    // Whether any field has been edited since open / the last clearDirty().
    bool isDirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

    // Mutations. `fieldIndex` indexes into pageFields(page). Each resynthesises the
    // touched page's widget appearances (pdf_update_page) and, on a real change,
    // invalidates that page's enumeration cache and marks the model dirty. Returns
    // true iff the value actually changed (so the caller knows to re-render).
    bool setTextValue(int page, int fieldIndex, const QString &value);
    bool setChoiceValue(int page, int fieldIndex, const QString &value);
    bool toggle(int page, int fieldIndex);

    // Persist the live (filled) document to `tmpPath` via a full MuPDF rewrite
    // (do_incremental = 0, do_encrypt = PDF_ENCRYPT_KEEP). The caller swaps it over
    // the original with the measurement atomic-swap flow. Returns false on failure
    // (and sets *error). Const: writing the document does not change model state.
    bool saveTo(const QString &tmpPath, QString *error = nullptr) const;

    // Drop all cached enumeration (the bound Document's object model may have
    // changed underneath us). Does not clear the dirty flag.
    void reset();

private:
    void invalidatePage(int page);

    const Document &doc_;
    mutable std::vector<std::optional<std::vector<FormField>>> cache_; // lazy, per page
    bool dirty_ = false;
};

} // namespace mervin
