@echo off
rem Usage: build.bat <output-exe-path>
setlocal
rem Locate the newest Visual Studio that has the C++ toolset (any version or edition).
set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
  echo Visual Studio with the "Desktop development with C++" workload was not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if "%~1"=="" ( set OUT=%~dp0..\app\ClaudeWidgetNative.exe ) else ( set OUT=%~1 )
cl /nologo /O1 /MT /EHsc /GS- /std:c++17 /utf-8 /DUNICODE /D_UNICODE "%~dp0main.cpp" /Fo"%~dp0main.obj" /Fe"%OUT%" /link /SUBSYSTEM:WINDOWS ^
  user32.lib gdi32.lib gdiplus.lib shell32.lib dwmapi.lib winhttp.lib advapi32.lib comctl32.lib
endlocal
