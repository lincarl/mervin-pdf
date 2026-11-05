#!/usr/bin/env python3
"""Build or validate the patched SDK, then select it for CI and packaging.

Only the installed SDK is cached. Exact cache keys include the source recipe,
support files, this helper and the native toolchain. An invalid restore fails
the job without rebuilding over it or falling back to the official Qt binaries.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("build_qt_patched", ROOT / "scripts/build-qt-patched.py")
BUILDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILDER)
BUILD_MARKER = "mervin-qt-build.json"
CACHE_MARKER = "mervin-qt-cache.json"


def output(command: list[str]) -> str:
    """Read a required native tool's identity or fail before using a cache."""
    return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT).strip()


def toolchain() -> dict:
    """Scope caches to the OS, compiler, SDK and installed development packages."""
    context = {
        "platform": sys.platform, "machine": platform.machine().lower(),
        "cmake": output(["cmake", "--version"]), "ninja": output(["ninja", "--version"]),
    }
    if sys.platform == "win32":
        if os.environ.get("VSCMD_ARG_TGT_ARCH") != "x64":
            raise RuntimeError("Use the native MSVC x64 developer environment")
        compiler = shutil.which("cl")
        if not compiler:
            raise RuntimeError("MSVC cl.exe is missing")
        context["compiler"] = BUILDER.digest(Path(compiler))
        for name in ("VCToolsVersion", "WindowsSDKVersion", "VSCMD_ARG_HOST_ARCH", "ImageOS", "ImageVersion"):
            context[name] = os.environ.get(name, "")
        if not context["VCToolsVersion"] or not context["WindowsSDKVersion"]:
            raise RuntimeError("The MSVC and Windows SDK versions must be available")
        context["windows"] = platform.version()
    elif sys.platform == "linux":
        if context["machine"] != "x86_64":
            raise RuntimeError("Only Linux x64 is supported")
        context["os"] = Path("/etc/os-release").read_text()
        context["compiler"] = output([os.environ.get("CXX", "c++"), "--version"])
        context["target"] = output([os.environ.get("CXX", "c++"), "-dumpmachine"])
        # Package versions cover auto-detected headers and feature changes too.
        # Exclude unrelated applications so test and packaging lanes share SDKs.
        if shutil.which("dpkg-query"):
            packages = output(["dpkg-query", "-W", "-f=${Package}=${Version}\n"])
            context["development"] = sorted(line for line in packages.splitlines()
                                             if "-dev=" in line or line.startswith(("libc6=", "gcc-", "g++-")))
        else:
            packages = output(["rpm", "-qa", "--qf", "%{NAME}=%{VERSION}-%{RELEASE}.%{ARCH}\n"])
            context["development"] = sorted(line for line in packages.splitlines()
                                             if "-devel=" in line or line.startswith(("glibc=", "gcc=" , "gcc-c++=")))
    else:
        raise RuntimeError("Only Linux and native Windows builds are supported")
    return context


def identity(official: Path) -> dict:
    """Bind the recipe to the support binaries and native build environment."""
    version = official / "lib/cmake/Qt6Core/Qt6CoreConfigVersionImpl.cmake"
    if not version.is_file() or 'set(PACKAGE_VERSION "6.12.0")' not in version.read_text():
        raise RuntimeError("The support SDK must be official Qt 6.12.0")
    return {"schema": 1, "recipe": BUILDER.recipe(), "toolchain": toolchain(),
            "support_files": {path.name: BUILDER.digest(path) for path in BUILDER.support_files(official)},
            "selection_helper": BUILDER.digest(Path(__file__)),
            "action": BUILDER.digest(ROOT / ".github/actions/prepare-qt/action.yml"),
            "dependencies": BUILDER.digest(ROOT / "scripts/install-qt-build-deps.sh")}


def inventory(sdk: Path) -> dict[str, str]:
    """Hash every installed file and reject links outside the installed SDK."""
    files = {}
    if sdk.is_symlink():
        raise RuntimeError("The cached SDK must not be a symlink")
    for path in sorted(sdk.rglob("*")):
        relative = str(path.relative_to(sdk))
        if path.is_symlink():
            if not path.resolve(strict=True).is_relative_to(sdk) or path.is_dir():
                raise RuntimeError(f"Unsafe SDK symlink {relative}")
        if relative in (BUILD_MARKER, CACHE_MARKER):
            if path.is_symlink():
                raise RuntimeError("SDK provenance must not be a symlink")
            continue
        if path.is_file():
            files[relative] = BUILDER.digest(path)
        elif not path.is_dir():
            raise RuntimeError(f"Unsupported SDK entry {relative}")
    if not files:
        raise RuntimeError("The SDK inventory is empty")
    return files


def validate(sdk: Path, expected: dict, *, restored: bool) -> None:
    """Check provenance and the complete inventory before executing cached code."""
    actual_files = inventory(sdk)
    built = json.loads((sdk / BUILD_MARKER).read_text())
    inputs = dict(built["inputs"])
    # Build locations can differ between runner workspaces. Keep the original
    # location in provenance, but compare every immutable recipe input.
    if not inputs.pop("official_sdk", None):
        raise RuntimeError("The build is missing its support SDK provenance")
    required = expected["recipe"] | {"platform": sys.platform, "support_files": expected["support_files"]}
    if inputs != required:
        raise RuntimeError("The SDK build recipe or support files do not match")
    if actual_files != built["files"]:
        raise RuntimeError("The SDK inventory does not match its build manifest")
    support_directory = "bin" if sys.platform == "win32" else "lib"
    for name, sha256 in expected["support_files"].items():
        if actual_files.get(str(Path(support_directory) / name)) != sha256:
            raise RuntimeError(f"The SDK support file does not match {name}")
    if restored and json.loads((sdk / CACHE_MARKER).read_text()) != expected:
        raise RuntimeError("The cached SDK toolchain or selection recipe does not match")


