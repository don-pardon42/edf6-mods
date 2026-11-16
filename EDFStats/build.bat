@echo off
rem Builds EDFStats.dll (x64, static CRT) into EDFStats\out.
rem If EDF6_DIR points to the game folder, the DLL is also copied to %EDF6_DIR%\Mods\Plugins.
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "delims=" %%i in ('"%VSWHERE%" -latest -products * -property installationPath') do set "VS=%%i"
if not defined VS (echo Visual Studio with C++ tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set D=..\deps
if not exist obj mkdir obj
if not exist out mkdir out
cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 /LD /W3 /I%D% /I%D%\minhook\include /I%D%\minhook\src /I%D%\imgui /I%D%\imgui\backends /Foobj\ ^
   src\edfstats.cpp %D%\minhook\src\buffer.c %D%\minhook\src\hook.c %D%\minhook\src\trampoline.c %D%\minhook\src\hde\hde64.c ^
   %D%\imgui\imgui.cpp %D%\imgui\imgui_draw.cpp %D%\imgui\imgui_tables.cpp %D%\imgui\imgui_widgets.cpp ^
   %D%\imgui\backends\imgui_impl_dx11.cpp %D%\imgui\backends\imgui_impl_win32.cpp ^
   /Fe:out\EDFStats.dll /link /DLL ws2_32.lib user32.lib d3d11.lib || exit /b 1
copy /y EDFStats.ini out\EDFStats.ini >nul
echo Built: EDFStats\out\EDFStats.dll
if defined EDF6_DIR (
  copy /y out\EDFStats.dll "%EDF6_DIR%\Mods\Plugins\EDFStats.dll" >nul && echo Copied to %EDF6_DIR%\Mods\Plugins
  if not exist "%EDF6_DIR%\Mods\Plugins\EDFStats.ini" copy /y EDFStats.ini "%EDF6_DIR%\Mods\Plugins\EDFStats.ini" >nul
)
