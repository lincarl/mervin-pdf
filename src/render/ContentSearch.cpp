#include "render/ContentSearch.h"

#include "render/RenderEngine.h"

#include <mupdf/fitz.h>

#include <QByteArray>

#include <cstring>

namespace mervin {

namespace {

// Extract a page's plain text (lines separated by '\n'). Returns true on
// success; on a per-page MuPDF error returns false and leaves `out` as-is so
// the caller can keep scanning the rest of the file.
bool extractPageText(fz_context *ctx, fz_document *doc, int pageNo, QString &out)
{
    fz_page *page = nullptr;
    fz_stext_page *stext = nullptr;
    fz_var(page);
    fz_var(stext);
    bool ok = false;
    fz_try(ctx) {
        page = fz_load_page(ctx, doc, pageNo);
        fz_stext_options opts;
        std::memset(&opts, 0, sizeof(opts));
        stext = fz_new_stext_page_from_page(ctx, page, &opts);
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
        ok = true;
    }
    fz_always(ctx) {
        if (stext)
            fz_drop_stext_page(ctx, stext);
        if (page)
            fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        ok = false;
    }
    return ok;
}

// Build a short, single-line preview of the first match in `text`: a window of
// surrounding characters with newlines/runs of whitespace collapsed to single
// spaces, bracketed by ellipses when text was trimmed off either end. The match
// itself is left intact so the UI can highlight it. Returns empty if not found.
QString makeSnippet(const QString &text, const QString &query)
{
    const int at = text.indexOf(query, 0, Qt::CaseInsensitive);
    if (at < 0)
        return QString();

    constexpr int kBefore = 32;  // context characters before the match
    constexpr int kAfter  = 96;  // and after it
    const int start = qMax(0, at - kBefore);
    const int end   = qMin(text.size(), at + query.size() + kAfter);

    QString snippet = text.mid(start, end - start).simplified();
    if (start > 0)
        snippet.prepend(QStringLiteral("… "));
    if (end < text.size())
        snippet.append(QStringLiteral(" …"));
    return snippet;
}

} // namespace

ContentSearch::ContentSearch(RenderEngine *engine, QObject *parent)
    : QObject(parent)
    , engine_(engine)
{
}

ContentSearch::~ContentSearch()
{
    stopWorker();
}

void ContentSearch::stopWorker()
{
    cancel_.store(true);
    generation_.fetch_add(1); // invalidate any in-flight worker's emissions
    if (worker_.joinable())
        worker_.join();
    running_.store(false);
}

void ContentSearch::cancel()
{
    stopWorker();
}

void ContentSearch::start(const QStringList &paths, const QString &query)
{
    stopWorker(); // cancel + join any previous run

    if (query.trimmed().isEmpty() || paths.isEmpty()) {
        emit finished(false, 0);
        return;
    }

    cancel_.store(false);
    running_.store(true);
    const quint64 gen = generation_.load();
    worker_ = std::thread(&ContentSearch::run, this, paths, query, gen);
}

void ContentSearch::run(QStringList paths, QString query, quint64 generation)
{
    fz_context *ctx = engine_ ? fz_clone_context(engine_->baseContext()) : nullptr;
    int matched = 0;
    int scanned = 0;
    bool canceled = false;

    auto current = [&] { return generation_.load() == generation && !cancel_.load(); };

    if (ctx) {
        for (const QString &path : paths) {
            if (!current()) {
                canceled = true;
                break;
            }

            const QByteArray utf8 = path.toUtf8();
            fz_document *doc = nullptr;
            int matchPage = 0; // 1-based; 0 == no match
            QString snippet;
            fz_var(doc);
            fz_try(ctx) {
                doc = fz_open_document(ctx, utf8.constData());
                const int n = doc ? fz_count_pages(ctx, doc) : 0;
                for (int p = 0; p < n; ++p) {
                    if (!current())
                        break;
                    QString text;
                    if (extractPageText(ctx, doc, p, text)
                        && text.contains(query, Qt::CaseInsensitive)) {
                        matchPage = p + 1;
                        snippet = makeSnippet(text, query);
                        break; // first matching page is enough
                    }
                }
            }
            fz_always(ctx) {
                if (doc)
                    fz_drop_document(ctx, doc);
            }
            fz_catch(ctx) {
                matchPage = 0; // unreadable file -> simply no hit
            }

            ++scanned;
            if (!current()) {
                canceled = true;
                break;
            }
            if (matchPage > 0) {
                ++matched;
                emit hit(path, matchPage, snippet);
            }
            emit progress(scanned, static_cast<int>(paths.size()));
        }
        fz_drop_context(ctx);
    }

    // Only the current generation reports completion; a superseded worker exits
    // silently so it can't disturb the run that replaced it.
    if (generation_.load() == generation) {
        running_.store(false);
        emit finished(canceled || cancel_.load(), matched);
    }
}

} // namespace mervin
