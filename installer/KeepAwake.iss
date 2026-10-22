; Установщик KeepAwake (Inno Setup 6).
; Ставит программу в профиль текущего пользователя, права администратора не нужны.
; Сборка: iscc /DAppVersion=2.5.0 installer\KeepAwake.iss
; KeepAwake.exe берётся из build\Release (сборка CMake + MSVC).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

#ifndef ExeSource
  #define ExeSource "..\build\Release\KeepAwake.exe"
#endif

[Setup]
AppId={{2F900208-98DF-4A05-9A32-53EB215FEC91}
AppName=KeepAwake
AppVersion={#AppVersion}
AppVerName=KeepAwake {#AppVersion}
AppPublisher=Goodvin013
AppPublisherURL=https://github.com/Goodvin-013/KeepAwake
VersionInfoVersion={#AppVersion}
PrivilegesRequired=lowest
DefaultDirName={autopf}\KeepAwake
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
MinVersion=10.0
OutputDir=..\build\installer
OutputBaseFilename=KeepAwake-Setup-{#AppVersion}
SetupIconFile=..\src\icons\KeepAwake-On.ico
UninstallDisplayIcon={app}\KeepAwake.exe
UninstallDisplayName=KeepAwake
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; Если KeepAwake запущен, установщик предложит закрыть его.
AppMutex=Local\KeepAwake.Tray
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#ExeSource}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autodesktop}\KeepAwake"; Filename: "{app}\KeepAwake.exe"; Tasks: desktopicon

[Code]
// Номер версии в заголовке окна установки: «Установка — KeepAwake 2.5.0».
procedure InitializeWizard;
begin
  WizardForm.Caption := WizardForm.Caption + ' {#AppVersion}';
end;

// При удалении убираем и сохранённый выбор языка.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\KeepAwake');
end;
