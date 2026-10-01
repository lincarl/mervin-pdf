#pragma once

#include "render/FormTypes.h"

#include <QString>

#include <optional>
#include <vector>

namespace mervin {

class Document;

// Owns AcroForm mutations for one Document. withPdfDocument serializes access against page
// loads; standard /V and /AP persist through savePdfTo. Recreate on document rebinding and
// destroy before the referenced Document.
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
