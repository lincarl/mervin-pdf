find_package(Qt6 REQUIRED COMPONENTS Test)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(MERVIN_FIXTURES_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_fixtures.py"
    "${MERVIN_FIXTURES_DIR}" COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS generate_fixtures.py)


# On Windows, ensure ctest can find Qt's runtime DLLs without manual PATH setup.
# vcpkg dependencies are static and must not need their bin directory. On Linux the
# dynamic loader finds the shared libs normally, so this stays empty there.
if(WIN32)
    file(TO_CMAKE_PATH "$ENV{QT6_DIR}/bin" _mervin_qt_bin)
    # Force Qt test output to stderr so CTest captures failures.
    set(_mervin_test_env
        "PATH=path_list_prepend:${_mervin_qt_bin}"
        "QT_ASSUME_STDERR_HAS_CONSOLE=set:1")
endif()

# Expose QTest skips without masking failures in the same executable.
# CI rejects skips in required targets; optional corpus tests have a separate label.
set(_mervin_skip_regex "Totals: [0-9]+ passed, 0 failed, [1-9][0-9]* skipped")

set(_mervin_tests
    tst_atomic_pdf_writer
    tst_perf_layout
    tst_document_output
    tst_render_lifetime
    tst_settings_store
    tst_ui_language
    tst_comfort_transform
    tst_ink_rects
    tst_preview_layer
    tst_message
    tst_single_instance
    tst_recent_store
    tst_mount_table
    tst_view_state_store
    tst_content_search
    tst_print_range
    tst_print_layout
    tst_url_open
    tst_merge_plan
    tst_extract_plan
    tst_text_index
    tst_qpdf
    tst_ocr
    tst_ocr_popup
    tst_settings_dialog
    tst_tessdata
    tst_ocr_provisioner
    tst_session_store
    tst_session_end
    tst_file_probe
    tst_app_files
    tst_background
    tst_job_context
    tst_startup_plan
    tst_closed_tab_stack
    tst_document
    tst_mupdf_store
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

# These tests inspect qpdf objects directly; mervin_core keeps its dependency private.
foreach(_t tst_atomic_pdf_writer tst_document_output)
    target_link_libraries(${_t} PRIVATE qpdf::libqpdf)
endforeach()

if(WIN32)
    # The native clipboard contract cannot be exercised by Qt's in-memory offscreen clipboard.
    set_tests_properties(tst_file_clipboard PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
endif()

# Widget-level guard rails for the Selection OCR language picker. OcrPopup
# belongs to the application target, so compile its self-contained sources into
# this test; the actual OCR engine remains covered separately by tst_ocr.
target_sources(tst_ocr_popup PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dialogs/OcrPopup.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/OcrPopup.h
    ${CMAKE_SOURCE_DIR}/src/dialogs/ManageLanguagesDialog.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/ManageLanguagesDialog.h
    ${CMAKE_SOURCE_DIR}/src/dialogs/LayoutMinimum.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/LayoutMinimum.h)
target_link_libraries(tst_ocr_popup PRIVATE Qt6::Widgets)

# Settings pages: navigation, the round trip of every field, the update
# controls, and the Windows-only default PDF handler control. The OCR page opens
# Manage OCR Languages, so its sources come along.
target_sources(tst_settings_dialog PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dialogs/SettingsDialog.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/SettingsDialog.h
    ${CMAKE_SOURCE_DIR}/src/dialogs/ManageLanguagesDialog.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/ManageLanguagesDialog.h
    ${CMAKE_SOURCE_DIR}/src/dialogs/LayoutMinimum.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/LayoutMinimum.h
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.h
    ${CMAKE_SOURCE_DIR}/src/ui/DocumentThemePicker.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/DocumentThemePicker.h
    ${CMAKE_SOURCE_DIR}/src/ui/ThemeCard.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ThemeCard.h
    ${CMAKE_SOURCE_DIR}/src/ui/UiThemePicker.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/UiThemePicker.h
    ${CMAKE_SOURCE_DIR}/src/ui/LanguageCombo.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/LanguageCombo.h)
target_link_libraries(tst_settings_dialog PRIVATE mervin_icons Qt6::Widgets)

# Exercise URL and file selection through the Qt picker on Linux. Windows checks
# native-dialog selection; shell URL-entry behavior still needs native interaction.
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

# The application and widget tests link the same viewer implementation.
qt_add_executable(tst_viewer_preview tst_viewer_preview.cpp)
target_link_libraries(tst_viewer_preview PRIVATE mervin_viewer Qt6::Test)
target_compile_definitions(tst_viewer_preview PRIVATE
    MERVIN_HOUSE_PDF="${MERVIN_FIXTURES_DIR}/house-drawing.pdf"
    # A multi-page fixture: the fit-mode anchoring case needs pages to land on.
    MERVIN_IMAGES_PDF="${MERVIN_FIXTURES_DIR}/images.pdf")
set_target_properties(tst_viewer_preview PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_viewer_preview COMMAND tst_viewer_preview)
set_tests_properties(tst_viewer_preview PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# Fit-mode regression tests use generated mixed-size documents.
qt_add_executable(tst_viewer_fit tst_viewer_fit.cpp)
target_link_libraries(tst_viewer_fit PRIVATE mervin_viewer Qt6::Test)
set_target_properties(tst_viewer_fit PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_viewer_fit COMMAND tst_viewer_fit)
set_tests_properties(tst_viewer_fit PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")
# The zoom cap depends on the device pixel ratio, which is 1 in a plain offscreen run.
set(_mervin_dpr2_env ${_mervin_test_env})
list(APPEND _mervin_dpr2_env "QT_SCALE_FACTOR=set:2")
add_test(NAME tst_viewer_fit_dpr2 COMMAND tst_viewer_fit deepZoomStopsAtTheRasteriserRange)
set_tests_properties(tst_viewer_fit_dpr2 PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_dpr2_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# Developer tool (not a test): renders pages through the real Comfort
# pipeline and writes PNGs for visual verification of the image treatments.
qt_add_executable(dump_comfort_render dump_comfort_render.cpp)
target_link_libraries(dump_comfort_render PRIVATE mervin_core)
set_target_properties(dump_comfort_render PROPERTIES WIN32_EXECUTABLE OFF)

# Guard rails for application artwork and Lucide icons. The assets come from the
# mervin_icons library; nothing else from the app is needed, so this test does
# not link mervin_core.
qt_add_executable(tst_icons tst_icons.cpp)
target_include_directories(tst_icons PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_icons PRIVATE mervin_icons Qt6::Gui Qt6::Test)
set_target_properties(tst_icons PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_icons COMMAND tst_icons)
set_tests_properties(tst_icons PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# The updater's release parsing, asset choice, and Windows install recognition.
# UpdatePlan is pure QtCore, so this test needs neither mervin_core nor widgets.
qt_add_executable(tst_update_plan tst_update_plan.cpp
    ${CMAKE_SOURCE_DIR}/src/update/UpdatePlan.cpp
    ${CMAKE_SOURCE_DIR}/src/update/UpdatePlan.h)
target_include_directories(tst_update_plan PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_update_plan PRIVATE Qt6::Core Qt6::Test)
set_target_properties(tst_update_plan PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_update_plan COMMAND tst_update_plan)
set_tests_properties(tst_update_plan PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# Carry-file failure recovery and the native Windows helper's actual argv and cleanup.
# Only package identity is substituted; the Windows process fixture never runs MSI.
qt_add_executable(tst_relaunch tst_relaunch.cpp
    ${CMAKE_SOURCE_DIR}/src/app/Relaunch.cpp
    ${CMAKE_SOURCE_DIR}/src/update/Installer.cpp
    ${CMAKE_SOURCE_DIR}/src/update/UpdatePlan.cpp
    ${CMAKE_SOURCE_DIR}/src/config/ConfigPaths.cpp
    ${CMAKE_SOURCE_DIR}/src/ipc/Message.cpp)
target_include_directories(tst_relaunch PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_relaunch PRIVATE Qt6::Core Qt6::Test)
if(WIN32)
    target_link_libraries(tst_relaunch PRIVATE user32)
endif()
set_target_properties(tst_relaunch PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_relaunch COMMAND tst_relaunch)
set_tests_properties(tst_relaunch PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# Exercise checks, downloads, automatic installation and cancellation with
# local replies and an isolated profile. Test-local Installer functions record
# platform calls without launching installers or changing this machine.
qt_add_executable(tst_updater tst_updater.cpp
    ${CMAKE_SOURCE_DIR}/src/update/Updater.cpp
    ${CMAKE_SOURCE_DIR}/src/update/Updater.h
    ${CMAKE_SOURCE_DIR}/src/update/UpdatePlan.cpp
    ${CMAKE_SOURCE_DIR}/src/update/UpdatePlan.h
    ${CMAKE_SOURCE_DIR}/src/update/Installer.h
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.h)
target_link_libraries(tst_updater PRIVATE mervin_core mervin_icons Qt6::Test Qt6::Widgets)
set_target_properties(tst_updater PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_updater COMMAND tst_updater)
set_tests_properties(tst_updater PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# Restoring old detached sidebars must not leave extra floating windows.
qt_add_executable(tst_sidebar_layout tst_sidebar_layout.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/SidebarLayout.h)
target_include_directories(tst_sidebar_layout PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_sidebar_layout PRIVATE Qt6::Test Qt6::Widgets)
set_target_properties(tst_sidebar_layout PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_sidebar_layout COMMAND tst_sidebar_layout)
set_tests_properties(tst_sidebar_layout PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# Guard rails for the colour vocabulary and the one stylesheet built from it.
# Theme.cpp belongs to the application target, so it is compiled in here (as
# tst_merge_dialog does); it links mervin_icons because the sheet's indicator
# images are rendered through it. QTEST_MAIN because QPalette needs a QApplication.
qt_add_executable(tst_theme tst_theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.h)
target_link_libraries(tst_theme PRIVATE mervin_icons mervin_core Qt6::Test Qt6::Widgets)
set_target_properties(tst_theme PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_theme COMMAND tst_theme)
set_tests_properties(tst_theme PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# The floating find card: opening, closing, Enter and Escape, the option clicks
# that must leave the caret in the field, and the field's paste trimming. It
# compiles the widget in directly and talks to it only through signals, so it
# needs no document (or MuPDF); PanelStack comes along because the card asks it
# for its position. It links mervin_icons for the card's glyphs.
qt_add_executable(tst_find_card tst_find_card.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/FindCard.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/FindCard.h
    ${CMAKE_SOURCE_DIR}/src/ui/PanelStack.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/PanelStack.h
    ${CMAKE_SOURCE_DIR}/src/ui/SearchLineEdit.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/SearchLineEdit.h)
target_include_directories(tst_find_card PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_find_card PRIVATE mervin_icons Qt6::Test Qt6::Widgets)
set_target_properties(tst_find_card PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_find_card COMMAND tst_find_card)
set_tests_properties(tst_find_card PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# Developer tool (not a test): a contact sheet of every icon, at the inspection
# size and at the 16/20/24 px the UI actually asks for, rendered through
# mervin_icons exactly as the application renders them.
qt_add_executable(dump_icons dump_icons.cpp)
target_link_libraries(dump_icons PRIVATE mervin_icons mervin_core Qt6::Gui)
set_target_properties(dump_icons PROPERTIES WIN32_EXECUTABLE OFF)

# tst_qpdf builds fixture PDFs directly with qpdf, so it links it explicitly.
target_link_libraries(tst_qpdf PRIVATE qpdf::libqpdf)

# Widget-level guard rails for the merge dialog. MergeDialog and the UI sources
# it uses (its shared row list and minimum size helper, theme) belong to the application target, so they
# are compiled in here (as tst_viewer_preview does for the viewer; the icons come
# from mervin_icons); the headers are listed so AUTOMOC processes them. The
# dialog is flagged WA_DontShowOnScreen and never exec()d, so no window appears and the normal
# platform plugin is enough. It builds its own encrypted fixture, so it links qpdf
# like tst_qpdf.
qt_add_executable(tst_merge_dialog tst_merge_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/MergeDialog.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/MergeDialog.h
    ${CMAKE_SOURCE_DIR}/src/dialogs/LayoutMinimum.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/LayoutMinimum.h
    ${CMAKE_SOURCE_DIR}/src/dialogs/RowList.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/RowList.h
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.h
    ${CMAKE_SOURCE_DIR}/src/ui/WaitBar.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/WaitBar.h)
target_link_libraries(tst_merge_dialog
    PRIVATE mervin_icons mervin_core Qt6::Test Qt6::Widgets qpdf::libqpdf)
set_target_properties(tst_merge_dialog PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_merge_dialog COMMAND tst_merge_dialog)
set_tests_properties(tst_merge_dialog PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

qt_add_executable(tst_merge_operations tst_merge_operations.cpp TestWaits.h)
target_link_libraries(tst_merge_operations PRIVATE mervin_app Qt6::Test)
set_target_properties(tst_merge_operations PROPERTIES WIN32_EXECUTABLE OFF)
target_compile_definitions(tst_merge_operations PRIVATE
    MERVIN_FIXTURE_PDF="${MERVIN_FIXTURES_DIR}/properties.pdf"
    MERVIN_UNREADABLE_PDF="${MERVIN_FIXTURES_DIR}/unreadable-measurements.pdf")
add_test(NAME tst_merge_operations COMMAND tst_merge_operations)
set_tests_properties(tst_merge_operations PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    TIMEOUT 180
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# Widget-level guard rails for the extract dialog, arranged like tst_merge_dialog
# above: the dialog, its strip, the row list and minimum size helper it shares
# with Merge and the theme source they paint with belong to the application target, so they are compiled
# in here (the icons come from mervin_icons), and the test builds its plain and encrypted fixtures with qpdf
# at runtime.
qt_add_executable(tst_extract_dialog tst_extract_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/ExtractDialog.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/ExtractDialog.h
    ${CMAKE_SOURCE_DIR}/src/dialogs/ExtractStrip.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/ExtractStrip.h
    ${CMAKE_SOURCE_DIR}/src/dialogs/LayoutMinimum.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/LayoutMinimum.h
    ${CMAKE_SOURCE_DIR}/src/dialogs/RowList.cpp
    ${CMAKE_SOURCE_DIR}/src/dialogs/RowList.h
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.h)
target_link_libraries(tst_extract_dialog
    PRIVATE mervin_icons mervin_core Qt6::Test Qt6::Widgets qpdf::libqpdf)
set_target_properties(tst_extract_dialog PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_extract_dialog COMMAND tst_extract_dialog)
set_tests_properties(tst_extract_dialog PROPERTIES
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
    MERVIN_SCHEMATIC_PDF="${MERVIN_FIXTURES_DIR}/schematic.pdf"
    MERVIN_FORM_PDF="${MERVIN_FIXTURES_DIR}/form_comment.pdf"
    MERVIN_HOUSE_PDF="${MERVIN_FIXTURES_DIR}/house-drawing.pdf"
    MERVIN_IMAGES_PDF="${MERVIN_FIXTURES_DIR}/images.pdf")

# tst_mupdf_store checks the patches in scripts/mupdf-patches through MuPDF's store
# API, with its own allocator and lock callbacks, so it needs MuPDF's headers too.
target_link_libraries(tst_mupdf_store PRIVATE MuPDF::MuPDF)
# Inspect the synchronous render context after an error has been returned.
target_link_libraries(tst_render_lifetime PRIVATE MuPDF::MuPDF)

# tst_view_layout builds its own /MediaBox-only fixtures, with exact integer page
# sizes so the layout arithmetic can be checked by hand.

# tst_tessdata validates generated model containers without a network dependency.
# Real OCR uses a pinned model downloaded only for tests, never for packaging.
# MERVIN_TEST_TESSDATA_DIR can override the default at runtime.
target_compile_definitions(tst_ocr PRIVATE
    MERVIN_OCR_PDF="${MERVIN_FIXTURES_DIR}/images.pdf"
    MERVIN_TEST_TESSDATA_DIR="${CMAKE_BINARY_DIR}/test-tessdata")

# tst_measure_export builds fixture PDFs and inspects flatten/annotate output
# with qpdf, so it links it explicitly too. Its data-gated case round-trips the
# shipped sample drawing through MuPDF (QSKIPs if the file is absent).
target_link_libraries(tst_measure_export PRIVATE qpdf::libqpdf)
target_compile_definitions(tst_measure_export PRIVATE
    MERVIN_DRAWING_PDF="${MERVIN_FIXTURES_DIR}/drawing.pdf")

# Measurement metadata and snapping use a generated CAD-style drawing.
target_compile_definitions(tst_measure PRIVATE
    MERVIN_DRAWING_PDF="${MERVIN_FIXTURES_DIR}/drawing.pdf")

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
    MERVIN_HOUSE_PDF="${MERVIN_FIXTURES_DIR}/house-drawing.pdf"
    # The document-open case times every sample in the directory: open cost is what
    # a multi-document start is made of, and whole-process timings cannot resolve it.
    MERVIN_EXAMPLES_DIR="${MERVIN_FIXTURES_DIR}")
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
    MERVIN_EXAMPLES_DIR="${MERVIN_FIXTURES_DIR}")
add_dependencies(tst_perf_startup MervinPDF)

foreach(_t tst_document_output tst_render_lifetime tst_content_search tst_atomic_pdf_writer
        tst_job_context)
    target_compile_definitions(${_t} PRIVATE MERVIN_FIXTURE_PDF="${MERVIN_FIXTURES_DIR}/properties.pdf")
endforeach()
target_compile_definitions(tst_document_output PRIVATE MERVIN_DRAWING_PDF="${MERVIN_FIXTURES_DIR}/drawing.pdf")

target_compile_definitions(tst_perf_layout PRIVATE MERVIN_LONG_PDF="${MERVIN_FIXTURES_DIR}/long.pdf")

# WindowManager prunes the recent history on a worker thread; it lives in mervin_app.
qt_add_executable(tst_recent_prune tst_recent_prune.cpp)
target_link_libraries(tst_recent_prune PRIVATE mervin_app Qt6::Test)
set_target_properties(tst_recent_prune PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_recent_prune COMMAND tst_recent_prune)
set_tests_properties(tst_recent_prune PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

qt_add_executable(tst_document_workflow tst_document_workflow.cpp)
target_link_libraries(tst_document_workflow PRIVATE mervin_app Qt6::Test)
target_compile_definitions(tst_document_workflow PRIVATE MERVIN_FIXTURE_PDF="${MERVIN_FIXTURES_DIR}/properties.pdf")
add_test(NAME tst_document_workflow COMMAND tst_document_workflow)
set_tests_properties(tst_document_workflow PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# Print dialog checks exercise worker-rendered previews and the actual PDF output path.
foreach(_t tst_print_dialog tst_print_output)
    qt_add_executable(${_t} ${_t}.cpp)
    target_link_libraries(${_t} PRIVATE mervin_app Qt6::Test)
    set_target_properties(${_t} PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME ${_t} COMMAND ${_t})
    set_tests_properties(${_t} PROPERTIES
        ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
        SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")
endforeach()

target_link_libraries(tst_print_output PRIVATE qpdf::libqpdf)

# The Recent page: Favourites and Recent sections, rows that move between them as
# soon as they are starred, and the Names, Contents and All search scopes (content hits are fed in
# by hand, as the window's ContentSearch would).
qt_add_executable(tst_recent_panel tst_recent_panel.cpp)
target_link_libraries(tst_recent_panel PRIVATE mervin_app Qt6::Test)
set_target_properties(tst_recent_panel PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_recent_panel COMMAND tst_recent_panel)
set_tests_properties(tst_recent_panel PROPERTIES ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# The UI language picker, the first-run window and the restart program.
qt_add_executable(tst_language_picker tst_language_picker.cpp)
target_link_libraries(tst_language_picker PRIVATE mervin_app Qt6::Test)
set_target_properties(tst_language_picker PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_language_picker COMMAND tst_language_picker)
set_tests_properties(tst_language_picker PROPERTIES ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# Check real translated dialogs, plus positive and negative cases for the
# geometry checker. These run in the ordinary Linux and Windows CI test jobs.
qt_add_executable(tst_text_fit tst_text_fit.cpp TextFit.h)
target_link_libraries(tst_text_fit PRIVATE Qt6::Widgets Qt6::Test)
qt_add_executable(tst_translation_layout tst_translation_layout.cpp TextFit.h)
target_link_libraries(tst_translation_layout PRIVATE mervin_app Qt6::Test)
target_compile_definitions(tst_translation_layout PRIVATE
    MERVIN_FIXTURES_DIR="${MERVIN_FIXTURES_DIR}"
    MERVIN_TRANSLATIONS_DIR="${CMAKE_SOURCE_DIR}/i18n")
foreach(_t tst_text_fit)
    set_target_properties(${_t} PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME ${_t} COMMAND ${_t})
    set_tests_properties(${_t} PROPERTIES
        ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
        SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")
endforeach()

set_target_properties(tst_translation_layout PROPERTIES WIN32_EXECUTABLE OFF)
foreach(lang IN ITEMS ${QT_I18N_SOURCE_LANGUAGE} ${QT_I18N_TRANSLATED_LANGUAGES})
    add_test(NAME tst_translation_layout_${lang} COMMAND tst_translation_layout)
    set_tests_properties(tst_translation_layout_${lang} PROPERTIES
        ENVIRONMENT_MODIFICATION "${_mervin_test_env};MERVIN_TEST_LANGUAGE=set:${lang}"
        TIMEOUT 180
        SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")
endforeach()

# The main menu button and its menu. The test sets up two offscreen monitors itself.
qt_add_executable(tst_main_menu tst_main_menu.cpp)
target_link_libraries(tst_main_menu PRIVATE mervin_app Qt6::Test)
set_target_properties(tst_main_menu PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_main_menu COMMAND tst_main_menu)
set_tests_properties(tst_main_menu PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

qt_add_executable(tst_document_suspension tst_document_suspension.cpp Stalls.h)
target_link_libraries(tst_document_suspension PRIVATE mervin_app Qt6::Test)
set_target_properties(tst_document_suspension PROPERTIES WIN32_EXECUTABLE OFF)
target_compile_definitions(tst_document_suspension PRIVATE MERVIN_FIXTURE_PDF="${MERVIN_FIXTURES_DIR}/properties.pdf")
add_test(NAME tst_document_suspension COMMAND tst_document_suspension)
set_tests_properties(tst_document_suspension PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# Rotate Pages and Split as their tab's operation, with the wait bar, Stop waiting and
# a window that stays usable while one tab waits.
qt_add_executable(tst_tab_operations tst_tab_operations.cpp Stalls.h)
target_link_libraries(tst_tab_operations PRIVATE mervin_app Qt6::Test qpdf::libqpdf)
set_target_properties(tst_tab_operations PROPERTIES WIN32_EXECUTABLE OFF)
target_compile_definitions(tst_tab_operations PRIVATE MERVIN_FIXTURE_PDF="${MERVIN_FIXTURES_DIR}/properties.pdf")
add_test(NAME tst_tab_operations COMMAND tst_tab_operations)
set_tests_properties(tst_tab_operations PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# New-tab placeholders load on workers and keep their identity and history across
# cancellation, canonical deduplication, password attempts and tab moves.
qt_add_executable(tst_background_open tst_background_open.cpp TestWaits.h Stalls.h)
target_link_libraries(tst_background_open PRIVATE mervin_app Qt6::Test)
set_target_properties(tst_background_open PROPERTIES WIN32_EXECUTABLE OFF)
target_compile_definitions(tst_background_open PRIVATE
    MERVIN_FIXTURE_PDF="${MERVIN_FIXTURES_DIR}/properties.pdf"
    MERVIN_APP_EXE="$<TARGET_FILE:MervinPDF>")
add_dependencies(tst_background_open MervinPDF)
add_test(NAME tst_background_open COMMAND tst_background_open)
set_tests_properties(tst_background_open PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    TIMEOUT 180
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")
qt_add_executable(tst_close_flow tst_close_flow.cpp)
target_link_libraries(tst_close_flow PRIVATE mervin_app Qt6::Test)
set_target_properties(tst_close_flow PROPERTIES WIN32_EXECUTABLE OFF)
target_compile_definitions(tst_close_flow PRIVATE MERVIN_FIXTURE_PDF="${MERVIN_FIXTURES_DIR}/properties.pdf")
add_test(NAME tst_close_flow COMMAND tst_close_flow)
set_tests_properties(tst_close_flow PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")
qt_add_executable(tst_instance_handoff tst_instance_handoff.cpp)
target_link_libraries(tst_instance_handoff PRIVATE mervin_app Qt6::Test)
set_target_properties(tst_instance_handoff PROPERTIES WIN32_EXECUTABLE OFF)
target_compile_definitions(tst_instance_handoff PRIVATE
    MERVIN_APP_EXE="$<TARGET_FILE:MervinPDF>"
    MERVIN_FIXTURE_PDF="${MERVIN_FIXTURES_DIR}/properties.pdf")
add_dependencies(tst_instance_handoff MervinPDF)
add_test(NAME tst_instance_handoff COMMAND tst_instance_handoff)
set_tests_properties(tst_instance_handoff PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")
qt_add_executable(tst_close_restart tst_close_restart.cpp)
target_link_libraries(tst_close_restart PRIVATE mervin_app Qt6::Test)
set_target_properties(tst_close_restart PROPERTIES WIN32_EXECUTABLE OFF)
target_compile_definitions(tst_close_restart PRIVATE MERVIN_FIXTURE_PDF="${MERVIN_FIXTURES_DIR}/properties.pdf")
add_test(NAME tst_close_restart COMMAND tst_close_restart)
set_tests_properties(tst_close_restart PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")
qt_add_executable(tst_save_edits tst_save_edits.cpp Stalls.h)
# qpdf builds fixtures with a stored measurement blob and reads its raw bytes back.
target_link_libraries(tst_save_edits PRIVATE mervin_app Qt6::Test qpdf::libqpdf)
set_target_properties(tst_save_edits PROPERTIES WIN32_EXECUTABLE OFF)
target_compile_definitions(tst_save_edits PRIVATE MERVIN_FIXTURE_PDF="${MERVIN_FIXTURES_DIR}/properties.pdf")
add_test(NAME tst_save_edits COMMAND tst_save_edits)
set_tests_properties(tst_save_edits PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")
qt_add_executable(tst_background_unload tst_background_unload.cpp Stalls.h)
# MuPDF directly, to inspect the shared resource store.
target_link_libraries(tst_background_unload PRIVATE mervin_app Qt6::Test MuPDF::MuPDF)
set_target_properties(tst_background_unload PROPERTIES WIN32_EXECUTABLE OFF)
target_compile_definitions(tst_background_unload PRIVATE
    MERVIN_FIXTURE_PDF="${MERVIN_FIXTURES_DIR}/properties.pdf"
    MERVIN_SCAN_PDF="${MERVIN_FIXTURES_DIR}/scan.pdf")
add_test(NAME tst_background_unload COMMAND tst_background_unload)
set_tests_properties(tst_background_unload PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

qt_add_executable(tst_page_dialog_operations tst_page_dialog_operations.cpp TestWaits.h Stalls.h)
target_link_libraries(tst_page_dialog_operations PRIVATE mervin_app Qt6::Test)
set_target_properties(tst_page_dialog_operations PROPERTIES WIN32_EXECUTABLE OFF)
target_compile_definitions(tst_page_dialog_operations PRIVATE
    MERVIN_FIXTURE_PDF="${MERVIN_FIXTURES_DIR}/properties.pdf"
    MERVIN_UNREADABLE_PDF="${MERVIN_FIXTURES_DIR}/unreadable-measurements.pdf")
add_test(NAME tst_page_dialog_operations COMMAND tst_page_dialog_operations)
set_tests_properties(tst_page_dialog_operations PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    TIMEOUT 180
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

# Security reads and writes run as tab jobs and keep dialog choices after failure.
qt_add_executable(tst_security_workflow tst_security_workflow.cpp TestWaits.h Stalls.h)
target_link_libraries(tst_security_workflow PRIVATE mervin_app Qt6::Test)
set_target_properties(tst_security_workflow PROPERTIES WIN32_EXECUTABLE OFF)
target_compile_definitions(tst_security_workflow PRIVATE
    MERVIN_FIXTURE_PDF="${MERVIN_FIXTURES_DIR}/properties.pdf")
add_test(NAME tst_security_workflow COMMAND tst_security_workflow)
set_tests_properties(tst_security_workflow PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    TIMEOUT 180
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")

qt_add_executable(tst_session_end_workflow tst_session_end_workflow.cpp)
target_link_libraries(tst_session_end_workflow PRIVATE mervin_app Qt6::Test)
target_compile_definitions(tst_session_end_workflow PRIVATE MERVIN_FIXTURE_PDF="${MERVIN_FIXTURES_DIR}/properties.pdf")
set_target_properties(tst_session_end_workflow PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME tst_session_end_workflow COMMAND tst_session_end_workflow)
set_tests_properties(tst_session_end_workflow PROPERTIES
    ENVIRONMENT_MODIFICATION "${_mervin_test_env}"
    SKIP_REGULAR_EXPRESSION "${_mervin_skip_regex}")
if(WIN32)
    set_tests_properties(tst_session_end_workflow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
endif()

set_tests_properties(tst_ink_rects PROPERTIES LABELS "optional-corpus")

# MuPDF's bundled libjpeg, FreeType, zlib and Brotli must stay out of the dynamic
# symbol table, or Qt's JPEG plugin, libqpdf and Qt's font code call MuPDF's
# copies (see cmake/FindMuPDF.cmake). Check the application, a test that links
# MuPDF directly and two that link qpdf directly. The release jobs run the same
# script on their builds.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME linux_symbol_exports
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/scripts/check-linux-exports.py"
            --nm "${CMAKE_NM}"
            "$<TARGET_FILE:MervinPDF>" "$<TARGET_FILE:tst_document>"
            "$<TARGET_FILE:tst_qpdf>" "$<TARGET_FILE:tst_print_output>")
endif()

target_compile_definitions(tst_document_output PRIVATE MERVIN_SCAN_PDF="${MERVIN_FIXTURES_DIR}/scan.pdf")
target_compile_definitions(tst_document_output PRIVATE MERVIN_INHERITED_PDF="${MERVIN_FIXTURES_DIR}/inherited-resources.pdf")

# Explicit local corpus profiling; never downloads files or skips required CI tests.
qt_add_executable(mervin_benchmark benchmark_corpus.cpp)
target_link_libraries(mervin_benchmark PRIVATE mervin_core)
set_target_properties(mervin_benchmark PROPERTIES WIN32_EXECUTABLE OFF)

# Checks that lupdate reads every string in the app's sources without a warning,
# that i18n/*.ts match what update_translations writes, and that every message
# is finished (see docs/TRANSLATING.md). The script runs the same lupdate project
# as update_translations, on copies in the build tree.
get_property(_mervin_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
set(_mervin_lupdate_project "${CMAKE_BINARY_DIR}/.lupdate/${PROJECT_NAME}_lupdate_project")
if(_mervin_multi_config)
    string(APPEND _mervin_lupdate_project ".$<CONFIG>")
endif()
list(JOIN MERVIN_LUPDATE_OPTIONS " " _mervin_lupdate_options)
add_test(NAME i18n_catalogs
    COMMAND ${CMAKE_COMMAND}
        "-DLUPDATE=$<TARGET_FILE:Qt6::lupdate>"
        "-DLCONVERT=$<TARGET_FILE:Qt6::lconvert>"
        "-DGENERATE_PROJECT=${Qt6LinguistTools_DIR}/GenerateLUpdateProject.cmake"
        "-DPROJECT_FILE=${_mervin_lupdate_project}.cmake"
        "-DCATALOG_DIR=${CMAKE_SOURCE_DIR}/i18n"
        "-DSOURCE_LANGUAGE=${QT_I18N_SOURCE_LANGUAGE}"
        "-DLUPDATE_OPTIONS=${_mervin_lupdate_options}"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/i18n_catalogs"
        -P "${CMAKE_SOURCE_DIR}/cmake/CheckTranslations.cmake")
set_tests_properties(i18n_catalogs PROPERTIES ENVIRONMENT_MODIFICATION "${_mervin_test_env}")
add_test(NAME i18n_content
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/scripts/check-translation-content.py"
            "${CMAKE_SOURCE_DIR}/i18n")
add_test(NAME tst_translation_content
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/scripts/test_translation_content.py")

# File dialogs persist Qt history independently of Mervin's isolated profile.
# Give every Linux test its own configuration folder, including parallel runs.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    get_property(_mervin_registered_tests DIRECTORY PROPERTY TESTS)
    foreach(_t IN LISTS _mervin_registered_tests)
        set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT_MODIFICATION
            "XDG_CONFIG_HOME=set:${CMAKE_CURRENT_BINARY_DIR}/test-config/${_t}")
    endforeach()
endif()
