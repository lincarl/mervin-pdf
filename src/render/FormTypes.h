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

// Widget value in zero-origin, y-down page points at 72 dpi. value is /V; options holds choice
// display strings; flags is the raw PDF field-flags word.
struct FormField {
    int page = -1;
    FormFieldType type = FormFieldType::Text;
    QRectF rect;         // app page-point space
    QString name;        // fully-qualified field name
    QString value;       // current /V
    QStringList options; // choice options (combo/list); empty otherwise
    unsigned flags = 0;  // raw PDF field flags (form_flags::*)

    // Resolved font size in page points. Preserve explicit /DA sizes (or the 12pt missing-/DA
    // default). Only 0 Tf means auto: multiline/list use 12pt; single-line/comb/combo derive
    // from inner height. The editor scales this to logical pixels.
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
