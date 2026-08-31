#pragma once

#include "render/Document.h"

#include <QWidget>

class QLabel;
class QScrollArea;
class QVBoxLayout;

namespace mervin {

// Read-only floating card for KiCad item/net properties embedded in PDF link
// JavaScript actions. Each row has a copy button for fast BOM/schematic lookup.
class PdfPropertiesPopup : public QWidget
{
    Q_OBJECT

public:
    explicit PdfPropertiesPopup(QWidget *parent = nullptr);

    void showFor(const PdfItemProperties &properties);
    void positionNear(const QRect &itemWidgetRect);

signals:
    void dismissed();

protected:
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override; // re-skin on light/dark switches

private:
    void clearRows();
    void addPropertyRow(const QString &text, int textColumnWidth);
    void copySelectedText() const;

    QLabel *headerLabel_ = nullptr;
    QScrollArea *scroll_ = nullptr;
    QWidget *rowsWidget_ = nullptr;
    QVBoxLayout *rowsLayout_ = nullptr;
    PdfItemProperties lastProps_; // rows rebuilt from this on a live theme switch
    bool appliedDark_ = false;    // last theme the stylesheet was built for; gates
                                  // changeEvent so re-applying it can't recurse
};

} // namespace mervin
