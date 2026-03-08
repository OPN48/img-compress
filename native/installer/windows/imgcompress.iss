#ifndef AppName
  #error AppName 未定义
#endif
#ifndef AppExeName
  #error AppExeName 未定义
#endif

[Setup]
AppName={#AppName}
AppVersion=1.0.0
AppPublisher={#AppName}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableDirPage=no
DisableProgramGroupPage=no
OutputDir=.
OutputBaseFilename={#AppName}-Setup
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2
SolidCompression=yes
LicenseFile=..\..\..\LICENSE
ShowLanguageDialog=yes
LanguageDetectionMethod=locale

[Languages]
#ifexist "..\lang\ChineseSimplified.isl"
Name: "chinesesimp"; MessagesFile: "..\lang\ChineseSimplified.isl"
#else
  #ifexist "compiler:Languages\ChineseSimplified.isl"
  Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
  #else
  Name: "chinesesimp"; MessagesFile: "compiler:Default.isl"
  #endif
#endif
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; Flags: unchecked

[Files]
Source: "..\..\dist\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{commondesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "安装完成后运行"; Flags: nowait postinstall skipifsilent
