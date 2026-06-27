; Locus - Inno Setup installer
;
; Builds Locus-Setup-<version>-win-x64.exe. The setup carries the app binaries
; only (exe + pdfium.dll + prism + model download scripts); the embedding /
; reranker GGUF models are NOT bundled (they are 58 MB - 1.27 GB). Instead a
; custom wizard page lets the user pick a model profile, and the matching
; PowerShell downloader is run on the first launch / post-install step into
; {app}\models.
;
; Compile:
;   "C:\Users\<you>\AppData\Local\Programs\Inno Setup 6\ISCC.exe" ^
;       /DAppVersion=0.1.0-beta installer\locus.iss
;
; AppVersion is supplied by the packaging script via /D. A fallback default is
; defined below so the .iss also compiles standalone.

#ifndef AppVersion
  #define AppVersion "0.1.0-beta"
#endif

#define AppName        "Locus"
#define AppPublisher   "Serhiy-Todchuk"
#define AppURL         "https://github.com/Serhiy-Todchuk/Locus"
#define AppExeName     "locus_gui.exe"

; PayloadDir is the staged portable tree (produced by package.ps1). Override
; with /DPayloadDir=... ; default matches package.ps1's layout.
#ifndef PayloadDir
  #define PayloadDir "..\dist\Locus-" + AppVersion + "-win-x64"
#endif

[Setup]
AppId={{B7E6F4A2-1C3D-4E9A-9F2B-7A5C8D1E0F33}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
AppUpdatesURL={#AppURL}/releases
DefaultDirName={autopf}\Locus
DefaultGroupName=Locus
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename=Locus-Setup-{#AppVersion}-win-x64
SetupIconFile=..\assets\icons\windows\locus.ico
UninstallDisplayIcon={app}\{#AppExeName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Per-user install by default so no admin prompt; user can elevate to install
; to Program Files if they choose a machine-wide dir.
PrivilegesRequiredOverridesAllowed=dialog
PrivilegesRequired=lowest

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; App binaries (whole staged payload). The models\ subfolder in the payload
; carries ONLY the download scripts + README, never the GGUFs.
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Locus"; Filename: "{app}\{#AppExeName}"
Name: "{group}\{cm:UninstallProgram,Locus}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Locus"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
; After install, optionally fetch the chosen model profile, then offer launch.
Filename: "powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\models\{code:GetDownloadScript}"" {code:GetDownloadArgs}"; \
  StatusMsg: "Downloading semantic-search models (this may take a while)..."; \
  Flags: runhidden waituntilterminated; \
  Check: ShouldDownloadModels
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,Locus}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Models the installer downloaded into {app}\models live alongside the scripts;
; remove the whole models dir on uninstall (workspace .locus/ data is untouched).
Type: filesandordirs; Name: "{app}\models"

[Code]
var
  ModelPage: TInputOptionWizardPage;
  RerankerPage: TInputOptionWizardPage;

const
  PROFILE_RECOMMENDED = 0;
  PROFILE_SMALL       = 1;
  PROFILE_NONE        = 2;

procedure InitializeWizard;
begin
  ModelPage := CreateInputOptionPage(wpSelectTasks,
    'Semantic search models',
    'Choose which embedding model Locus downloads for semantic code/document search.',
    'Locus works without these models (full-text + symbol search still function), ' +
    'but semantic search needs a local embedding model. You can also skip now and ' +
    'run models\download.ps1 later. Models download into the install folder.',
    True, False);
  ModelPage.Add('Recommended - multilingual (bge-m3),  ~635 MB embedder');
  ModelPage.Add('Small - English only (bge-small-en),  ~37 MB embedder');
  ModelPage.Add('None - I will add my own model later');
  ModelPage.SelectedValueIndex := PROFILE_RECOMMENDED;

  RerankerPage := CreateInputOptionPage(ModelPage.ID,
    'Reranker model (optional)',
    'A reranker improves semantic-search result ordering.',
    'The reranker is optional. It adds accuracy at the cost of disk and a little ' +
    'latency per search. The size matches the profile you picked on the previous page.',
    False, False);
  RerankerPage.Add('Also download the matching reranker model');
  RerankerPage.Values[0] := True;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  { Skip the reranker page entirely when the user picked "None". }
  if (PageID = RerankerPage.ID) and (ModelPage.SelectedValueIndex = PROFILE_NONE) then
    Result := True;
end;

function ShouldDownloadModels: Boolean;
begin
  Result := ModelPage.SelectedValueIndex <> PROFILE_NONE;
end;

{ Which PowerShell script to invoke for the chosen profile. }
function GetDownloadScript(Param: String): String;
begin
  if ModelPage.SelectedValueIndex = PROFILE_SMALL then
    Result := 'download-small.ps1'
  else
    Result := 'download.ps1';
end;

{ Args control embedder-only vs embedder+reranker. The two scripts take
  different flag shapes for "no reranker":
    download.ps1        -Embedder            (embedder only)
    download-small.ps1  -Embedder -NoReranker (embedder only)
  With reranker we pass no flags so both download both. }
function GetDownloadArgs(Param: String): String;
var
  withReranker: Boolean;
begin
  withReranker := RerankerPage.Values[0];
  if withReranker then
    Result := ''
  else if ModelPage.SelectedValueIndex = PROFILE_SMALL then
    Result := '-Embedder -NoReranker'
  else
    Result := '-Embedder';
end;
