; PowerOff installer (NSIS 3). Per-user install, no admin rights needed.
;   makensis /DVERSION=1.0.0 installer\poweroff.nsi   ->  PowerOff-setup.exe
Unicode true
!define MUI_ICON "..\poweroff.ico"
!define MUI_UNICON "..\poweroff.ico"
SetCompressor /SOLID lzma
RequestExecutionLevel user

!ifndef VERSION
  !define VERSION "1.0.1"
!endif
!define APP "PowerOff"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}"

Name "${APP}"
OutFile "..\PowerOff-setup.exe"
InstallDir "$LOCALAPPDATA\Programs\${APP}"
InstallDirRegKey HKCU "${UNINST_KEY}" "InstallLocation"
BrandingText "${APP} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "FileDescription" "${APP} setup"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" ""

!include "MUI2.nsh"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\poweroff.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Open ${APP}"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; A running copy holds the exe open; ask it to go away (tray copy included).
!macro CLOSE_RUNNING
  nsExec::Exec 'taskkill /IM poweroff.exe /F'
  Pop $0
  Sleep 300
!macroend

Section "Install"
  SetShellVarContext current
  !insertmacro CLOSE_RUNNING
  SetOutPath "$INSTDIR"
  File "..\poweroff.exe"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortcut "$SMPROGRAMS\${APP}.lnk" "$INSTDIR\poweroff.exe" "" "$INSTDIR\poweroff.exe" 0 \
    SW_SHOWNORMAL "" "Shut down, restart or sleep the PC on a schedule"

  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "Denis Platonov"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\poweroff.exe"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" 100
SectionEnd

Section "Uninstall"
  SetShellVarContext current
  !insertmacro CLOSE_RUNNING
  ; scheduled jobs PowerOff created (GUI hand-off and --install-daily)
  nsExec::Exec 'schtasks /delete /tn "PowerOff" /f'
  Pop $0
  nsExec::Exec 'schtasks /delete /tn "PowerOff daily" /f'
  Pop $0
  nsExec::Exec 'schtasks /delete /tn "PowerOff wake" /f'
  Pop $0
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "PowerOff"

  Delete "$SMPROGRAMS\${APP}.lnk"
  Delete "$INSTDIR\poweroff.exe"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  RMDir /r "$APPDATA\PowerOff"   ; settings (poweroff.ini)
  DeleteRegKey HKCU "${UNINST_KEY}"
SectionEnd
