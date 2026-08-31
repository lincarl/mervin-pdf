# FindMuPDF.cmake
#
# Locates the MuPDF static libraries built from source. MuPDF is built from the
# official source release (not vcpkg/apt) so the OCR + codec feature set matches
# across platforms. See scripts/build-mupdf-windows.ps1 and
# scripts/build-mupdf-linux.sh (also used by the CI jobs).
#
#   Windows: built via the VS solution -> ${MUPDF_DIR}/platform/win32/x64/Release/*.lib
#            (every produced .lib is over-linked; ordering is irrelevant there).
#   Unix:    built via the Makefile with USE_TESSERACT=yes ->
#            ${MUPDF_DIR}/build/release/libmupdf.a + libmupdf-third.a. These are
#            self-contained (bundled freetype/harfbuzz/jpeg/openjpeg/zlib/brotli/
#            jbig2dec/tesseract/leptonica); only system m/pthread/dl are needed.
#            GNU ld resolves archives in a single pass, so they are wrapped in
#            --start-group/--end-group to satisfy mutual references.
#
# Point MUPDF_DIR (cache var or environment variable) at the MuPDF source root.
# Provides the imported target  MuPDF::MuPDF.

if(NOT MUPDF_DIR)
    if(DEFINED ENV{MUPDF_DIR})
        set(MUPDF_DIR "$ENV{MUPDF_DIR}" CACHE PATH "MuPDF source root")
    elseif(WIN32)
        set(MUPDF_DIR "C:/dev/src/mupdf-1.28.0-source" CACHE PATH "MuPDF source root")
    else()
        set(MUPDF_DIR "$ENV{HOME}/src/mupdf-1.28.0-source" CACHE PATH "MuPDF source root")
    endif()
endif()

# If MUPDF_DIR moved (e.g. a version bump) the cached find results from a
# previous configure still point at the old tree; keeping them silently mixes
# old headers with new libs (fz_new_context then fails at runtime with
# "incompatible header and library versions"). Drop stale cache entries.
file(TO_CMAKE_PATH "${MUPDF_DIR}/include" _mupdf_expected_include)
if(MUPDF_INCLUDE_DIR AND NOT MUPDF_INCLUDE_DIR PATH_EQUAL "${_mupdf_expected_include}")
    unset(MUPDF_INCLUDE_DIR CACHE)
    unset(MUPDF_CORE_LIBRARY CACHE)
endif()

find_path(MUPDF_INCLUDE_DIR
    NAMES mupdf/fitz.h
    HINTS "${MUPDF_DIR}/include")

if(WIN32)
    set(_mupdf_libdir "${MUPDF_DIR}/platform/win32/x64/Release")
    find_library(MUPDF_CORE_LIBRARY NAMES libmupdf mupdf HINTS "${_mupdf_libdir}")
    # Link every static lib MuPDF produced (core + thirdparty codecs + harfbuzz +
    # resources + barcode/zxing + tesseract/leptonica). Over-linking is harmless
    # and avoids fragile per-lib ordering.
    file(GLOB MUPDF_ALL_LIBS "${_mupdf_libdir}/*.lib")
else()
    set(_mupdf_libdir "${MUPDF_DIR}/build/release")
    find_library(MUPDF_CORE_LIBRARY NAMES mupdf HINTS "${_mupdf_libdir}")
    file(GLOB MUPDF_ALL_LIBS CONFIGURE_DEPENDS "${_mupdf_libdir}/libmupdf*.a")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MuPDF
    REQUIRED_VARS MUPDF_INCLUDE_DIR MUPDF_CORE_LIBRARY MUPDF_ALL_LIBS)

if(MuPDF_FOUND AND NOT TARGET MuPDF::MuPDF)
    add_library(MuPDF::MuPDF INTERFACE IMPORTED)
    if(WIN32)
        set(_mupdf_link "${MUPDF_ALL_LIBS}")
    else()
        set(_mupdf_link
            -Wl,--start-group ${MUPDF_ALL_LIBS} -Wl,--end-group
            m pthread ${CMAKE_DL_LIBS})
    endif()
    set_target_properties(MuPDF::MuPDF PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${MUPDF_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${_mupdf_link}")
endif()
