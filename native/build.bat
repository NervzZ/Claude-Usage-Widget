@echo off
rem Usage: build.bat <output-exe-path>
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if "%~1"=="" ( set OUT=%~dp0..\app\ClaudeWidgetNative.exe ) else ( set OUT=%~1 )
cl /nologo /O1 /MT /EHsc /GS- /std:c++17 /utf-8 /DUNICODE /D_UNICODE "%~dp0main.cpp" /Fo"%~dp0main.obj" /Fe"%OUT%" /link /SUBSYSTEM:WINDOWS ^
  user32.lib gdi32.lib gdiplus.lib shell32.lib dwmapi.lib winhttp.lib advapi32.lib comctl32.lib
endlocal
