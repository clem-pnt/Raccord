; Raccord Windows installer (NSIS 3, Modern UI 2).
; makensis -DVERSION=0.1.0 -DSRCDIR=<deploy dir> -DOUTFILE=<setup.exe> installer.nsi
Unicode true
Target amd64-unicode
SetCompressor /SOLID lzma
!include "MUI2.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"

!define APPNAME "Raccord"
!define REGKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Raccord"

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APPNAME}"
InstallDirRegKey HKLM "Software\${APPNAME}" "InstallDir"
RequestExecutionLevel admin
BrandingText "${APPNAME} ${VERSION}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "FileDescription" "${APPNAME} installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Raccord contributors"

!define MUI_ICON "raccord.ico"
!define MUI_UNICON "raccord.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\raccord.exe"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "French"
!insertmacro MUI_LANGUAGE "English"

LangString SecMain ${LANG_FRENCH} "Raccord (requis)"
LangString SecMain ${LANG_ENGLISH} "Raccord (required)"
LangString SecDesktop ${LANG_FRENCH} "Raccourci sur le bureau"
LangString SecDesktop ${LANG_ENGLISH} "Desktop shortcut"
LangString SecAssoc ${LANG_FRENCH} "Ouvrir les projets .raccord avec Raccord"
LangString SecAssoc ${LANG_ENGLISH} "Open .raccord projects with Raccord"
LangString Need64 ${LANG_FRENCH} "Raccord nécessite Windows 64 bits."
LangString Need64 ${LANG_ENGLISH} "Raccord requires 64-bit Windows."
LangString ProjectType ${LANG_FRENCH} "Projet Raccord"
LangString ProjectType ${LANG_ENGLISH} "Raccord project"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "$(Need64)"
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Section "$(SecMain)" SEC_MAIN
  SectionIn RO
  ; Remove files of a previous version (keeps user data, which lives in AppData).
  RMDir /r "$INSTDIR\platforms"
  RMDir /r "$INSTDIR\python"
  SetOutPath "$INSTDIR"
  File /r "${SRCDIR}\*.*"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\raccord.exe"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "Software\${APPNAME}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${REGKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKLM "${REGKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${REGKEY}" "Publisher" "Raccord"
  WriteRegStr HKLM "${REGKEY}" "DisplayIcon" "$INSTDIR\raccord.exe"
  WriteRegStr HKLM "${REGKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${REGKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${REGKEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${REGKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${REGKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${REGKEY}" "EstimatedSize" "$0"
SectionEnd

Section "$(SecDesktop)" SEC_DESKTOP
  CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\raccord.exe"
SectionEnd

Section "$(SecAssoc)" SEC_ASSOC
  WriteRegStr HKLM "Software\Classes\.raccord" "" "Raccord.Project"
  WriteRegStr HKLM "Software\Classes\Raccord.Project" "" "$(ProjectType)"
  WriteRegStr HKLM "Software\Classes\Raccord.Project\DefaultIcon" "" "$INSTDIR\raccord.exe,0"
  WriteRegStr HKLM "Software\Classes\Raccord.Project\shell\open\command" "" '"$INSTDIR\raccord.exe" "%1"'
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\${APPNAME}.lnk"
  RMDir /r "$SMPROGRAMS\${APPNAME}"
  ReadRegStr $0 HKLM "Software\Classes\.raccord" ""
  ${If} $0 == "Raccord.Project"
    DeleteRegKey HKLM "Software\Classes\.raccord"
  ${EndIf}
  DeleteRegKey HKLM "Software\Classes\Raccord.Project"
  DeleteRegKey HKLM "${REGKEY}"
  DeleteRegKey HKLM "Software\${APPNAME}"
  RMDir /r "$INSTDIR"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd
