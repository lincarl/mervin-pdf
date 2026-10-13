#pragma once

#include <QSize>

class QWidget;

namespace mervin {

// Keeps `window`'s minimum size at `floor`, or larger wherever its layout needs
// more room, for as long as the window exists. The height also covers wrapped
// text at the minimum width. Call once, after building the window's layout.
//
// Qt stops raising a window's minimum once code sets one, so a minimum measured
// in the constructor goes stale as the layout grows after the window opens: a
// list gains its scroll bar, a longer message arrives, a translated choice is
// added. The user could then shrink the window until text clips.
void followLayoutMinimum(QWidget &window, QSize floor);

} // namespace mervin
