# License texts

This directory contains the license and notice texts shipped with Mervin PDF distributions.

- `MuPDF-AGPL-3.0.txt` and the bundled-component files were extracted from the verified MuPDF 1.28.5 source archive used by the build.
- `Qt-*` comes from Qt Base 6.12.0.
- `qpdf-*` comes from qpdf 12.4.2.
- `tomlplusplus-LICENSE.txt` comes from the pinned toml++ 3.4.0 commit.
- `lucide-LICENSE.txt` comes from the lucide-static 1.49.0 npm package, the source of the vendored icons in `resources/icons/lucide`.

See [`../THIRD_PARTY_LICENSES.md`](../THIRD_PARTY_LICENSES.md) for the component inventory and redistribution notes.

Windows deployment additionally copies the target vcpkg packages' copyright
files into `licenses/vcpkg` in the package. Those notices cover the pinned static
dependencies, including libjpeg-turbo and libspng, and supplement this source
directory. They must remain in the installer even though the dependency DLLs are
no longer shipped.
