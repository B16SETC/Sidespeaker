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

[Types]
Name: "full"; Description: "Full installation (iPhone and Android support)"
Name: "compact"; Description: "Sidespeaker only"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "app"; Description: "Sidespeaker"; Types: full compact custom; Flags: fixed
Name: "apple"; Description: "iPhone support: Apple Devices (installed from the Microsoft Store)"; Types: full
Name: "adb"; Description: "Android support: Android platform-tools with adb (downloaded from Google)"; Types: full

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked
Name: "startup"; Description: "Start Sidespeaker when I sign in to Windows"; Flags: unchecked

[Files]
Source: "{#SourceDir}\sidespeaker.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: app
Source: "{#SourceDir}\config.ini"; DestDir: "{app}"; Flags: onlyifdoesntexist; Components: app

[Icons]
Name: "{group}\Sidespeaker"; Filename: "{app}\sidespeaker.exe"; WorkingDir: "{app}"
Name: "{group}\Edit config"; Filename: "notepad.exe"; Parameters: """{app}\config.ini"""
Name: "{group}\Uninstall Sidespeaker"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Sidespeaker"; Filename: "{app}\sidespeaker.exe"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{userstartup}\Sidespeaker"; Filename: "{app}\sidespeaker.exe"; WorkingDir: "{app}"; Flags: runminimized; Tasks: startup

[UninstallDelete]
Type: filesandordirs; Name: "{app}\platform-tools"

[Run]
Filename: "{app}\sidespeaker.exe"; Description: "Run Sidespeaker now"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[Code]
const
  AdbUrl = 'https://dl.google.com/android/repository/platform-tools-latest-windows.zip';
  AdbZip = 'platform-tools.zip';
  AppleStoreId = '9NP83LWLPZ9K';
  WingetUpdateNotApplicable = -1978335189;
  WingetAlreadyInstalled = -1978335135;

var
  DownloadPage: TDownloadWizardPage;

function OnDownloadProgress(const Url, FileName: String; const Progress, ProgressMax: Int64): Boolean;
begin
  Result := True;
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing), SetupMessage(msgPreparingDesc), @OnDownloadProgress);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpReady) and WizardIsComponentSelected('adb') then
  begin
    DownloadPage.Clear;
    DownloadPage.Add(AdbUrl, AdbZip, '');
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
      except
        if not DownloadPage.AbortedByUser then
          SuppressibleMsgBox('Could not download Android platform-tools:' + #13#10 + GetExceptionMessage + #13#10#13#10 +
            'Check your internet connection, or untick the Android component.', mbCriticalError, MB_OK, IDOK);
        Result := False;
      end;
    finally
      DownloadPage.Hide;
    end;
  end;
end;

procedure InstallAdb;
var
  Rc: Integer;
  Zip, Dest, Args: String;
begin
  WizardForm.StatusLabel.Caption := 'Installing Android platform-tools...';
  Zip := ExpandConstant('{tmp}\') + AdbZip;
  Dest := ExpandConstant('{app}');
  Args := '-NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -LiteralPath ''' + Zip + ''' -DestinationPath ''' + Dest + ''' -Force"';
  if not Exec('powershell.exe', Args, '', SW_HIDE, ewWaitUntilTerminated, Rc) or (Rc <> 0) then
    SuppressibleMsgBox('Could not unpack Android platform-tools. Install them manually and set adb_path in config.ini.', mbError, MB_OK, IDOK);
end;

procedure InstallApple;
var
  Rc: Integer;
  Args: String;
begin
  WizardForm.StatusLabel.Caption := 'Installing Apple Devices (this can take a few minutes)...';
  Args := '/C winget install --id ' + AppleStoreId + ' --source msstore --silent --accept-package-agreements --accept-source-agreements';
  if not Exec(ExpandConstant('{cmd}'), Args, '', SW_HIDE, ewWaitUntilTerminated, Rc) or
     ((Rc <> 0) and (Rc <> WingetUpdateNotApplicable) and (Rc <> WingetAlreadyInstalled)) then
    SuppressibleMsgBox('Could not install Apple Devices automatically. Install "Apple Devices" from the Microsoft Store to use an iPhone.', mbError, MB_OK, IDOK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    if WizardIsComponentSelected('adb') then InstallAdb;
    if WizardIsComponentSelected('apple') then InstallApple;
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
var
  Notes: String;
begin
  if CurPageID = wpFinished then
  begin
    Notes := '';
    if not WizardIsComponentSelected('apple') then
      Notes := Notes + #13#10 + 'iPhone: needs Apple Devices (Microsoft Store) or iTunes.';
    if not WizardIsComponentSelected('adb') then
      Notes := Notes + #13#10 + 'Android: needs Android platform-tools (adb) and USB debugging.';
    if Notes <> '' then
      WizardForm.FinishedLabel.Caption := WizardForm.FinishedLabel.Caption + #13#10 + Notes;
  end;
end;
