#ifndef PayloadDir
  #error PayloadDir is required
#endif
#ifndef OutputDir
  #define OutputDir "..\..\artifacts"
#endif
[Setup]
AppId={{9B640453-B1CB-4653-A0B7-7DCCBA5F3D6E}
AppName=Aura Browser
AppVersion=0.2.0-preview.3
AppPublisher=Sttanors
AppPublisherURL=https://github.com/Sttanors/aura-browser
AppSupportURL=https://github.com/Sttanors/aura-browser/issues
DefaultDirName={localappdata}\Programs\Aura Browser
DefaultGroupName=Aura Browser
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=aura2-windows64-setup
Compression=lzma2/fast
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\..\resources\branding\aura.ico
UninstallDisplayIcon={app}\aura_browser.exe
CloseApplications=yes
RestartApplications=no
[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
[Icons]
Name: "{group}\Aura Browser"; Filename: "{app}\aura_browser.exe"; Parameters: """--data-dir={localappdata}\Aura Browser\profile"""; WorkingDir: "{app}"
Name: "{group}\{cm:UninstallProgram,Aura Browser}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Aura Browser"; Filename: "{app}\aura_browser.exe"; Parameters: """--data-dir={localappdata}\Aura Browser\profile"""; WorkingDir: "{app}"; Tasks: desktopicon
[Run]
Filename: "{app}\aura_browser.exe"; Parameters: """--data-dir={localappdata}\Aura Browser\profile"""; Description: "{cm:LaunchProgram,Aura Browser}"; Flags: nowait postinstall skipifsilent
; User profile is deliberately outside {app}; uninstall never removes it.
