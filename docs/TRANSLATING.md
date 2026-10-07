# Translating Mervin PDF

Mervin's UI text is written in English in the source code. Every other language
has a Qt Linguist catalog in `i18n/`, named `mervin_<id>.ts`, where `<id>` is Qt's
locale ID, such as `sv` for Swedish and `zh_CN` for Simplified Chinese.
Montenegrin uses the explicit app ID `cnr`, described below. The build
compiles the catalogs into the application, so there are no translation files to
install. [design.md](design.md#ui-languages) explains how Mervin picks and loads a
language.

The translations are drafted with AI or machine translation and reviewed against
the English source before they are marked finished. Structural checks do not
replace linguistic review. Native-speaker review remains useful for terminology
and natural phrasing.

The app ships 53 UI languages. European coverage includes national and EU official
languages, both Norwegian written standards, European Portuguese, Romansh, and
national languages of transcontinental countries. Regional and minority languages
are outside this expansion, except languages that are national elsewhere, such as
Catalan in Andorra. Japanese, Korean, Arabic, Hindi, Indonesian, Vietnamese, Thai,
Brazilian Portuguese, Simplified Chinese and Traditional Chinese are also included.
Traditional Chinese uses `zh_TW` and Taiwan terminology. Taiwan, Hong Kong, Macao
and explicit Traditional Han OS preferences select this catalog. Its first-run
OCR model is `chi_tra.traineddata`, following the existing language and script
mapping rules.

## English is the source of truth

Base every existing translation, revision and new language directly on the current
English source text and its translator notes. Do not translate from another
translation. If the English wording or intended behavior is unclear, clarify it
in the source before translating it. Preserve conditions such as "if left empty"
and "defaults to", and distinguish actions such as removing a setting from merely
ignoring it.

Prefer short, clear UI text and translations. Use concise labels and remove
unnecessary words. Preserve the meaning, conditions and established terminology;
do not force a translation to match the English character count. If a correct,
concise translation needs more room, fix the layout rather than omit information.

The authoritative English strings live in C++ translation calls such as
`tr("Reset")`. `lupdate` copies them into each catalog's `<source>` fields. Edit
the C++ source to change the English text, then regenerate the catalogs.
`mervin_en.ts` contains only English plural forms, not a complete English catalog.

## Explain purpose and intent in the source

Add a `//:` translator comment immediately before the translation call when adding
or changing ambiguous wording, technical terms, placeholders, destructive actions
or security text. Review the existing comment when revising a translation. A class
name identifies the component but does not explain what the user sees or does.

Explain where the text appears, what action or state it describes, and any
distinction that affects its meaning. For each placeholder, describe its value
and give a representative example when useful. For destructive actions, state
what is removed and what remains. For security text, explain the password or
permission involved and what an empty field means. Straightforward labels need
no comment that merely repeats their text.

```cpp
//: Placeholder in the owner-password field. If empty, use the open password
//: to protect the PDF permission settings. The user can enter a different one.
ownerEdit_->setPlaceholderText(tr("defaults to the open password"));
```

`lupdate` copies these comments into `<extracomment>`. Keep explanations specific
to a string in its source comment rather than editing the extracted comment in
the catalog. When a fix reveals a reusable translation lesson, add it to this
guide in the same change so later translators and agents can apply it.

## Terminology

Use the same term for the same concept throughout each language. These meanings
guide the choice of words, even when the English term has several translations.

| English term | Meaning in Mervin |
| --- | --- |
| Open password | Password needed to open an encrypted PDF. An empty value means no open password is required. |
| Owner password | Password guarding the PDF permission settings. An empty field uses the open password. It does not refer to an account owner. |
| Permissions | PDF flags for printing, copying, modifying and annotating. Mervin does not enforce these flags. |
| Scale | Drawing scale, such as 1:100, used to convert page distances to real distances. Distinguish it from the viewer's zoom level. |
| Calibrate | Set the drawing scale using a line of known real length. |
| Reset calibration | Remove the user's scale override and restore the scale embedded in the PDF. |
| OCR script | A writing system, such as Latin or Cyrillic, rather than a program or text detection. |

Short labels need their interface context. A search "match" is an occurrence of
the query, and "Match case" distinguishes uppercase and lowercase letters.
"Close to tray" describes keeping the application running in the notification
area. "Strip Owner Restrictions" removes PDF permission restrictions. Font names
such as "Light" and "Black" describe weight, while "Sat" in a color dialog means
saturation. Review these senses against the English notes.

In print dialogs, "Collate" groups pages into complete copies, and "facing pages"
means pairs of pages shown together. In font dialogs, preserve the distinction
between writing systems, such as Malayalam and Malay. Review reused Qt text
against these meanings as well.

Distinguish access-key ampersands from literal conjunctions. The plain menu
heading "Select & Annotate" means "Select and Annotate". A translation that spells
out the conjunction must not gain an access-key marker.

When a translation service batches strings, verify each source-to-target pairing.
Matching output counts or separator counts do not prove alignment. Redraft broken
entries individually. Preserve product names, command-line switches, file-filter
patterns and license identifiers. Use the local written standard deliberately,
including Bokmål versus Nynorsk and European versus Brazilian Portuguese.
Translate Chinese written standards independently from English. Use Traditional
Chinese terminology consistently, such as 檔案 for file, 儲存 for save and 列印
for print, rather than converting Simplified Chinese characters mechanically.

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

Fill every plural slot required by Qt even when the translated wording is
identical. Georgian and Kazakh require two slots and Macedonian three. Missing
slots can make the whole message fall back to English without an `lrelease`
warning. The UI-language test probes representative counts in the compiled files.

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

## Check text in the interface

Catalog checks establish that messages are extracted and finished. They do not
prove that a translation preserves the English meaning or fits where it appears.
Review translated wording against the English text and notes, then check its
actual interface context. Do not shorten a translation by dropping a condition or
changing an action just to make it fit.

Run the translation and layout checks after building the test targets. For a
local Linux run, set `mervin_build_dir` to your existing isolated build directory:

```bash
mervin_build_dir=/absolute/path/to/isolated/build
python3 scripts/fetch-test-font.py "$HOME/dev/mervin-layout-fonts"
MERVIN_TEST_FONT="$HOME/dev/mervin-layout-fonts/NotoSansCJKsc-Regular.otf" \
QT_QPA_PLATFORM=offscreen ctest --test-dir "$mervin_build_dir" \
  -R 'i18n_catalogs|i18n_content|tst_text_fit|tst_translation_layout' --output-on-failure
```

GitHub Actions supplies the same pinned Noto Sans fonts on Linux and Windows.
The fetch script verifies every font and its SIL OFL license by SHA-256. The set
covers extended Latin, Greek, Cyrillic, Armenian, Georgian, Arabic, Devanagari,
Thai and CJK scripts. It includes separate Simplified Chinese, Traditional Chinese,
Japanese and Korean fonts to preserve their Han glyph shapes.

`MERVIN_TEST_FONT` continues to point to `NotoSansCJKsc-Regular.otf`; the test loads
the complete set from the same directory. When the variable is unset, install all
families listed in the test. The suite checks glyph coverage for every application
and supplemental Qt catalog, including all plural forms. Missing glyphs must fail instead of occupying
less space and appearing to fit.

The fonts are test dependencies, not bundled application fonts. At runtime the
app uses installed system fonts. Linux installations need fonts covering the
selected language, such as Noto Sans and the corresponding Noto CJK families.

`tst_translation_layout` opens the ten custom dialogs in `src/dialogs/`, plus
the measurement and annotation panels, using real Qt widgets and the default
dark stylesheet. It discovers every compiled language through
`i18n::availableLanguages()` and repeats each scenario at the default and minimum
layout dimensions with 10-point and 15-point base fonts. CTest runs a separate
`tst_translation_layout_<id>` test for each language to keep failures and timeouts
independent. `MERVIN_TEST_LANGUAGE=<id>` selects one language when invoking the
executable directly; without it the executable checks them all. Text with a fixed pixel
font size in the stylesheet retains that size.

The scenarios include both calibration modes, all Settings pages, long filenames,
populated lists, document security states, print range validation and selected
input, file and network errors. The OCR language catalog uses a fake network
reply so the test exercises the real dialog without external requests.
`tst_text_fit` checks that the fitting helper accepts valid layouts and rejects
deliberate clipping. Both tests run through CTest in GitHub Actions without AI.

When adding a dialog or a state with different text, extend
`tests/tst_translation_layout.cpp` with a representative scenario. Add a test
slot and a matching `_data` slot that calls `addLanguageMatrix()`. Include long
placeholder values, error messages and the smallest supported size where they
affect layout. When fixing a clipping problem, add a focused regression case.
Editable values may scroll horizontally, and scrollable lists may extend beyond
their viewport. For other intentional text clipping, record a specific allowance
and its reason in the test. Do not exempt a whole dialog to hide one failure.

Wrapped labels need enough height at the dialog's minimum width, including after
their text changes. Check sibling overlap as well as each label's own text area.
A preview and its navigation can overlap even when both pass individual fitting
checks, so these cases need geometry assertions and screenshot inspection.

For widgets embedded in list items, derive the item's size from its polished
layout. Verify that the item contains its controls and that scrolling can reach
the full content. A child's own text can fit while the list still hides it or
overlaps it with the next row.

Layout checks cover only the states and widgets the tests exercise. Inspect
custom rendering, native platform dialogs, the separate open-file picker and
untested states manually, and record any remaining verification limits with the
change. These checks do not compare screenshots or establish fitting with every
theme, system font or display scale. Automated checks cannot judge translation
meaning.

## Add a language

1. Add the ID to `I18N_TRANSLATED_LANGUAGES` in `CMakeLists.txt` and a native and
   English name to `kNames` in `src/i18n/UiLanguage.cpp`. The English name must use
   `QT_TRANSLATE_NOOP("UiLanguage", ...)` so search works in the selected language.
2. Provide standard widget translations. Prefer Qt's `qtbase_<id>.qm`. If Qt does
   not provide one or it lacks current dialog contexts, translate `i18n/qt/qt_ui_source.ts` into
   `i18n/qt/qtbase_<id>.ts`. Keep its exact contexts, sources and disambiguation
   comments. Configure fails when neither catalog exists. Kazakh also has a
   supplement because supported Linux distributions omit Qt's Kazakh catalog.
   Arabic, Slovak and Traditional Chinese fill missing current contexts in Qt's
   supplied catalogs. Supplements load after upstream text, preserving its
   translations outside the reference.
3. Build `update_translations`, translate every application message directly from
   English and its comments, then run that target again to normalize the files.
4. Keep the required plural forms. Montenegrin is not a Qt locale, so its files
   are named with `cnr` but their TS language is `sr_Latn_ME` for Qt's compatible
   plural rules. This does not substitute a Serbian translation. The picker uses
   Montenegrin text and an explicit `cnr` language tag selects it. An OS that drops
   unrecognized locale tags may require manual selection. The catalog is Latin;
   an explicit `cnr-Cyrl` preference does not select it.
5. Check font coverage and CJK fallback. `preferLanguageFont()` chooses installed
   Simplified Chinese, Traditional Chinese, Japanese or Korean fonts and removes
   its previous fallback when the UI language changes. It also selects a primary font for these languages
   and Arabic and Thai so controls size themselves for
   the script's height. It preserves size and weight and restores the original
   font families when returning to other languages. Add pinned test fonts if a
   new script needs them.
6. Run the catalog, content, picker and layout checks. Inspect representative
   dialogs and smallest supported sizes. Record native-platform or linguistic
   verification that could not be performed.

`i18n_content` compares supplemental catalogs with their checked-in English
reference and rejects missing messages, drafts, empty translations, changed
placeholders, lost line breaks and altered HTML. It performs the same structural
checks on application catalogs. The existing `i18n_catalogs` check still uses
`lupdate` to verify application source extraction and canonical formatting.

Application and Qt catalogs are embedded separately. The loader installs Qt's
catalog first, then the application's, preserving app overrides and Swedish OK
capitalization. Supplements cover the standard controls and dialogs in their
reference, not Qt's complete internal error-message catalog. Some low-level Qt
errors can therefore remain English. OS-owned dialogs can follow the operating
system's language, including the native Windows file picker. See
`i18n/qt/README.md` for the reference's source and maintenance.
