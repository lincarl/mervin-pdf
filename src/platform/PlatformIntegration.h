#pragma once

namespace mervin {

// Cross-platform "default PDF app" integration. Each OS provides one
// implementation, selected at build time (see CMakeLists.txt):
//   - Windows (platform/win/WindowsIntegration.cpp): per-user (HKCU) registry
//     capabilities + opens Settings -> Default Apps (the supported Win10/11
//     pattern; apps may not silently take over an association).
//   - Linux (platform/linux/DesktopIntegration.cpp): xdg-mime against the
//     installed/bundled mervin-pdf.desktop (which carries MimeType=application/pdf).
// The current first-run prompt and Settings button are Windows-only. The Linux
// implementation remains available for non-UI integration.
namespace PlatformIntegration {

// Make Mervin a candidate .pdf handler and, on Windows, open the OS picker for
// the user to confirm; on Linux, set it as the user's default via xdg-mime.
// Returns false if the underlying registration call failed.
bool registerPdfHandlerAndPromptDefault();

// Whether Mervin is the user's CURRENT default .pdf handler. One cheap query;
// callers gate it so it runs at most once (first launch), off the hot path.
bool isDefaultPdfHandler();

} // namespace PlatformIntegration

} // namespace mervin
