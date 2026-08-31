#pragma once

#include <QRectF>
#include <QString>
#include <QStringList>

namespace mervin {

// The kind of fillable AcroForm widget, collapsed from MuPDF's pdf_widget_type
// into the set the fill UI distinguishes. Push buttons and signatures are
// surfaced for completeness but are never editable (see FormField::editable).
enum class FormFieldType {
    Text,        // single- or multi-line text entry  (PDF_WIDGET_TYPE_TEXT)
    CheckBox,    // toggle                              (PDF_WIDGET_TYPE_CHECKBOX)
    RadioButton, // one-of-group toggle                 (PDF_WIDGET_TYPE_RADIOBUTTON)
    ComboBox,    // drop-down choice                    (PDF_WIDGET_TYPE_COMBOBOX)
    ListBox,     // list choice                         (PDF_WIDGET_TYPE_LISTBOX)
    Signature,   // signature field - surfaced, not editable
    PushButton,  // action button - surfaced, not editable
};

// Raw PDF field-flag bits we care about, mirroring MuPDF's PDF_FIELD_IS_* /
// PDF_TX_FIELD_IS_* (which mirror the PDF spec's stable bit positions). Kept here
// so FormTypes.h stays free of any MuPDF include - this is a pure value-type
// header, like MeasureTypes.h.
namespace form_flags {
constexpr unsigned ReadOnly = 1u;          // PDF_FIELD_IS_READ_ONLY
constexpr unsigned Required = 1u << 1;     // PDF_FIELD_IS_REQUIRED
constexpr unsigned Multiline = 1u << 12;   // PDF_TX_FIELD_IS_MULTILINE
constexpr unsigned Comb = 1u << 24;        // PDF_TX_FIELD_IS_COMB
} // namespace form_flags

// One AcroForm widget on a page. A plain value type (no fz_*/pdf_* leakage),
// mirroring MeasureViewport. `rect` is in app page-point space (top-left origin,
// y-down, 72 dpi, unrotated - the same space Measurement::pts live in), so it
// survives zoom/rotation. `value` is the current /V; `options` lists the display
// strings for choice fields (empty otherwise). `flags` is the raw PDF field-flag
// word (the form_flags bits above).
struct FormField {
    int page = -1;
    FormFieldType type = FormFieldType::Text;
    QRectF rect;         // app page-point space
    QString name;        // fully-qualified field name
    QString value;       // current /V
    QStringList options; // choice options (combo/list); empty otherwise
    unsigned flags = 0;  // raw PDF field flags (form_flags::*)

    // Effective text size for the inline editor, in PDF page points (text space,
    // 72 dpi, unrotated - the same space as `rect`). Already resolved from the
    // field's /DA in FormModel::pageFields: an explicit /DA size (including the
    // 12 pt MuPDF returns when a field has no /DA at all) is used verbatim; only
    // the true auto-size sentinel ("0 Tf") is resolved per field type (multiline
    // / list box -> 12; single-line / comb / combo -> a fraction of the inner
    // field height). The editor multiplies this by the view scale to get a
    // logical-pixel font size that matches the rendered (printed) glyph height.
    float fontSizePt = 12.0f;

    // The /DA base14 font tag mapped to a Qt family ("Helvetica", "Times New
    // Roman", "Courier New", "Symbol", "ZapfDingbats"; "Helvetica" when absent or
    // unknown). Cosmetic - affects glyph shape only, not the size match.
    QString fontFamily;

    bool readOnly() const { return flags & form_flags::ReadOnly; }
    bool required() const { return flags & form_flags::Required; }
    bool multiline() const { return flags & form_flags::Multiline; }
    bool comb() const { return flags & form_flags::Comb; }

    // A toggle (no inline text/choice editor; a click flips it directly).
    bool isToggle() const
    {
        return type == FormFieldType::CheckBox || type == FormFieldType::RadioButton;
    }

    // Whether the user can interact with this field at all: not read-only, and not
    // a signature or push button (which carry no fillable value).
    bool editable() const
    {
        if (readOnly())
            return false;
        return type != FormFieldType::Signature && type != FormFieldType::PushButton;
    }
};

} // namespace mervin
