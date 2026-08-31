#include "render/OcrService.h"

#include "ocr/TessdataFile.h"
#include "render/Document.h"
#include "render/RenderEngine.h"

#include <mupdf/fitz.h>

#include <QByteArray>
#include <QDir>

#include <cstring>
#include <mutex>
#include <string>

namespace mervin {

namespace {

// Concatenate an stext page into plain text (one '\n' per line).
QString stextToString(fz_stext_page *stext)
{
    QString out;
    for (fz_stext_block *block = stext->first_block; block; block = block->next) {
        if (block->type != FZ_STEXT_BLOCK_TEXT)
            continue;
        for (fz_stext_line *line = block->u.t.first_line; line; line = line->next) {
            for (fz_stext_char *ch = line->first_char; ch; ch = ch->next) {
                const int c = ch->c;
                if (c < 0x20 && c != 0x09)
                    continue;
                if (QChar::requiresSurrogates(c)) {
                    out.append(QChar(QChar::highSurrogate(c)));
                    out.append(QChar(QChar::lowSurrogate(c)));
                } else {
                    out.append(QChar(c));
                }
            }
            out.append(QLatin1Char('\n'));
        }
    }
    return out;
}

} // namespace

OcrService::OcrService(RenderEngine *engine)
    : engine_(engine)
{
}

QString OcrService::recognize(Document *doc, int pageNo, const QRectF &pageRect,
                              const QStringList &languages, const QString &tessdataDir,
                              QString *error)
{
    if (!engine_ || !doc || pageRect.isEmpty()) {
        if (error)
            *error = QStringLiteral("Invalid OCR request.");
        return {};
    }

    fz_context *ctx = engine_->baseContext();
    // Serialize against the worker pool / TextIndex (shared document state).
    std::lock_guard<std::mutex> docLk(doc->accessMutex());

    // MuPDF wants comma-separated languages; default to English.
    QStringList langs = languages;
    langs.removeAll(QString());

    // Damaged language data does not make Tesseract return an error - it makes it
    // abort() or write past the end of a vector, neither of which fz_catch below
    // can intercept, so the whole app would go down. Check the files first.
    if (!TessdataFile::validateLanguages(tessdataDir, langs, error))
        return {};
    const std::string lang = (langs.isEmpty() ? QStringLiteral("eng") : langs.join(QLatin1Char(',')))
                                 .toStdString();
    const std::string datadir = QDir::toNativeSeparators(tessdataDir).toStdString();

    const double s = static_cast<double>(kOcrDpi) / 72.0;
    const fz_matrix ctm = fz_scale(static_cast<float>(s), static_cast<float>(s));
    const fz_rect mediabox{static_cast<float>(pageRect.left()), static_cast<float>(pageRect.top()),
                           static_cast<float>(pageRect.right()),
                           static_cast<float>(pageRect.bottom())};

    fz_page *page = nullptr;
    fz_device *ocr = nullptr;
    fz_device *sdev = nullptr;
    fz_stext_page *stext = nullptr;
    fz_var(page);
    fz_var(ocr);
    fz_var(sdev);
    fz_var(stext);

    QString result;
    fz_try(ctx) {
        page = fz_load_page(ctx, doc->handle(), pageNo);

        stext = fz_new_stext_page(ctx, fz_transform_rect(mediabox, ctm));
        fz_stext_options opts;
        std::memset(&opts, 0, sizeof(opts));
        sdev = fz_new_stext_device(ctx, stext, &opts);

        // OCR device renders the mediabox (in points) at ctm resolution (300 DPI)
        // into an internal bitmap, OCRs it, and forwards text to the stext device.
        //
        // with_list MUST be 0 here. With a list (=1), MuPDF records every text
        // drawing call from the whole page into a display list and, on close,
        // replays it to the target clipped only by the mediabox *scissor* - and a
        // scissor merely culls whole nodes whose bbox lies entirely outside it. Any
        // original text object (a full line/paragraph show-text op) that merely
        // overlaps the selection is then replayed in full, so the result leaks text
        // from well beyond the marked area. With list=0 the only text reaching the
        // stext target is what Tesseract recognises in the internal bitmap, which is
        // pixel-clipped exactly to the selection - so OCR is scoped to the region.
        ocr = fz_new_ocr_device(ctx, sdev, ctm, mediabox, /*with_list=*/0, lang.c_str(),
                                datadir.c_str(), nullptr, nullptr);
        fz_run_page(ctx, page, ocr, ctm, nullptr);
        fz_close_device(ctx, ocr);
        fz_close_device(ctx, sdev);

        result = stextToString(stext);
    }
    fz_always(ctx) {
        if (ocr)
            fz_drop_device(ctx, ocr);
        if (sdev)
            fz_drop_device(ctx, sdev);
        if (stext)
            fz_drop_stext_page(ctx, stext);
        if (page)
            fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        if (error)
            *error = QString::fromUtf8(fz_caught_message(ctx));
        return {};
    }
    return result;
}

} // namespace mervin
