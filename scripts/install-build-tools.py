"""Install pinned CMake and Ninja into the Python environment running this script.

Run with the Python selected by setup-python or an activated virtual environment.
Supports Linux x86_64 with glibc 2.17 or newer and Windows amd64. The environment
must already contain pip. Its scripts directory is added to GITHUB_PATH in CI.
"""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import sysconfig
import tempfile


CMAKE_VERSION = "4.4.4"
NINJA_VERSION = "1.13.2"
# The PyPI release includes Kitware's jobserver patch and reports this suffix.
NINJA_EXECUTABLE_VERSION = "1.13.2.git.kitware.jobserver-pipe-1"

# SHA-256 values from the release metadata at these version-specific PyPI URLs.
# Each pair covers the py3-none manylinux2014_x86_64/manylinux_2_17_x86_64 wheel
# followed by the py3-none win_amd64 wheel. Source distributions are not allowed.
# https://pypi.org/pypi/cmake/4.4.4/json
# https://pypi.org/pypi/ninja/1.13.2/json
REQUIREMENTS = f"""\
cmake=={CMAKE_VERSION} \\
    --hash=sha256:f773a0544c66370408f451acd487c2c643b0dfebb48c5d64f3638bc1bb820238 \\
    --hash=sha256:ad8e0a38b5707e27882701146bdfffecacd80e7703fa0fc36c77528518d545af
ninja=={NINJA_VERSION} \\
    --hash=sha256:65a24341b5ac09fcadcc37082660be40a94174e51a937fabf6e2cae26225fa2c \\
    --hash=sha256:1293f4078278b70d0ee4b6cc8f3a9e030656c9b2f59909970343c4fe76070118
"""


def install() -> None:
    """Install verified wheels and check the installed executables before CI use."""
    scratch = Path(__file__).resolve().parents[1] / "dev-temp"
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="install-build-tools-", dir=scratch) as temporary:
        requirements = Path(temporary) / "requirements.txt"
        requirements.write_text(REQUIREMENTS, encoding="utf-8")
        subprocess.run(
            [
                sys.executable,
                "-m", "pip",
                "--isolated",
                "--disable-pip-version-check",
                "install",
                "--index-url", "https://pypi.org/simple",
                "--only-binary=:all:",
                "--no-deps",
                "--require-hashes",
                # Reinstall so an already present version cannot bypass hash checks.
                "--force-reinstall",
                "--requirement", str(requirements),
            ],
            check=True,
            # --isolated alone still reads global and environment pip config files.
            env={**os.environ, "PIP_CONFIG_FILE": os.devnull},
        )

    scripts = Path(sysconfig.get_path("scripts"))
    suffix = ".exe" if os.name == "nt" else ""
    for tool, expected in (
        ("cmake", f"cmake version {CMAKE_VERSION}"),
        ("ctest", f"ctest version {CMAKE_VERSION}"),
        ("cpack", f"cpack version {CMAKE_VERSION}"),
        ("ninja", NINJA_EXECUTABLE_VERSION),
    ):
        output = subprocess.check_output(
            [str(scripts / f"{tool}{suffix}"), "--version"], text=True
        )
        first_line = output.splitlines()[0] if output else ""
        if first_line != expected:
            raise RuntimeError(f"Unexpected {tool} version {first_line!r}, expected {expected!r}")
        print(first_line, flush=True)

    if github_path := os.environ.get("GITHUB_PATH"):
        with Path(github_path).open("a", encoding="utf-8") as destination:
            destination.write(f"{scripts}\n")
    print(f"Build tools installed in {scripts}", flush=True)


if __name__ == "__main__":
    argparse.ArgumentParser(description=__doc__).parse_args()
    install()
