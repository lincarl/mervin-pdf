#pragma once

namespace mervin {

// A caret position within a document's extracted text: a page plus a character
// offset (UTF-16 code units, matching TextIndex). An invalid position has
// page < 0.
struct TextPos
{
    int page = -1;
    int offset = 0;

    bool valid() const { return page >= 0; }

    bool operator==(const TextPos &o) const { return page == o.page && offset == o.offset; }
    bool operator!=(const TextPos &o) const { return !(*this == o); }
    bool operator<(const TextPos &o) const
    {
        return page < o.page || (page == o.page && offset < o.offset);
    }
};

// Holds the current text selection as an anchor (where the drag began) and a
// caret (where it currently ends). The ordered [start, end) range is derived on
// demand. Positions are document-text positions, independent of zoom/rotation,
// so a selection survives view changes. The model carries no MuPDF state; the
// viewer turns the range into text/rects via TextIndex.
class SelectionModel
{
public:
    void begin(TextPos p)
    {
        anchor_ = p;
        caret_ = p;
    }
    void extendTo(TextPos p) { caret_ = p; }
    void set(TextPos a, TextPos b)
    {
        anchor_ = a;
        caret_ = b;
    }
    void clear()
    {
        anchor_ = TextPos{};
        caret_ = TextPos{};
    }

    bool hasSelection() const { return anchor_.valid() && caret_.valid() && anchor_ != caret_; }

    TextPos anchor() const { return anchor_; }
    TextPos caret() const { return caret_; }
    TextPos start() const { return anchor_ < caret_ ? anchor_ : caret_; }
    TextPos end() const { return anchor_ < caret_ ? caret_ : anchor_; }

private:
    TextPos anchor_;
    TextPos caret_;
};

} // namespace mervin
