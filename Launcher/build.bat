@echo off
rem Builds EDF6Launcher.exe (x64, static CRT, no runtime needed) into Launcher\out.
setlocal
cd /d "%~dp0"
for /f "delims=" %%i in ('"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath') do set "VS=%%i"
if not defined VS (echo Visual Studio with C++ tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set D=..\deps
if not exist obj mkdir obj
if not exist out mkdir out
cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 /W3 /DUNICODE /D_UNICODE /I%D%\imgui /I%D%\imgui\backends /Foobj\ ^
   src\launcher.cpp %D%\imgui\imgui.cpp %D%\imgui\imgui_draw.cpp %D%\imgui\imgui_tables.cpp %D%\imgui\imgui_widgets.cpp ^
   %D%\imgui\backends\imgui_impl_dx11.cpp %D%\imgui\backends\imgui_impl_win32.cpp ^
   /Fe:out\EDF6Launcher.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib d3d11.lib winhttp.lib bcrypt.lib shell32.lib || exit /b 1
echo Built: Launcher\out\EDF6Launcher.exe
