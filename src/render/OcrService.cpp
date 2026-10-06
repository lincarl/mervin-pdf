#include "render/OcrService.h"

#include "ocr/TessdataFile.h"
#include "render/Document.h"
#include "render/RenderEngine.h"

#include <mupdf/fitz.h>

#include <QByteArray>
#include <QCoreApplication>
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
    : ctx_(engine ? fz_clone_context(engine->baseContext()) : nullptr)
{
}

OcrService::~OcrService()
{
    if (ctx_)
        fz_drop_context(ctx_);
}

QString OcrService::recognize(Document *doc, int pageNo, const QRectF &pageRect,
                              const QStringList &languages, const QString &tessdataDir,
                              QString *error)
{
    return recognize(doc ? doc->lifetime() : nullptr, pageNo, pageRect, languages,
                     tessdataDir, error);
}

QString OcrService::recognize(const std::shared_ptr<DocumentLifetime> &lifetime, int pageNo,
                              const QRectF &pageRect, const QStringList &languages,
                              const QString &tessdataDir, QString *error,
                              const std::atomic<bool> *canceled)
{
    if (error)
        error->clear();
    if (!ctx_ || !lifetime || pageRect.isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("mervin::OcrService", "Invalid OCR request.");
        return {};
    }
    if (canceled && canceled->load())
        return {};
    fz_context *ctx = ctx_;

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

    fz_display_list *list = nullptr;
    fz_var(list);
    fz_rect bounds = fz_empty_rect;
    {
        std::lock_guard gate(lifetime->mutex);
        Document *doc = lifetime->document;
        if (!doc)
            return {};
        std::lock_guard documentLock(doc->accessMutex());
        fz_page *page = nullptr;
        fz_var(page);
        fz_try(ctx) {
            page = fz_load_page(ctx, doc->handle(), pageNo);
            bounds = fz_bound_page(ctx, page);
            list = fz_new_display_list_from_page(ctx, page);
        }
        fz_always(ctx)
            fz_drop_page(ctx, page);
        fz_catch(ctx) {
            if (error)
                *error = QString::fromUtf8(fz_caught_message(ctx));
            return {};
        }
    }
    fz_device *ocr = nullptr;
    fz_device *sdev = nullptr;
    fz_stext_page *stext = nullptr;
    fz_var(ocr);
    fz_var(sdev);
    fz_var(stext);

    QString result;
    fz_try(ctx) {
        stext = fz_new_stext_page(ctx, fz_transform_rect(mediabox, ctm));
        fz_stext_options opts;
        std::memset(&opts, 0, sizeof(opts));
        sdev = fz_new_stext_device(ctx, stext, &opts);

        // with_list=0 prevents original text outside the selection leaking into OCR.
        const auto progress = [](fz_context *, void *arg, int) -> int {
            auto *flag = static_cast<const std::atomic<bool> *>(arg);
            return flag && flag->load() ? 1 : 0;
        };
        ocr = fz_new_ocr_device(ctx, sdev, ctm, mediabox, 0, lang.c_str(),
                                datadir.c_str(), progress, const_cast<std::atomic<bool> *>(canceled));
        const fz_matrix pageCtm = fz_concat(fz_translate(-bounds.x0, -bounds.y0), ctm);
        fz_run_display_list(ctx, list, ocr, pageCtm, fz_infinite_rect, nullptr);
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
        fz_drop_display_list(ctx, list);
    }
    fz_catch(ctx) {
        if (error)
            *error = QString::fromUtf8(fz_caught_message(ctx));
        return {};
    }
    return result;
}

} // namespace mervin
