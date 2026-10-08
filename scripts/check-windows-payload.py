#!/usr/bin/env python3
"""Check an x64 Windows payload without resolving dependencies through PATH.

Read normal and delayed PE imports in every packaged executable and plugin.
Windows supplies API-set contracts and System32 DLLs. All VC runtime imports
must instead resolve inside the package, even on a developer machine.
"""

import argparse
import json
import os
from pathlib import Path
import re
import struct
import sys


FORBIDDEN_DLLS = frozenset((
    "tomlplusplus-3.dll", "qpdf30.dll", "jpeg62.dll", "turbojpeg.dll",
    "spng.dll", "z.dll",
))
# Qt loads these at runtime, so import tables alone cannot establish their
# presence. Keep Windows TLS, the platform plugin, and graphics fallbacks.
REQUIRED_FILES = (
    "MervinPDF.exe", "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll",
    "dxcompiler.dll", "dxil.dll", "opengl32sw.dll", "platforms/qwindows.dll",
    "tls/qschannelbackend.dll",
)
VC_RUNTIME = re.compile(r"(?:msvcp|vcruntime|concrt|vccorlib)\d[^/\\]*\.dll\Z", re.I)


def read_imports(path):
    """Return normal and delay-load DLL names from a bounds-checked x64 PE."""
    data = path.read_bytes()

    def unpack(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data):
            raise ValueError("truncated PE data")
        return struct.unpack_from(fmt, data, offset)

    if data[:2] != b"MZ":
        raise ValueError("missing DOS signature")
    pe, = unpack("<I", 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")
    machine, section_count = unpack("<HH", pe + 4)
    optional_size, = unpack("<H", pe + 20)
    optional = pe + 24
    magic, = unpack("<H", optional)
    if machine != 0x8664 or magic != 0x20B:
        raise ValueError("payload must contain only x64 PE32+ binaries")
    if optional_size < 112:
        raise ValueError("truncated PE optional header")
    header_size, = unpack("<I", optional + 60)
    image_base, = unpack("<Q", optional + 24)
    directory_count, = unpack("<I", optional + 108)
    if 112 + 8 * directory_count > optional_size:
        raise ValueError("data directories exceed the optional header")
    sections = []
    for index in range(section_count):
        section = optional + optional_size + index * 40
        virtual_size, address, size, offset = unpack("<IIII", section + 8)
        sections.append((address, size, offset))

    def at_rva(address, size):
        if address < header_size and address + size <= min(header_size, len(data)):
            return address
        for start, raw_size, offset in sections:
            if start <= address and address + size <= start + raw_size:
                result = offset + address - start
                if result + size <= len(data):
                    return result
        raise ValueError(f"unmapped PE address 0x{address:x}")

    def dll_name(address):
        name = bytearray()
        for index in range(260):
            byte = data[at_rva(address + index, 1)]
            if byte == 0:
                result = name.decode("ascii")
                if not result or any(char in result for char in "/\\:"):
                    raise ValueError("invalid import DLL name")
                return result
            name.append(byte)
        raise ValueError("unterminated import DLL name")

    result = {"imports": [], "delay_imports": []}
    for directory_index, kind, descriptor_size in (
        (1, "imports", 20), (13, "delay_imports", 32),
    ):
        if directory_index >= directory_count:
            continue
        address, size = unpack("<II", optional + 112 + directory_index * 8)
        if not address and not size:
            continue
        if not address or size < descriptor_size:
            raise ValueError(f"invalid {kind} directory")
        for index in range(size // descriptor_size):
            descriptor = unpack(
                "<" + "I" * (descriptor_size // 4),
                at_rva(address + index * descriptor_size, descriptor_size),
            )
            if not any(descriptor):
                break
            if kind == "imports":
                name_address = descriptor[3]
            else:
                attributes, name_address = descriptor[:2]
                if attributes & ~1:
                    raise ValueError("unsupported delayed import attributes")
                if not attributes & 1:
                    name_address -= image_base
            result[kind].append(dll_name(name_address))
        else:
            raise ValueError(f"unterminated {kind} directory")
    return result


def check_payload(directory, system_directory):
    """Inspect the whole tree, including DLLs loaded explicitly as Qt plugins."""
    directory = directory.resolve()
    files = {
        path.relative_to(directory).as_posix().lower(): path
        for path in directory.rglob("*") if path.is_file()
    }
    system_dlls = {
        path.name.lower() for path in system_directory.iterdir() if path.is_file()
    }
    errors = [f"Missing required runtime file {name}" for name in REQUIRED_FILES
              if name.lower() not in files]
    binaries = []
    system_imports = set()
    for relative, path in sorted(files.items()):
        if path.suffix.lower() not in (".dll", ".exe"):
            continue
        if path.name.lower() in FORBIDDEN_DLLS:
            errors.append(f"Forbidden dependency DLL in payload {relative}")
        try:
            imports = read_imports(path)
        except (ValueError, UnicodeError, OSError) as error:
            errors.append(f"{relative}: {error}")
            continue
        binaries.append({"file": path.relative_to(directory).as_posix(), **imports})
        for kind, names in imports.items():
            for name in names:
                normalized = name.lower()
                if normalized in FORBIDDEN_DLLS:
                    errors.append(f"{relative} {kind} forbidden dependency {name}")
                    continue
                # A DLL in an unrelated plugin folder cannot satisfy this import.
                sibling = (Path(relative).parent / normalized).as_posix()
                if normalized in files or sibling in files:
                    continue
                if not VC_RUNTIME.fullmatch(normalized) and (
                    normalized.startswith(("api-ms-win-", "ext-ms-win-"))
                    or normalized in system_dlls
                ):
                    system_imports.add(normalized)
                    continue
                errors.append(f"{relative} {kind} missing dependency {name}")
    return {
        "directory": str(directory),
        "system_directory": str(system_directory.resolve()),
        "dll_count": sum(path.suffix.lower() == ".dll" for path in files.values()),
        "binaries": binaries,
        "system_imports": sorted(system_imports),
        "errors": errors,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", required=True, type=Path)
    system_root = os.environ.get("SystemRoot")
    parser.add_argument("--system-directory", type=Path,
                        default=Path(system_root) / "System32" if system_root else None)
    parser.add_argument("--report", type=Path, help="Write import and DLL inventory JSON")
    args = parser.parse_args()
    if not args.directory.is_dir():
        parser.error("--directory must be an existing payload directory")
    if args.system_directory is None or not args.system_directory.is_dir():
        parser.error("Provide the target Windows System32 directory with --system-directory")
    report = check_payload(args.directory, args.system_directory)
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for binary in report["binaries"]:
        print(binary["file"])
    if report["errors"]:
        print("\n".join(report["errors"]), file=sys.stderr)
        return 1
    print(f"Windows payload verified. {report['dll_count']} DLLs, "
          "normal and delayed imports resolved without PATH.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
