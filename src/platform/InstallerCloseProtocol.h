#pragma once

#include <cstdint>

namespace mervin::installerclose {

// Shared by the application and the MSI action, which has no Qt dependency.
// A registered Windows message reaches hidden tray windows as well as visible ones.
inline constexpr wchar_t messageName[] = L"MervinPDF.CloseForInstallation.v1";
enum class Command : std::uintptr_t { Probe = 0, Begin = 1, Poll = 2, Cancel = 3 };
// Zero is deliberately unsupported, matching old windows' DefWindowProc reply.
enum class Reply : std::intptr_t {
    Unsupported = 0, Ready = 0x4d5601, Pending, Declined, Closing, Busy
};

} // namespace mervin::installerclose
