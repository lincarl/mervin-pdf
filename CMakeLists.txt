cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.cmake)
project(MervinPDF VERSION ${MERVIN_VERSION_BASE} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# ---- Dependencies ----------------------------------------------------------
# Network: QLocalServer/QLocalSocket (IPC + single-instance) live in QtNetwork.
# PrintSupport: QPrinter/QPrintDialog for Document -> Print.
# LinguistTools: lupdate/lrelease for the UI translations (see "Translations"
# below). Keep Qt 6.9 as the supported minimum for the translation toolchain.
find_package(Qt6 6.9 REQUIRED COMPONENTS Core Gui Widgets Network PrintSupport Svg LinguistTools)
# English is the source language; each translated language has a catalog
# i18n/mervin_<id>.ts. Qt widget catalogs are bundled separately below.
qt_standard_project_setup(
    I18N_SOURCE_LANGUAGE en
    I18N_TRANSLATED_LANGUAGES
        ar az be bg bs ca cnr cs da de el es et fi
        fr ga hi hr hu hy id is it ja ka kk ko lb
        lt lv mk mt nb nl nn pl pt_BR pt_PT rm ro ru sk
        sl sq sr sv th tr uk vi zh_CN zh_TW)
find_package(MuPDF REQUIRED)
# Prefer a system/vcpkg config package for tomlplusplus. The vcpkg port builds a
# compiled library. Fall back to the header-only CMake release so distros without
# libtomlplusplus-dev (e.g. Fedora) still configure cleanly.
find_package(tomlplusplus CONFIG QUIET)
if(NOT tomlplusplus_FOUND)
    include(FetchContent)
    FetchContent_Declare(tomlplusplus
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG 30172438cee64926dc41fdd9c11fb3ba5b2ba9de)
    FetchContent_MakeAvailable(tomlplusplus)
endif()
# Security and page operations. Requires qpdf >= 11 (setR*EncryptionParametersInsecure);
# NOT pinned via find_package because qpdf's config is SameMajorVersion-compatible, so a
# "11.0.0" request would reject qpdf 12.x (vcpkg / Ubuntu 26.04 / Fedora all ship 12).
find_package(qpdf CONFIG REQUIRED)
if(WIN32 AND VCPKG_TARGET_TRIPLET STREQUAL "x64-windows-static-md")
    # qpdf 12.4.2 exports bare pkg-config library names without their install
    # directories. Preserve its configuration expressions and other dependencies,
    # but resolve zlib and JPEG through their configuration-aware imported targets.
    find_package(ZLIB REQUIRED)
    find_package(JPEG REQUIRED)
    get_target_property(_mervin_qpdf_links qpdf::libqpdf INTERFACE_LINK_LIBRARIES)
    list(TRANSFORM _mervin_qpdf_links REPLACE
        "(^|>:)(zs|zsd)([.]lib)?(>|$)" "\\1ZLIB::ZLIB\\4")
    list(TRANSFORM _mervin_qpdf_links REPLACE
        "(^|>:)jpeg([.]lib)?(>|$)" "\\1JPEG::JPEG\\3")
    set_property(TARGET qpdf::libqpdf PROPERTY INTERFACE_LINK_LIBRARIES "${_mervin_qpdf_links}")
endif()

option(MERVIN_BUILD_TESTS "Build the Qt Test unit tests" ON)

# ---- Generated version header ----------------------------------------------
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/package-version.json.in
    ${CMAKE_BINARY_DIR}/generated/package-version.json
    @ONLY)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in
    ${CMAKE_BINARY_DIR}/generated/mervin_version.h
    @ONLY)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/mervin-pdf.metainfo.xml
    ${CMAKE_BINARY_DIR}/generated/mervin-pdf.metainfo.xml
    @ONLY)

# ---- Generated Win32 resource (icon + version info) ------------------------
# The version resource's FileDescription is the friendly name Windows shows in
# the "Open with" menu and Settings > Default apps. Configured here so the name
# and version stay sourced from cmake/Version.cmake. The icon is referenced by
# an absolute, forward-slashed path because the generated .rc lives in the build
# tree, not next to mervin-icon.ico (forward slashes avoid RC backslash escapes).
if(WIN32)
    set(MERVIN_RC_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/mervin-icon.ico)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/mervin.rc.in
        ${CMAKE_BINARY_DIR}/generated/mervin.rc
        @ONLY)
