find_package(Qt6 REQUIRED COMPONENTS Test)

# Windows only: ensure ctest can find the runtime DLLs without manual PATH setup
# (Qt's bin + the vcpkg-installed bin where qpdf and its deps live). On Linux the
# dynamic loader finds the shared libs normally, so this stays empty there.
if(WIN32)
    file(TO_CMAKE_PATH "$ENV{QT6_DIR}/bin" _mervin_qt_bin)
    set(_mervin_vcpkg_bin "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin")
    # PATH: the test exes are not deployed with their DLLs, so Qt's bin and the
    # vcpkg-installed bin (where qpdf and its deps live) must be reachable or
    # every test dies with 0xc0000135 before reaching main(). On Linux the
    # dynamic loader finds the shared libs normally, so this stays empty there.
    #
    # QT_ASSUME_STDERR_HAS_CONSOLE: on Windows, QPlainTestLogger hands the whole
    # test log to OutputDebugStringA instead of writing it to stdout whenever Qt
    # decides stderr is not attached to a console - and under ctest stderr is a
    # pipe, so it never is. The symptom is an empty LastTest.log and a
    # --output-on-failure that explains nothing at all, which is exactly how a
    # broken test used to be undiagnosable here. Telling Qt to assume a console
    # puts the log back on stdout where ctest can capture it. (The older
    # QT_LOGGING_TO_CONSOLE=1 does the same thing but Qt 6 deprecates it.)
    #
    # Note this is a *runtime* fix. WIN32_EXECUTABLE OFF below is still needed
    # and was never the problem: the exes were already console-subsystem.
    set(_mervin_test_env
        "PATH=path_list_prepend:${_mervin_qt_bin}"
        "PATH=path_list_prepend:${_mervin_vcpkg_bin}"
        "QT_ASSUME_STDERR_HAS_CONSOLE=set:1")
endif()

# A QSKIP makes QTest exit 0, so ctest reports a test that silently ran nothing
# as "Passed" - which is how a corrupt shipped eng.traineddata stayed hidden for
# six weeks behind a test that skipped itself (see tst_tessdata). Several cases
# here legitimately skip when their fixture data is absent, so a skip must not
# fail the build; it just has to be visible in the ctest summary.
#
# The regex deliberately anchors on QTest's totals line and requires "0 failed".
# Matching the bare "SKIP   :" line would be wrong: SKIP_REGULAR_EXPRESSION wins
# "regardless of the process exit code", so a test that skipped one function and
# genuinely FAILED another would be reported as Skipped and the failure lost.
# Verified both ways against ctest 4.3.3 before settling on this.
#
# This only works because of the stdout fix above - with no captured output there
# is nothing for the regex to match.
set(_mervin_skip_regex "Totals: [0-9]+ passed, 0 failed, [1-9][0-9]* skipped")

set(_mervin_tests
    tst_comfort_transform
    tst_ink_rects
    tst_preview_layer
    tst_message
    tst_single_instance
    tst_recent_store
    tst_view_state_store
    tst_content_search
    tst_print_range
    tst_url_open
    tst_merge_plan
    tst_text_index
    tst_qpdf
    tst_ocr
    tst_ocr_popup
    tst_settings_dialog
    tst_tessdata
    tst_session_store
    tst_startup_plan
    tst_closed_tab_stack
    tst_document
    tst_view_layout
    tst_measure
    tst_measure_export
    tst_form_model
    tst_annot_model
    tst_file_clipboard
    tst_perf_render
    tst_perf_startup)

