"""Check that package verification rejects missing or system application libraries."""

import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("linux_runtime", ROOT / "scripts/check-linux-runtime.py")
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)


class RuntimeTests(unittest.TestCase):
    def test_resolution_and_isolated_startup(self):
        """Only a complete private runtime may start, without SDK environment leaks."""
        scratch = ROOT / "dev-temp"
        scratch.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as temporary:
            prefix = Path(temporary) / "usr"
            libraries = prefix / "lib/mervin-pdf/lib"
            libraries.mkdir(parents=True)
            names = ["Qt6Core", "Qt6Gui", "Qt6Widgets", "Qt6Network", "Qt6PrintSupport",
                     "Qt6Svg", "qpdf", "tomlplusplus", "jpeg", "z"]
            lines = []
            for name in names:
                path = libraries / f"lib{name}.so"
                path.touch()
                lines.append(f"lib{name}.so => {path} (0x123)")
            system = Path(temporary) / "system-libz.so"
            system.touch()
            variants = {
                "private": "\n".join(lines),
                "missing": "\n".join(line for line in lines if not line.startswith("libqpdf.")),
                "unresolved": "\n".join(lines) + "\nlibicuuc.so.73 => not found\n",
                "system": "\n".join(lines).replace(str(libraries / "libz.so"), str(system)),
            }
            for case, output in variants.items():
                with self.subTest(case=case), contextlib.redirect_stdout(io.StringIO()), \
                     patch.object(runtime.subprocess, "check_output", return_value=output), \
                     patch.object(runtime.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "", "")) as launch, \
                     patch.dict(os.environ, {"LD_LIBRARY_PATH": "/sdk/lib", "QT_PLUGIN_PATH": "/sdk/plugins"}):
                    if case != "private":
                        with self.assertRaises(RuntimeError):
                            runtime.check(prefix)
                        launch.assert_not_called()
                        continue
                    runtime.check(prefix)
                    args, kwargs = launch.call_args
                    self.assertEqual(args[0][0], str(prefix / "bin/MervinPDF"))
                    self.assertIn("--quit-after-startup", args[0])
                    self.assertIn("--profile", args[0])
                    self.assertNotIn("LD_LIBRARY_PATH", kwargs["env"])
                    self.assertNotIn("QT_PLUGIN_PATH", kwargs["env"])
                    self.assertEqual(kwargs["env"]["QT_QPA_PLATFORM"], "offscreen")
                    self.assertTrue(kwargs["check"])


if __name__ == "__main__":
    unittest.main()
