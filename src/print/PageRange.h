#pragma once

#include <QList>
#include <QString>

// Parsing of the "Custom" page-range field in the print dialog (e.g. "1-3, 5,
// 8-10"). Kept GUI-free and in mervin_core so it can be unit-tested without
// pulling in QtWidgets - see tests/tst_print_range.cpp.
namespace PageRange {

// Parse a 1-based page-range spec against a document of `pageCount` pages.
//
// Grammar: comma-separated tokens, each either a single page ("5") or a range
// ("8-10"). Whitespace around tokens/commas/dashes is ignored, and empty tokens
// from stray commas ("1,,3") are skipped. Open-ended ranges are allowed: "3-"
// means 3..last and "-5" means 1..5.
//
// Ranges expand ascending; tokens are kept in the order written and duplicates
// are preserved (so "5-6,1" prints 5, 6, 1 and "1,1" prints page 1 twice) - the
// literal reading of what the user typed.
//
// On success returns the expanded list and clears *error. On any malformed or
// out-of-bounds input returns an empty list and sets *error to a user-facing
// message (a non-empty result therefore always means success).
QList<int> parse(const QString &spec, int pageCount, QString *error);

// As parse(), but additionally accepts "all" (any case, any surrounding
// whitespace) meaning every page in order. An empty spec is still an error.
//
// This is the grammar docs/spec.md has always documented for the page-operation
// prompts ("all", "1-9", "1,3,5-9"); parse() alone rejects "all", which is why
// the Document menu grew a second, laxer parser of its own. New callers should
// use this one.
QList<int> parseAllowingAll(const QString &spec, int pageCount, QString *error);

} // namespace PageRange
