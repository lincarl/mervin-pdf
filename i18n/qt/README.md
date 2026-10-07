# Standard widget translations

Qt does not ship catalogs for every supported UI language. The `qtbase_<id>.ts`
files supply those languages' standard controls, file and print dialogs, font and
color dialogs, and related accessible actions. They are separate from Mervin's
application catalogs, so running `update_translations` does not delete Qt messages.

`qt_ui_source.ts` contains 562 English messages in 42 contexts, extracted from
[Qt Translations 6.12.0, qtbase_de.ts](https://github.com/qt/qttranslations/blob/v6.12.0/translations/qtbase_de.ts).
Only the English source, source comments and disambiguation were used as the
translation reference. German translations were removed. The Kazakh supplement
reuses the matching finished translations from the same release's `qtbase_kk.ts`.
The Arabic, Slovak and Traditional Chinese supplements likewise reuse matching
translations from Qt 6.12's catalogs, with corrections reviewed against English.
Other supplements were translated from English.

The extracted Qt text and reused translations are covered by Qt's LGPL-3.0-only,
GPL-2.0-only or GPL-3.0-only licensing alternatives. See the bundled `licenses/Qt-*`
license texts and upstream's `licenseRule.json`. Original copyright belongs to
The Qt Company Ltd. and Qt contributors. Qt's long license explanation remains
English as required by its translator note, which identifies that text as the
authoritative version.

When upgrading Qt, review the corresponding dialog sources for changed messages.
Update this English reference and every supplemental catalog together. Preserve
context, source, `<comment>` and plural flags, including technical paper-size
identifiers. Keep translations based on the English source and notes.

Run `python3 scripts/check-translation-content.py i18n` and the CTest catalog and
layout checks after editing. `lconvert -i FILE -o FILE` normalizes a supplemental
catalog without scanning application sources. The build compiles every available
supplement and loads it after the installed Qt catalog, retaining upstream
translations outside the reference. Kazakh is present here because some supported
Linux distributions omit it. Arabic, Slovak and Traditional Chinese fill current
widget contexts missing from Qt's supplied catalogs. Traditional Chinese also
corrects upstream meanings for collated printing, facing pages, font weights and
writing systems. Its paper-size names retain standard identifiers.

These supplements do not cover all internal Qt diagnostic messages. Native
operating-system dialogs may use the system display language.
