#pragma once

#include <QWidget>

#include <functional>

class QFontMetrics;
class QLabel;
class QPushButton;

namespace mervin {

// Puts a file name and a folder into a translated sentence, such as
// tr("Opening %1 from %2.").arg(name, folder). A sentence that names only the folder
// ignores the name.
using FileSentence = std::function<QString(const QString &name, const QString &folder)>;

// The sentence `compose` makes of the file `name` in `folder`, for a word-wrapped label
// `width` wide. It wraps where Qt allows, as at spaces, and the folder also after each
// separator that ends a folder name, which Qt does not allow after a backslash. No line
// ends inside or right after the separators a path starts with, such as the "\\" of a
// network share, right before another separator or after the ellipsis of a shortened
// part. A name or folder with a piece still too wide for a line is shortened in the
// middle, each only as far as its own pieces need. Both are bidirectional isolates, so
// a path keeps its order in a right-to-left sentence.
QString fitFileSentence(const FileSentence &compose, const QString &name, const QString &folder,
                        const QFontMetrics &metrics, int width);

// The bar at the top of a busy tab that says what its operation waits for, with Stop
// waiting on the right. It sits over the tab's still picture, from the left edge to the
// vertical scroll bar's column, which stays free. Its text wraps and is fitted again
// whenever its width changes, and the bar is as tall as the text needs.
class WaitBar : public QWidget
{
    Q_OBJECT

public:
    // `stopText` is the button's label.
    explicit WaitBar(const QString &stopText, QWidget *parent = nullptr);

    // Says `compose` of the file `name` in `folder` (see fitFileSentence).
    void setNotice(FileSentence compose, const QString &name, const QString &folder);
    // Places the bar at the top of its parent, from `left` and `width` wide, as tall as
    // its text needs.
    void place(int left, int width);
    QPushButton *stopButton() const { return stop_; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Fits the notice to the label's width, and the bar's height to the text.
    void fitText();

    QLabel *text_ = nullptr;
    QPushButton *stop_ = nullptr;
    FileSentence compose_;
    QString name_;
    QString folder_;
};

} // namespace mervin
