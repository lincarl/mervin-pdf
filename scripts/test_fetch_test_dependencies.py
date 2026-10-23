"""Check verified test-dependency downloads without making network requests."""

from contextlib import ExitStack, redirect_stdout
import hashlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]


def load_script(name):
    """Load a fetch script without invoking its command-line entry point."""
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FetchTests(unittest.TestCase):
    """Exercise the cache and verification contracts of both dependency fetchers."""

    def setUp(self):
        """Keep all scratch files in the current checkout."""
        scratch = ROOT / "dev-temp"
        scratch.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch, prefix="fetch-tests-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def run_case(self, kind, initial, response, fails=False):
        """Fetch one pinned payload and return the observed requests and file bytes."""
        module = load_script(f"fetch-test-{kind}")
        payload = b"verified test dependency\x00\xff"
        digest = hashlib.sha256(payload).hexdigest()
        name = "eng.traineddata" if kind == "tessdata" else "fixture.otf"
        destination = self.directory / kind
        destination.mkdir()
        target = destination / name
        if initial is not None:
            target.write_bytes(initial)
        with ExitStack() as stack:
            if kind == "font":
                stack.enter_context(patch.object(module, "FILES", (("fixture.otf", name, digest),)))
                expected_url = (
                    "https://raw.githubusercontent.com/notofonts/noto-cjk/"
                    "523d033d6cb47f4a80c58a35753646f5c3608a78/fixture.otf"
                )
            else:
                stack.enter_context(patch.object(module, "SHA256", digest))
                expected_url = (
                    "https://raw.githubusercontent.com/tesseract-ocr/tessdata_best/"
                    "e12c65a915945e4c28e237a9b52bc4a8f39a0cec/eng.traineddata"
                )
            network = stack.enter_context(patch.object(module, "urlopen"))
            if isinstance(response, Exception):
                network.side_effect = response
            else:
                network.return_value = io.BytesIO(payload if response is None else response)
            output = stack.enter_context(redirect_stdout(io.StringIO()))
            if fails:
                with self.assertRaises((ValueError, OSError)):
                    module.fetch(destination)
            else:
                module.fetch(destination)
                self.assertEqual(output.getvalue().strip(), str(target))
            if initial == payload:
                network.assert_not_called()
            else:
                network.assert_called_once_with(expected_url, timeout=60)
        return target.read_bytes() if target.exists() else None

    def test_missing_and_corrupt_caches_download_exact_bytes(self):
        """Missing or corrupt cache entries must be replaced with verified data."""
        for kind in ("font", "tessdata"):
            for initial in (None, b"corrupt cache"):
                with self.subTest(kind=kind, initial=initial):
                    self.assertEqual(self.run_case(kind, initial, None), b"verified test dependency\x00\xff")
                    for path in (self.directory / kind).iterdir():
                        path.unlink()
                    (self.directory / kind).rmdir()

    def test_verified_cache_never_downloads(self):
        """A verified cached payload needs no network access."""
        for kind in ("font", "tessdata"):
            with self.subTest(kind=kind):
                payload = b"verified test dependency\x00\xff"
                self.assertEqual(self.run_case(kind, payload, OSError("must not connect")), payload)

    def test_bad_download_keeps_existing_bytes(self):
        """A failed hash check must not publish or overwrite a cache file."""
        for kind in ("font", "tessdata"):
            with self.subTest(kind=kind):
                self.assertEqual(self.run_case(kind, b"old", b"bad response", True), b"old")

    def test_transport_failure_does_not_publish(self):
        """An interrupted download must not leave a payload that looks complete."""
        for kind in ("font", "tessdata"):
            with self.subTest(kind=kind):
                self.assertIsNone(self.run_case(kind, None, OSError("interrupted"), True))


if __name__ == "__main__":
    unittest.main()