foreach(_t IN LISTS _mervin_tests)
    qt_add_executable(${_t} ${_t}.cpp)
    target_link_libraries(${_t} PRIVATE mervin_core Qt6::Test)
    # Console subsystem: necessary for ctest to capture output, but not on its own
    # sufficient on Windows - see QT_ASSUME_STDERR_HAS_CONSOLE above.
    set_target_properties(${_t} PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME ${_t} COMMAND ${_t})
    set_tests_properties(${_t} PROPERTIES
        ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
        SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")
endforeach()

# Widget-level guard rails for the Selection OCR language picker. OcrPopup
# belongs to the application target, so compile its self-contained sources into
# this test; the actual OCR engine remains covered separately by tst_ocr.
target_sources(tst_ocr_popup PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dialogs/OcrPopup.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/OcrPopup.h
    ${CMAKE_SOURCE_DIR}/src/dialogs/ManageLanguagesDialog.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/ManageLanguagesDialog.h)
target_link_libraries(tst_ocr_popup PRIVATE Qt6::Widgets)

# Settings owns the only in-app entry point for changing the default PDF
# handler. Keep that platform-specific control out of non-Windows builds.
target_sources(tst_settings_dialog PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dialogs/SettingsDialog.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/SettingsDialog.h
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.h
    ${CMAKE_SOURCE_DIR}/src/ui/Icons.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Icons.h)
target_link_libraries(tst_settings_dialog PRIVATE Qt6::Widgets)

# Real picker-path guard rail: enter a URL into the visible file-name field and
# press its Open button. This catches regressions where the native Windows shell
# downloads the URL before Mervin can show progress or apply URL compatibility.
qt_add_executable(tst_open_pdf_dialog tst_open_pdf_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/OpenPdfDialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/OpenPdfDialog.h)
target_link_libraries(tst_open_pdf_dialog PRIVATE mervin_core Qt6::Test Qt6::Widgets)
if(WIN32)
    target_link_libraries(tst_open_pdf_dialog PRIVATE comctl32 ole32 shell32 uuid)
endif()
set_target_properties(tst_open_pdf_dialog PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_open_pdf_dialog COMMAND tst_open_pdf_dialog)
set_tests_properties(tst_open_pdf_dialog PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# Widget-level guard rail for the zoom preview layer. ViewerWidget lives in the
# application target, so its four self-contained UI sources are compiled into
# this test as well (the headers are listed so AUTOMOC processes them here).
# The viewer is flagged WA_DontShowOnScreen, so no window ever appears and the
# normal platform plugin is enough - no offscreen plugin to deploy, and the exe
# can be run directly like the perf guardrails.
qt_add_executable(tst_viewer_preview tst_viewer_preview.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ViewerWidget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ViewerWidget.h
    ${CMAKE_SOURCE_DIR}/src/ui/AnnotPopup.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/AnnotPopup.h
    ${CMAKE_SOURCE_DIR}/src/ui/PdfPropertiesPopup.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/PdfPropertiesPopup.h
    ${CMAKE_SOURCE_DIR}/src/ui/Icons.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Icons.h)
target_link_libraries(tst_viewer_preview PRIVATE mervin_core Qt6::Test Qt6::Widgets)
target_compile_definitions(tst_viewer_preview PRIVATE
    MERVIN_HOUSE_PDF="${CMAKE_SOURCE_DIR}/examples/house-drawing.pdf"
    # A multi-page fixture: the fit-mode anchoring case needs pages to land on.
    MERVIN_IMAGES_PDF="${CMAKE_SOURCE_DIR}/examples/images.pdf")
set_target_properties(tst_viewer_preview PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_viewer_preview COMMAND tst_viewer_preview)
set_tests_properties(tst_viewer_preview PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# Widget-level guard rails for the fit modes on a mixed-size document. Same
# arrangement as tst_viewer_preview above: ViewerWidget belongs to the application
# target, so its four self-contained UI sources are compiled in here too (headers
# listed so AUTOMOC processes them), and the viewer is flagged WA_DontShowOnScreen
# so no window appears.
qt_add_executable(tst_viewer_fit tst_viewer_fit.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ViewerWidget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ViewerWidget.h
    ${CMAKE_SOURCE_DIR}/src/ui/AnnotPopup.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/AnnotPopup.h
    ${CMAKE_SOURCE_DIR}/src/ui/PdfPropertiesPopup.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/PdfPropertiesPopup.h
    ${CMAKE_SOURCE_DIR}/src/ui/Icons.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Icons.h)
target_link_libraries(tst_viewer_fit PRIVATE mervin_core Qt6::Test Qt6::Widgets)
set_target_properties(tst_viewer_fit PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_viewer_fit COMMAND tst_viewer_fit)
set_tests_properties(tst_viewer_fit PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# Developer tool (not a test): renders pages through the real Comfort
# pipeline and writes PNGs for visual verification of the image treatments.
qt_add_executable(dump_comfort_render dump_comfort_render.cpp)
target_link_libraries(dump_comfort_render PRIVATE mervin_core)
set_target_properties(dump_comfort_render PROPERTIES WIN32_EXECUTABLE OFF)

# Guard rails for the house icon set. Icons.cpp belongs to the application
# target, so it is compiled in here (as tst_viewer_preview does for the viewer);
# nothing else from the app is needed, so this test does not link mervin_core.
qt_add_executable(tst_icons tst_icons.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Icons.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Icons.h
    ${CMAKE_SOURCE_DIR}/src/ui/IconList.h)
target_include_directories(tst_icons PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_icons PRIVATE Qt6::Gui Qt6::Test)
set_target_properties(tst_icons PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_icons COMMAND tst_icons)
set_tests_properties(tst_icons PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# Guard rails for the colour vocabulary and the one stylesheet built from it.
# Theme.cpp belongs to the application target, so it is compiled in here (as
# tst_merge_dialog does); Icons.cpp comes along because the sheet's glyphs are
# painted through it. QTEST_MAIN because QPalette needs a QApplication.
qt_add_executable(tst_theme tst_theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.h
    ${CMAKE_SOURCE_DIR}/src/ui/Icons.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Icons.h)
target_link_libraries(tst_theme PRIVATE mervin_core Qt6::Test Qt6::Widgets)
set_target_properties(tst_theme PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_theme COMMAND tst_theme)
set_tests_properties(tst_theme PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# Guard rails for the shared find bar's two search contexts (document find vs the
# recent-list filter), which used to leak into each other across a view switch.
# FindBar.cpp belongs to the application target, so it is compiled in here (as
# tst_icons does); Icons.cpp comes along because the bar paints its magnifier
# glyph through it. The bar talks to the rest of the app only through signals, so
# nothing else is needed and this test does not link mervin_core - which also
# keeps it building when MuPDF is not around. QTEST_MAIN because it is a QWidget.
qt_add_executable(tst_find_bar tst_find_bar.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/FindBar.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/FindBar.h
    ${CMAKE_SOURCE_DIR}/src/ui/Icons.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Icons.h)
target_include_directories(tst_find_bar PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_find_bar PRIVATE Qt6::Test Qt6::Widgets)
set_target_properties(tst_find_bar PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_find_bar COMMAND tst_find_bar)
set_tests_properties(tst_find_bar PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# Developer tool (not a test): a contact sheet of every house icon, at the
# inspection size and at the 16/20/24 px the UI actually asks for. Icons.cpp is
# compiled in directly - it belongs to the application target, not mervin_core.
qt_add_executable(dump_icons dump_icons.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Icons.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Icons.h)
target_link_libraries(dump_icons PRIVATE mervin_core Qt6::Gui)
set_target_properties(dump_icons PROPERTIES WIN32_EXECUTABLE OFF)

# tst_qpdf builds fixture PDFs directly with qpdf, so it links it explicitly.
target_link_libraries(tst_qpdf PRIVATE qpdf::libqpdf)

# Widget-level guard rails for the merge dialog. MergeDialog and the two UI
# sources it uses belong to the application target, so they are compiled in here
# (as tst_viewer_preview does for the viewer); the headers are listed so AUTOMOC
# processes them. The dialog is flagged WA_DontShowOnScreen and never exec()d, so
# no window appears and the normal platform plugin is enough. It builds its own
# encrypted fixture, so it links qpdf like tst_qpdf.
qt_add_executable(tst_merge_dialog tst_merge_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/MergeDialog.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/MergeDialog.h
    ${CMAKE_SOURCE_DIR}/src/ui/Icons.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Icons.h
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.h)
target_link_libraries(tst_merge_dialog
    PRIVATE mervin_core Qt6::Test Qt6::Widgets qpdf::libqpdf)
set_target_properties(tst_merge_dialog PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_merge_dialog COMMAND tst_merge_dialog)
set_tests_properties(tst_merge_dialog PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# tst_document compares Document's cheap page-object size walk against what a
# fully loaded page reports, so it calls fz_load_page/fz_bound_page itself and
# needs MuPDF's headers (mervin_core links MuPDF privately). The sample documents
# are the interesting inputs: schematic.pdf is link-annotation heavy and
# form_comment.pdf carries widget + markup annotations - the pages whose
# annotations the old walk resolved just to read a width and a height.
target_link_libraries(tst_document PRIVATE MuPDF::MuPDF)
target_compile_definitions(tst_document PRIVATE
    MERVIN_SCHEMATIC_PDF="${CMAKE_SOURCE_DIR}/examples/schematic.pdf"
    MERVIN_FORM_PDF="${CMAKE_SOURCE_DIR}/examples/form_comment.pdf"
    MERVIN_HOUSE_PDF="${CMAKE_SOURCE_DIR}/examples/house-drawing.pdf"
    MERVIN_IMAGES_PDF="${CMAKE_SOURCE_DIR}/examples/images.pdf")

# tst_view_layout builds its own /MediaBox-only fixtures, with exact integer page
# sizes so the layout arithmetic can be checked by hand.

# tst_content_search searches a real document through MuPDF text extraction. It
# uses an optional local fixture; MERVIN_TEST_PDF still overrides it.
target_compile_definitions(tst_content_search PRIVATE
    MERVIN_SEARCH_PDF="${CMAKE_SOURCE_DIR}/examples/properties.pdf")

# tst_tessdata validates the language model the installers ship, and checks that
# the validator rejects the damage classes that would otherwise abort the process
# inside Tesseract. Needs only the file - no OCR run, no language installed.
target_compile_definitions(tst_tessdata PRIVATE
    MERVIN_TESSDATA_ENG="${CMAKE_SOURCE_DIR}/resources/tessdata/eng.traineddata")

# tst_ocr runs real OCR. The source tree's tessdata folder is preferred over the
# developer's own so the test exercises the model we package; the PDF fixture is
# optional and local.
target_compile_definitions(tst_ocr PRIVATE
    MERVIN_OCR_PDF="${CMAKE_SOURCE_DIR}/examples/images.pdf"
    MERVIN_TESSDATA_DIR="${CMAKE_SOURCE_DIR}/resources/tessdata")

# tst_measure_export builds fixture PDFs and inspects flatten/annotate output
# with qpdf, so it links it explicitly too. Its data-gated case round-trips the
# shipped sample drawing through MuPDF (QSKIPs if the file is absent).
target_link_libraries(tst_measure_export PRIVATE qpdf::libqpdf)
target_compile_definitions(tst_measure_export PRIVATE
    MERVIN_DRAWING_PDF="${CMAKE_SOURCE_DIR}/docs/drawing.pdf")

# tst_measure's data-gated cases open the sample drawing shipped in docs/. The
# path is baked in; the test QSKIPs the data cases if the file is absent.
target_compile_definitions(tst_measure PRIVATE
    MERVIN_DRAWING_PDF="${CMAKE_SOURCE_DIR}/docs/drawing.pdf")

# tst_file_clipboard verifies the Explorer-style file copy against the real
# Win32 clipboard (CF_HDROP + Preferred DropEffect), so it needs user32/shell32.
if(WIN32)
    target_link_libraries(tst_file_clipboard PRIVATE user32 shell32)
endif()

# Performance guardrails (see the headers of the two files): tst_perf_render
# times rasterization + the Comfort/invert pixel passes on a local sample drawing;
# tst_perf_startup launches the real app binary with an isolated
# --profile and times the whole process. Their numbers print to stdout - run
# the exes directly (or `ctest -R tst_perf -V`) to read them.
target_compile_definitions(tst_perf_render PRIVATE
    MERVIN_HOUSE_PDF="${CMAKE_SOURCE_DIR}/examples/house-drawing.pdf"
    # The document-open case times every sample in the directory: open cost is what
    # a multi-document start is made of, and whole-process timings cannot resolve it.
    MERVIN_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/examples")
# tst_ink_rects renders pages of optional local image-heavy samples end to end
# through the Comfort pipeline (QSKIPs if a file is absent).
target_compile_definitions(tst_ink_rects PRIVATE
    MERVIN_IMAGES_PDF="${CMAKE_SOURCE_DIR}/examples/images.pdf"
    MERVIN_IMAGES2_PDF="${CMAKE_SOURCE_DIR}/examples/images2.pdf"
    MERVIN_PCBA_PDF="${CMAKE_SOURCE_DIR}/examples/pcba.pdf"
    MERVIN_HOUSE_PDF="${CMAKE_SOURCE_DIR}/examples/house-drawing.pdf")
target_compile_definitions(tst_perf_startup PRIVATE
    MERVIN_APP_EXE="$<TARGET_FILE:MervinPDF>"
    # Documents for the restored-session case: a real multi-document startup is
    # what the staged open + cheap page walk changed, and the empty-profile run
    # cannot see any of it.
    MERVIN_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/examples")
add_dependencies(tst_perf_startup MervinPDF)