endif()

# ---- mervin_core: rendering + IPC/single-instance core (GUI-free, testable) -
# Holds all MuPDF usage AND the GUI-free IPC / single-instance core so the test
# targets can link it without pulling in the widgets executable.
add_library(mervin_core STATIC
    src/render/RenderTypes.h
    src/render/ComfortTransform.cpp
    src/render/ComfortTransform.h
    src/render/Document.cpp
    src/render/Document.h
    src/render/RenderEngine.cpp
    src/render/RenderEngine.h
    src/render/PageCache.cpp
    src/render/PageCache.h
    src/render/PreviewLayer.cpp
    src/render/PreviewLayer.h
    src/render/DocumentSearch.cpp
    src/render/DocumentSearch.h
    src/render/TextIndex.cpp
    src/render/TextIndex.h
    src/render/ContentSearch.cpp
    src/render/ContentSearch.h
    src/render/OcrService.cpp
    src/render/OcrService.h
    src/render/ThumbnailCache.h
    src/render/SelectionModel.h
    src/render/MeasureTypes.h
    src/render/MeasureMath.h
    src/render/MeasureModel.h
    src/render/MeasureContent.cpp
    src/render/MeasureContent.h
    src/render/FormTypes.h
    src/render/FormModel.cpp
    src/render/FormModel.h
    src/render/AnnotTypes.h
    src/render/AnnotModel.cpp
    src/render/AnnotModel.h
    src/ocr/TessdataManager.cpp
    src/ocr/TessdataManager.h
    src/ocr/TessdataFile.cpp
    src/ocr/TessdataFile.h
    src/ocr/OcrLanguageMapping.cpp
    src/ocr/OcrLanguageMapping.h
    src/ocr/OcrProvisioner.cpp
    src/ocr/OcrProvisioner.h
    src/session/SessionStore.cpp
    src/session/SessionStore.h
    src/session/StartupPlan.cpp
    src/session/StartupPlan.h
    src/session/ClosedTabStack.cpp
    src/session/ClosedTabStack.h
    src/render/ViewLayout.cpp
    src/render/ViewLayout.h
    src/render/ZoomAnimation.cpp
    src/render/ZoomAnimation.h
    src/print/PageRange.cpp
    src/print/PageRange.h
    src/print/PrintLayout.cpp
    src/print/PrintLayout.h
    src/net/UrlOpen.cpp
    src/net/UrlOpen.h
    src/net/UrlDownloadLocation.h
    src/merge/MergePlan.cpp
    src/merge/MergePlan.h
    src/merge/RowMoves.h
    src/extract/ExtractPlan.cpp
    src/extract/ExtractPlan.h
    src/config/ConfigPaths.cpp
    src/config/ConfigPaths.h
    src/config/Settings.cpp
    src/config/Settings.h
    src/recent/RecentEntry.h
    src/recent/ViewState.h
    src/recent/PathKey.h
    src/recent/RecentStore.cpp
    src/recent/RecentStore.h
    src/recent/ViewStateStore.cpp
    src/recent/ViewStateStore.h
    src/ipc/Message.cpp
    src/ipc/Message.h
    src/ipc/PipeName.cpp
    src/ipc/PipeName.h
    src/ipc/SingleInstanceServer.cpp
    src/ipc/SingleInstanceServer.h
    src/security/QpdfService.cpp
    src/security/QpdfService.h
    src/security/AtomicPdfWriter.cpp
    src/security/AtomicPdfWriter.h
    src/security/DocumentOutput.cpp
    src/security/DocumentOutput.h
    src/security/MeasurementPages.cpp
    src/security/MeasurementPages.h
    src/security/PageOps.cpp
    src/security/PageOps.h
    src/security/MeasureExport.cpp
    src/security/MeasureExport.h
    src/platform/PlatformIntegration.h
    src/platform/FileClipboard.cpp
    src/platform/FileClipboard.h
    src/i18n/UiLanguage.cpp
    src/i18n/UiLanguage.h
    # The colour vocabulary. Values only (QColor/QPalette, no widgets), so it
    # lives here rather than in the application target: the widget-level test
    # targets compile app sources that read it, and this way they get it from
    # mervin_core instead of each listing it separately.
    src/ui/ThemeTokens.cpp
    src/ui/ThemeTokens.h
)
set_target_properties(mervin_core PROPERTIES AUTOMOC ON)
target_include_directories(mervin_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(mervin_core
    PUBLIC Qt6::Core Qt6::Gui Qt6::Network
    PRIVATE MuPDF::MuPDF tomlplusplus::tomlplusplus qpdf::libqpdf)

# Default-PDF-app integration is platform-specific: Windows writes HKCU registry
# capabilities (WindowsIntegration.cpp); other platforms use xdg-mime
# (DesktopIntegration.cpp). Both define mervin::PlatformIntegration.
if(WIN32)
    target_sources(mervin_core PRIVATE src/platform/win/WindowsIntegration.cpp)
else()
    target_sources(mervin_core PRIVATE src/platform/linux/DesktopIntegration.cpp)
endif()

# ---- mervin_icons: the Lucide icon set ---------------------------------------
# Icons.cpp renders the vendored Lucide SVGs (resources/icons/lucide, ISC). It is
# a library of its own so the application and every test that paints icons link
# one copy of the renderer, and the Qt resource holding the SVGs travels with it.
add_library(mervin_icons STATIC
    src/ui/Icons.cpp
    src/ui/Icons.h
    src/ui/IconList.h)
target_include_directories(mervin_icons PUBLIC src)
target_link_libraries(mervin_icons PUBLIC Qt6::Gui PRIVATE Qt6::Svg Qt6::Widgets)
file(GLOB MERVIN_LUCIDE_SVGS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/lucide/*.svg")
qt_add_resources(mervin_icons "lucide_icons"
    PREFIX "/icons/lucide"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/lucide"
    FILES ${MERVIN_LUCIDE_SVGS})

# Use the approved PNG frames on every platform without an ICO image plugin.
file(GLOB MERVIN_APP_ICON_FRAMES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/mervin-icon/*.png")
qt_add_resources(mervin_icons "app_icon"
    PREFIX "/icons"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons"
    FILES resources/icons/mervin-icon.png ${MERVIN_APP_ICON_FRAMES})

# Shared viewer implementation for the application and widget regressions.
add_library(mervin_viewer STATIC
    src/ui/ViewerWidget.cpp
    src/ui/ViewerWidget.h
    src/ui/ViewerDocumentTools.cpp
    src/ui/ViewerMeasurements.cpp
    src/ui/AnnotPopup.cpp
    src/ui/AnnotPopup.h
    src/ui/PdfPropertiesPopup.cpp
    src/ui/PdfPropertiesPopup.h
)
target_link_libraries(mervin_viewer PUBLIC mervin_core mervin_icons Qt6::Widgets)

# ---- MervinPDF: the application --------------------------------------------
add_library(mervin_app STATIC
    src/app/Relaunch.cpp
    src/app/Relaunch.h
    src/app/WindowManager.cpp
    src/app/WindowManager.h
    src/dialogs/FirstRunDialog.cpp
    src/dialogs/FirstRunDialog.h
    src/ui/LanguageCombo.cpp
    src/ui/LanguageCombo.h
    src/ui/DocumentActions.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindowLifecycle.cpp
    src/ui/MainWindow.h
    src/ui/SidebarLayout.h
    src/ui/OpenPdfDialog.cpp
    src/ui/OpenPdfDialog.h
    src/ui/Theme.cpp
    src/ui/Theme.h
    src/ui/DocumentThemePicker.cpp
    src/ui/DocumentThemePicker.h
    src/ui/ThemeCard.cpp
    src/ui/ThemeCard.h
    src/ui/UiThemePicker.cpp
    src/ui/UiThemePicker.h
    src/ui/RecentFilesPanel.cpp
    src/ui/RecentFilesPanel.h
    src/ui/DetachableTabBar.cpp
    src/ui/DetachableTabBar.h
    src/ui/OutlineSidebar.cpp
    src/ui/OutlineSidebar.h
    src/ui/ThumbnailSidebar.cpp
    src/ui/ThumbnailSidebar.h
    src/ui/CommentsSidebar.cpp
    src/ui/CommentsSidebar.h
    src/ui/TabPage.cpp
    src/ui/TabPage.h
    src/ui/MeasureTypes.h
    src/ui/PanelStack.cpp
    src/ui/PanelStack.h
    src/ui/AnnotPanel.cpp
    src/ui/AnnotPanel.h
    src/dialogs/ExportMeasureDialog.cpp
    src/dialogs/ExportMeasureDialog.h
    src/ui/MeasurePanel.cpp
    src/ui/MeasurePanel.h
    src/ui/FindCard.cpp
    src/ui/FindCard.h
    src/ui/RecentSearchField.cpp
    src/ui/RecentSearchField.h
    src/ui/SearchLineEdit.cpp
    src/ui/SearchLineEdit.h
    src/ui/FileContextMenu.cpp
    src/ui/FileContextMenu.h
    src/dialogs/PrintDialog.cpp
    src/dialogs/PrintDialog.h
    src/print/PrintPreviewWidget.cpp
    src/print/PrintPreviewWidget.h
    src/dialogs/SettingsDialog.cpp
    src/dialogs/SettingsDialog.h
    src/dialogs/SecurityDialog.cpp
    src/dialogs/SecurityDialog.h
    src/dialogs/OcrPopup.cpp
    src/dialogs/OcrPopup.h
    src/dialogs/CalibrationDialog.cpp
    src/dialogs/CalibrationDialog.h
    src/dialogs/ManageLanguagesDialog.cpp
    src/dialogs/ManageLanguagesDialog.h
    src/dialogs/MergeDialog.cpp
    src/dialogs/MergeDialog.h
    src/dialogs/RowList.cpp
    src/dialogs/RowList.h
    src/dialogs/ExtractDialog.cpp
    src/dialogs/ExtractDialog.h
    src/dialogs/ExtractStrip.cpp
    src/dialogs/ExtractStrip.h
    src/update/Installer.cpp
    src/update/Installer.h
    src/update/ReleaseConfig.h
    src/update/UpdatePlan.cpp
    src/update/UpdatePlan.h
    src/update/Updater.cpp
    src/update/Updater.h
)
qt_add_executable(MervinPDF src/main.cpp)
target_link_libraries(MervinPDF PRIVATE mervin_app)
target_link_libraries(mervin_app PUBLIC mervin_viewer mervin_core mervin_icons Qt6::Widgets Qt6::Network
    Qt6::PrintSupport)

# The Linux package this build is for, which is how the in-app updater knows to
# install a new .deb or .rpm (Windows installs and AppImages are recognised at
# runtime instead). release.yml sets it for the package jobs; leave it empty for
# every other build so a dev build never tries to replace a system package.
set(MERVIN_PACKAGE_FORMAT "" CACHE STRING "Linux package this build is for: deb, rpm, or empty")
set_property(CACHE MERVIN_PACKAGE_FORMAT PROPERTY STRINGS "" deb rpm)
if(NOT MERVIN_PACKAGE_FORMAT MATCHES "^(|deb|rpm)$")
    message(FATAL_ERROR "MERVIN_PACKAGE_FORMAT must be deb, rpm, or empty (got '${MERVIN_PACKAGE_FORMAT}')")
endif()
target_compile_definitions(mervin_app PRIVATE MERVIN_PACKAGE_FORMAT="${MERVIN_PACKAGE_FORMAT}")

# Embed the application icon + version info (generated above) as a Win32
# resource so Explorer, the Start menu, and the "default app for .pdf" picker
# show the icon, and "Open with" / Default apps show the FileDescription name
# "Mervin PDF" instead of the bare filename. The running window/taskbar icon is
# set separately at runtime via QApplication::setWindowIcon. MSVC compiles the
# .rc automatically. The ICO and runtime PNG frames contain the same artwork.
if(WIN32)
    target_sources(MervinPDF PRIVATE ${CMAKE_BINARY_DIR}/generated/mervin.rc)
    # Dark-theme caption colouring (DwmSetWindowAttribute in MainWindow.cpp).
    target_link_libraries(mervin_app PUBLIC comctl32 dwmapi ole32 shell32 uuid)
endif()

# Build as a Windows GUI (no-console) app by DEFAULT so launching Mervin never
# pops up a stray terminal window behind it (a console-subsystem exe makes Windows
# allocate a console on every launch - from Explorer, the Start menu, or a file
# double-click). qDebug()/stderr is NOT lost in dev: main.cpp calls
# AttachConsole(ATTACH_PARENT_PROCESS) so output still appears when Mervin is run
# from an existing terminal. Set -DMERVIN_WIN32_SUBSYSTEM=OFF for a pure
# console-subsystem build. Windows-only; WIN32_EXECUTABLE is a no-op elsewhere.
option(MERVIN_WIN32_SUBSYSTEM "Build MervinPDF as a Windows GUI app (no console)" ON)
if(WIN32)
    set_target_properties(MervinPDF PROPERTIES WIN32_EXECUTABLE ${MERVIN_WIN32_SUBSYSTEM})
endif()

# ---- Translations ------------------------------------------------------------
# lupdate reads the app's sources into i18n/mervin_<id>.ts (run the
# update_translations target by hand; see docs/TRANSLATING.md). The build turns
# each .ts into a .qm and compiles it into mervin_core under :/i18n, so every
# package format and the tests carry the catalogs without install rules.
# mervin_en.ts holds only English plural forms ("%n pages"). Unfinished
# messages are left out, so they show in English instead of as drafts.
# Standard widget text comes from Qt where available. Missing upstream
# languages have maintained supplements for Qt's controls and dialogs.
if(QT_INTERNAL_ABSOLUTE_TRANSLATIONS_DIR)
    set(MERVIN_QT_TRANSLATIONS_DIR "${QT_INTERNAL_ABSOLUTE_TRANSLATIONS_DIR}")
else()
    set(MERVIN_QT_TRANSLATIONS_DIR "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_TRANSLATIONS}")
endif()
set(MERVIN_QT_CATALOGS "")
set(MERVIN_QT_SUPPLEMENTS "")
foreach(lang IN LISTS QT_I18N_TRANSLATED_LANGUAGES)
    set(qt_catalog "${MERVIN_QT_TRANSLATIONS_DIR}/qtbase_${lang}.qm")
    set(supplement "${CMAKE_CURRENT_SOURCE_DIR}/i18n/qt/qtbase_${lang}.ts")
    if(EXISTS "${qt_catalog}")
        list(APPEND MERVIN_QT_CATALOGS "${qt_catalog}")
    endif()
    if(EXISTS "${supplement}")
        list(APPEND MERVIN_QT_SUPPLEMENTS "${supplement}")
    elseif(NOT EXISTS "${qt_catalog}")
        message(FATAL_ERROR
            "No standard-widget translation for ${lang}. Install Qt's translation "
            "catalog qtbase_${lang}.qm, or provide the reviewed supplement "
            "${supplement}. See docs/TRANSLATING.md.")
    endif()
endforeach()
foreach(qt_catalog IN LISTS MERVIN_QT_CATALOGS)
    get_filename_component(qt_name "${qt_catalog}" NAME)
    set_source_files_properties("${qt_catalog}" PROPERTIES QT_RESOURCE_ALIAS "${qt_name}")
endforeach()
if(MERVIN_QT_SUPPLEMENTS)
    qt_add_lrelease(
        TS_FILES ${MERVIN_QT_SUPPLEMENTS}
        LRELEASE_TARGET mervin_qt_lrelease
        OPTIONS -nounfinished
        QM_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/i18n-qt"
        QM_FILES_OUTPUT_VARIABLE supplement_qms)
    # Retain upstream internal diagnostics while filling gaps in dialog text.
    foreach(qt_catalog IN LISTS supplement_qms)
        get_filename_component(qt_name "${qt_catalog}" NAME)
        string(REPLACE "qtbase_" "qtbase_supplement_" qt_name "${qt_name}")
        set_source_files_properties("${qt_catalog}" PROPERTIES QT_RESOURCE_ALIAS "${qt_name}")
    endforeach()
    list(APPEND MERVIN_QT_CATALOGS ${supplement_qms})
endif()
qt_add_resources(mervin_core mervin_qt_translations
    PREFIX "/i18n"
    FILES ${MERVIN_QT_CATALOGS})
# tests/CMakeLists.txt runs lupdate the same way for its catalog check.
set(MERVIN_LUPDATE_OPTIONS -locations none -no-obsolete)
qt_add_translations(
    TARGETS mervin_core
    SOURCE_TARGETS mervin_core mervin_icons mervin_viewer mervin_app MervinPDF
    TS_FILE_BASE mervin
    TS_FILE_DIR i18n
    LUPDATE_OPTIONS ${MERVIN_LUPDATE_OPTIONS}
    LRELEASE_OPTIONS -nounfinished)

# ---- Tests -----------------------------------------------------------------
if(MERVIN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- Linux install rules + packaging (.deb / .rpm via CPack) ----------------
# The CI jobs build with -DMERVIN_WIN32_SUBSYSTEM unused here and run
# `cpack -G DEB` / `cpack -G RPM` (generator chosen at pack time). Desktop entry,
# icon, MIME and AppStream metainfo all install under
# /usr so file managers, menus, and "Open with" pick Mervin up.
if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)

    install(TARGETS MervinPDF RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES packaging/linux/mervin-pdf.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES ${CMAKE_BINARY_DIR}/generated/mervin-pdf.metainfo.xml
            DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
    install(FILES resources/icons/mervin-icon.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
            RENAME mervin-pdf.png)
    # hicolor indexes these raster sizes. Other ICO sizes stay in Qt resources.
    set(MERVIN_LINUX_ICON_SIZES 16 24 32 36 48 64 72 96 128)
    foreach(icon_size IN LISTS MERVIN_LINUX_ICON_SIZES)
        install(FILES resources/icons/mervin-icon/${icon_size}.png
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${icon_size}x${icon_size}/apps
                RENAME mervin-pdf.png)
    endforeach()
    install(FILES resources/icons/mervin-icon.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
            RENAME mervin-pdf.svg)
    install(FILES LICENSE THIRD_PARTY_LICENSES.md
            DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/mervin-pdf)
    install(DIRECTORY licenses/
            DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/mervin-pdf/licenses)

    # ---- Shared package metadata (sourced from cmake/Version.cmake) ----------
    set(CPACK_PACKAGE_NAME "mervin-pdf")
    set(CPACK_PACKAGE_VERSION "${MERVIN_VERSION}")
    set(CPACK_PACKAGE_VENDOR "${MERVIN_ORG_NAME}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A lightweight, trustworthy PDF reader")
    set(CPACK_PACKAGE_CONTACT "Mervin PDF maintainers <289399892+lincarl@users.noreply.github.com>")
    set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/lincarl/mervin-pdf")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_STRIP_FILES ON)
    set(CPACK_PACKAGE_FILE_NAME "mervin-pdf_${MERVIN_VERSION}_amd64")

    # ---- DEB (dpkg-shlibdeps auto-resolves the Qt/qpdf/system deps) ----------
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
    set(CPACK_DEBIAN_PACKAGE_VERSION "${MERVIN_PACKAGE_VERSION}")
    set(CPACK_DEBIAN_PACKAGE_SECTION "graphics")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_SOURCE_DIR}/packaging/linux/postinst;${CMAKE_SOURCE_DIR}/packaging/linux/postrm")

    # ---- RPM (rpmdeps AUTOREQ auto-resolves the runtime deps) ----------------
    set(CPACK_RPM_PACKAGE_LICENSE "AGPL-3.0-only")
    set(CPACK_RPM_PACKAGE_VERSION "${MERVIN_PACKAGE_VERSION}")
    set(CPACK_RPM_PACKAGE_GROUP "Applications/Productivity")
    set(CPACK_RPM_PACKAGE_AUTOREQ ON)
    set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_RPM_FILE_NAME "mervin-pdf-${MERVIN_VERSION}.x86_64.rpm")
    set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${CMAKE_SOURCE_DIR}/packaging/linux/rpm-post.sh")
    set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE "${CMAKE_SOURCE_DIR}/packaging/linux/rpm-postun.sh")
    # Don't claim ownership of standard freedesktop dirs (avoids rpmbuild file
    # conflicts with filesystem/-provided directories).
    set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
        "/usr/share/applications" "/usr/share/icons" "/usr/share/icons/hicolor"
        "/usr/share/icons/hicolor/256x256" "/usr/share/icons/hicolor/256x256/apps"
        "/usr/share/icons/hicolor/scalable" "/usr/share/icons/hicolor/scalable/apps"
        "/usr/share/metainfo")
    foreach(icon_size IN LISTS MERVIN_LINUX_ICON_SIZES)
        list(APPEND CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
            "/usr/share/icons/hicolor/${icon_size}x${icon_size}"
            "/usr/share/icons/hicolor/${icon_size}x${icon_size}/apps")
    endforeach()

    include(CPack)
endif()