def sdk_environment(sdk: Path, official: Path) -> dict[str, str]:
    """Select one SDK for tools, CMake, plugins and runtime loading."""
    environment = dict(os.environ)
    for name in ("Qt6_DIR", "Qt6Core_DIR", "Qt6Gui_DIR", "QTDIR", "QT_PLUGIN_PATH",
                 "QT_QPA_PLATFORM_PLUGIN_PATH", "LD_LIBRARY_PATH", "LD_PRELOAD"):
        environment.pop(name, None)
    paths = [value for value in environment.get("PATH", "").split(os.pathsep)
             if value and not Path(value).resolve().is_relative_to(official)]
    environment.update({"PATH": os.pathsep.join([str(sdk / "bin"), *paths]),
                        "QT_ROOT_DIR": str(sdk), "QT6_DIR": str(sdk), "QTDIR": str(sdk),
                        "CMAKE_PREFIX_PATH": str(sdk), "QT_PLUGIN_PATH": str(sdk / "plugins"),
                        "QT_ENABLE_REGEXP_JIT": "1"})
    if sys.platform == "linux":
        environment["LD_LIBRARY_PATH"] = str(sdk / "lib")
    return environment


def check_api(sdk: Path, official: Path, work: Path, jobs: int) -> None:
    """Compile and execute the current Qt API regressions for every SDK use."""
    work.mkdir(parents=True, exist_ok=True)
    environment = sdk_environment(sdk, official)
    commands = [
        ["cmake", "--fresh", "-S", str(ROOT / "scripts/qt-check"), "-B", str(work / "build"),
         "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_PREFIX_PATH={sdk}",
         f"-DQt6_DIR={sdk / 'lib/cmake/Qt6'}", f"-DEXPECTED_QT_PREFIX={sdk}"],
        ["cmake", "--build", str(work / "build"), "--parallel", str(jobs)],
        [str(work / "build" / ("verify-qt-dependencies.exe" if sys.platform == "win32"
                               else "verify-qt-dependencies"))],
    ]
    for name, command in zip(("configure", "build", "runtime"), commands):
        with (work / f"{name}.log").open("w") as log:
            result = subprocess.run(command, env=environment, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            print((work / f"{name}.log").read_text(), file=sys.stderr)
            raise RuntimeError(f"Qt API {name} failed with exit code {result.returncode}")
    print((work / "runtime.log").read_text(), end="")


def prepare(destination: Path, official: Path, expected: dict, jobs: int) -> Path:
    """Build on a cache miss and fail closed on an incomplete or invalid hit."""
    if (destination == official or official in destination.parents or destination in official.parents
            or destination.is_symlink()):
        raise RuntimeError("Use a separate, non-symlink destination")
    sdk = destination / "sdk"
    restored = sdk.exists()
    if not restored:
        environment = sdk_environment(sdk, official)
        subprocess.run([sys.executable, str(ROOT / "scripts/build-qt-patched.py"),
                        "--official-sdk", str(official), "--destination", str(destination),
                        "--jobs", str(jobs)], env=environment, check=True)
    validate(sdk, expected, restored=restored)
    check_api(sdk, official, destination / "validation", jobs)
    if not restored:
        (sdk / CACHE_MARKER).write_text(json.dumps(expected, indent=2) + "\n")
    return sdk


def select(sdk: Path, official: Path) -> None:
    """Export only the verified SDK to later workflow steps."""
    environment = sdk_environment(sdk, official)
    with Path(os.environ["GITHUB_ENV"]).open("a", encoding="utf-8") as stream:
        for name in ("QT_ROOT_DIR", "QT6_DIR", "QTDIR", "CMAKE_PREFIX_PATH", "QT_PLUGIN_PATH"):
            stream.write(f"{name}={environment[name]}\n")
    with Path(os.environ["GITHUB_PATH"]).open("a", encoding="utf-8") as stream:
        stream.write(str(sdk / "bin") + "\n")


def main() -> None:
    """Expose cache identity separately from preparation for Actions restore."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("key", "prepare"))
    parser.add_argument("--official-sdk", required=True, type=Path)
    parser.add_argument("--destination", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--export", action="store_true", help="select the SDK through GitHub environment files")
    args = parser.parse_args()
    if args.jobs < 1 or args.destination.is_symlink():
        parser.error("Use a positive job count and a non-symlink destination")
    official = args.official_sdk.resolve(strict=True)
    expected = identity(official)
    if args.command == "key":
        key = "mervin-qt-v1-" + hashlib.sha256(json.dumps(expected, sort_keys=True).encode()).hexdigest()
        print(key)
        if args.export:
            with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as stream:
                stream.write(f"key={key}\n")
        return
    sdk = prepare(args.destination.resolve(), official, expected, args.jobs)
    if args.export:
        select(sdk, official)
    print(sdk)


if __name__ == "__main__":
    main()
