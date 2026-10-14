#pragma once

#include <QPixmap>
#include <QWidget>

namespace mervin {

// Stands in for a busy tab's viewer with a picture of the pages as they were when the
// operation started, so nothing reads the document while the operation runs. It shows
// the picture at its natural size on the page canvas, and once the wait bar shows, a
// veil over the part below the bar. It takes no input.
class StillPicture : public QWidget
{
    Q_OBJECT

public:
    explicit StillPicture(QWidget *parent = nullptr);

    // A null picture shows the plain canvas.
    void setPicture(const QPixmap &picture);
    // Veils the area below `bar` and as wide as it. An empty rectangle veils nothing.
    void setVeil(const QRect &bar);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QPixmap picture_;
    QRect bar_;
};

} // namespace mervin
