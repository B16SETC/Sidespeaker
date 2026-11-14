#ifndef AppVersion
#define AppVersion "1.0.0"
#endif
#ifndef SourceDir
#define SourceDir "..\dist"
#endif

[Setup]
AppId={{F2CFBF33-4849-495E-999E-2AE1FDFD9522}
AppName=Sidespeaker
AppVersion={#AppVersion}
AppPublisher=Sidespeaker
DefaultDirName={autopf}\Sidespeaker
DefaultGroupName=Sidespeaker
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\sidespeaker.exe
OutputDir=..\installer-out
OutputBaseFilename=Sidespeaker-Setup

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked
Name: "startup"; Description: "Start Sidespeaker when I sign in to Windows"; Flags: unchecked

[Files]
Source: "{#SourceDir}\sidespeaker.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\config.ini"; DestDir: "{app}"; Flags: onlyifdoesntexist

[Icons]
Name: "{group}\Sidespeaker"; Filename: "{app}\sidespeaker.exe"; WorkingDir: "{app}"
Name: "{group}\Edit config"; Filename: "notepad.exe"; Parameters: """{app}\config.ini"""
Name: "{group}\Uninstall Sidespeaker"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Sidespeaker"; Filename: "{app}\sidespeaker.exe"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{userstartup}\Sidespeaker"; Filename: "{app}\sidespeaker.exe"; WorkingDir: "{app}"; Flags: runminimized; Tasks: startup

[Run]
Filename: "{app}\sidespeaker.exe"; Description: "Run Sidespeaker now"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[Code]
procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then
    WizardForm.FinishedLabel.Caption := WizardForm.FinishedLabel.Caption + #13#10#13#10 +
      'iPhone: needs Apple Devices (Microsoft Store) or iTunes.' + #13#10 +
      'Android: needs Android platform-tools (adb) and USB debugging.';
end;
