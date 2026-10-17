; Nada installer script for Inno Setup (https://jrsoftware.org/isinfo.php)
;
; 1. Build Nada.exe first (build_msvc.bat or build_mingw.sh).
; 2. Open this file in Inno Setup and press Ctrl+F9 (Build > Compile).
; 3. The installer appears in the "installer" folder as Nada-Setup-<version>.exe.

#define MyAppName "Nada"
#define MyAppVersion "1.1.0"
#define MyAppExeName "Nada.exe"

[Setup]
AppId={{B7C1E2A4-5D3F-4E8A-9A61-2F4D7C0E9B15}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=KingOfClaude
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#MyAppExeName}
SetupIconFile=nada.ico
LicenseFile=LICENSE
OutputDir=installer
OutputBaseFilename=Nada-Setup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequiredOverridesAllowed=dialog
MinVersion=10.0

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "Nada.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
