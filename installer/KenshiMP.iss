; Kenshi-Online installer (Inno Setup script)
;
; Builds a single KenshiMP-Setup.exe that:
;   - Auto-detects the Kenshi install directory (Steam library + GOG fallbacks).
;   - Refuses to install while kenshi_x64.exe is running.
;   - Installs the DLL, dedicated server, GUI layouts, mod files.
;   - Backs up Plugins_x64.cfg, __mods.list, and Kenshi_MainMenu.layout
;     before mutation, into <KenshiDir>\KenshiMP_backup\.
;   - Adds "Plugin=KenshiMP.Core" to Plugins_x64.cfg if missing.
;   - Adds "kenshi-online" to data\__mods.list if missing.
;   - Provides an Uninstaller that restores the backups it took.
;
; Build:
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DSourceDir=release\Kenshi-Online ^
;     /DAppVersion=0.1.0 installer\KenshiMP.iss
;
; The CI release.yml passes /DSourceDir and /DAppVersion at build time.

#ifndef SourceDir
  #define SourceDir "release\Kenshi-Online"
#endif
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif

[Setup]
AppId={{B5F7A2C1-4E3B-4D1A-9F8E-KENSHIMPSETUP}}
AppName=Kenshi-Online
AppVersion={#AppVersion}
AppPublisher=mattebin
AppPublisherURL=https://github.com/mattebin/Kenshi-Online
AppSupportURL=https://github.com/mattebin/Kenshi-Online/issues
DefaultDirName={code:DetectKenshiDir}
DefaultGroupName=Kenshi-Online
DisableProgramGroupPage=yes
DisableWelcomePage=no
DisableDirPage=no
UsePreviousAppDir=yes
AppendDefaultDirName=no
OutputDir=.
OutputBaseFilename=KenshiMP-Setup
Compression=lzma2/max
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern
DirExistsWarning=no
DiskSpanning=no
UninstallDisplayIcon={app}\KenshiMP.Injector.exe

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; Core binaries
Source: "{#SourceDir}\KenshiMP.Core.dll";       DestDir: "{app}";                                 Flags: ignoreversion
Source: "{#SourceDir}\KenshiMP.Server.exe";     DestDir: "{app}";                                 Flags: ignoreversion
Source: "{#SourceDir}\KenshiMP.Injector.exe";   DestDir: "{app}";                                 Flags: ignoreversion
Source: "{#SourceDir}\KenshiMP.MasterServer.exe"; DestDir: "{app}";                               Flags: ignoreversion skipifsourcedoesntexist

; GUI layouts
Source: "{#SourceDir}\Kenshi_MainMenu.layout";         DestDir: "{app}\data\gui\layout"; Flags: ignoreversion
Source: "{#SourceDir}\Kenshi_MultiplayerPanel.layout"; DestDir: "{app}\data\gui\layout"; Flags: ignoreversion
Source: "{#SourceDir}\Kenshi_MultiplayerHUD.layout";   DestDir: "{app}\data\gui\layout"; Flags: ignoreversion

; Mod (required — game crashes during character creation without it)
Source: "{#SourceDir}\kenshi-online.mod"; DestDir: "{app}\data";                  Flags: ignoreversion
Source: "{#SourceDir}\kenshi-online.mod"; DestDir: "{app}\mods\kenshi-online";    Flags: ignoreversion

; Server config template — only write if user doesn't already have one
Source: "{#SourceDir}\server.json"; DestDir: "{app}"; Flags: onlyifdoesntexist skipifsourcedoesntexist

; Bundled docs / scripts (optional — fall back to install.bat if user wants the script flow)
Source: "{#SourceDir}\JOINING.md";    DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\install.bat";   DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\uninstall.bat"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\Kenshi-Online (Multiplayer Launcher)"; Filename: "{app}\KenshiMP.Injector.exe"
Name: "{group}\Kenshi-Online Server";                 Filename: "{app}\KenshiMP.Server.exe"
Name: "{group}\Uninstall Kenshi-Online";              Filename: "{uninstallexe}"

[Run]
Filename: "{app}\KenshiMP.Injector.exe"; Description: "Launch Kenshi-Online now"; Flags: nowait postinstall skipifsilent unchecked

[Code]
const
  KenshiAppId    = '233860'; // Steam app id for Kenshi
  ModsListFile   = 'data\__mods.list';
  PluginsCfgFile = 'Plugins_x64.cfg';
  ModName        = 'kenshi-online';
  PluginLine     = 'Plugin=KenshiMP.Core';

// ---- Path helpers ----

function FileContains(const FileName, Needle: String): Boolean;
var
  Lines: TArrayOfString;
  i: Integer;
begin
  Result := False;
  if not LoadStringsFromFile(FileName, Lines) then Exit;
  for i := 0 to GetArrayLength(Lines) - 1 do
    if Trim(Lines[i]) = Needle then begin
      Result := True;
      Exit;
    end;
end;

procedure AppendLineUnique(const FileName, Line: String);
var
  Existing: AnsiString;
  Updated: AnsiString;
begin
  if FileExists(FileName) then begin
    if FileContains(FileName, Line) then Exit;
    if not LoadStringFromFile(FileName, Existing) then Exit;
    Updated := Existing;
    if (Length(Updated) > 0) and (Updated[Length(Updated)] <> #10) then
      Updated := Updated + #13#10;
    Updated := Updated + Line + #13#10;
    SaveStringToFile(FileName, Updated, False);
  end else begin
    SaveStringToFile(FileName, Line + #13#10, False);
  end;
end;

// ---- Steam library detection ----

function ReadSteamPath(): String;
var
  S: String;
begin
  Result := '';
  if RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', S) and (S <> '') then begin
    StringChangeEx(S, '/', '\', True);
    Result := S;
    Exit;
  end;
  if RegQueryStringValue(HKLM, 'Software\WOW6432Node\Valve\Steam', 'InstallPath', S) and (S <> '') then begin
    Result := S;
    Exit;
  end;
  if RegQueryStringValue(HKLM, 'Software\Valve\Steam', 'InstallPath', S) and (S <> '') then begin
    Result := S;
    Exit;
  end;
end;

// Walk libraryfolders.vdf for a Steam library that has Kenshi (app id 233860).
// Returns the library root (without "\steamapps") or '' if not found.
function FindKenshiLibraryRoot(): String;
var
  SteamPath, VdfPath: String;
  Lines: TArrayOfString;
  i: Integer;
  LineLower, CurrentPath, Token: String;
  HasKenshiInBlock: Boolean;
  PathStart, PathEnd: Integer;
begin
  Result := '';
  SteamPath := ReadSteamPath();
  if SteamPath = '' then Exit;
  VdfPath := SteamPath + '\steamapps\libraryfolders.vdf';
  if not FileExists(VdfPath) then Exit;
  if not LoadStringsFromFile(VdfPath, Lines) then Exit;

  CurrentPath := '';
  HasKenshiInBlock := False;
  for i := 0 to GetArrayLength(Lines) - 1 do begin
    LineLower := Lowercase(Lines[i]);
    // "path"  "C:\\SteamLibrary"
    PathStart := Pos('"path"', LineLower);
    if PathStart > 0 then begin
      // capture everything between the second and third quote on the line
      Token := Lines[i];
      Delete(Token, 1, PathStart + Length('"path"') - 1);
      PathStart := Pos('"', Token);
      if PathStart > 0 then begin
        Delete(Token, 1, PathStart);
        PathEnd := Pos('"', Token);
        if PathEnd > 1 then begin
          CurrentPath := Copy(Token, 1, PathEnd - 1);
          StringChangeEx(CurrentPath, '\\', '\', True);
        end;
      end;
      HasKenshiInBlock := False;
      Continue;
    end;
    // "233860"  "1234567"
    if Pos('"' + KenshiAppId + '"', Lines[i]) > 0 then
      HasKenshiInBlock := True;
    // end of block
    if Trim(Lines[i]) = '}' then begin
      if HasKenshiInBlock and (CurrentPath <> '') then begin
        Result := CurrentPath;
        Exit;
      end;
      CurrentPath := '';
      HasKenshiInBlock := False;
    end;
  end;
end;

function ProbeKenshiPath(const Path: String): Boolean;
begin
  Result := (Path <> '') and FileExists(Path + '\kenshi_x64.exe');
end;

function DetectKenshiDir(Param: String): String;
var
  LibRoot, Candidate: String;
  I: Integer;
  Probes: array[0..8] of String;
begin
  // 1. Steam library that lists app 233860
  LibRoot := FindKenshiLibraryRoot();
  if LibRoot <> '' then begin
    Candidate := LibRoot + '\steamapps\common\Kenshi';
    if ProbeKenshiPath(Candidate) then begin
      Result := Candidate;
      Exit;
    end;
  end;

  // 2. Common fallback locations
  Probes[0] := ExpandConstant('{commonpf32}') + '\Steam\steamapps\common\Kenshi';
  Probes[1] := ExpandConstant('{commonpf}')   + '\Steam\steamapps\common\Kenshi';
  Probes[2] := 'C:\SteamLibrary\steamapps\common\Kenshi';
  Probes[3] := 'D:\SteamLibrary\steamapps\common\Kenshi';
  Probes[4] := 'E:\SteamLibrary\steamapps\common\Kenshi';
  Probes[5] := 'F:\SteamLibrary\steamapps\common\Kenshi';
  Probes[6] := 'G:\SteamLibrary\steamapps\common\Kenshi';
  Probes[7] := 'C:\GOG Games\Kenshi';
  Probes[8] := 'D:\GOG Games\Kenshi';

  for I := 0 to 8 do
    if ProbeKenshiPath(Probes[I]) then begin
      Result := Probes[I];
      Exit;
    end;

  // 3. Last-resort default — user can browse from the Directory page
  Result := ExpandConstant('{commonpf32}') + '\Steam\steamapps\common\Kenshi';
end;

// ---- Pre-install validation ----

function IsKenshiRunning(): Boolean;
var
  ResultCode: Integer;
  TmpFile: String;
  Output: AnsiString;
begin
  Result := False;
  TmpFile := ExpandConstant('{tmp}\tasklist.txt');
  // tasklist /FI works on every supported Windows
  if not Exec(ExpandConstant('{cmd}'),
              '/C tasklist /FI "IMAGENAME eq kenshi_x64.exe" > "' + TmpFile + '"',
              '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Exit;
  if not LoadStringFromFile(TmpFile, Output) then Exit;
  Result := Pos('kenshi_x64.exe', Output) > 0;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpReady then begin
    if IsKenshiRunning() then begin
      MsgBox('Kenshi is currently running. Close it before installing Kenshi-Online.',
             mbError, MB_OK);
      Result := False;
      Exit;
    end;
    if not ProbeKenshiPath(WizardDirValue()) then begin
      if MsgBox('kenshi_x64.exe was not found in:' + #13#10 + WizardDirValue() + #13#10#13#10 +
                'Continue anyway? (The mod will not work unless this is the Kenshi install folder.)',
                mbConfirmation, MB_YESNO) = IDNO then
        Result := False;
    end;
  end;
end;

// ---- Backup + config patch ----

procedure BackupIfMissing(const Src, BackupDir, BackupName: String);
var
  Dest: String;
begin
  Dest := BackupDir + '\' + BackupName;
  if FileExists(Src) and (not FileExists(Dest)) then
    FileCopy(Src, Dest, False);
end;

procedure PatchPluginsCfg(const KenshiDir: String);
var
  CfgPath: String;
begin
  CfgPath := KenshiDir + '\' + PluginsCfgFile;
  AppendLineUnique(CfgPath, PluginLine);
end;

procedure PatchModsList(const KenshiDir: String);
var
  ListPath: String;
begin
  ListPath := KenshiDir + '\' + ModsListFile;
  ForceDirectories(ExtractFileDir(ListPath));
  AppendLineUnique(ListPath, ModName);
end;

procedure DoPostInstall();
var
  KenshiDir, BackupDir: String;
begin
  KenshiDir := ExpandConstant('{app}');
  BackupDir := KenshiDir + '\KenshiMP_backup';
  ForceDirectories(BackupDir);

  BackupIfMissing(KenshiDir + '\' + PluginsCfgFile, BackupDir, 'Plugins_x64.cfg.bak');
  BackupIfMissing(KenshiDir + '\' + ModsListFile,   BackupDir, '__mods.list.bak');
  BackupIfMissing(KenshiDir + '\data\gui\layout\Kenshi_MainMenu.layout',
                  BackupDir, 'Kenshi_MainMenu.layout.bak');

  PatchPluginsCfg(KenshiDir);
  PatchModsList(KenshiDir);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    DoPostInstall();
end;

// ---- Uninstall ----

procedure RemovePluginLineFromCfg(const CfgPath: String);
var
  Lines, Kept: TArrayOfString;
  Joined: AnsiString;
  i, n: Integer;
begin
  if not FileExists(CfgPath) then Exit;
  if not LoadStringsFromFile(CfgPath, Lines) then Exit;
  SetArrayLength(Kept, 0);
  n := 0;
  for i := 0 to GetArrayLength(Lines) - 1 do
    if Trim(Lines[i]) <> PluginLine then begin
      SetArrayLength(Kept, n + 1);
      Kept[n] := Lines[i];
      n := n + 1;
    end;
  Joined := '';
  for i := 0 to GetArrayLength(Kept) - 1 do
    Joined := Joined + Kept[i] + #13#10;
  SaveStringToFile(CfgPath, Joined, False);
end;

procedure RemoveModFromList(const ListPath: String);
var
  Lines, Kept: TArrayOfString;
  Joined: AnsiString;
  i, n: Integer;
begin
  if not FileExists(ListPath) then Exit;
  if not LoadStringsFromFile(ListPath, Lines) then Exit;
  SetArrayLength(Kept, 0);
  n := 0;
  for i := 0 to GetArrayLength(Lines) - 1 do
    if Trim(Lines[i]) <> ModName then begin
      SetArrayLength(Kept, n + 1);
      Kept[n] := Lines[i];
      n := n + 1;
    end;
  Joined := '';
  for i := 0 to GetArrayLength(Kept) - 1 do
    Joined := Joined + Kept[i] + #13#10;
  SaveStringToFile(ListPath, Joined, False);
end;

procedure RestoreBackupIfPresent(const KenshiDir, RelPath, BackupName: String);
var
  Backup, Target: String;
begin
  Backup := KenshiDir + '\KenshiMP_backup\' + BackupName;
  Target := KenshiDir + '\' + RelPath;
  if FileExists(Backup) then begin
    DeleteFile(Target);
    FileCopy(Backup, Target, False);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  KenshiDir: String;
begin
  if CurUninstallStep = usUninstall then begin
    KenshiDir := ExpandConstant('{app}');
    // Prefer restoring the original files when we have backups; otherwise
    // strip our additions in place.
    if FileExists(KenshiDir + '\KenshiMP_backup\Plugins_x64.cfg.bak') then
      RestoreBackupIfPresent(KenshiDir, PluginsCfgFile, 'Plugins_x64.cfg.bak')
    else
      RemovePluginLineFromCfg(KenshiDir + '\' + PluginsCfgFile);

    if FileExists(KenshiDir + '\KenshiMP_backup\__mods.list.bak') then
      RestoreBackupIfPresent(KenshiDir, ModsListFile, '__mods.list.bak')
    else
      RemoveModFromList(KenshiDir + '\' + ModsListFile);

    if FileExists(KenshiDir + '\KenshiMP_backup\Kenshi_MainMenu.layout.bak') then
      RestoreBackupIfPresent(KenshiDir, 'data\gui\layout\Kenshi_MainMenu.layout',
                             'Kenshi_MainMenu.layout.bak');

    // Remove the mod files (the [Files] uninstall handles our binaries + layouts;
    // these are the pieces we copied to data\ and mods\).
    DeleteFile(KenshiDir + '\data\kenshi-online.mod');
    DelTree(KenshiDir + '\mods\kenshi-online', True, True, True);
  end;
end;
