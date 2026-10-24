# CPack runs this after installation, so dependency filters reflect the actual
# bundled SONAMEs instead of a list that can miss a library or hide another ABI.
if(NOT CPACK_GENERATOR STREQUAL "RPM")
    return()
endif()

find_program(_mervin_readelf readelf REQUIRED)
file(GLOB _mervin_libraries "${CPACK_TEMPORARY_DIRECTORY}/usr/lib/mervin-pdf/lib/*.so*")
set(_mervin_sonames "")
foreach(_mervin_library IN LISTS _mervin_libraries)
    if(IS_SYMLINK "${_mervin_library}")
        continue()
    endif()
    execute_process(COMMAND "${_mervin_readelf}" -d "${_mervin_library}"
        OUTPUT_VARIABLE _mervin_dynamic COMMAND_ERROR_IS_FATAL ANY)
    if(_mervin_dynamic MATCHES "\\(SONAME\\)[^\n]*\\[([^]]+)\\]")
        set(_mervin_soname "${CMAKE_MATCH_1}")
        if(NOT _mervin_soname MATCHES "^[A-Za-z0-9_.+-]+$")
            message(FATAL_ERROR "Unexpected bundled SONAME: ${_mervin_soname}")
        endif()
        string(REGEX REPLACE "([.+])" "[\\1]" _mervin_pattern "${_mervin_soname}")
        list(APPEND _mervin_sonames "${_mervin_pattern}")
    endif()
endforeach()
if(NOT _mervin_sonames)
    message(FATAL_ERROR "No bundled library SONAMEs found for RPM dependency filtering")
endif()
list(REMOVE_DUPLICATES _mervin_sonames)
list(JOIN _mervin_sonames "|" _mervin_private_requires)
# Private libraries must neither satisfy other packages' dependencies nor add
# requirements for distribution copies. Continue scanning every ELF file for
# external OS dependencies, including those needed by Qt plugins.
string(APPEND CPACK_RPM_SPEC_MORE_DEFINE
    "\n%global __provides_exclude_from ^/usr/lib/mervin-pdf/.*$"
    "\n%global __requires_exclude ^(${_mervin_private_requires})([(].*[)])?$\n")
