# Lucide icons

Every icon in the app comes from [Lucide](https://lucide.dev). The SVGs here are
copied unmodified from the `lucide-static` npm package, version **1.49.0**, and
compiled into the `mervin_icons` library as a Qt resource (see `CMakeLists.txt`).
Licence: ISC, with the icons Lucide derives from Feather under MIT; the full text
ships as `licenses/lucide-LICENSE.txt`.

At run time `src/ui/Icons.cpp` replaces Lucide's `stroke="currentColor"` with the
theme's ink and its `stroke-width="2"` with the app's lighter 1.75 (heavier for a
few small stylesheet indicators), so these files never need editing.

## Adding or changing an icon

1. Copy the SVG from the same `lucide-static` version into this folder (keep the
   whole set on one version).
2. Add a `Glyph` to `src/ui/Icons.h` and `src/ui/IconList.h`, and map it to the
   file name in `lucideName()` in `src/ui/Icons.cpp`.
3. Re-run CMake so the resource picks up the new file, and check the result with
   `dump_icons` (a contact sheet of every glyph) and `tst_icons`.

Only the files the app uses are vendored. Remove a file here when its glyph goes.
