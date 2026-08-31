cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.cmake)
project(MervinPDF VERSION ${MERVIN_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# ---- Dependencies ----------------------------------------------------------
# Network: QLocalServer/QLocalSocket (IPC + single-instance) live in QtNetwork.
# PrintSupport: QPrinter/QPrintDialog for Document -> Print.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network PrintSupport)
qt_standard_project_setup()
find_package(MuPDF REQUIRED)
# tomlplusplus is header-only. Prefer a system/vcpkg config package; fall back to
# fetching the single-header release so distros without libtomlplusplus-dev (e.g.
# Fedora) still configure cleanly.
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

option(MERVIN_BUILD_TESTS "Build the Qt Test unit tests" ON)

# ---- Generated version header ----------------------------------------------
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
# tree, not next to mervin.ico (forward slashes avoid RC backslash escapes).
if(WIN32)
    set(MERVIN_RC_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/mervin.ico)
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
    src/session/SessionStore.cpp
    src/session/SessionStore.h
    src/session/StartupPlan.cpp
    src/session/StartupPlan.h
    src/session/ClosedTabStack.cpp
    src/session/ClosedTabStack.h
    src/render/ViewLayout.cpp
    src/render/ViewLayout.h
    src/print/PageRange.cpp
    src/print/PageRange.h
    src/net/UrlOpen.cpp
    src/net/UrlOpen.h
    src/net/UrlDownloadLocation.h
    src/merge/MergePlan.cpp
    src/merge/MergePlan.h
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
    src/security/PageOps.cpp
    src/security/PageOps.h
    src/security/MeasureExport.cpp
    src/security/MeasureExport.h
    src/platform/PlatformIntegration.h
    src/platform/FileClipboard.cpp
    src/platform/FileClipboard.h
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

# ---- MervinPDF: the application --------------------------------------------
qt_add_executable(MervinPDF
    src/main.cpp
    src/app/WindowManager.cpp
    src/app/WindowManager.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/OpenPdfDialog.cpp
    src/ui/OpenPdfDialog.h
    src/ui/Theme.cpp
    src/ui/Theme.h
    src/ui/Icons.cpp
    src/ui/Icons.h
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
    src/ui/ViewerWidget.cpp
    src/ui/ViewerWidget.h
    src/ui/MeasureTypes.h
    src/ui/PanelStack.cpp
    src/ui/PanelStack.h
    src/ui/AnnotPanel.cpp
    src/ui/AnnotPanel.h
    src/ui/AnnotPopup.cpp
    src/ui/AnnotPopup.h
    src/ui/PdfPropertiesPopup.cpp
    src/ui/PdfPropertiesPopup.h
    src/dialogs/ExportMeasureDialog.cpp
    src/dialogs/ExportMeasureDialog.h
    src/ui/MeasurePanel.cpp
    src/ui/MeasurePanel.h
    src/ui/FindBar.cpp
    src/ui/FindBar.h
    src/ui/FileContextMenu.cpp
    src/ui/FileContextMenu.h
    src/dialogs/AboutDialog.cpp
    src/dialogs/AboutDialog.h
    src/dialogs/PrintDialog.cpp
    src/dialogs/PrintDialog.h
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
    src/update/UpdateChecker.cpp
    src/update/UpdateChecker.h
    src/update/ReleaseConfig.h
)
target_link_libraries(MervinPDF PRIVATE mervin_core Qt6::Widgets Qt6::Network Qt6::PrintSupport)

# Embed the application icon + version info (generated above) as a Win32
# resource so Explorer, the Start menu, and the "default app for .pdf" picker
# show the icon, and "Open with" / Default apps show the FileDescription name
# "Mervin PDF" instead of the bare filename. The running window/taskbar icon is
# set separately at runtime via QApplication::setWindowIcon. MSVC compiles the
# .rc automatically. Regenerate the .ico with resources/icons/make_icon.py.
if(WIN32)
    target_sources(MervinPDF PRIVATE ${CMAKE_BINARY_DIR}/generated/mervin.rc)
    # Dark-theme caption colouring (DwmSetWindowAttribute in MainWindow.cpp).
    target_link_libraries(MervinPDF PRIVATE comctl32 dwmapi ole32 shell32 uuid)
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

# ---- Tests -----------------------------------------------------------------
if(MERVIN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- Linux install rules + packaging (.deb / .rpm via CPack) ----------------
# The CI jobs build with -DMERVIN_WIN32_SUBSYSTEM unused here and run
# `cpack -G DEB` / `cpack -G RPM` (generator chosen at pack time). Desktop entry,
# icon, MIME, AppStream metainfo and the bundled OCR language all install under
# /usr so file managers, menus, and "Open with" pick Mervin up.
if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)

    install(TARGETS MervinPDF RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES packaging/linux/mervin-pdf.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES ${CMAKE_BINARY_DIR}/generated/mervin-pdf.metainfo.xml
            DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
    install(FILES resources/icons/mervin-256.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
            RENAME mervin-pdf.png)
    install(FILES LICENSE THIRD_PARTY_LICENSES.md
            DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/mervin-pdf)
    install(DIRECTORY licenses/
            DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/mervin-pdf/licenses)
    if(EXISTS ${CMAKE_SOURCE_DIR}/resources/tessdata/eng.traineddata)
        install(FILES resources/tessdata/eng.traineddata
                DESTINATION ${CMAKE_INSTALL_DATADIR}/mervin-pdf/tessdata)
    endif()

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
    set(CPACK_DEBIAN_PACKAGE_SECTION "graphics")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_SOURCE_DIR}/packaging/linux/postinst;${CMAKE_SOURCE_DIR}/packaging/linux/postrm")

    # ---- RPM (rpmdeps AUTOREQ auto-resolves the runtime deps) ----------------
    set(CPACK_RPM_PACKAGE_LICENSE "AGPL-3.0-only")
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
        "/usr/share/metainfo")

    include(CPack)
endif()
