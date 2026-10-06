# Checks the UI translation catalogs, i18n/mervin_<id>.ts. The i18n_catalogs test
# (tests/CMakeLists.txt) runs it with cmake -P. It fails when:
#   - lupdate warns about the app's sources (a string it can't extract can never
#     be translated);
#   - a committed catalog differs from what the update_translations target would
#     write for the current sources, byte for byte apart from line endings;
#   - a committed catalog still has unfinished messages.
# It only reads the committed catalogs. lupdate runs on copies in WORK_DIR.
#
# Variables (-D):
#   LUPDATE, LCONVERT  Qt's lupdate and lconvert
#   GENERATE_PROJECT   Qt's GenerateLUpdateProject.cmake
#   PROJECT_FILE       the .lupdate/<target>_project.cmake that qt_add_translations
#                      writes at configure time; lists the sources and catalogs
#   SOURCE_LANGUAGE    the source language ("en"); its catalog holds only plural forms
#   LUPDATE_OPTIONS    the LUPDATE_OPTIONS given to qt_add_translations, space-separated
#   WORK_DIR           scratch directory, emptied first

cmake_minimum_required(VERSION 3.25)

foreach(var IN ITEMS LUPDATE LCONVERT GENERATE_PROJECT PROJECT_FILE SOURCE_LANGUAGE WORK_DIR)
    if("${${var}}" STREQUAL "")
        message(FATAL_ERROR "CheckTranslations.cmake needs -D${var}=...")
    endif()
endforeach()
foreach(file IN ITEMS "${LUPDATE}" "${LCONVERT}" "${GENERATE_PROJECT}" "${PROJECT_FILE}")
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "${file} does not exist. Reconfigure the build directory.")
    endif()
endforeach()
separate_arguments(lupdate_options UNIX_COMMAND "${LUPDATE_OPTIONS}")

# Writes up to 10 unfinished messages of `ts_file` as "Context: source" lines into
# `out_lines` and how many there are in total into `out_count`. Vanished and
# obsolete messages have their own type, so they don't count.
function(unfinished_messages ts_file out_count out_lines)
    file(READ "${ts_file}" text)
    set(marker "<translation type=\"unfinished\"")
    string(LENGTH "${marker}" marker_length)
    string(REGEX MATCHALL "${marker}" hits "${text}")
    list(LENGTH hits count)
    set(lines "")
    set(context "")
    set(shown 0)
    while(shown LESS 10)
        string(FIND "${text}" "${marker}" at)
        if(at EQUAL -1)
            break()
        endif()
        # The text since the previous hit may hold a new context's <name>, then holds
        # this message's <source>.
        string(SUBSTRING "${text}" 0 ${at} head)
        string(FIND "${head}" "<name>" name_at REVERSE)
        if(NOT name_at EQUAL -1)
            string(SUBSTRING "${head}" ${name_at} -1 tail)
            string(REGEX REPLACE "^<name>([^<]*)</name>.*" "\\1" context "${tail}")
        endif()
        string(FIND "${head}" "<source>" source_at REVERSE)
        set(source "?")
        if(NOT source_at EQUAL -1)
            string(SUBSTRING "${head}" ${source_at} -1 tail)
            string(REGEX REPLACE "^<source>([^<]*)</source>.*" "\\1" source "${tail}")
        endif()
        string(REPLACE "\n" "\\n" source "${source}")
        string(REPLACE "&lt;" "<" source "${source}")
        string(REPLACE "&gt;" ">" source "${source}")
        string(REPLACE "&quot;" "\"" source "${source}")
        string(REPLACE "&apos;" "'" source "${source}")
        string(REPLACE "&amp;" "&" source "${source}")
        string(APPEND lines "    ${context}: ${source}\n")
        math(EXPR shown "${shown} + 1")
        math(EXPR next "${at} + ${marker_length}")
        string(SUBSTRING "${text}" ${next} -1 text)
    endwhile()
    if(count GREATER shown)
        math(EXPR more "${count} - ${shown}")
        string(APPEND lines "    and ${more} more\n")
    endif()
    set(${out_count} ${count} PARENT_SCOPE)
    set(${out_lines} "${lines}" PARENT_SCOPE)
