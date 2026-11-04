#!/usr/bin/env python3
"""Build an isolated Qt 6.12.0 SDK with the bundled security updates.

This opt-in builder does not modify the official SDK or select itself in CI.
It rebuilds the modules Mervin uses, including Linguist, from verified sources.
The official SDK supplies only ICU 73.2 on Linux and graphics DLLs on Windows.
Use Python 3.12 or newer and a matching x64 compiler environment.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request


VERSION = "6.12.0"
MODULES = {
    "qtbase": "a951bd163c7b80fc6b8c88d7668fb56abf91c152373e13c10666763238131307",
    "qtsvg": "e4ab39534ec97987b1b9b60ef7f4d3253d5912a69f8c713a01753174a4029331",
    "qttools": "8dab8f3611496486a470ad5f115ceea584f36bc22a2b8b6f6ebdbafbb8160693",
    "qttranslations": "85929c0c30d6f273f23bd879bb69803ea17b010ab73cab2f8abf95357f6f6bbb",
}
ICU_URL = "https://github.com/unicode-org/icu/releases/download/release-73-2/icu4c-73_2-src.tgz"
ICU_SHA256 = "818a80712ed3caacd9b652305e01afc7fa167e6f2e94996da44b90c2ab604ce1"
PATCH_DIR = Path(__file__).resolve().parent / "qt-patches"
CHECK_DIR = Path(__file__).resolve().parent / "qt-check"


def digest(path: Path) -> str:
    """Hash an input or produced file without loading it into memory."""
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def recipe() -> dict:
    """Describe immutable build inputs for cache invalidation and provenance."""
    required = {
        "0001-libpng-update-to-1.6.59.patch", "0002-pcre2-update-to-10.49.patch",
        "0003-qtbase-fix-empty-cbor-url.patch",
    }
    patches = {p.name: digest(p) for p in sorted(PATCH_DIR.glob("*.patch"))}
    if not required <= patches.keys():
        raise RuntimeError("The required Qt security patches are missing")
    return {
        "qt": VERSION,
        "modules": MODULES,
        "icu_headers": ICU_SHA256,
        "script": digest(Path(__file__)),
        "patches": patches,
        "verification": {name: digest(CHECK_DIR / name) for name in ("CMakeLists.txt", "main.cpp")},
    }


def fetch(url: str, path: Path, expected: str) -> None:
    """Download a pinned archive and reject corrupt cached or downloaded data."""
    if not path.exists():
        partial = path.with_suffix(path.suffix + ".partial")
        with urllib.request.urlopen(url, timeout=120) as response, partial.open("wb") as output:
            shutil.copyfileobj(response, output)
        if digest(partial) != expected:
            raise RuntimeError(f"SHA-256 mismatch for {url}")
        partial.replace(path)
    if digest(path) != expected:
        raise RuntimeError(f"SHA-256 mismatch for {path}")


def run(command: list[str], log: Path, *, cwd: Path | None = None) -> None:
    """Run a checked build step and retain its complete diagnostic output."""
    print(f"Running {command[0]}, log {log}", flush=True)
    with log.open("w", encoding="utf-8") as output:
        subprocess.run(command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT, check=True)


def prepare(destination: Path, official: Path) -> tuple[Path, dict]:
    """Prepare only a new destination, or resume this exact recipe and SDK."""
    if destination == official or official in destination.parents or destination in official.parents:
        raise RuntimeError("The destination and official SDK must be separate directories")
    if destination.is_symlink():
        raise RuntimeError("The destination must not be a symlink")
    version_file = official / "lib/cmake/Qt6Core/Qt6CoreConfigVersionImpl.cmake"
    if not version_file.is_file() or 'set(PACKAGE_VERSION "6.12.0")' not in version_file.read_text():
        raise RuntimeError("--official-sdk must point to the official Qt 6.12.0 x64 SDK")
    inputs = recipe() | {"platform": sys.platform, "official_sdk": str(official)}
    support = (
        [official / "bin" / name for name in ("dxcompiler.dll", "dxil.dll", "opengl32sw.dll")]
        if sys.platform == "win32"
        else [official / "lib" / f"libicu{name}.so.73.2" for name in ("data", "i18n", "uc")]
    )
    inputs["support_files"] = {p.name: digest(p) for p in support}
    marker = destination / "prepared.json"
    if destination.exists() and any(destination.iterdir()):
        if not marker.is_file() or json.loads(marker.read_text()) != inputs:
            raise RuntimeError("Choose a new empty destination for a different or incomplete recipe")
        return destination / "sdk", inputs
    destination.mkdir(parents=True, exist_ok=True)
    for name in ("downloads", "source", "build", "logs", "sdk"):
        (destination / name).mkdir()
    for module, sha256 in MODULES.items():
        name = f"{module}-everywhere-src-{VERSION}.tar.xz"
        archive = destination / "downloads" / name
        fetch(f"https://download.qt.io/official_releases/qt/6.12/{VERSION}/submodules/{name}", archive, sha256)
        with tarfile.open(archive) as source:
            source.extractall(destination / "source", filter="data")
    qtbase = destination / "source" / f"qtbase-everywhere-src-{VERSION}"
    # Check and apply the complete set atomically, also when inside a Git checkout.
    patches = b"".join(p.read_bytes() for p in sorted(PATCH_DIR.glob("*.patch")))
    env = os.environ | {"GIT_CEILING_DIRECTORIES": str(qtbase.parent)}
    subprocess.run(["git", "apply", "--check", "-"], input=patches, cwd=qtbase, env=env, check=True)
    subprocess.run(["git", "apply", "-"], input=patches, cwd=qtbase, env=env, check=True)
    sdk = destination / "sdk"
    if sys.platform == "win32":
        (sdk / "bin").mkdir()
        for path in support:
            shutil.copy2(path, sdk / "bin" / path.name)
    else:
        archive = destination / "downloads/icu4c-73_2-src.tgz"
        fetch(ICU_URL, archive, ICU_SHA256)
        with tarfile.open(archive) as source:
            source.extractall(destination / "source", filter="data")
        # Keep the official ICU ABI and runtime notices. Only its matching headers
        # are needed to compile Qt, and no distribution ICU is bundled accidentally.
        headers = destination / "icu-include/unicode"
        headers.mkdir(parents=True)
        for area in ("common", "i18n"):
            for path in (destination / "source/icu/source" / area / "unicode").glob("*.h"):
                shutil.copy2(path, headers / path.name)
        (sdk / "lib").mkdir()
        for path in support:
            shutil.copy2(path, sdk / "lib" / path.name)
            (sdk / "lib" / path.name.removesuffix(".2")).symlink_to(path.name)
    marker.write_text(json.dumps(inputs, indent=2) + "\n")
    return sdk, inputs


def build(destination: Path, sdk: Path, jobs: int, inputs: dict) -> None:
    """Build matching shared modules and record the resulting SDK inventory."""
    complete = sdk / "mervin-qt-build.json"
    complete.unlink(missing_ok=True)
    for module in MODULES:
        source = destination / "source" / f"{module}-everywhere-src-{VERSION}"
        binary = destination / "build" / module
        command = [
            "cmake", "-S", str(source), "-B", str(binary), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={sdk}",
            f"-DCMAKE_PREFIX_PATH={sdk}", "-DBUILD_SHARED_LIBS=ON",
            "-DQT_BUILD_TESTS=OFF", "-DQT_BUILD_EXAMPLES=OFF",
            "-DQT_INSTALL_CONFIG_INFO_FILES=ON",
        ]
        if module == "qtbase":
            command += [
                "-DQT_FEATURE_system_png=OFF", "-DQT_FEATURE_system_pcre2=OFF",
                "-DQT_FEATURE_system_jpeg=OFF", "-DQT_FEATURE_system_harfbuzz=OFF",
                "-DQT_FEATURE_system_doubleconversion=OFF",
            ]
            if sys.platform == "win32":
                command += ["-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL", "-DQT_FEATURE_system_zlib=OFF"]
            else:
                command += [
                    "-DQT_FEATURE_xcb=ON", "-DQT_FEATURE_wayland=ON", "-DQT_FEATURE_icu=ON",
                    "-DQT_FEATURE_libudev=OFF", "-DQT_FEATURE_liburing=OFF",
                    f"-DICU_INCLUDE_DIR={destination / 'icu-include'}",
                    f"-DICU_UC_LIBRARY_RELEASE={sdk / 'lib/libicuuc.so.73.2'}",
                    f"-DICU_I18N_LIBRARY_RELEASE={sdk / 'lib/libicui18n.so.73.2'}",
                    f"-DICU_DATA_LIBRARY_RELEASE={sdk / 'lib/libicudata.so.73.2'}",
                ]
        if module == "qttools":
            # Mervin has no QML sources. Build the C++ Linguist tools and Windows
            # deploy tool without importing unrebuilt Qt private-API modules.
            command += [
                "-DCMAKE_DISABLE_FIND_PACKAGE_Qt6Qml=ON", "-DFEATURE_assistant=OFF",
                "-DFEATURE_designer=OFF", "-DFEATURE_qdoc=OFF",
                "-DFEATURE_distancefieldgenerator=OFF", "-DFEATURE_linguist=ON",
            ]
        run(command, destination / "logs" / f"{module}-configure.log")
        run(["cmake", "--build", str(binary), "--parallel", str(jobs)],
            destination / "logs" / f"{module}-build.log")
        run(["cmake", "--install", str(binary)], destination / "logs" / f"{module}-install.log")
    check = destination / "build/verify"
    run(["cmake", "-S", str(CHECK_DIR), "-B", str(check), "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_PREFIX_PATH={sdk}"],
        destination / "logs/verify-configure.log")
    run(["cmake", "--build", str(check), "--parallel", str(jobs)], destination / "logs/verify-build.log")
    executable = check / ("verify-qt-dependencies.exe" if sys.platform == "win32" else "verify-qt-dependencies")
    env = os.environ | {
        "PATH": str(sdk / "bin") + os.pathsep + os.environ.get("PATH", ""),
        "QT_ENABLE_REGEXP_JIT": "1",
    }
    with (destination / "logs/verify-runtime.log").open("w") as output:
        subprocess.run([str(executable)], env=env, stdout=output, stderr=subprocess.STDOUT, check=True)
    inventory = {str(p.relative_to(sdk)): digest(p) for p in sorted(sdk.rglob("*")) if p.is_file()}
    complete.write_text(json.dumps({"inputs": inputs, "files": inventory}, indent=2) + "\n")
    print(sdk)


def main() -> None:
    """Parse the isolated builder options and print a usable SDK on success."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path)
    parser.add_argument("--official-sdk", type=Path)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--recipe", action="store_true", help="print the build cache identity without building")
    args = parser.parse_args()
    if args.recipe:
        print(hashlib.sha256(json.dumps(recipe(), sort_keys=True).encode()).hexdigest())
        return
    if not args.destination or not args.official_sdk or args.jobs < 1:
        parser.error("--destination, --official-sdk and a positive --jobs are required")
    if sys.platform not in ("linux", "win32"):
        parser.error("Only Linux and native Windows builds are supported")
    # Check before resolving so a symlink cannot conceal an occupied destination.
    if args.destination.is_symlink():
        parser.error("The destination must not be a symlink")
    destination = args.destination.resolve()
    sdk, inputs = prepare(destination, args.official_sdk.resolve())
    if not args.prepare_only:
        build(destination, sdk, args.jobs, inputs)


if __name__ == "__main__":
    main()
