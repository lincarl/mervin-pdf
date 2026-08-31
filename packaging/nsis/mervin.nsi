; Mervin PDF - per-user NSIS installer.
;
; Per-user install to %LOCALAPPDATA% so there is no admin prompt (the path also
; stays writable for a future self-updater). All registration is under HKCU.
;
; Required defines (passed by scripts/deploy.ps1):
;   /DDEPLOY_DIR=<dir>   staged application tree (exe + Qt/qpdf DLLs)
;   /DVERSION=<x.y.z>    version string
; Optional:
;   /DTESSDATA=<file>    eng.traineddata to seed the per-user tessdata folder
;   /DOUTFILE=<path>     output installer path

Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"

!define APPNAME "Mervin PDF"
!define EXENAME "MervinPDF.exe"
; Per-user data folder name. NOTE: this is NOT "${APPNAME}" - the app stores its
; config/recent/tessdata under %APPDATA%\MervinPDF (no space; see
; ConfigPaths::configDir), so the OCR language must be seeded there to be found.
!define DATANAME "MervinPDF"
!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef DEPLOY_DIR
  !error "DEPLOY_DIR must be defined (the staged application tree)"
!endif
!ifndef OUTFILE
  !define OUTFILE "MervinPDF-Setup-${VERSION}.exe"
!endif

!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"

Name "${APPNAME}"
OutFile "${OUTFILE}"
RequestExecutionLevel user            ; per-user: no UAC
InstallDir "$LOCALAPPDATA\${APPNAME}"
InstallDirRegKey HKCU "Software\${APPNAME}" "InstallDir"
SetCompressor /SOLID lzma

; Installer + uninstaller icon (the Setup .exe icon and the wizard's title icon).
; Resolve relative to this script's own directory so it works regardless of the
; makensis working directory. Shares the app's icon: resources/icons/mervin.ico.
!define MUI_ICON   "${__FILEDIR__}\..\..\resources\icons\mervin.ico"
!define MUI_UNICON "${__FILEDIR__}\..\..\resources\icons\mervin.ico"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXENAME}"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Install"
  ; A previously installed copy that is still running locks MervinPDF.exe, so the
  ; "File /r" below would abort with "Error opening file for writing". Close it
  ; first: detect a running instance, let the user save and close it, and force
  ; it closed on Retry if anything still holds the file.
  mervin_check_running:
    nsExec::ExecToStack 'cmd /c tasklist /NH /FI "IMAGENAME eq ${EXENAME}" | find /I "${EXENAME}"'
    Pop $0   ; find exits 0 when the image name is present, 1 when it is not
    Pop $1
    ${If} $0 == 0
      ; /SD IDRETRY: under a silent (in-app auto-update) install there is no one to
      ; click, so default to Retry - force-close the running copy and proceed.
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Mervin PDF is currently running and must be closed to continue.$\n$\nPlease save your work and close it, then click Retry.$\n$\nRetry will force it closed if it is still open." /SD IDRETRY IDCANCEL mervin_abort
      nsExec::Exec 'taskkill /F /IM "${EXENAME}"'
      Sleep 800
      Goto mervin_check_running
    ${EndIf}
  Goto mervin_proceed
  mervin_abort:
    Abort "Installation cancelled - Mervin PDF is still running."
  mervin_proceed:

  SetOutPath "$INSTDIR"
  File /r "${DEPLOY_DIR}\*.*"

  ; Seed the per-user OCR language folder (the app reads %APPDATA%\MervinPDF\tessdata).
  !ifdef TESSDATA
    SetOutPath "$APPDATA\${DATANAME}\tessdata"
    File "${TESSDATA}"
    SetOutPath "$INSTDIR"
  !endif

  CreateShortcut "$SMPROGRAMS\${APPNAME}.lnk" "$INSTDIR\${EXENAME}"

  ; Apps & Features entry (per-user) + uninstaller.
  WriteRegStr   HKCU "Software\${APPNAME}" "InstallDir" "$INSTDIR"
  WriteRegStr   HKCU "${UNINST_KEY}" "DisplayName"     "${APPNAME}"
  WriteRegStr   HKCU "${UNINST_KEY}" "DisplayVersion"  "${VERSION}"
  WriteRegStr   HKCU "${UNINST_KEY}" "Publisher"       "Mervin"
  WriteRegStr   HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKCU "${UNINST_KEY}" "DisplayIcon"     "$INSTDIR\${EXENAME}"
  WriteRegStr   HKCU "${UNINST_KEY}" "UninstallString" "$INSTDIR\uninstall.exe"
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; A silent (in-app auto-update) install shows no Finish page, so relaunch the
  ; freshly installed app ourselves to complete the updater's restart. Interactive
  ; installs skip this - MUI_FINISHPAGE_RUN offers the relaunch there instead.
  IfSilent 0 +2
    Exec '"$INSTDIR\${EXENAME}"'
SectionEnd

Section "Uninstall"
  ; Stop the app if running so its files are not locked.
  ExecWait 'taskkill /IM ${EXENAME} /F'

  Delete "$SMPROGRAMS\${APPNAME}.lnk"

  ; Remove HKCU registration written by the app's Windows integration.
  DeleteRegValue HKCU "Software\RegisteredApplications" "MervinPDF"
  DeleteRegKey   HKCU "Software\MervinPDF"
  DeleteRegKey   HKCU "Software\Classes\MervinPDF.Document"
  DeleteRegValue HKCU "Software\Classes\.pdf\OpenWithProgids" "MervinPDF.Document"
  DeleteRegKey   HKCU "${UNINST_KEY}"
  DeleteRegKey   HKCU "Software\${APPNAME}"

  RMDir /r "$INSTDIR"
  ; Note: user data in %APPDATA%\MervinPDF (config, recent, tessdata) is left intact.
SectionEnd
