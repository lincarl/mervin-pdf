#pragma once

#include <QRectF>
#include <QString>
#include <QStringList>

namespace mervin {

class RenderEngine;
class Document;

// Selection OCR backed by MuPDF's built-in Tesseract (fz_new_ocr_device), so
// no separate Tesseract dependency is needed and all MuPDF use stays in the
// render subsystem (this header exposes no fz_* types). Renders the selected
// page region to an internal bitmap at a fixed high DPI - independent of the
// on-screen zoom, which is the single biggest factor in OCR quality - and
// returns the recognized text.
//
// recognize() is synchronous and runs under the document's access lock, so the
// caller should show a wait cursor; OCR of a small selection is typically a
// second or two.
class OcrService
{
public:
    explicit OcrService(RenderEngine *engine);

    // OCR the page-point rectangle `pageRect` on page `pageNo`. `languages` are
    // Tesseract language codes (e.g. {"eng","swe"}); empty defaults to English.
    // `tessdataDir` holds the .traineddata files. Returns recognized text
    // (empty on failure, with *error set).
    QString recognize(Document *doc, int pageNo, const QRectF &pageRect,
                      const QStringList &languages, const QString &tessdataDir,
                      QString *error = nullptr);

    // The DPI the selection is re-rendered at before OCR.
    static constexpr int kOcrDpi = 300;

private:
    RenderEngine *engine_ = nullptr;
};

} // namespace mervin
