# Release builds pass MERVIN_VERSION from the vMAJOR.MINOR.PATCH Git tag.
# Untagged local builds keep this fallback and can override it with
# -DMERVIN_VERSION=x.y.z.
if(NOT DEFINED MERVIN_VERSION)
    set(MERVIN_VERSION 0.0.0)
endif()
if(NOT MERVIN_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "MERVIN_VERSION must be MAJOR.MINOR.PATCH")
endif()
set(MERVIN_APP_NAME "Mervin PDF")
set(MERVIN_ORG_NAME "Mervin")
