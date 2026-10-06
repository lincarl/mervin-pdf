#pragma once

#include <QString>
#include <QWidget>

class QButtonGroup;
class QRadioButton;

namespace mervin {

// Selects the document colour treatment using previews of the same sample page.
// An existing custom/follow-ui value remains available until the user changes it.
class DocumentThemePicker : public QWidget
{
    Q_OBJECT

public:
    explicit DocumentThemePicker(QWidget *parent = nullptr);

    void setTheme(const QString &theme);
    QString theme() const;

private:
    QButtonGroup *choices_ = nullptr;
    QRadioButton *followUi_ = nullptr;
    QString followUiTheme_;
};

} // namespace mervin
