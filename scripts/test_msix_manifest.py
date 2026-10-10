#!/usr/bin/env python3
"""Check Store version ordering and the package's deployable manifest inputs."""

import importlib.util
from pathlib import Path
import struct
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "prepare_msix_manifest", ROOT / "scripts/prepare-msix-manifest.py"
)
msix = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(msix)
NS = {
    "m": "http://schemas.microsoft.com/appx/manifest/foundation/windows10",
    "uap": "http://schemas.microsoft.com/appx/manifest/uap/windows10",
    "uap10": "http://schemas.microsoft.com/appx/manifest/uap/windows10/10",
    "rescap": "http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities",
}


class StoreVersionTests(unittest.TestCase):
    def test_releases_upgrade_in_order_and_reserve_store_field(self):
        versions = (
            ("1.64.10-rc1", "1.64.1001.0"),
            ("1.64.10-rc2", "1.64.1002.0"),
            ("1.64.10-rc98", "1.64.1098.0"),
            ("1.64.10", "1.64.1099.0"),
            ("1.64.11-rc1", "1.64.1101.0"),
            ("255.255.654", "255.255.65499.0"),
        )
        previous = (0, 0, 0, 0)
        for public, expected in versions:
            with self.subTest(version=public):
                actual = msix.package_version(public)
                self.assertEqual(actual, expected)
                fields = tuple(map(int, actual.split(".")))
                self.assertGreater(fields, previous)
                self.assertEqual(fields[3], 0)
                self.assertTrue(all(field <= 65535 for field in fields))
                previous = fields

    def test_invalid_public_versions_fail(self):
        for public in (
            "0.0.0", "0.1.1", "v1.64.10", "1.64.10.0", "01.64.10",
            "1.64.10-rc0", "1.64.10-rc99", "1.64.10-rc01", "256.0.0",
            "1.256.0", "1.0.655", "1.0.0\n", '1.0.0" Publisher="other',
        ):
            with self.subTest(version=public), self.assertRaises(ValueError):
                msix.manifest(public)


class ManifestTests(unittest.TestCase):
    def test_store_identity_desktop_activation_and_pdf_registration(self):
        document = ET.fromstring(msix.manifest("1.64.10"))
        self.assertEqual(document.find("m:Identity", NS).attrib, {
            "Name": "Lincarl.MervinPDF",
            "Publisher": "CN=7444C5EF-8AD3-459D-A81A-9B90254807FC",
            "Version": "1.64.1099.0",
            "ProcessorArchitecture": "x64",
        })
        app = document.find("m:Applications/m:Application", NS)
        self.assertEqual(app.get("Executable"), "MervinPDF.exe")
        self.assertEqual(app.get(f"{{{NS['uap10']}}}RuntimeBehavior"), "packagedClassicApp")
        self.assertEqual(app.get(f"{{{NS['uap10']}}}TrustLevel"), "mediumIL")
        self.assertEqual(document.find("m:Capabilities/rescap:Capability", NS).get("Name"),
                         "runFullTrust")
        target = document.find("m:Dependencies/m:TargetDeviceFamily", NS)
        self.assertEqual(target.get("Name"), "Windows.Desktop")
        self.assertEqual(target.get("MinVersion"), "10.0.22000.0")
        association = app.find("m:Extensions/uap:Extension/uap:FileTypeAssociation", NS)
        self.assertEqual(association.find("uap:SupportedFileTypes/uap:FileType", NS).text,
                         ".pdf")
        self.assertIsNone(document.find("m:Dependencies/m:PackageDependency", NS))

    def test_manifest_assets_exist_at_required_dimensions(self):
        document = ET.fromstring(msix.manifest("1.0.0"))
        visual = document.find("m:Applications/m:Application/uap:VisualElements", NS)
        assets = (
            (document.find("m:Properties/m:Logo", NS).text, 50),
            (visual.get("Square44x44Logo"), 44),
            (visual.get("Square150x150Logo"), 150),
        )
        for relative, size in assets:
            with self.subTest(asset=relative):
                data = (ROOT / "packaging/windows/msix" / relative.replace("\\", "/")).read_bytes()
                self.assertEqual(data[:16], b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR")
                self.assertEqual(struct.unpack_from(">IIBB", data, 16), (size, size, 8, 6))


if __name__ == "__main__":
    unittest.main()