endfunction()

# The catalogs update_translations writes, as absolute paths.
include("${PROJECT_FILE}")
set(committed ${lupdate_translations})
if(committed STREQUAL "")
    message(FATAL_ERROR "${PROJECT_FILE} lists no translation catalogs.")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(copies "")
set(plurals_copy "")
foreach(ts IN LISTS committed)
    get_filename_component(name "${ts}" NAME)
    file(COPY_FILE "${ts}" "${WORK_DIR}/${name}")
    list(APPEND copies "${WORK_DIR}/${name}")
    if(name MATCHES "_${SOURCE_LANGUAGE}\\.ts$")
        set(plurals_copy "${WORK_DIR}/${name}")
    endif()
endforeach()

# The same lupdate run as update_translations, pointed at the copies.
file(WRITE "${WORK_DIR}/project.cmake"
    "include(\"${PROJECT_FILE}\")\nset(lupdate_translations \"${copies}\")\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DIN_FILE=${WORK_DIR}/project.cmake"
            "-DOUT_FILE=${WORK_DIR}/project.json" -P "${GENERATE_PROJECT}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Writing the lupdate project failed.")
endif()
execute_process(
    COMMAND "${LUPDATE}" -project "${WORK_DIR}/project.json" ${lupdate_options}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE summary
    ERROR_VARIABLE warnings
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "lupdate failed (${result}):\n${warnings}")
endif()
if(NOT plurals_copy STREQUAL "")
    # update_translations keeps only the plural forms in the source language's catalog.
    execute_process(
        COMMAND "${LCONVERT}" -pluralonly -i "${plurals_copy}" -o "${plurals_copy}"
        RESULT_VARIABLE result
        ERROR_VARIABLE lconvert_error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "lconvert failed (${result}):\n${lconvert_error}")
    endif()
endif()

set(failures 0)

if(NOT warnings STREQUAL "")
    math(EXPR failures "${failures} + 1")
    message(NOTICE "lupdate warns about these sources. Strings it cannot extract can never be "
        "translated, so fix each one (see docs/TRANSLATING.md):\n${warnings}\n")
endif()

set(stale "")
foreach(ts copy IN ZIP_LISTS committed copies)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files --ignore-eol "${ts}" "${copy}"
        RESULT_VARIABLE differs
        OUTPUT_QUIET ERROR_QUIET)
    if(NOT differs EQUAL 0)
        get_filename_component(name "${ts}" NAME)
        string(APPEND stale "    ${name}\n")
    endif()
endforeach()
if(NOT stale STREQUAL "")
    math(EXPR failures "${failures} + 1")
    message(NOTICE "These catalogs differ from what update_translations writes, because "
        "strings were added or removed or because the file is not in lupdate's format "
        "(for example a hand-typed quote that lupdate writes as &quot;):\n${stale}"
        "Build the update_translations target, translate any new strings, and commit the "
        "catalogs. lupdate reported:\n${summary}\n")
endif()

foreach(ts IN LISTS committed)
    unfinished_messages("${ts}" count lines)
    if(count GREATER 0)
        math(EXPR failures "${failures} + 1")
        get_filename_component(name "${ts}" NAME)
        message(NOTICE "${name} has ${count} unfinished message(s). Translate them and mark "
            "them finished:\n${lines}")
    endif()
endforeach()

if(failures GREATER 0)
    message(FATAL_ERROR "The translation catalogs failed ${failures} check(s); see above.")
endif()
list(LENGTH committed catalogs)
message(STATUS "${catalogs} translation catalogs are up to date and fully translated.")
