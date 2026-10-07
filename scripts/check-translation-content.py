"""Check translation structure against English, including supplemental Qt text."""

from collections import Counter
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


PLACEHOLDER = re.compile(r"%L?(?:[1-9][0-9]*|[npmv])")
MARKUP = re.compile(r"</?(?:a|b|br|div|em|h[1-6]|i|li|ol|p|span|strong|ul)\b[^>]*>", re.I)


def messages(path: Path) -> dict[tuple[str, str, str, bool], ET.Element]:
    """Use Qt's context, English source, disambiguation and plural flag as the key."""
    result = {}
    for context in ET.parse(path).getroot().findall("context"):
        for message in context.findall("message"):
            key = (context.findtext("name", ""), message.findtext("source", ""),
                   message.findtext("comment", ""), message.get("numerus") == "yes")
            if key in result:
                raise ValueError(f"{path.name} contains duplicate message {key}")
            result[key] = message
    return result


def check(path: Path, reference: Path) -> list[str]:
    """Reject missing messages, drafts and damage to translated runtime formatting."""
    expected = messages(reference)
    actual = messages(path)
    errors = []
    for key in expected.keys() - actual.keys():
        errors.append(f"Missing {key[0]} / {key[1]}")
    for key in actual.keys() - expected.keys():
        errors.append(f"Unexpected {key[0]} / {key[1]}")
    for key, message in actual.items():
        context, source, _, plural = key
        translation = message.find("translation")
        label = f"{context} / {source}"
        if translation is None or translation.get("type") in ("unfinished", "vanished", "obsolete"):
            errors.append(f"Unfinished {label}")
            continue
        forms = translation.findall("numerusform") if plural else [translation]
        if not forms:
            errors.append(f"Missing plural forms {label}")
        for form in forms:
            text = "".join(form.itertext())
            if not text.strip():
                errors.append(f"Empty {label}")
            note = expected.get(key, message).findtext("extracomment", "")
            if "Leave this text untranslated or include a verbatim copy" in note and source not in text:
                errors.append(f"Missing authoritative English text {label}")
            if Counter(PLACEHOLDER.findall(source)) != Counter(PLACEHOLDER.findall(text)):
                errors.append(f"Changed placeholders {label}")
            if source.count("\n") != text.count("\n"):
                errors.append(f"Changed line breaks {label}")
            if Counter(MARKUP.findall(source)) != Counter(MARKUP.findall(text)):
                errors.append(f"Changed HTML {label}")
    return errors


def main(directory: Path) -> int:
    failures = []
    catalogs = sorted(directory.glob("mervin_*.ts"))
    for path in catalogs:
        # English holds only plural messages; lupdate checks their source coverage.
        reference = path if path.name == "mervin_en.ts" else directory / "mervin_sv.ts"
        for error in check(path, reference):
            failures.append(f"{path.name}: {error}")
    supplements = sorted((directory / "qt").glob("qtbase_*.ts"))
    for path in supplements:
        for error in check(path, directory / "qt" / "qt_ui_source.ts"):
            failures.append(f"{path.name}: {error}")
    if failures:
        print("\n".join(failures))
        return 1
    print(f"Checked {len(catalogs)} application and {len(supplements)} Qt catalogs.")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: python scripts/check-translation-content.py I18N_DIRECTORY")
    raise SystemExit(main(Path(sys.argv[1])))
