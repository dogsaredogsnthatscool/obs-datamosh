; Windows installer for the Datamosh OBS filters.
; Build: ./build.sh installer   (needs makensis; VERSION passed with /DVERSION=x.y.z)
Unicode true
!ifndef VERSION
  !define VERSION "dev"
!endif
!define NAME "Datamosh OBS Filters"
!define UNKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\DatamoshOBS"

Name "${NAME} ${VERSION}"
OutFile "dist\datamosh-filter-setup.exe"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
InstallDir "$PROGRAMFILES64\obs-studio"
InstallDirRegKey HKLM "SOFTWARE\OBS Studio" ""
DirText "Setup will add the Datamosh, Point Cloud, Face Censor, Face Sticker, Laser Eyes and Rainbow Trail filters to OBS Studio. This should be the folder OBS is installed in (it contains bin and obs-plugins)."

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Function .onInit
  SetRegView 64
FunctionEnd

Function .onVerifyInstDir
  IfFileExists "$INSTDIR\bin\64bit\obs64.exe" +2
    Abort
FunctionEnd

!macro CloseOBS
  retry:
  nsExec::ExecToStack 'cmd /c tasklist /FI "IMAGENAME eq obs64.exe" | find /I "obs64.exe"'
  Pop $0
  StrCmp $0 "0" 0 done
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "OBS Studio is open. Close it, then click Retry." IDRETRY retry
    Abort
  done:
!macroend

Section "Install"
  !insertmacro CloseOBS
  SetOutPath "$INSTDIR\obs-plugins\64bit"
  File "dist\datamosh-filter.dll"
  ; old manual installs into the per-user folder would load twice
  Delete "$APPDATA\obs-studio\plugins\datamosh-filter\bin\64bit\datamosh-filter.dll"
  RMDir /r "$APPDATA\obs-studio\plugins\datamosh-filter"

  WriteUninstaller "$INSTDIR\obs-plugins\64bit\datamosh-filter-uninstall.exe"
  WriteRegStr HKLM "${UNKEY}" "DisplayName" "${NAME}"
  WriteRegStr HKLM "${UNKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNKEY}" "Publisher" "dogsaredogsnthatscool"
  WriteRegStr HKLM "${UNKEY}" "UninstallString" '"$INSTDIR\obs-plugins\64bit\datamosh-filter-uninstall.exe"'
  WriteRegDWORD HKLM "${UNKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNKEY}" "NoRepair" 1
SectionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

Section "Uninstall"
  !insertmacro CloseOBS
  Delete "$INSTDIR\datamosh-filter.dll"
  Delete "$INSTDIR\datamosh-filter-uninstall.exe"
  DeleteRegKey HKLM "${UNKEY}"
SectionEnd
