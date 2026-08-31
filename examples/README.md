# Local PDF fixtures

PDF test fixtures are intentionally not committed because the original private repository used personal documents and third-party files without redistribution permission.

Place local fixtures in this directory when running the data-dependent tests. The expected filenames are defined in `tests/CMakeLists.txt`; tests without an available fixture skip only their data-dependent cases. `tst_viewer_fit` now generates its geometry fixtures at runtime.
