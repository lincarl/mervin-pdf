#include "ui/SearchLineEdit.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>

#include <memory>

namespace mervin {

void SearchLineEdit::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Paste)) {
        insertTrimmedClipboardText();
        return;
    }
    QLineEdit::keyPressEvent(event);
}

void SearchLineEdit::contextMenuEvent(QContextMenuEvent *event)
{
    std::unique_ptr<QMenu> menu(createStandardContextMenu());
    const QList<QKeySequence> pasteKeys = QKeySequence::keyBindings(QKeySequence::Paste);
    for (QAction *action : menu->actions()) {
        bool isPaste = false;
        for (const QKeySequence &shortcut : action->shortcuts()) {
            for (const QKeySequence &key : pasteKeys) {
                if (shortcut.matches(key) == QKeySequence::ExactMatch) {
                    isPaste = true;
                    break;
                }
            }
            if (isPaste)
                break;
        }
        if (isPaste) {
            QObject::disconnect(action, nullptr, nullptr, nullptr);
            connect(action, &QAction::triggered, this, [this] { insertTrimmedClipboardText(); });
            break;
        }
    }
    menu->exec(event->globalPos());
}

void SearchLineEdit::insertTrimmedClipboardText()
{
    if (!isReadOnly())
        insert(QApplication::clipboard()->text().trimmed());
}

} // namespace mervin
