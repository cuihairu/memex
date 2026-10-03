; Memex Windows installer (Inno Setup 7；ISCC 6.x 亦可编译).
; Built by nightly workflow:
;   ISCC.exe /DAppVersion=<x.y.z> /DStageDir=<abs stage dir> /O<outdir> packaging\windows\memex.iss
; Output: memex-windows-x64-setup.exe (fixed asset name, install.ps1 direct link).
; Payload = windeployqt/vcpkg-staged client tree (GUI subsystem exe, no console).

#ifndef AppVersion
#define AppVersion "0.1.0"
#endif
#ifndef StageDir
#define StageDir "..\..\stage-client"
#endif

; AppId must NEVER change: Add/Remove Programs upgrade-in-place identity.
[Setup]
AppId={{7B6F2A31-9C4E-4B2D-8A15-3E2F0D6C9A47}
AppName=Memex
AppVersion={#AppVersion}
AppVerName=Memex {#AppVersion}
AppPublisher=cuihairu
AppPublisherURL=https://github.com/cuihairu/memex
DefaultDirName={autopf}\Memex
DefaultGroupName=Memex
DisableProgramGroupPage=yes
OutputBaseFilename=memex-windows-x64-setup
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
Compression=lzma
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Memex
UninstallDisplayIcon={app}\memex-client.exe

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"

[Icons]
Name: "{group}\Memex"; Filename: "{app}\memex-client.exe"
Name: "{group}\Uninstall Memex"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Memex"; Filename: "{app}\memex-client.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\memex-client.exe"; Description: "Launch Memex"; Flags: nowait postinstall skipifsilent
