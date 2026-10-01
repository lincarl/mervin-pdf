#pragma once

#include <QRectF>
#include <QString>
#include <QStringList>
#include <atomic>
#include <memory>

typedef struct fz_context fz_context;

namespace mervin {

class RenderEngine;
class Document;
struct DocumentLifetime;

// Selection OCR backed by MuPDF's built-in Tesseract (fz_new_ocr_device), so
// no separate Tesseract dependency is needed and all MuPDF use stays in the
// render subsystem (this header exposes no fz_* types). Renders the selected
// page region to an internal bitmap at a fixed high DPI - independent of the
// on-screen zoom, which is the single biggest factor in OCR quality - and
// returns the recognized text.
//
// Capture the page under its document lock, then recognize on a private context.
// Calls may run on a worker; serialize calls on each service instance.
class OcrService
{
public:
    explicit OcrService(RenderEngine *engine);
    ~OcrService();
    OcrService(const OcrService &) = delete;
    OcrService &operator=(const OcrService &) = delete;

    // OCR the page-point rectangle `pageRect` on page `pageNo`. `languages` are
    // Tesseract language codes (e.g. {"eng","swe"}); empty defaults to English.
    // `tessdataDir` holds the .traineddata files. Returns recognized text
    // (empty on failure, with *error set).
    QString recognize(Document *doc, int pageNo, const QRectF &pageRect,
                      const QStringList &languages, const QString &tessdataDir,
                      QString *error = nullptr);

    QString recognize(const std::shared_ptr<DocumentLifetime> &document, int pageNo,
                      const QRectF &pageRect, const QStringList &languages,
                      const QString &tessdataDir, QString *error,
                      const std::atomic<bool> *canceled = nullptr);

    // The DPI the selection is re-rendered at before OCR.
    static constexpr int kOcrDpi = 300;

private:
    fz_context *ctx_ = nullptr; // private context; calls must be serial
};

} // namespace mervin
