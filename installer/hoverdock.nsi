; Hoverdock NSIS installer — per-user, no elevation.
;
; Build via CMake so VERSION / paths are injected:
;   cmake -S . -B out -G "Visual Studio 17 2022" -A x64
;   cmake --build out --config Release --target installer
;
; Or directly (defaults allow a plain `makensis installer\hoverdock.nsi`):
;   makensis /DVERSION=1.1.0 installer\hoverdock.nsi
;
; Silent auto-update path: the running dock downloads this Setup exe and runs
;   Hoverdock-Setup-<ver>.exe /S
; which closes the dock, replaces files, and relaunches the new build.

!ifndef VERSION
  !define VERSION "1.1.0"
!endif
!ifndef SRCBIN
  !define SRCBIN "..\out\bin\Release\Dock.exe"
!endif
!ifndef SRCDIR
  !define SRCDIR ".."
!endif
!ifndef OUTFILE
  !define OUTFILE "..\out\Hoverdock-Setup-${VERSION}.exe"
!endif

!define APPNAME "Hoverdock"
!define APPEXE "Dock.exe"
!define PUBLISHER "Hoverdock"
!define RUNVALUE "Hoverdock"
!define UNINSTALLKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Hoverdock"

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
Unicode True
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\Hoverdock"
InstallDirRegKey HKCU "Software\Hoverdock" "InstallDir"
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "LogicLib.nsh"

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APPEXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${APPNAME} now"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; --- Close a running dock so the exe can be replaced -----------------------
; The dock restores the native taskbar on WM_CLOSE / normal exit, so closing
; it here is also the taskbar failsafe. Falls back to taskkill for hangs.
; Duplicated for the uninstaller: NSIS uninstaller functions must live in the
; `un.` namespace and cannot Call installer functions.
!macro CloseDockBody
  Push $0
  Push $1
  FindWindow $0 "LiquidGlassDockWindow" ""
  ${If} $0 != 0
    SendMessage $0 16 0 0 ; WM_CLOSE
    IntOp $1 0 + 0
    loop_window:
      Sleep 200
      IntOp $1 $1 + 1
      FindWindow $0 "LiquidGlassDockWindow" ""
      ${If} $0 == 0
        Goto after_window
      ${EndIf}
      ${If} $1 >= 25 ; ~5 s grace, then force
        ExecWait '"$SYSDIR\taskkill.exe" /F /IM ${APPEXE}' $0
        Sleep 500
        Goto after_window
      ${EndIf}
      Goto loop_window
  ${EndIf}
  after_window:
  ; Window can go away while Dock.exe still maps the file. Always wait briefly
  ; then taskkill (no-op if already gone) so File can replace the binary
  ; before Version is written.
  Sleep 800
  ExecWait '"$SYSDIR\taskkill.exe" /F /IM ${APPEXE}' $0
  Sleep 400
  Pop $1
  Pop $0
!macroend

Function CloseRunningDock
  !insertmacro CloseDockBody
FunctionEnd

Function un.CloseRunningDock
  !insertmacro CloseDockBody
FunctionEnd

Section "Install"
  SetOutPath "$INSTDIR"
  Call CloseRunningDock
  ClearErrors
  File "${SRCBIN}"
  IfErrors 0 +3
    DetailPrint "Failed to replace ${APPEXE} (file in use?). Aborting without changing Version."
    Abort
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Install location + Add/Remove Programs entry (per-user hive, no admin).
  WriteRegStr HKCU "Software\Hoverdock" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\Hoverdock" "Version" "${VERSION}"
  WriteRegStr HKCU "${UNINSTALLKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKCU "${UNINSTALLKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTALLKEY}" "Publisher" "${PUBLISHER}"
  WriteRegStr HKCU "${UNINSTALLKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALLKEY}" "UninstallString" "$INSTDIR\Uninstall.exe"
  WriteRegDWORD HKCU "${UNINSTALLKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALLKEY}" "NoRepair" 1

  ; Start Menu shortcuts.
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\${APPEXE}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Uninstall.lnk" "$INSTDIR\Uninstall.exe"

  ; Desktop shortcut on interactive installs only (IfSilent = /S auto-update).
  IfSilent skipDesktop 0
    CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\${APPEXE}"
  skipDesktop:

  ; Launch-at-startup default: on for fresh installs so the dock survives
  ; reboot; the in-app Settings toggle owns the key afterwards. Silent
  ; re-installs (auto-update) never clobber an explicit user choice.
  ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "${RUNVALUE}"
  ${If} $0 == ""
    ; No prior choice: enable. An existing value (even for an older path) is
    ; left alone here; the dock repairs stale paths on next launch.
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "${RUNVALUE}" '"$INSTDIR\${APPEXE}"'
  ${EndIf}
SectionEnd

Section "Uninstall"
  Call un.CloseRunningDock
  Delete "$INSTDIR\${APPEXE}"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\Uninstall.lnk"
  RMDir "$SMPROGRAMS\${APPNAME}"
  Delete "$DESKTOP\${APPNAME}.lnk"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "${RUNVALUE}"
  DeleteRegKey HKCU "${UNINSTALLKEY}"
  DeleteRegKey HKCU "Software\Hoverdock"
SectionEnd

; Silent installs (auto-update) always relaunch so the new build opens.
Function .onInstSuccess
  IfSilent 0 +2
    Exec '"$INSTDIR\${APPEXE}"'
FunctionEnd
