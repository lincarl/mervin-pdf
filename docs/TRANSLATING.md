# Translating Mervin PDF

Mervin's UI text is written in English in the source code. Every other language
has a Qt Linguist catalog in `i18n/`, named `mervin_<id>.ts`, where `<id>` is Qt's
own catalog ID: `sv` for Swedish and `zh_CN` for Simplified Chinese. The build
compiles the catalogs into the application, so there are no translation files to
install. [design.md](design.md#ui-languages) explains how Mervin picks and loads a
language.

The translations are drafted with AI and reviewed before they are marked finished.

## Update the catalogs after a code change

The normal build never changes `i18n/*.ts`. After adding or changing UI text, build
the `update_translations` target:

```bash
cmake --build --preset linux-release --target update_translations
# Windows: cmake --build --preset x64-release --target update_translations
```

It runs `lupdate` over the application's sources and rewrites every catalog. New
strings arrive unfinished, strings the code no longer has are dropped, and no
source line numbers are written, which keeps the diffs small. Review the diff,
translate the new messages, and commit the catalogs together with the code change.
The build needs Qt's LinguistTools for this; see
[BUILDING.md](BUILDING.md#qt-modules-for-the-ui-translations).

## Translate messages

Edit `i18n/mervin_<id>.ts` in Qt Linguist (`linguist`, part of Qt's tools) or in a
text editor. A message looks like this:

```xml
<message>
    <source>Done</source>
    <extracomment>Title of the message after a new file was written.</extracomment>
    <translation type="unfinished"></translation>
</message>
```

Write the translation, then mark the message finished. In Qt Linguist, mark it
done. In a text editor, remove `type="unfinished"`. The build leaves unfinished
messages out, so they show in English rather than as drafts.

Before you commit, build `update_translations` once more. It rewrites the catalogs
in `lupdate`'s exact format and keeps the translations. The `i18n_catalogs` test
compares the files byte for byte, ignoring only line endings. Without that step a
catalog edited by hand or by a script can fail it, for example over a `"` or `'`
that `lupdate` writes as `&quot;` or `&apos;`, or over indentation.

- `<extracomment>` comes from a `//:` comment in the code and explains what the
  text is for. `<comment>` tells apart two messages with the same English text.
- Keep every placeholder. `%1`, `%2` and so on may move within the sentence. `%n`
  is the number in a plural message.
- Keep `&` access-key markers, line breaks and HTML tags. Keep closing punctuation
  such as `:` or `…` in the form the language uses.
- A plural message (`numerus="yes"`) has one `<numerusform>` per plural form of
  the language, two for Swedish and one for Chinese. Qt Linguist shows the right
  number of fields.

## English plural forms

The English source text writes plurals as `%n page(s)`. `mervin_en.ts` holds only
the plural messages, and gives the real English forms:

```xml
<message numerus="yes">
    <source>%n page(s)</source>
    <translation>
        <numerusform>%n page</numerusform>
        <numerusform>%n pages</numerusform>
    </translation>
</message>
```

Fill both forms and mark the message finished whenever `update_translations` adds
one. Until then English shows the `(s)` text, for example "1 page(s)".

## The i18n_catalogs test

The `i18n_catalogs` CTest test (`cmake/CheckTranslations.cmake`) runs the same
`lupdate` as `update_translations`, on copies in the build directory, and never
changes the committed catalogs. CI runs it on Linux and Windows. Run it alone with:

```bash
ctest --test-dir build/linux-release -R i18n_catalogs --output-on-failure
```

It fails when:

- `lupdate` warns about a source file. The usual warnings mean a string that can
  never be translated:
  - "Class 'X' lacks Q_OBJECT macro": `tr()` in a class without `Q_OBJECT`. At run
    time that `tr()` looks the text up in another context than the one `lupdate`
    filed it under. Add `Q_OBJECT` to a class declared in a header, or
    `Q_DECLARE_TR_FUNCTIONS(X)` to one in a `.cpp` file, or call
    `QCoreApplication::translate("X", "text")`.
  - "Cannot invoke tr() like this": `tr()` called through an object or pointer.
    Call the class's own `tr()` or `QCoreApplication::translate()`.
- A catalog differs from what `update_translations` would write, because strings
  were added or removed or because the file is not in `lupdate`'s format. Build
  that target, translate any new strings and commit the catalogs.
- A message in any catalog is still unfinished. The test lists the first ten per
  catalog.

`lupdate` reads only the files the current platform builds, so a file under
`src/platform/win` is read on Windows and not on Linux. Text to translate in such a
file would make the catalogs differ between platforms and fail the test on one of
them. Keep it in shared files; `lupdate` reads both branches of an `#ifdef`.

## Add a language

1. Check that Qt ships its own catalog for the language, `qtbase_<id>.qm` in Qt's
   `translations` folder, and use that ID. Qt 6.12 has `ar bg ca cs da de es fa
   fi fr gd he hr hu it ja ka kk ko lg lv nl nn pl pt_BR ru sk sv tr uk zh_CN
   zh_TW`; Ubuntu 26.04 and Fedora 44 lack `kk`. Configure stops when the catalog
   is missing, because Qt's buttons and dialogs would stay English.
2. Add the ID to `I18N_TRANSLATED_LANGUAGES` in the `qt_standard_project_setup`
   call in `CMakeLists.txt`. The next configure creates an empty
   `i18n/mervin_<id>.ts`.
3. Add a line to `kNames` in `src/i18n/UiLanguage.cpp` with the ID, the
   language's name in that language (UTF-8), and its English name inside
   `QT_TRANSLATE_NOOP("UiLanguage", ...)`. Without it the picker falls back to
   Qt's name for the language. The English name is a new message in every
   catalog.
4. Build `update_translations`, translate every message, and commit the new
   catalog with the other changes.
5. Check the fonts. Han characters are drawn differently in Simplified Chinese,
   Traditional Chinese and Japanese, and the OS may fall back to a font for the
   wrong one. While the UI is `zh_CN`, `preferChineseFont()` in `UiLanguage.cpp`
   adds an installed Simplified Chinese font as the fallback for Han text. A
   Traditional Chinese (`zh_TW`) or Japanese UI needs its own font list there, for
   example Microsoft JhengHei and Noto Sans CJK TC for Traditional Chinese.
