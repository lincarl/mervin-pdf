#pragma once

#include <QColor>
#include <QString>
#include <QWidget>

class QButtonGroup;

namespace mervin {

// Selects the application's light/dark scheme using previews of a Mervin window.
// Follow system shows the window split between the two schemes. A saved value the
// picker does not offer selects Follow system, which is how the app treats it.
class UiThemePicker : public QWidget
{
    Q_OBJECT

public:
    explicit UiThemePicker(QWidget *parent = nullptr);

    void setScheme(const QString &scheme); // "dark" | "light" | "system"
    QString scheme() const;

    // The colour the previews mark the open tab and page with. An invalid colour
    // draws the accent the "system" setting gives each scheme.
    void setAccent(const QColor &accent);

private:
    QButtonGroup *choices_ = nullptr;
    QColor accent_;
};

} // namespace mervin
