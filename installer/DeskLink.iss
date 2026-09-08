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
#ifdef DevelopmentSecure
AppName=DeskLink Development Secure
AppVerName=DeskLink {#AppVersion} Development Secure (Self-Signed)
#else
#ifdef ExperimentalWindows10
AppName=DeskLink Beta
AppVerName=DeskLink {#AppVersion} Beta 1 (Unsigned)
#else
AppName=DeskLink
AppVerName=DeskLink {#AppVersion}
#endif
#endif
AppVersion={#AppVersion}
AppPublisher=DeskLink
AppPublisherURL=https://github.com/gmoddev/DeskLink
AppSupportURL=https://github.com/gmoddev/DeskLink/issues
AppUpdatesURL=https://github.com/gmoddev/DeskLink/releases
#ifdef DevelopmentSecure
AppComments=Self-signed DeskLink development build for explicitly trusted test PCs.
#else
#ifdef ExperimentalWindows10
AppComments=Unsigned DeskLink beta; Windows 10 OpenSSL/CNG remains experimental.
#else
AppComments=Secure local keyboard, mouse, audio, and clipboard roaming.
#endif
#endif
AppMutex=Local\DeskLink.Shell.v1,Local\DeskLink.Alpha.v1,Local\DeskLink.Runtime.v1,Local\DeskLink.RuntimeBroker.v1
SetupMutex=Local\DeskLink.Setup.v1
#ifdef DevelopmentSecure
DefaultDirName={autopf}\DeskLink Development Secure
#else
DefaultDirName={localappdata}\Programs\DeskLink
#endif
DefaultGroupName=DeskLink
DisableDirPage=yes
DisableProgramGroupPage=yes
UsePreviousAppDir=no
UsePreviousGroup=no
#ifdef DevelopmentSecure
PrivilegesRequired=admin
#else
PrivilegesRequired=lowest
#endif
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
#ifdef DevelopmentSecure
VersionInfoDescription=DeskLink Development Secure machine-wide installer
#else
VersionInfoDescription=DeskLink current-user installer
#endif
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
  Source: "{#StagePath}\desklink_virtual_microphone_installer.exe"; DestDir: "{app}"; Flags: ignoreversion signonce
#ifdef DevelopmentSecure
  Source: "{#StagePath}\desklink_secure_input_service.exe"; DestDir: "{app}"; Flags: ignoreversion signonce
  Source: "{#StagePath}\desklink_secure_input_helper.exe"; DestDir: "{app}"; Flags: ignoreversion signonce
  Source: "{#StagePath}\desklink_secure_input_configurator.exe"; DestDir: "{app}"; Flags: ignoreversion signonce
#endif
#else
  Source: "{#StagePath}\ui\desklink.exe"; DestDir: "{app}"; Flags: ignoreversion
  Source: "{#StagePath}\desklink_alpha.exe"; DestDir: "{app}"; Flags: ignoreversion
  Source: "{#StagePath}\desklink_pair.exe"; DestDir: "{app}"; Flags: ignoreversion
  Source: "{#StagePath}\desklink_runtime.exe"; DestDir: "{app}"; Flags: ignoreversion
  Source: "{#StagePath}\desklink_update.exe"; DestDir: "{app}"; Flags: ignoreversion
  Source: "{#StagePath}\desklink_virtual_microphone_installer.exe"; DestDir: "{app}"; Flags: ignoreversion
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
#ifdef DevelopmentSecure
Source: "{#StagePath}\ALPHA_WRAPPER.md"; DestDir: "{app}"; Flags: ignoreversion; AfterInstall: InstallSecureInputService
#else
Source: "{#StagePath}\ALPHA_WRAPPER.md"; DestDir: "{app}"; Flags: ignoreversion
#endif
#ifdef VirtualMicrophonePackage
Source: "{#StagePath}\driver\DeskLinkVirtualMicrophone\*"; DestDir: "{app}\driver\DeskLinkVirtualMicrophone"; Flags: ignoreversion
#endif
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

#ifdef DevelopmentSecure
function RunSecureInputServiceCommand(
  Parameters: String; AllowAlreadyStopped: Boolean): Boolean;
var
  ResultCode: Integer;
begin
  ResultCode := -1;
  Log('DeskLink secure-input service command: ' + Parameters);
  Result := Exec(
    ExpandConstant('{sys}\sc.exe'), Parameters, '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode);
  if Result and AllowAlreadyStopped and
     ((ResultCode = 1060) or (ResultCode = 1062)) then
    ResultCode := 0;
  Result := Result and (ResultCode = 0);
  Log('DeskLink secure-input service command exit: ' +
    IntToStr(ResultCode));
end;

function StopSecureInputService(): Boolean;
begin
  if not RegKeyExists(
      HKLM64, 'SYSTEM\CurrentControlSet\Services\DeskLinkSecureInput') then
  begin
    Result := True;
    exit;
  end;
  Result := RunSecureInputServiceCommand(
    'stop DeskLinkSecureInput', True);
  if Result then
    Sleep(1000);
end;

function ConfigureSecureInputService(): Boolean;
var
  ExecutablePath: String;
  ExpectedImagePath: String;
  InstalledImagePath: String;
  InstalledAccount: String;
  Parameters: String;
begin
  Result := False;
  ExecutablePath :=
    ExpandConstant('{app}\desklink_secure_input_service.exe');
  ExpectedImagePath := '"' + ExecutablePath + '"';
  if RegKeyExists(
      HKLM64, 'SYSTEM\CurrentControlSet\Services\DeskLinkSecureInput') then
    Parameters := 'config DeskLinkSecureInput binPath= \"' +
      ExecutablePath +
      '\" start= delayed-auto obj= LocalSystem DisplayName= "DeskLink Secure Input Broker"'
  else
    Parameters := 'create DeskLinkSecureInput binPath= \"' +
      ExecutablePath +
      '\" start= delayed-auto obj= LocalSystem DisplayName= "DeskLink Secure Input Broker"';
  if not RunSecureInputServiceCommand(Parameters, False) then
    exit;
  if not RunSecureInputServiceCommand(
      'description DeskLinkSecureInput "Authenticated, local-only broker for explicitly approved elevated input"',
      False) then
    exit;
  if not RunSecureInputServiceCommand(
      'sdset DeskLinkSecureInput D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)',
      False) then
    exit;
  if not RunSecureInputServiceCommand(
      'failure DeskLinkSecureInput reset= 86400 actions= restart/1000/restart/5000/""/0',
      False) then
    exit;
  if not RunSecureInputServiceCommand(
      'failureflag DeskLinkSecureInput 1', False) then
    exit;
  if not RegQueryStringValue(
      HKLM64, 'SYSTEM\CurrentControlSet\Services\DeskLinkSecureInput',
      'ImagePath', InstalledImagePath) or
     (CompareText(InstalledImagePath, ExpectedImagePath) <> 0) or
     not RegQueryStringValue(
       HKLM64, 'SYSTEM\CurrentControlSet\Services\DeskLinkSecureInput',
       'ObjectName', InstalledAccount) or
     (CompareText(InstalledAccount, 'LocalSystem') <> 0) then
    exit;
  Result := RunSecureInputServiceCommand(
    'start DeskLinkSecureInput', False);
end;

procedure InstallSecureInputService();
begin
  if ConfigureSecureInputService() then
    exit;
  RunSecureInputServiceCommand('stop DeskLinkSecureInput', True);
  RunSecureInputServiceCommand('delete DeskLinkSecureInput', True);
  RaiseException(
    'The DeskLink secure-input broker could not be installed safely. Setup is rolling back.');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not StopSecureInputService() then
    Result :=
      'DeskLink Setup could not stop the existing secure-input broker. No files were changed.';
end;
#endif

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
var
  ResultCode: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
#ifdef DevelopmentSecure
    if not StopSecureInputService() then
      Log('DeskLink secure-input broker did not acknowledge stop before removal.');
    if not RunSecureInputServiceCommand(
        'delete DeskLinkSecureInput', True) then
      Log('DeskLink secure-input broker service registration could not be removed.');
    RegDeleteKeyIncludingSubkeys(HKLM64, 'SOFTWARE\DeskLink\SecureInput');
#endif
    if RegKeyExists(
        HKLM64,
        'SYSTEM\CurrentControlSet\Services\DeskLinkVirtualMicrophone') then
    begin
      if not ShellExec(
          'runas',
          ExpandConstant('{app}\desklink_virtual_microphone_installer.exe'),
          'uninstall', ExpandConstant('{app}'), SW_SHOWNORMAL,
          ewWaitUntilTerminated, ResultCode) then
        Log('DeskLink Virtual Microphone removal was not started; application uninstall will continue.')
      else if (ResultCode <> 0) and (ResultCode <> 3010) then
        Log('DeskLink Virtual Microphone removal returned ' +
          IntToStr(ResultCode) +
          '; application uninstall will continue.');
    end;
    RegDeleteValue(HKCU,
      'Software\Microsoft\Windows\CurrentVersion\Run', 'DeskLink');
  end;
end;
