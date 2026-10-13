#pragma once

#include <mupdf/fitz.h>

namespace mervin {

// The lock callbacks every MuPDF context in the process uses, the render engine's
// and the Recent content search's alike. MuPDF keeps some state global, such as the
// OpenJPEG and HarfBuzz allocators, and guards it only with these locks, so all
// contexts in use at a time must share them. They live as long as the process, so
// a context left behind by work that nothing waits for can still use them.
const fz_locks_context *sharedMuPdfLocks();

} // namespace mervin
