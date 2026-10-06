#pragma once

#include <QLineEdit>

namespace mervin {

// The search field used by the find card and the Recent search bar. Pasting
// (Ctrl+V or the context menu) trims outer whitespace, so a copied word or line
// searches for its text rather than its trailing newline. Typed spaces are kept.
class SearchLineEdit final : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    void insertTrimmedClipboardText();
};

} // namespace mervin
