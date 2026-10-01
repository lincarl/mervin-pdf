#pragma once

namespace mervin {

// Build-selected integration: Windows registers per-user capabilities and opens Default Apps;
// Linux uses xdg-mime and mervin-pdf.desktop. First-run/Settings controls are Windows-only;
// Linux remains available to other callers.
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
