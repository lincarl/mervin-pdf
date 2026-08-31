#pragma once

#include "ui/Icons.h"

#include <array>

namespace mervin::icons {

// The complete Glyph roster with printable names. Used by the icon contact-sheet
// tool (tests/dump_icons.cpp) and by tst_icons, which walks it to prove every
// glyph in the enum actually paints something. Add a Glyph, add it here: the
// test's count check fails otherwise, which is the point.
struct GlyphEntry {
    Glyph id;
    const char *name;
};

inline constexpr std::array<GlyphEntry, 46> kGlyphs{{
    // Toolbar
    {Glyph::Open, "Open"},
    {Glyph::PrevPage, "PrevPage"},
    {Glyph::NextPage, "NextPage"},
    {Glyph::ChevronDown, "ChevronDown"},
    {Glyph::Search, "Search"},
    {Glyph::ZoomOut, "ZoomOut"},
    {Glyph::ZoomIn, "ZoomIn"},
    {Glyph::FitMode, "FitMode"},
    {Glyph::RotateLeft, "RotateLeft"},
    {Glyph::RotateRight, "RotateRight"},
    {Glyph::Print, "Print"},
    {Glyph::Copy, "Copy"},
    {Glyph::Save, "Save"},
    {Glyph::FillForm, "FillForm"},
    {Glyph::Ocr, "Ocr"},
    {Glyph::Measure, "Measure"},
    {Glyph::Document, "Document"},
    {Glyph::Menu, "Menu"},
    // Hamburger menu
    {Glyph::FitPage, "FitPage"},
    {Glyph::FitWidth, "FitWidth"},
    {Glyph::FullScreen, "FullScreen"},
    {Glyph::ContinuousScroll, "ContinuousScroll"},
    {Glyph::SinglePage, "SinglePage"},
    {Glyph::TwoPageSpread, "TwoPageSpread"},
    {Glyph::Outline, "Outline"},
    {Glyph::Thumbnails, "Thumbnails"},
    {Glyph::Comments, "Comments"},
    {Glyph::SelectAll, "SelectAll"},
    {Glyph::HighlightFields, "HighlightFields"},
    {Glyph::UiTheme, "UiTheme"},
    {Glyph::Sun, "Sun"},
    {Glyph::DocumentTheme, "DocumentTheme"},
    {Glyph::AlwaysOnTop, "AlwaysOnTop"},
    {Glyph::Settings, "Settings"},
    {Glyph::Keyboard, "Keyboard"},
    {Glyph::About, "About"},
    // Document popover
    {Glyph::ExtractPages, "ExtractPages"},
    {Glyph::SplitPages, "SplitPages"},
    {Glyph::MergePages, "MergePages"},
    {Glyph::Security, "Security"},
    {Glyph::Delete, "Delete"},
    // Context menus and panels
    {Glyph::OpenInNewWindow, "OpenInNewWindow"},
    {Glyph::ShowAllWindows, "ShowAllWindows"},
    {Glyph::Close, "Close"},
    {Glyph::Broom, "Broom"},
    {Glyph::DragHandle, "DragHandle"},
}};

inline constexpr const std::array<GlyphEntry, 46> &allGlyphs() { return kGlyphs; }

} // namespace mervin::icons
