"""Focused acceptance and rejection cases for the catalog content check."""

import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "translation_content", Path(__file__).with_name("check-translation-content.py"))
content = importlib.util.module_from_spec(spec)
spec.loader.exec_module(content)


class TranslationContentTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
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


if __name__ == "__main__":
    unittest.main()
