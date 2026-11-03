#pragma once

class QApplication;

namespace mervin {

// Fits newly shown application windows to their screen's work area, including
// the native frame. Menus, tooltips and maximized/fullscreen windows keep Qt's
// placement. Call once after constructing QApplication, before showing windows.
void keepWindowsOnScreen(QApplication &application);

} // namespace mervin
