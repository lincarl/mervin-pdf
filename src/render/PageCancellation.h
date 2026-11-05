#pragma once
#include <mupdf/fitz.h>

namespace mervin {
// Wrap a page device with cancellation checkpoints on its parsing thread.
// The caller owns the returned device and its target.
fz_device *cancellablePageDevice(fz_context *ctx, fz_device *target, fz_cookie *cookie);
}
