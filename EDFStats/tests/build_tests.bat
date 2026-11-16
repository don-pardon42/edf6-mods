@echo off
rem Builds and runs the offline tests (no game needed): network/report, rescue detection, overlay rendering.
setlocal
cd /d "%~dp0"
for /f "delims=" %%i in ('"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath') do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set SRC=..\src
set D=..\..\deps
set LIBS=%D%\minhook\src\buffer.c %D%\minhook\src\hook.c %D%\minhook\src\trampoline.c %D%\minhook\src\hde\hde64.c %D%\imgui\imgui.cpp %D%\imgui\imgui_draw.cpp %D%\imgui\imgui_tables.cpp %D%\imgui\imgui_widgets.cpp %D%\imgui\backends\imgui_impl_dx11.cpp %D%\imgui\backends\imgui_impl_win32.cpp
if not exist obj mkdir obj
if not exist out mkdir out
for %%t in (test_network test_rescue test_overlay) do (
  cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 /W3 /I%SRC% /I%D% /I%D%\minhook\include /I%D%\imgui /I%D%\imgui\backends /Foobj\ %%t.cpp %LIBS% /Fe:out\%%t.exe /link ws2_32.lib user32.lib d3d11.lib >nul || (echo BUILD FAILED %%t & exit /b 1)
)
echo ==== network / report & out\test_network.exe "%CD%\out\\"
echo ==== rescue & out\test_rescue.exe
echo ==== overlay (writes out\live.bmp and out\board.bmp) & out\test_overlay.exe "%CD%\out\\"
