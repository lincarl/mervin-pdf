#include "render/ContentSearch.h"

#include "io/Background.h"
#include "io/JobContext.h"
#include "recent/PathKey.h"
#include "render/MuPdfLocks.h"

#include <mupdf/fitz.h>

#include <QByteArray>
#include <QFileInfo>
#include <QPointer>

#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

namespace mervin {

struct ContentSearch::Shared
{
    struct Request
    {
        QStringList paths;
        QString query;
        quint64 generation = 0;
    };
    std::mutex mutex;
    std::optional<Request> pending; // guarded by mutex: the newest query not started yet
    bool running = false;           // guarded by mutex: a task is running searches
    // Bumped on every start()/cancel(); a search whose generation is stale stops
    // without emitting (guards against a just-canceled run racing the next one).
    std::atomic<quint64> generation{0};
    // Set on the UI thread and only copied elsewhere: making a QPointer from the
    // object, or converting one, reads the object, which may be going.
    QPointer<QObject> owner;
};

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

ContentSearch::ContentSearch(QObject *parent)
    : QObject(parent)
    , shared_(std::make_shared<Shared>())
{
    shared_->owner = this;
}

ContentSearch::~ContentSearch()
{
    shared_->generation.fetch_add(1);
    std::lock_guard lock(shared_->mutex);
    shared_->pending.reset();
}

void ContentSearch::cancel()
{
    shared_->generation.fetch_add(1);
    {
        std::lock_guard lock(shared_->mutex);
        shared_->pending.reset();
    }
    if (std::exchange(running_, false))
        emit finished(true, 0);
}

void ContentSearch::start(const QStringList &paths, const QString &query)
{
    const quint64 generation = shared_->generation.fetch_add(1) + 1;
    const bool empty = query.trimmed().isEmpty() || paths.isEmpty();
    running_ = !empty;
    if (empty) {
        {
            std::lock_guard lock(shared_->mutex);
            shared_->pending.reset();
        }
        emit finished(false, 0);
        return;
    }
    bool startTask = false;
    {
        std::lock_guard lock(shared_->mutex);
        shared_->pending = Shared::Request{paths, query, generation};
        startTask = !std::exchange(shared_->running, true);
    }
    if (startTask)
        background::start([shared = shared_] { drain(shared); });
}

void ContentSearch::drain(const std::shared_ptr<Shared> &shared)
{
    // A context of its own, with a store of its own, so it does not depend on the
    // render engine. It shares the process's MuPDF locks, which guard MuPDF's
    // global state. Each file is dropped once scanned, so a small store is enough.
    fz_context *ctx = fz_new_context(nullptr, sharedMuPdfLocks(), 32 << 20);
    if (ctx) {
        fz_set_warning_callback(ctx, [](void *, const char *) {}, nullptr);
        fz_register_document_handlers(ctx);
    }
    for (;;) {
        Shared::Request request;
        {
            std::lock_guard lock(shared->mutex);
            if (!shared->pending) {
                shared->running = false;
                break;
            }
            request = std::move(*shared->pending);
            shared->pending.reset();
        }
        run(shared, request.paths, request.query, request.generation, ctx);
    }
    if (ctx)
        fz_drop_context(ctx);
}

// Searches the current generation while excluding paths owned by active output writers.
void ContentSearch::run(const std::shared_ptr<Shared> &shared, const QStringList &paths,
                        const QString &query, quint64 generation, fz_context *ctx)
{
    int matched = 0;
    int scanned = 0;
    bool canceled = false;

    auto current = [&] { return shared->generation.load() == generation; };
    const QPointer<QObject> owner = shared->owner; // set before any search starts
    // Runs `fn` on the owner's thread if the owner is still there and this run is
    // still the current one.
    const auto deliver = [owner, shared, generation](std::function<void(ContentSearch *)> fn) {
        background::post(owner, [owner, shared, generation, fn] {
            if (shared->generation.load() == generation)
                fn(static_cast<ContentSearch *>(owner.data()));
        });
    };

    if (ctx) {
        for (const QString &path : paths) {
            if (!current()) {
                canceled = true;
                break;
            }

            // Resolve links on this worker. The query skips known writers without
            // reserving reads against a writer that starts afterward.
            const QString key = fileKey(QFileInfo(path));
            if (!current()) {
                canceled = true;
                break;
            }
            int matchPage = 0; // 1-based; 0 == no match
            QString snippet;
            if (!job::destinationInUse(key)) {
                const QByteArray utf8 = path.toUtf8();
                fz_document *doc = nullptr;
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
            }

            // A skipped writer still counts as a checked input, like an unreadable file.
            ++scanned;
            if (!current()) {
                canceled = true;
                break;
            }
            if (matchPage > 0) {
                ++matched;
                deliver([path, matchPage, snippet](ContentSearch *search) {
                    emit search->hit(path, matchPage, snippet);
                });
            }
            deliver([scanned, total = int(paths.size())](ContentSearch *search) {
                emit search->progress(scanned, total);
            });
        }
    }

    deliver([canceled, matched](ContentSearch *search) {
        search->running_ = false;
        emit search->finished(canceled, matched);
    });
}

} // namespace mervin
