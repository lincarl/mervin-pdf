#pragma once

namespace mervin {

// Windows uses manifest associations for packaged apps and per-user capabilities
// for MSI/portable copies. Linux uses xdg-mime and mervin-pdf.desktop.
namespace PlatformIntegration {

// Windows package identity takes precedence over any coexisting MSI installation.
// False on platforms without Windows package identity.
bool hasPackageIdentity();

// Make Mervin a candidate .pdf handler and, on Windows, open the OS picker for
// the user to confirm; on Linux, set it as the user's default via xdg-mime.
// Returns false if the underlying registration call failed.
bool registerPdfHandlerAndPromptDefault();

// Whether Mervin is the user's CURRENT default .pdf handler. One cheap query;
// callers gate it so it runs at most once (first launch), off the hot path.
bool isDefaultPdfHandler();

} // namespace PlatformIntegration

} // namespace mervin
