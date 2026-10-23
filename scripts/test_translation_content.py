"""Focused acceptance and rejection cases for the catalog content check."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "translation_content", Path(__file__).with_name("check-translation-content.py"))
content = importlib.util.module_from_spec(spec)
spec.loader.exec_module(content)


class TranslationContentTests(unittest.TestCase):
    def setUp(self):
        temporary_root = Path(__file__).resolve().parents[1] / "dev-temp"
        temporary_root.mkdir(exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(prefix="translation-content-", dir=temporary_root)
        self.addCleanup(self.directory.cleanup)
        self.reference = Path(self.directory.name) / "source.ts"
        self.translation = Path(self.directory.name) / "target.ts"
        self.reference.write_text(self.catalog("Saved %1\non %2"), encoding="utf-8")

    @staticmethod
    def catalog(text, state="", source="Saved %1\non %2"):
        return (f'<TS><context><name>Dialog</name><message><source>{source}</source>'
                f'<translation {state}>{text}</translation></message></context></TS>')

    def test_allows_reordered_placeholders(self):
        self.translation.write_text(self.catalog("Am %2\ngespeichert %1"), encoding="utf-8")
        self.assertEqual(content.check(self.translation, self.reference), [])

    def test_rejects_lost_placeholder_and_line_break(self):
        self.translation.write_text(self.catalog("Gespeichert %1"), encoding="utf-8")
        errors = content.check(self.translation, self.reference)
        self.assertTrue(any("placeholders" in error for error in errors))
        self.assertTrue(any("line breaks" in error for error in errors))

    def test_rejects_drafts_and_changed_source(self):
        self.translation.write_text(
            self.catalog("Gespeichert %1\nam %2", 'type="unfinished"'), encoding="utf-8")
        self.assertTrue(any("Unfinished" in error
                            for error in content.check(self.translation, self.reference)))
        self.translation.write_text(self.catalog("Andere", source="Other"), encoding="utf-8")
        self.assertTrue(any("Missing" in error
                            for error in content.check(self.translation, self.reference)))

    def test_rejects_changed_link_destination(self):
        original = '&lt;a href="https://example.org"&gt;Open&lt;/a&gt;'
        changed = '&lt;a href="https://elsewhere.org"&gt;Öffnen&lt;/a&gt;'
        self.reference.write_text(self.catalog(original, source=original), encoding="utf-8")
        self.translation.write_text(self.catalog(changed, source=original), encoding="utf-8")
        self.assertTrue(any("HTML" in error
                            for error in content.check(self.translation, self.reference)))

    def test_preserves_authoritative_text_when_note_requires_it(self):
        source = self.catalog("Legal", source="Legal").replace(
            "<translation", "<extracomment>Leave this text untranslated or include a verbatim copy"
            "</extracomment><translation")
        self.reference.write_text(source, encoding="utf-8")
        self.translation.write_text(self.catalog("Právne", source="Legal"), encoding="utf-8")
        self.assertTrue(any("authoritative" in error
                            for error in content.check(self.translation, self.reference)))
        self.translation.write_text(self.catalog("Legal", source="Legal"), encoding="utf-8")
        self.assertEqual(content.check(self.translation, self.reference), [])

        self.translation.write_text(self.catalog("Právne (Legal)", source="Legal"), encoding="utf-8")
        self.assertEqual(content.check(self.translation, self.reference), [])

    def test_placeholder_multiplicity_and_localization_are_preserved(self):
        source = "%1 %1 %L2 %n"
        self.reference.write_text(self.catalog(source, source=source), encoding="utf-8")
        for text, valid in (("%n %L2 %1 %1", True), ("%1 %L2 %n", False),
                            ("%1 %1 %2 %n", False), ("%1 %1 %L2 %n %n", False)):
            with self.subTest(text=text):
                self.translation.write_text(self.catalog(text, source=source), encoding="utf-8")
                errors = content.check(self.translation, self.reference)
                self.assertEqual(not errors, valid, errors)

    def test_missing_empty_and_inactive_translations_are_rejected(self):
        for replacement, diagnostic in (
            ("", "Unfinished"), ("<translation/>", "Empty"),
            ("<translation>  </translation>", "Empty"),
            *[(f'<translation type="{state}">Saved %1\non %2</translation>', "Unfinished")
              for state in ("unfinished", "obsolete", "vanished")],
        ):
            with self.subTest(replacement=replacement):
                catalog = self.catalog("Saved %1\non %2")
                start, end = catalog.index("<translation"), catalog.index("</translation>") + len("</translation>")
                self.translation.write_text(catalog[:start] + replacement + catalog[end:], encoding="utf-8")
                self.assertTrue(any(diagnostic in error
                                    for error in content.check(self.translation, self.reference)))

    def test_plural_forms_are_each_validated(self):
        def plural(forms):
            """Build a two-form source without relying on the production parser."""
            return ('<TS><context><name>Dialog</name><message numerus="yes">'
                    '<source>%n files for %1</source><translation>' + forms
                    + '</translation></message></context></TS>')

        good = '<numerusform>%n fil för %1</numerusform><numerusform>%n filer för %1</numerusform>'
        self.reference.write_text(plural(good), encoding="utf-8")
        for forms, diagnostic in ((good, ""), ("", "Missing plural forms"),
                                  (good.replace("%n filer för %1", "%n filer"), "placeholders"),
                                  (good.replace("%n filer för %1", ""), "Empty")):
            with self.subTest(forms=forms):
                self.translation.write_text(plural(forms), encoding="utf-8")
                errors = content.check(self.translation, self.reference)
                if diagnostic:
                    self.assertTrue(any(diagnostic in error for error in errors), errors)
                else:
                    self.assertEqual(errors, [])

    def test_duplicate_and_distinct_message_keys(self):
        catalog = self.catalog("Saved %1\non %2")
        message = catalog[catalog.index("<message>"):catalog.index("</message>") + len("</message>")]
        self.translation.write_text(catalog.replace("</context>", message + "</context>"), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate message"):
            content.check(self.translation, self.reference)
        for replacement in (
            catalog.replace("<name>Dialog</name>", "<name>Other</name>"),
            catalog.replace("<source>", "<comment>Other meaning</comment><source>"),
            catalog.replace("<message>", '<message numerus="yes">'),
        ):
            with self.subTest(replacement=replacement):
                self.translation.write_text(replacement, encoding="utf-8")
                errors = content.check(self.translation, self.reference)
                self.assertTrue(any("Missing " in error for error in errors), errors)
                self.assertTrue(any("Unexpected " in error for error in errors), errors)

    def test_cli_rejects_damaged_catalog(self):
        directory = Path(self.directory.name)
        (directory / "mervin_sv.ts").write_text(self.catalog("Saved %1\non %2"), encoding="utf-8")
        target = directory / "mervin_de.ts"
        for text, valid in (("Am %2\ngespeichert %1", True), ("Gespeichert %1", False)):
            with self.subTest(text=text):
                target.write_text(self.catalog(text), encoding="utf-8")
                result = subprocess.run(
                    [sys.executable, "-B", str(Path(content.__file__)), str(directory)],
                    capture_output=True, text=True, check=False,
                )
                self.assertEqual(result.returncode == 0, valid, result.stdout + result.stderr)
                if not valid:
                    self.assertIn("Changed placeholders", result.stdout)


if __name__ == "__main__":
    unittest.main()
