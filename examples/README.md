# Local PDF fixtures

Required tests use synthetic PDFs generated in the build directory by
`tests/generate_fixtures.py`, plus forms and encrypted files created in individual tests.

The optional `tst_ink_rects` photographic reference checks still use local
`images.pdf`, `images2.pdf`, `pcba.pdf`, and `house-drawing.pdf`. These personal or
third-party documents are not redistributable and must not be committed.
The target has the CTest label `optional-corpus`. Generated tests separately cover
scan inversion, clipped rendering, OCR, measurements, text, and mixed page sizes.
