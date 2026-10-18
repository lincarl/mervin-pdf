# FindMuPDF.cmake
#
# Locates the MuPDF static libraries built from source. MuPDF is built from the
# official source release (not vcpkg/apt) so the OCR + codec feature set matches
# across platforms. See scripts/build-mupdf-windows.ps1 and
# scripts/build-mupdf-linux.sh (also used by the CI jobs), which apply the
# patches in scripts/mupdf-patches. This module refuses libraries built without
# the current patch set.
#
#   Windows: built via the VS solution -> ${MUPDF_DIR}/platform/win32/x64/Release/*.lib
#            (every produced .lib is over-linked; ordering is irrelevant there).
#   Unix:    built via the Makefile with USE_TESSERACT=yes ->
#            ${MUPDF_DIR}/build/release/libmupdf.a + libmupdf-third.a. These are
#            self-contained (bundled freetype/harfbuzz/jpeg/openjpeg/zlib/brotli/
#            jbig2dec/tesseract/leptonica); only system m/pthread/dl are needed.
#            GNU ld resolves archives in a single pass, so they are wrapped in
#            --start-group/--end-group to satisfy mutual references.
#            The system libqpdf, Qt and Qt's JPEG plugin load the system jpeg,
#            freetype, zlib and brotli. Without --exclude-libs, GNU ld exports
#            the archives' definitions of the names those libraries use, and the
#            dynamic linker binds the libraries to MuPDF's copies. Qt's JPEG
#            writer, built for the system libjpeg, then calls MuPDF's JPEG 10
#            code, which rejects the call because the library versions differ,
#            so colour prints lose their page images. --exclude-libs keeps every
#            symbol of these archives out of the dynamic symbol table, which the
#            linux_symbol_exports test checks with scripts/check-linux-exports.py.
#
# Point MUPDF_DIR (cache var or environment variable) at the MuPDF source root.
# Provides the imported target  MuPDF::MuPDF.

if(NOT MUPDF_DIR)
    if(DEFINED ENV{MUPDF_DIR})
        set(MUPDF_DIR "$ENV{MUPDF_DIR}" CACHE PATH "MuPDF source root")
    elseif(WIN32)
        set(MUPDF_DIR "C:/dev/src/mupdf-1.28.5-source" CACHE PATH "MuPDF source root")
    else()
        set(MUPDF_DIR "$ENV{HOME}/src/mupdf-1.28.5-source" CACHE PATH "MuPDF source root")
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
    # Static qpdf uses a different JPEG ABI. Require the Windows build that
    # prefixes MuPDF's bundled JPEG symbols so the two libraries can coexist.
    set(_mupdf_flavor_file "${_mupdf_libdir}/mervin-build-flavor.txt")
    if(EXISTS "${_mupdf_flavor_file}")
        file(READ "${_mupdf_flavor_file}" _mupdf_flavor)
        string(STRIP "${_mupdf_flavor}" _mupdf_flavor)
    endif()
    if(NOT _mupdf_flavor MATCHES "^(native|ltcg)-jpeg-prefix-v1$")
        message(FATAL_ERROR
            "MuPDF at ${MUPDF_DIR} lacks the required JPEG symbol prefix. "
            "Run scripts/build-mupdf-windows.ps1 with a new -Dest and configure "
            "with -DMUPDF_DIR pointing to that directory.")
    endif()
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

# The build scripts apply the patches in scripts/mupdf-patches and record the
# patch set beside the libraries. Without the patches, render workers can read
# freed store entries and one document can render with another's colours (see
# "MuPDF patches" in docs/design.md), so refuse libraries built without the
# current set. The patch set's identity is the SHA-256 of sha256sum's lines
# ("<hash>  <name>") for the patches in name order, as the scripts and the CI
# cache keys compute it.
file(GLOB _mupdf_patches CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../scripts/mupdf-patches/*.patch")
list(SORT _mupdf_patches)
set(_mupdf_patch_lines "")
foreach(_mupdf_patch IN LISTS _mupdf_patches)
    file(SHA256 "${_mupdf_patch}" _mupdf_patch_hash)
    get_filename_component(_mupdf_patch_name "${_mupdf_patch}" NAME)
    string(APPEND _mupdf_patch_lines "${_mupdf_patch_hash}  ${_mupdf_patch_name}\n")
endforeach()
string(SHA256 _mupdf_patch_set "${_mupdf_patch_lines}")
set(_mupdf_patch_marker "${_mupdf_libdir}/mervin-patch-set.sha256")
# Check again when a patch changes or the libraries are rebuilt.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_mupdf_patches} "${_mupdf_patch_marker}")
set(_mupdf_built_patch_set "none")
if(EXISTS "${_mupdf_patch_marker}")
    file(READ "${_mupdf_patch_marker}" _mupdf_built_patch_set)
    string(STRIP "${_mupdf_built_patch_set}" _mupdf_built_patch_set)
endif()
if(NOT _mupdf_built_patch_set STREQUAL _mupdf_patch_set)
    if(WIN32)
        set(_mupdf_rebuild "scripts\\build-mupdf-windows.ps1 -Dest ${MUPDF_DIR}, with -NoLtcg if it was built that way")
    else()
        set(_mupdf_rebuild "scripts/build-mupdf-linux.sh ${MUPDF_DIR}")
    endif()
    message(FATAL_ERROR
        "MuPDF at ${MUPDF_DIR} was built with patch set ${_mupdf_built_patch_set}, not "
        "${_mupdf_patch_set} from scripts/mupdf-patches. Run ${_mupdf_rebuild}. "
        "It applies the current patches and rebuilds what changed.")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MuPDF
    REQUIRED_VARS MUPDF_INCLUDE_DIR MUPDF_CORE_LIBRARY MUPDF_ALL_LIBS)

if(MuPDF_FOUND AND NOT TARGET MuPDF::MuPDF)
    add_library(MuPDF::MuPDF INTERFACE IMPORTED)
    if(WIN32)
        set(_mupdf_link "${MUPDF_ALL_LIBS}")
    else()
        # Hide every archive the glob found. -Wl, splits its argument at
        # commas, so the archive file names are joined with colons.
        set(_mupdf_archives "${MUPDF_ALL_LIBS}")
        list(TRANSFORM _mupdf_archives REPLACE "^.*/" "")
        list(JOIN _mupdf_archives ":" _mupdf_archives)
        set(_mupdf_link
            -Wl,--start-group ${MUPDF_ALL_LIBS} -Wl,--end-group
            "-Wl,--exclude-libs,${_mupdf_archives}"
            m pthread ${CMAKE_DL_LIBS})
    endif()
    set_target_properties(MuPDF::MuPDF PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${MUPDF_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${_mupdf_link}")
endif()
