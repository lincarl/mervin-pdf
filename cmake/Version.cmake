# Release builds pass MERVIN_VERSION from the stable or release-candidate tag.
# Untagged local builds keep this fallback and can override it with
# -DMERVIN_VERSION=x.y.z or -DMERVIN_VERSION=x.y.z-rcN.
if(NOT DEFINED MERVIN_VERSION)
    set(MERVIN_VERSION 0.0.0)
endif()
if(NOT MERVIN_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(-rc([1-9][0-9]*))?$")
    message(FATAL_ERROR "MERVIN_VERSION must be MAJOR.MINOR.PATCH or MAJOR.MINOR.PATCH-rcN")
endif()
set(MERVIN_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(MERVIN_VERSION_MINOR "${CMAKE_MATCH_2}")
set(MERVIN_VERSION_PATCH "${CMAKE_MATCH_3}")
set(MERVIN_VERSION_RC "${CMAKE_MATCH_5}")
set(MERVIN_VERSION_BASE "${MERVIN_VERSION_MAJOR}.${MERVIN_VERSION_MINOR}.${MERVIN_VERSION_PATCH}")
if(MERVIN_VERSION_MAJOR GREATER 255 OR MERVIN_VERSION_MINOR GREATER 255 OR MERVIN_VERSION_PATCH GREATER 654)
    message(FATAL_ERROR "Version exceeds MSI limits of major/minor 255 and patch 654")
endif()

# MSI compares three numeric fields. Stable sorts after all 98 candidates for
# its patch. The public app version and artifact filenames retain the suffix.
set(MERVIN_VERSION_STAGE 99)
set(MERVIN_PACKAGE_VERSION "${MERVIN_VERSION_BASE}")
if(NOT MERVIN_VERSION_RC STREQUAL "")
    if(MERVIN_VERSION_RC GREATER 98)
        message(FATAL_ERROR "Release candidate number must be between 1 and 98")
    endif()
    set(MERVIN_VERSION_STAGE "${MERVIN_VERSION_RC}")
    # Both dpkg and RPM sort a tilde before the final release.
    set(MERVIN_PACKAGE_VERSION "${MERVIN_VERSION_BASE}~rc${MERVIN_VERSION_RC}")
endif()
math(EXPR MERVIN_MSI_BUILD "${MERVIN_VERSION_PATCH} * 100 + ${MERVIN_VERSION_STAGE}")
set(MERVIN_MSI_VERSION "${MERVIN_VERSION_MAJOR}.${MERVIN_VERSION_MINOR}.${MERVIN_MSI_BUILD}")
set(MERVIN_APP_NAME "Mervin PDF")
set(MERVIN_ORG_NAME "Mervin")
