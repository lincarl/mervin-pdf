#!/usr/bin/env python3
"""Exercise import parsing and the Windows package dependency boundary."""

import importlib.util
import os
from pathlib import Path
import secrets
import string
import struct
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "windows_payload", Path(__file__).with_name("check-windows-payload.py"))
payload = importlib.util.module_from_spec(spec)
spec.loader.exec_module(payload)


def pe_binary(imports=(), delayed=()):
    """Build a small PE with real import directory layouts and RVA mappings."""
    data = bytearray(4096)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", data, 0x84, 0x8664, 1)
    struct.pack_into("<H", data, 0x94, 240)
    optional = 0x98
    struct.pack_into("<H", data, optional, 0x20B)
    struct.pack_into("<Q", data, optional + 24, 0x140000000)
    struct.pack_into("<I", data, optional + 60, 512)
    struct.pack_into("<I", data, optional + 108, 16)
    section = optional + 240
    data[section:section + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", data, section + 8, 3584, 0x1000, 3584, 512)
    name_offset = 2048
    for index, names, offset, width in ((1, imports, 512, 20), (13, delayed, 1024, 32)):
        if not names:
            continue
        struct.pack_into("<II", data, optional + 112 + index * 8,
                         offset - 512 + 0x1000, (len(names) + 1) * width)
        for item, name in enumerate(names):
            name_rva = name_offset - 512 + 0x1000
            if index == 1:
                struct.pack_into("<IIIII", data, offset + item * width, 0, 0, 0, name_rva, 0)
            else:
                struct.pack_into("<IIIIIIII", data, offset + item * width, 1, name_rva, 0, 0, 0, 0, 0, 0)
            encoded = name.encode("ascii") + b"\0"
            data[name_offset:name_offset + len(encoded)] = encoded
            name_offset += len(encoded)
    return data


class WindowsPayloadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        suffix = "".join(secrets.choice(string.ascii_lowercase) for _ in range(4))
        temporary_root = Path("C:/dev-temp") if os.name == "nt" else Path.home() / "dev-temp"
        cls.session = temporary_root / f"mervin-payload-check-{suffix}"
        cls.session.mkdir(parents=True)

    def setUp(self):
        self.work = self.session / self._testMethodName
        self.directory = self.work / "payload"
        self.directory.mkdir(parents=True)
        self.system = self.work / "System32"
        self.system.mkdir()
        (self.system / "KERNEL32.dll").write_bytes(b"Windows supplies system DLLs")
        for name in payload.REQUIRED_FILES:
            self.write(name)

    def write(self, name, imports=(), delayed=()):
        path = self.directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(pe_binary(imports, delayed))
        return path

    def check(self):
        return payload.check_payload(self.directory, self.system)

    def test_valid_payload_checks_transitive_and_delayed_imports(self):
        self.write("MervinPDF.exe", ("Qt6Core.dll", "KERNEL32.dll"))
        self.write("Qt6Core.dll", ("MSVCP140.dll", "api-ms-win-core-synch-l1-2-0.dll"))
        plugin = self.write("platforms/qwindows.dll", ("Qt6Core.dll",), ("dxcompiler.dll",))
        self.assertEqual(payload.read_imports(plugin), {
            "imports": ["Qt6Core.dll"], "delay_imports": ["dxcompiler.dll"],
        })
        report = self.check()
        self.assertEqual(report["errors"], [])
        self.assertEqual(len(report["binaries"]), len(payload.REQUIRED_FILES) + 1)
        self.assertIn("kernel32.dll", report["system_imports"])

    def test_forbidden_dlls_fail_in_inventory_and_both_import_tables(self):
        for name in payload.FORBIDDEN_DLLS:
            with self.subTest(name=name):
                dll = self.write(name.upper())
                self.write("MervinPDF.exe", (name,), (name.upper(),))
                errors = self.check()["errors"]
                self.assertTrue(any("Forbidden dependency DLL in payload" in error for error in errors))
                self.assertTrue(any(" imports forbidden dependency" in error for error in errors))
                self.assertTrue(any(" delay_imports forbidden dependency" in error for error in errors))
                dll.unlink()

    def test_missing_imports_ignore_path_and_unrelated_plugin_directories(self):
        self.write("MervinPDF.exe", ("Qt6Core.dll",))
        self.write("Qt6Core.dll", ("transitive.dll",), ("delayed.dll",))
        self.write("other-plugin/transitive.dll")
        (self.work / "delayed.dll").write_bytes(pe_binary())
        with patch.dict(os.environ, {"PATH": str(self.work)}):
            errors = self.check()["errors"]
        self.assertIn("qt6core.dll imports missing dependency transitive.dll", errors)
        self.assertIn("qt6core.dll delay_imports missing dependency delayed.dll", errors)

    def test_system_vc_runtime_does_not_hide_missing_app_local_runtime(self):
        self.write("MervinPDF.exe", ("msvcp140_2.dll",))
        (self.system / "msvcp140_2.dll").write_bytes(pe_binary())
        self.assertIn("mervinpdf.exe imports missing dependency msvcp140_2.dll", self.check()["errors"])
        self.write("msvcp140_2.dll")
        self.assertEqual(self.check()["errors"], [])

    def test_explicitly_loaded_qt_runtime_files_are_required(self):
        for name in ("dxil.dll", "platforms/qwindows.dll", "tls/qschannelbackend.dll"):
            (self.directory / name).unlink()
            self.assertIn(f"Missing required runtime file {name}", self.check()["errors"])
            self.write(name)

    def test_invalid_binary_and_unmapped_import_fail_closed(self):
        path = self.directory / "MervinPDF.exe"
        for data in (b"not a PE", pe_binary(("Qt6Core.dll",))[:520]):
            with self.subTest(size=len(data)):
                path.write_bytes(data)
                self.assertTrue(any(error.startswith("mervinpdf.exe:") for error in self.check()["errors"]))
        data = pe_binary(("Qt6Core.dll",))
        struct.pack_into("<I", data, 512 + 12, 0xFFFF0000)
        path.write_bytes(data)
        self.assertTrue(any("unmapped PE address" in error for error in self.check()["errors"]))


if __name__ == "__main__":
    unittest.main()
