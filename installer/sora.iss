; Sora installer (Inno Setup 6). Per-user, no admin: %LOCALAPPDATA%\Programs\Sora.
; Ships the whole self-contained app folder made by installer\assemble_app.ps1: the window, the engine (CPU, Intel
; GPUs through OpenVINO, any DirectX 12 discrete GPU through DirectML), Python, Node and the renderer with its
; headless Chrome - nothing has to be installed by hand.
;   build.bat && build.bat gui && powershell -File installer\assemble_app.ps1
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\sora.iss
#define AppVer "0.3.0"
#define AppDir GetEnv("USERPROFILE") + "\music-native\app"
#define Downloads GetEnv("USERPROFILE") + "\music-native\downloads"

[Setup]
AppId={{8C3E5A27-1F64-4D9B-B07A-6E2D94F1C358}
AppName=Sora
AppVersion={#AppVer}
AppVerName=Sora {#AppVer}
AppPublisher=Sora
DefaultDirName={localappdata}\Programs\Sora
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#AppDir}\..\dist
OutputBaseFilename=Sora-Setup-{#AppVer}
SetupIconFile=..\app\gui\sora.ico
WizardSmallImageFile=..\app\gui\sora_logo.png
UninstallDisplayIcon={app}\Sora.exe
UninstallDisplayName=Sora
Compression=lzma2/max
SolidCompression=yes
LZMANumBlockThreads=4
; the 32-bit compiler runs out of address space on 2 GB; compress in Inno's separate 64-bit process
LZMAUseSeparateProcess=yes
WizardStyle=modern
ChangesEnvironment=yes
CloseApplications=yes
VersionInfoVersion={#AppVer}
VersionInfoDescription=Sora setup

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked
Name: "addtopath"; Description: "Add the command-line engine (mtn) to my PATH"; Flags: unchecked

[Files]
Source: "{#AppDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; \
    Excludes: "\tool\renderer\public\edits\*,\tool\renderer\public\audio\*,\tool\output\*,__pycache__,*.pyc"
Source: "{#Downloads}\MicrosoftEdgeWebview2Setup.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall; Check: NeedsWebView2

[Icons]
Name: "{autoprograms}\Sora"; Filename: "{app}\Sora.exe"
Name: "{autodesktop}\Sora"; Filename: "{app}\Sora.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}\engine"; \
    Tasks: addtopath; Check: NeedsAddPath(ExpandConstant('{app}\engine'))

[Run]
; WebView2 (the Edge engine that draws the window) ships with Windows 11; older Windows 10 may lack it
Filename: "{tmp}\MicrosoftEdgeWebview2Setup.exe"; Parameters: "/silent /install"; StatusMsg: "Installing Microsoft Edge WebView2..."; \
    Check: NeedsWebView2
Filename: "{app}\Sora.exe"; Description: "Open Sora"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; working files, the SSD chunk cache, compiled GPU kernels, the window's browser data, Python byte-code
Type: filesandordirs; Name: "{localappdata}\Sora"
Type: filesandordirs; Name: "{app}\tool\renderer\public\edits"
Type: filesandordirs; Name: "{app}\tool\renderer\public\audio"
Type: filesandordirs; Name: "{app}\tool\output"
Type: filesandordirs; Name: "{app}\python\Lib\site-packages\__pycache__"

[Code]
// the deepest file (inside Remotion's headless Chrome) is ~160 characters below the install folder, and Windows
// paths stop at 260: keep the folder short
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and (Length(WizardDirValue) > 90) then
  begin
    MsgBox('Please choose a shorter folder (at most 90 characters). Sora contains deep folders, and Windows ' +
           'cannot install files whose full path is longer than 260 characters.', mbError, MB_OK);
    Result := False;
  end;
end;

function WebView2Version(Root: Integer; Key: string): string;
begin
  if not RegQueryStringValue(Root, Key, 'pv', Result) then Result := '';
end;

function NeedsWebView2: Boolean;
var
  V: string;
begin
  V := WebView2Version(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}');
  if (V = '') or (V = '0.0.0.0') then
    V := WebView2Version(HKCU, 'Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}');
  Result := (V = '') or (V = '0.0.0.0');
end;

function NeedsAddPath(Param: string): Boolean;
var
  Path: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Path) then
  begin
    Result := True;
    exit;
  end;
  Result := Pos(';' + Uppercase(Param) + ';', ';' + Uppercase(Path) + ';') = 0;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Path, Dir: string;
  P: Integer;
begin
  if CurUninstallStep <> usPostUninstall then exit;
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Path) then exit;
  Dir := ExpandConstant('{app}\engine');
  P := Pos(';' + Uppercase(Dir), Uppercase(Path));
  if P > 0 then
  begin
    Delete(Path, P, Length(Dir) + 1);
    RegWriteExpandStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Path);
  end;
end;
