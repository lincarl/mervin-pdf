#pragma once

#include <QRectF>
#include <QString>

#include <optional>
#include <functional>
#include <memory>
#include <vector>

// Forward declaration of the MuPDF context (defined in <mupdf/fitz.h>). Keeps
// the heavy MuPDF headers confined to render/ translation units.
typedef struct fz_context fz_context;

namespace mervin {

class Document;
struct DocumentLifetime;

// One match of a search query: a contiguous run of UTF-16 code units within a
// single page's extracted text. Page text never spans pages, so matches don't
// either.
struct TextMatch
{
    int page = 0;
    int start = 0;  // offset into the page's text (UTF-16 code units)
    int length = 0;
};

// One URL-shaped text run found in the extracted page text. `url` is normalized
// for opening (for example, "www.example.com" becomes "https://www.example.com").
struct TextLink
{
    int page = 0;
    int start = 0;
    int length = 0;
    QString url;
};

// Lazily cache MuPDF page text, character/line geometry and links for selection and case/whole-
// word search. Coordinates are unscaled, unrotated page points at 72 dpi.
// Owns a cloned MuPDF context. Serialize calls; viewer and background search use separate
// indexes. Destroy before RenderEngine.
class TextIndex
{
public:
    TextIndex(fz_context *baseCtx, Document *doc);
    ~TextIndex();

    TextIndex(const TextIndex &) = delete;
    TextIndex &operator=(const TextIndex &) = delete;

    int pageCount() const { return static_cast<int>(pages_.size()); }

    // Full plain text of a page, lines joined by '\n'. Extracted lazily, cached.
    const QString &pageText(int pageNo);
    int pageTextLength(int pageNo);

    // Caret offset nearest a page-space point (for placing/extending selection).
    int offsetAt(int pageNo, QPointF pagePoint);

    // Word boundaries [start,end) around an offset (for double-click select).
    void wordBoundsAt(int pageNo, int offset, int *start, int *end);

    // Text of a [start, start+length) range on a page (for copy).
    QString textRange(int pageNo, int start, int length);

    // Highlight rectangles (page-point space) for a [start, start+length) range,
    // one merged rect per visual line the range covers.
    std::vector<QRectF> rangeRects(int pageNo, int start, int length);

    // URL-shaped text under a page-space point, if any.
    std::optional<TextLink> linkAt(int pageNo, QPointF pagePoint);

    // All matches of `query` across the whole document, ordered by (page, start).
    // An empty query yields no matches. Pages are extracted lazily and cached.
    std::vector<TextMatch> search(const QString &query, bool caseSensitive, bool wholeWord,
                                  const std::function<bool()> &canceled = {});

private:
    struct LineSpan
    {
        int start = 0; // [start, end) into PageText::text, excludes the '\n'
        int end = 0;
        QRectF bbox;   // page-point space
    };

    struct PageText
    {
        bool ready = false;
        QString text;                 // line-joined, includes '\n' separators
        std::vector<QRectF> rects;    // one per UTF-16 code unit in `text`
        std::vector<LineSpan> lines;  // visual lines, in reading order
        bool linksReady = false;
        std::vector<TextLink> links;
    };

    const PageText &ensure(int pageNo);
    static std::vector<TextLink> detectLinksInText(const QString &text, int page);
    static std::vector<TextMatch> matchInText(const QString &text, int page,
                                              const QString &query,
                                              bool caseSensitive, bool wholeWord);

    fz_context *ctx_ = nullptr; // cloned from the base context; owned
    std::shared_ptr<DocumentLifetime> lifetime_;
    Document *doc_ = nullptr;   // not owned
    std::vector<PageText> pages_;
};

} // namespace mervin
