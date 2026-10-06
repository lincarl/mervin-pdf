#pragma once

#include <QList>
#include <QString>

// Parsing of the "Custom" page-range field in the print dialog (e.g. "1-3, 5,
// 8-10"). Kept GUI-free and in mervin_core so it can be unit-tested without
// pulling in QtWidgets - see tests/tst_print_range.cpp.
namespace PageRange {

// Parse one-based comma-separated pages/ranges; ignore whitespace and empty tokens. Open ranges
// 3- and -5 mean 3..last and 1..5. Expand ranges ascending while preserving token order and
// duplicates. Success clears *error; malformed/out-of-range input returns empty and sets a
// user-facing error.
QList<int> parse(const QString &spec, int pageCount, QString *error);

// As parse(), also accepting case-insensitive, trimmed "all". Empty input remains an error.
QList<int> parseAllowingAll(const QString &spec, int pageCount, QString *error);

} // namespace PageRange
