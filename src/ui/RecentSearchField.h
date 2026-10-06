#pragma once

#include "ui/SearchLineEdit.h"

class QAction;
class QButtonGroup;
class QFrame;
class QToolButton;

namespace mervin {

// The Recent page's search field. Its right end holds a small clear button and
// the scope as three text toggles: Names (file names), Contents (inside the
// documents) and All (names first, then contents). The toggles and the clear
// button never take focus from a click, so the caret stays in the field.
class RecentSearchField : public SearchLineEdit
{
    Q_OBJECT

public:
    enum class Scope { Names, Contents, All };

    explicit RecentSearchField(QWidget *parent = nullptr);

    Scope scope() const { return scope_; }
    // Select a scope as if its toggle were clicked; emits scopeChanged() on a change.
    void setScope(Scope scope);

    // Content-search progress as a line along the inside bottom edge of the field:
    // `fraction` from 0 to 1 over a faint full-length track, or below 0 for none.
    void setProgress(qreal fraction);
    qreal progress() const { return progress_; }

    // The Settings value ("names", "contents", "all") for a scope, and back.
    // An unknown value reads as Names.
    static QString settingValue(Scope scope);
    static Scope scopeFromSetting(const QString &value);

signals:
    void scopeChanged(Scope scope);

protected:
    bool event(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void layoutRightEnd();
    void updateScopeLook();
    void updateSearchIcon();

    Scope scope_ = Scope::Names;
    qreal progress_ = -1;
    QAction *searchAction_ = nullptr; // leading magnifier
    QToolButton *clearBtn_ = nullptr;
    QFrame *rule_ = nullptr;
    QToolButton *scopeBtns_[3] = {};
    QButtonGroup *scopeGroup_ = nullptr;
};

} // namespace mervin
