Unicode True
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "x64.nsh"

!ifndef STAGE_DIR
  !error "STAGE_DIR is required"
!endif
!ifndef OUTPUT_FILE
  !error "OUTPUT_FILE is required"
!endif
!ifndef BUILD_VERSION
  !define BUILD_VERSION "32.2.2-capture-fix"
!endif

!define PRODUCT_NAME "OBS Studio Capture Stutter Fix"
!define PRODUCT_PUBLISHER "veelasama"
!define PRODUCT_REGKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\OBSStudioCaptureStutterFix"

Name "${PRODUCT_NAME} ${BUILD_VERSION}"
OutFile "${OUTPUT_FILE}"
InstallDir "$PROGRAMFILES64\obs-studio-capture-stutter-fix"
InstallDirRegKey HKLM "${PRODUCT_REGKEY}" "InstallLocation"
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "32.2.2.1"
VIAddVersionKey /LANG=1033 "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${BUILD_VERSION}"
VIAddVersionKey /LANG=1033 "FileDescription" "OBS Studio capture frame pacing experimental fork"
VIAddVersionKey /LANG=1033 "LegalCopyright" "OBS contributors; GPL-2.0-or-later"

!define MUI_ABORTWARNING
!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\orange-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\orange-uninstall.ico"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Install" SEC_MAIN
  SetRegView 64
  SetOutPath "$INSTDIR"
  File /r "${STAGE_DIR}\*"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\OBS Studio Capture Stutter Fix"
  CreateShortcut "$SMPROGRAMS\OBS Studio Capture Stutter Fix\OBS Studio Capture Stutter Fix.lnk" "$INSTDIR\bin\64bit\obs64.exe" "" "$INSTDIR\bin\64bit\obs64.exe" 0 SW_SHOWNORMAL "" "$INSTDIR\bin\64bit"
  CreateShortcut "$SMPROGRAMS\OBS Studio Capture Stutter Fix\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\OBS Studio Capture Stutter Fix.lnk" "$INSTDIR\bin\64bit\obs64.exe" "" "$INSTDIR\bin\64bit\obs64.exe" 0 SW_SHOWNORMAL "" "$INSTDIR\bin\64bit"

  WriteRegStr HKLM "${PRODUCT_REGKEY}" "DisplayName" "${PRODUCT_NAME}"
  WriteRegStr HKLM "${PRODUCT_REGKEY}" "DisplayVersion" "${BUILD_VERSION}"
  WriteRegStr HKLM "${PRODUCT_REGKEY}" "Publisher" "${PRODUCT_PUBLISHER}"
  WriteRegStr HKLM "${PRODUCT_REGKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${PRODUCT_REGKEY}" "DisplayIcon" "$INSTDIR\bin\64bit\obs64.exe"
  WriteRegStr HKLM "${PRODUCT_REGKEY}" "UninstallString" '$"$INSTDIR\Uninstall.exe$"'
  WriteRegDWORD HKLM "${PRODUCT_REGKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${PRODUCT_REGKEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  SetRegView 64
  Delete "$DESKTOP\OBS Studio Capture Stutter Fix.lnk"
  RMDir /r "$SMPROGRAMS\OBS Studio Capture Stutter Fix"
  DeleteRegKey HKLM "${PRODUCT_REGKEY}"
  RMDir /r "$INSTDIR"
SectionEnd
