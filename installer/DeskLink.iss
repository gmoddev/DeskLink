#ifndef StagePath
  #error StagePath must identify the validated DeskLink staging directory.
#endif
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef VersionInfoVersion
  #define VersionInfoVersion "0.1.0.0"
#endif
#ifndef OutputPath
  #define OutputPath "."
#endif
#ifndef OutputName
  #define OutputName "DeskLink-unsigned"
#endif

[Setup]
AppId={{58944975-11A2-4DD6-B881-A0700574270F}
#ifdef ExperimentalWindows10
AppName=DeskLink Beta
AppVerName=DeskLink {#AppVersion} Beta 1 (Unsigned)
#else
AppName=DeskLink
AppVerName=DeskLink {#AppVersion}
#endif
AppVersion={#AppVersion}
AppPublisher=DeskLink
AppPublisherURL=https://github.com/gmoddev/DeskLink
AppSupportURL=https://github.com/gmoddev/DeskLink/issues
AppUpdatesURL=https://github.com/gmoddev/DeskLink/releases
#ifdef ExperimentalWindows10
AppComments=Unsigned DeskLink beta; Windows 10 OpenSSL/CNG remains experimental.
#else
AppComments=Secure local keyboard, mouse, audio, and clipboard roaming.
#endif
AppMutex=Local\DeskLink.Shell.v1,Local\DeskLink.Alpha.v1,Local\DeskLink.Runtime.v1,Local\DeskLink.RuntimeBroker.v1
SetupMutex=Local\DeskLink.Setup.v1
DefaultDirName={localappdata}\Programs\DeskLink
DefaultGroupName=DeskLink
DisableDirPage=yes
DisableProgramGroupPage=yes
UsePreviousAppDir=no
UsePreviousGroup=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupArchitecture=x64
#ifdef ExperimentalWindows10
MinVersion=10.0.19045
InfoBeforeFile={#StagePath}\WINDOWS10_BETA_NOTICE.md
#else
MinVersion=10.0.20348
#endif
AllowNetworkDrive=no
AllowRootDirectory=no
AllowUNCPath=no
CloseApplications=no
RestartApplications=no
RestartIfNeededByRun=no
Compression=lzma2/max
SolidCompression=yes
MergeDuplicateFiles=yes
OutputDir={#OutputPath}
OutputBaseFilename={#OutputName}
UninstallDisplayIcon={app}\desklink.exe
AppReadmeFile={app}\ALPHA_WRAPPER.md
LicenseFile={#StagePath}\ui\WindowsAppSDK-LICENSE.txt
VersionInfoCompany=DeskLink
VersionInfoDescription=DeskLink current-user installer
VersionInfoProductName=DeskLink
VersionInfoProductVersion={#VersionInfoVersion}
VersionInfoVersion={#VersionInfoVersion}
WizardStyle=modern dynamic
SetupLogging=yes
ASLRCompatible=yes
DEPCompatible=yes
RedirectionGuard=yes
#ifdef SignedBuild
SignTool=DeskLinkReleaseSign
SignedUninstaller=yes
#else
SignedUninstaller=no
#endif

[Files]
#ifdef SignedBuild
  Source: "{#StagePath}\ui\desklink.exe"; DestDir: "{app}"; Flags: ignoreversion signonce
  Source: "{#StagePath}\desklink_alpha.exe"; DestDir: "{app}"; Flags: ignoreversion signonce
  Source: "{#StagePath}\desklink_pair.exe"; DestDir: "{app}"; Flags: ignoreversion signonce
  Source: "{#StagePath}\desklink_runtime.exe"; DestDir: "{app}"; Flags: ignoreversion signonce
  Source: "{#StagePath}\desklink_update.exe"; DestDir: "{app}"; Flags: ignoreversion signonce
#else
  Source: "{#StagePath}\ui\desklink.exe"; DestDir: "{app}"; Flags: ignoreversion
  Source: "{#StagePath}\desklink_alpha.exe"; DestDir: "{app}"; Flags: ignoreversion
  Source: "{#StagePath}\desklink_pair.exe"; DestDir: "{app}"; Flags: ignoreversion
  Source: "{#StagePath}\desklink_runtime.exe"; DestDir: "{app}"; Flags: ignoreversion
  Source: "{#StagePath}\desklink_update.exe"; DestDir: "{app}"; Flags: ignoreversion
#endif
Source: "{#StagePath}\ui\*"; Excludes: "desklink.exe"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StagePath}\runtime\schannel\msquic.dll"; DestDir: "{app}\runtime\schannel"; Flags: ignoreversion
#ifdef ExperimentalWindows10
Source: "{#StagePath}\runtime\openssl\msquic.dll"; DestDir: "{app}\runtime\openssl"; Flags: ignoreversion
Source: "{#StagePath}\runtime\openssl\libcrypto-3-x64.dll"; DestDir: "{app}\runtime\openssl"; Flags: ignoreversion
Source: "{#StagePath}\runtime\openssl\libssl-3-x64.dll"; DestDir: "{app}\runtime\openssl"; Flags: ignoreversion
#endif
Source: "{#StagePath}\concrt140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StagePath}\msvcp140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StagePath}\msvcp140_1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StagePath}\msvcp140_2.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StagePath}\msvcp140_atomic_wait.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StagePath}\msvcp140_codecvt_ids.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StagePath}\vcruntime140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StagePath}\vcruntime140_1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StagePath}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StagePath}\OPUS-LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StagePath}\ALPHA_WRAPPER.md"; DestDir: "{app}"; Flags: ignoreversion
#ifdef ExperimentalWindows10
Source: "{#StagePath}\WINDOWS10_BETA_NOTICE.md"; DestDir: "{app}"; Flags: ignoreversion
#endif

[Icons]
Name: "{group}\DeskLink"; Filename: "{app}\desklink.exe"; WorkingDir: "{app}"; Comment: "Open DeskLink"
Name: "{group}\DeskLink diagnostics (Alpha)"; Filename: "{app}\desklink_alpha.exe"; WorkingDir: "{app}"; Comment: "Open the temporary engineering diagnostics shell"

[Run]
Filename: "{app}\desklink.exe"; Description: "Open DeskLink"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[Code]
const
  InstallerMutexName = 'Local\DeskLink.Install.v1';
  UpdateMutexName = 'Local\DeskLink.Update.v1';
  ApplicationMutexNames =
    'Local\DeskLink.Shell.v1,Local\DeskLink.Alpha.v1,Local\DeskLink.Runtime.v1,Local\DeskLink.RuntimeBroker.v1';

function HasExactCommandLineParameter(Value: String): Boolean;
var
  Index: Integer;
begin
  Result := False;
  for Index := 1 to ParamCount do
  begin
    if CompareText(ParamStr(Index), Value) = 0 then
    begin
      Result := True;
      exit;
    end;
  end;
end;

function AcquireInstallerGate(): Boolean;
var
  CoordinatedUpdate: Boolean;
  UpdateActive: Boolean;
begin
  Result := False;
  CoordinatedUpdate :=
    HasExactCommandLineParameter('/DESKLINKCOORDINATED');
  UpdateActive := CheckForMutexes(UpdateMutexName);
  if CoordinatedUpdate <> UpdateActive then
  begin
    SuppressibleMsgBox(
      'DeskLink Setup cannot overlap or impersonate a coordinated update.',
      mbError, MB_OK, IDOK);
    exit;
  end;
  if CheckForMutexes(InstallerMutexName) then
    exit;
  CreateMutex(InstallerMutexName);
  if CheckForMutexes(ApplicationMutexNames) then
  begin
    SuppressibleMsgBox(
      'DeskLink started while Setup was preparing. Close DeskLink and retry.',
      mbError, MB_OK, IDOK);
    exit;
  end;
  Result := True;
end;

function InitializeSetup(): Boolean;
begin
  Result := AcquireInstallerGate();
end;

function InitializeUninstall(): Boolean;
begin
  Result := AcquireInstallerGate();
end;

procedure MigrateLegacyStartupRegistration();
var
  CurrentValue: String;
  LegacyExecutable: String;
  ProductExecutable: String;
begin
  if not RegQueryStringValue(
      HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run',
      'DeskLink', CurrentValue) then
    exit;
  LegacyExecutable := '"' + ExpandConstant('{app}\desklink_alpha.exe') + '"';
  ProductExecutable := '"' + ExpandConstant('{app}\desklink.exe') +
    '" --background';
  if (CompareText(CurrentValue, LegacyExecutable) = 0) or
     (CompareText(CurrentValue, LegacyExecutable + ' --background') = 0) then
    RegWriteStringValue(
      HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run',
      'DeskLink', ProductExecutable);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    MigrateLegacyStartupRegistration();
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RegDeleteValue(HKCU,
      'Software\Microsoft\Windows\CurrentVersion\Run', 'DeskLink');
end;
